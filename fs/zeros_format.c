/*
 * zeros_format.c — Formateador del filesystem ZEROS
 *
 * Crea una imagen de disco con la estructura definida en zeros_fs.h.
 * Todos los parámetros se calculan a partir del tamaño del disco —
 * ningún valor está hardcodeado.
 *
 * Uso: ./zeros_format <archivo> <tamaño>
 *   Ejemplos:
 *     ./zeros_format disk.img 64M
 *     ./zeros_format disk.img 512K
 *     ./zeros_format disk.img 1G
 *
 * Conceptos cubiertos:
 *   - Layout de un filesystem real en disco
 *   - Bitmaps para gestión de espacio libre
 *   - Inodos y extents
 *   - Entradas de directorio
 *   - pread/pwrite para I/O posicional
 *   - ftruncate para crear archivos dispersos (sparse files)
 */

#define _POSIX_C_SOURCE 200809L

#include "zeros_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>   /* fstat, S_ISBLK */
#include <sys/ioctl.h>  /* ioctl */
#ifdef __linux__
/* 0x80081272 = _IOR(0x12, 114, size_t) en x86-64 — evita incluir linux/fs.h */
#ifndef BLKGETSIZE64
#define BLKGETSIZE64  0x80081272
#endif
#endif

/* ── Macros ──────────────────────────────────────────────── */

/* División entera redondeando hacia arriba */
#define DIV_CEIL(a, b)  (((uint64_t)(a) + (b) - 1) / (b))

/* Tamaño mínimo: suficiente para superboque + bitmaps + 1 bloque de datos */
#define MIN_DISK_SIZE   (64 * 1024)   /* 64 KB */

/* ══════════════════════════════════════════════════════════
 * Bitmap
 *
 * Representamos bloques e inodos libres/usados como un array
 * de bytes donde cada bit corresponde a un bloque o inodo.
 * Bit 0 del byte 0 = bloque/inodo 0, bit 1 = bloque/inodo 1...
 * ══════════════════════════════════════════════════════════ */

static void bitmap_set(uint8_t *bm, uint32_t bit) {
    bm[bit / 8] |= (1u << (bit % 8));
}

static int bitmap_test(const uint8_t *bm, uint32_t bit) {
    return (bm[bit / 8] >> (bit % 8)) & 1;
}

/*
 * Encuentra el primer bit libre en el bitmap, lo marca como usado
 * y devuelve el índice. Devuelve UINT32_MAX si no hay bits libres.
 *
 * No toca el superbloque — el caller decrementa free_blocks/free_inodes.
 * Esto evita tomar la dirección de miembros de structs packed, lo que
 * puede causar accesos desalineados en ARM.
 */
static uint32_t bitmap_alloc(uint8_t *bm, uint32_t total) {
    for (uint32_t i = 0; i < total; i++) {
        if (!bitmap_test(bm, i)) {
            bitmap_set(bm, i);
            return i;
        }
    }
    return UINT32_MAX;
}

/* ══════════════════════════════════════════════════════════
 * I/O de bloques
 *
 * pwrite/pread hacen I/O en una posición concreta del fichero
 * sin mover el puntero de archivo — más limpio que lseek+write.
 * ══════════════════════════════════════════════════════════ */

static int write_block(int fd, uint32_t block_num, const void *data) {
    off_t off = (off_t)block_num * ZEROS_BLOCK_SIZE;
    if (pwrite(fd, data, ZEROS_BLOCK_SIZE, off) != ZEROS_BLOCK_SIZE) {
        perror("zeros_format: write_block");
        return -1;
    }
    return 0;
}

static int read_block(int fd, uint32_t block_num, void *data) {
    off_t off = (off_t)block_num * ZEROS_BLOCK_SIZE;
    if (pread(fd, data, ZEROS_BLOCK_SIZE, off) != ZEROS_BLOCK_SIZE) {
        perror("zeros_format: read_block");
        return -1;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════
 * I/O de inodos
 *
 * Un inodo es parte de un bloque de la tabla de inodos.
 * Para leer/escribir un inodo concreto:
 *   1. Calculamos en qué bloque de la tabla está
 *   2. Leemos ese bloque completo
 *   3. Modificamos el inodo dentro del bloque
 *   4. Escribimos el bloque de vuelta
 * (Read-Modify-Write — necesario porque un bloque contiene
 * ZEROS_INODES_PER_BLOCK inodos y no podemos escribir menos)
 * ══════════════════════════════════════════════════════════ */

static int write_inode(int fd, const zeros_superblock *sb,
                       uint32_t ino, const zeros_inode *inode) {
    uint32_t blk    = sb->inode_table_start + ino / ZEROS_INODES_PER_BLOCK;
    uint32_t offset = (ino % ZEROS_INODES_PER_BLOCK) * ZEROS_INODE_SIZE;
    uint8_t  buf[ZEROS_BLOCK_SIZE];

    if (read_block(fd, blk, buf)  < 0) return -1;
    memcpy(buf + offset, inode, ZEROS_INODE_SIZE);
    return write_block(fd, blk, buf);
}

static int read_inode(int fd, const zeros_superblock *sb,
                      uint32_t ino, zeros_inode *inode) {
    uint32_t blk    = sb->inode_table_start + ino / ZEROS_INODES_PER_BLOCK;
    uint32_t offset = (ino % ZEROS_INODES_PER_BLOCK) * ZEROS_INODE_SIZE;
    uint8_t  buf[ZEROS_BLOCK_SIZE];

    if (read_block(fd, blk, buf) < 0) return -1;
    memcpy(inode, buf + offset, ZEROS_INODE_SIZE);
    return 0;
}

/* ══════════════════════════════════════════════════════════
 * Operaciones de directorio
 * ══════════════════════════════════════════════════════════ */

/*
 * Busca una entrada con nombre 'name' dentro del directorio dir_ino.
 * Recorre todos los bloques del directorio (sus extents).
 * Devuelve el número de inodo si encuentra la entrada, UINT32_MAX si no.
 */
static uint32_t dir_lookup(int fd, const zeros_superblock *sb,
                           uint32_t dir_ino, const char *name) {
    zeros_inode dir;
    if (read_inode(fd, sb, dir_ino, &dir) < 0) return UINT32_MAX;

    uint8_t buf[ZEROS_BLOCK_SIZE];
    for (uint16_t e = 0; e < dir.extent_count; e++) {
        for (uint32_t b = 0; b < dir.extents[e].length; b++) {
            if (read_block(fd, dir.extents[e].start_block + b, buf) < 0)
                return UINT32_MAX;
            zeros_dirent *entries = (zeros_dirent *)buf;
            for (int i = 0; i < ZEROS_DIRENTS_PER_BLOCK; i++) {
                if (entries[i].inode == ZEROS_DIRENT_FREE) continue;
                if (strcmp(entries[i].name, name) == 0)
                    return entries[i].inode;
            }
        }
    }
    return UINT32_MAX;
}

/*
 * Añade una entrada (name → child_ino, type) al directorio dir_ino.
 *
 * Primero busca un slot libre en los bloques existentes del directorio.
 * Si no hay hueco, asigna un bloque nuevo (nuevo extent).
 */
static int dir_add_entry(int fd, zeros_superblock *sb, uint8_t *block_bitmap,
                         uint32_t dir_ino, const char *name,
                         uint32_t child_ino, uint32_t type) {
    zeros_inode dir;
    if (read_inode(fd, sb, dir_ino, &dir) < 0) return -1;

    uint8_t buf[ZEROS_BLOCK_SIZE];

    /* ── Buscar slot libre en bloques existentes ── */
    for (uint16_t e = 0; e < dir.extent_count; e++) {
        for (uint32_t b = 0; b < dir.extents[e].length; b++) {
            uint32_t blk = dir.extents[e].start_block + b;
            if (read_block(fd, blk, buf) < 0) return -1;
            zeros_dirent *entries = (zeros_dirent *)buf;
            for (int i = 0; i < ZEROS_DIRENTS_PER_BLOCK; i++) {
                if (entries[i].inode != ZEROS_DIRENT_FREE) continue;
                /* Hueco encontrado — escribimos la entrada */
                entries[i].inode = child_ino;
                entries[i].type  = type;
                strncpy(entries[i].name, name, ZEROS_NAME_MAX);
                entries[i].name[ZEROS_NAME_MAX] = '\0';
                if (write_block(fd, blk, buf) < 0) return -1;
                dir.size += sizeof(zeros_dirent);
                return write_inode(fd, sb, dir_ino, &dir);
            }
        }
    }

    /* ── Sin hueco: necesitamos un bloque nuevo ── */
    if (dir.extent_count >= ZEROS_MAX_EXTENTS) {
        fprintf(stderr, "zeros_format: '%s': directorio lleno\n", name);
        return -1;
    }
    uint32_t new_blk = bitmap_alloc(block_bitmap, sb->total_blocks);
    if (new_blk == UINT32_MAX) {
        fprintf(stderr, "zeros_format: disco lleno al añadir '%s'\n", name);
        return -1;
    }
    sb->free_blocks--;

    /* Marcamos todas las entradas del bloque como libres (0xFF → inode = UINT32_MAX) */
    memset(buf, 0xFF, ZEROS_BLOCK_SIZE);
    zeros_dirent *entries = (zeros_dirent *)buf;
    entries[0].inode = child_ino;
    entries[0].type  = type;
    strncpy(entries[0].name, name, ZEROS_NAME_MAX);
    entries[0].name[ZEROS_NAME_MAX] = '\0';
    if (write_block(fd, new_blk, buf) < 0) return -1;

    dir.extents[dir.extent_count].start_block = new_blk;
    dir.extents[dir.extent_count].length      = 1;
    dir.extent_count++;
    dir.size += sizeof(zeros_dirent);
    return write_inode(fd, sb, dir_ino, &dir);
}

/*
 * Crea un nuevo directorio vacío con nombre 'name' dentro de parent_ino.
 * Devuelve el número de inodo asignado, o UINT32_MAX en error.
 *
 * Un directorio recién creado contiene:
 *   "."  → su propio inodo (referencia a sí mismo)
 *   ".." → el inodo del padre
 * Esto es lo que hace que los filesystems POSIX sean un grafo
 * y no un árbol estricto (el "." crea un ciclo).
 */
static uint32_t create_dir(int fd, zeros_superblock *sb,
                           uint8_t *inode_bitmap, uint8_t *block_bitmap,
                           uint32_t parent_ino, const char *name) {
    /* Asignamos inodo y bloque de datos */
    uint32_t ino = bitmap_alloc(inode_bitmap, sb->total_inodes);
    if (ino == UINT32_MAX) { fprintf(stderr, "zeros_format: inodos agotados\n"); return UINT32_MAX; }
    sb->free_inodes--;

    uint32_t blk = bitmap_alloc(block_bitmap, sb->total_blocks);
    if (blk == UINT32_MAX) { fprintf(stderr, "zeros_format: bloques agotados\n"); return UINT32_MAX; }
    sb->free_blocks--;

    int64_t now = (int64_t)time(NULL);

    /* Construimos el inodo del directorio */
    zeros_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.mode  = ZEROS_FT_DIR
                | ZEROS_S_IRUSR | ZEROS_S_IWUSR | ZEROS_S_IXUSR  /* rwx propietario */
                | ZEROS_S_IRGRP | ZEROS_S_IXGRP                   /* r-x grupo */
                | ZEROS_S_IROTH | ZEROS_S_IXOTH;                   /* r-x otros → 0755 */
    inode.ctime        = now;
    inode.mtime        = now;
    inode.atime        = now;
    inode.link_count   = 2;   /* uno por "." + uno por la entrada en el padre */
    inode.extent_count = 1;
    inode.extents[0].start_block = blk;
    inode.extents[0].length      = 1;
    inode.size = 0;

    /* Inicializamos el bloque de datos del directorio (todas las entradas libres) */
    uint8_t buf[ZEROS_BLOCK_SIZE];
    memset(buf, 0xFF, ZEROS_BLOCK_SIZE);
    if (write_block(fd, blk, buf) < 0) return UINT32_MAX;

    /* Escribimos el inodo */
    if (write_inode(fd, sb, ino, &inode) < 0) return UINT32_MAX;

    /* Añadimos "." y ".." */
    if (dir_add_entry(fd, sb, block_bitmap, ino, ".",  ino,        ZEROS_FT_DIR) < 0) return UINT32_MAX;
    if (dir_add_entry(fd, sb, block_bitmap, ino, "..", parent_ino, ZEROS_FT_DIR) < 0) return UINT32_MAX;

    /* Registramos la entrada en el padre (excepto para root, que es su propio padre) */
    if (parent_ino != ino) {
        if (dir_add_entry(fd, sb, block_bitmap, parent_ino, name, ino, ZEROS_FT_DIR) < 0)
            return UINT32_MAX;
    }

    return ino;
}

/* ══════════════════════════════════════════════════════════
 * zeros_format — función principal
 * ══════════════════════════════════════════════════════════ */

int zeros_format(const char *path, uint64_t disk_size_bytes) {
    /* ── 0. Auto-detectar tamaño para dispositivos de bloque ──
     *
     * Si disk_size_bytes == 0 (o el path es un dispositivo de bloque),
     * usamos ioctl(BLKGETSIZE64) para obtener el tamaño real.
     * Esto permite llamar a zeros_format("/dev/sda", 0) desde init.
     */
    {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISBLK(st.st_mode)) {
            int fd_tmp = open(path, O_RDONLY);
            if (fd_tmp >= 0) {
                uint64_t dev_size = 0;
#ifdef BLKGETSIZE64
                ioctl(fd_tmp, BLKGETSIZE64, &dev_size);
#endif
                close(fd_tmp);
                if (dev_size > 0) disk_size_bytes = dev_size;
            }
        }
    }

    if (disk_size_bytes < MIN_DISK_SIZE) {
        fprintf(stderr, "zeros_format: tamaño mínimo es %d bytes\n", MIN_DISK_SIZE);
        return -1;
    }

    /* ── 1. Calcular layout ──────────────────────────────
     *
     * Todo se deriva de disk_size_bytes. El orden importa:
     * primero calculamos cuántos inodos y bloques hay, luego
     * cuánto espacio ocupan los bitmaps y la tabla de inodos,
     * y por último dónde empieza el área de datos.
     */
    uint32_t total_blocks       = disk_size_bytes / ZEROS_BLOCK_SIZE;

    /* 10% del disco para inodos */
    uint32_t inode_table_blocks = total_blocks * ZEROS_INODE_RATIO / 100;
    if (inode_table_blocks == 0) inode_table_blocks = 1;
    uint32_t total_inodes       = inode_table_blocks * ZEROS_INODES_PER_BLOCK;

    /* Cuántos bloques necesitan los bitmaps */
    uint32_t inode_bitmap_blocks = (uint32_t)DIV_CEIL(total_inodes, ZEROS_BITS_PER_BLOCK);
    uint32_t block_bitmap_blocks = (uint32_t)DIV_CEIL(total_blocks, ZEROS_BITS_PER_BLOCK);

    /* Offsets de cada región */
    uint32_t inode_bitmap_start = 1;   /* bloque 0 = superbloque */
    uint32_t block_bitmap_start = inode_bitmap_start + inode_bitmap_blocks;
    uint32_t inode_table_start  = block_bitmap_start + block_bitmap_blocks;
    uint32_t data_start         = inode_table_start  + inode_table_blocks;

    if (data_start >= total_blocks) {
        fprintf(stderr, "zeros_format: disco demasiado pequeño para los metadatos\n");
        return -1;
    }
    uint32_t data_blocks = total_blocks - data_start;

    /* ── 2. Abrir el destino (archivo o dispositivo de bloque) ──
     *
     * Para archivos normales: O_CREAT|O_TRUNC + ftruncate para fijar tamaño.
     * Para dispositivos de bloque: solo O_RDWR — el tamaño lo da el hardware
     * y no se puede cambiar con ftruncate. Usamos ioctl(BLKGETSIZE64).
     */
    struct stat st;
    int is_blkdev = 0;

    /* Comprobamos si ya existe y es un bloque antes de abrir */
    if (stat(path, &st) == 0 && S_ISBLK(st.st_mode))
        is_blkdev = 1;

    int fd;
    if (is_blkdev) {
        fd = open(path, O_RDWR);
        if (fd < 0) { perror("zeros_format: open"); return -1; }
        /* Obtener el tamaño real del dispositivo */
        uint64_t dev_size = 0;
#ifdef BLKGETSIZE64
        if (ioctl(fd, BLKGETSIZE64, &dev_size) < 0) {
            perror("zeros_format: ioctl BLKGETSIZE64"); close(fd); return -1;
        }
#endif
        if (disk_size_bytes == 0)
            disk_size_bytes = dev_size;
        /* Recalcular total_blocks con el tamaño real */
        total_blocks = disk_size_bytes / ZEROS_BLOCK_SIZE;
    } else {
        fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror("zeros_format: open"); return -1; }
        if (ftruncate(fd, (off_t)disk_size_bytes) < 0) {
            perror("zeros_format: ftruncate"); close(fd); return -1;
        }
    }

    /* ── 3. Bitmaps en memoria ───────────────────────────
     *
     * Asignamos arrays de bytes suficientes para cubrir
     * total_inodes y total_blocks bits respectivamente.
     * calloc() los inicializa a 0 (= todo libre).
     */
    uint32_t ibm_bytes = inode_bitmap_blocks * ZEROS_BLOCK_SIZE;
    uint32_t bbm_bytes = block_bitmap_blocks * ZEROS_BLOCK_SIZE;

    uint8_t *inode_bitmap = calloc(1, ibm_bytes);
    uint8_t *block_bitmap = calloc(1, bbm_bytes);
    if (!inode_bitmap || !block_bitmap) {
        fprintf(stderr, "zeros_format: sin memoria para bitmaps\n");
        free(inode_bitmap); free(block_bitmap); close(fd); return -1;
    }

    /* Marcamos los bloques de metadatos (0..data_start-1) como usados.
     * Así bitmap_alloc nunca intentará asignarlos como datos. */
    for (uint32_t i = 0; i < data_start; i++)
        bitmap_set(block_bitmap, i);

    /* ── 4. Construir el superbloque ─────────────────────*/
    zeros_superblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic               = ZEROS_MAGIC;
    sb.version             = ZEROS_FS_VERSION;
    sb.block_size          = ZEROS_BLOCK_SIZE;
    sb.total_blocks        = total_blocks;
    sb.total_inodes        = total_inodes;
    sb.free_blocks         = data_blocks;   /* se irá reduciendo con cada alloc */
    sb.free_inodes         = total_inodes;  /* ídem */
    sb.inode_bitmap_start  = inode_bitmap_start;
    sb.inode_bitmap_blocks = inode_bitmap_blocks;
    sb.block_bitmap_start  = block_bitmap_start;
    sb.block_bitmap_blocks = block_bitmap_blocks;
    sb.inode_table_start   = inode_table_start;
    sb.inode_table_blocks  = inode_table_blocks;
    sb.data_start          = data_start;
    sb.created_at          = (int64_t)time(NULL);
    sb.mounted_at          = sb.created_at;

    /* ── 5. Crear directorio raíz (inode 0) ──────────────
     *
     * El root es especial: su ".." apunta a sí mismo (ino 0).
     * Usamos create_dir con parent_ino == ino para indicarlo.
     */
    uint32_t root_ino = create_dir(fd, &sb, inode_bitmap, block_bitmap,
                                   ZEROS_ROOT_INODE, "/");
    if (root_ino != ZEROS_ROOT_INODE) {
        fprintf(stderr, "zeros_format: error creando directorio raíz\n");
        free(inode_bitmap); free(block_bitmap); close(fd); return -1;
    }

    /* ── 6. Crear árbol de directorios inicial ───────────
     *
     * Recorremos ZEROS_INITIAL_DIRS en orden (padres antes
     * que hijos). Para cada ruta absoluta:
     *   - Navegamos componente a componente desde root
     *   - Si un componente ya existe (lookup != UINT32_MAX), lo usamos
     *   - Si no existe, lo creamos
     */
    for (int d = 0; ZEROS_INITIAL_DIRS[d] != NULL; d++) {
        /* Hacemos una copia mutable para strtok */
        char path_copy[ZEROS_NAME_MAX * 4];
        strncpy(path_copy, ZEROS_INITIAL_DIRS[d], sizeof(path_copy) - 1);
        path_copy[sizeof(path_copy) - 1] = '\0';

        uint32_t cur_ino = ZEROS_ROOT_INODE;
        char *component = strtok(path_copy, "/");

        while (component != NULL) {
            uint32_t next_ino = dir_lookup(fd, &sb, cur_ino, component);
            if (next_ino == UINT32_MAX) {
                /* No existe: lo creamos */
                next_ino = create_dir(fd, &sb, inode_bitmap, block_bitmap,
                                      cur_ino, component);
                if (next_ino == UINT32_MAX) {
                    fprintf(stderr, "zeros_format: error creando '%s'\n",
                            ZEROS_INITIAL_DIRS[d]);
                    free(inode_bitmap); free(block_bitmap); close(fd); return -1;
                }
            }
            cur_ino = next_ino;
            component = strtok(NULL, "/");
        }
    }

    /* ── 7. Escribir metadatos a disco ───────────────────
     *
     * Escribimos en este orden:
     *   a) Superbloque  (bloque 0)
     *   b) Bitmap de inodos
     *   c) Bitmap de bloques
     *
     * La tabla de inodos ya fue escrita bloque a bloque
     * durante write_inode() — no necesita una pasada extra.
     * El superbloque se escribe al final para que los contadores
     * free_blocks y free_inodes reflejen el estado real.
     */
    if (write_block(fd, 0, &sb) < 0) {
        free(inode_bitmap); free(block_bitmap); close(fd); return -1;
    }
    for (uint32_t i = 0; i < inode_bitmap_blocks; i++) {
        if (write_block(fd, inode_bitmap_start + i,
                        inode_bitmap + i * ZEROS_BLOCK_SIZE) < 0) {
            free(inode_bitmap); free(block_bitmap); close(fd); return -1;
        }
    }
    for (uint32_t i = 0; i < block_bitmap_blocks; i++) {
        if (write_block(fd, block_bitmap_start + i,
                        block_bitmap + i * ZEROS_BLOCK_SIZE) < 0) {
            free(inode_bitmap); free(block_bitmap); close(fd); return -1;
        }
    }

    /* ── 8. Resumen ──────────────────────────────────────*/
    printf("\nZEROS filesystem formateado correctamente\n");
    printf("  Archivo        : %s\n", path);
    printf("  Tamaño         : %llu bytes (%u bloques de %d B)\n",
           (unsigned long long)disk_size_bytes, total_blocks, ZEROS_BLOCK_SIZE);
    printf("  Inodos         : %u  (%u libres)\n", total_inodes, sb.free_inodes);
    printf("\n  Layout del disco:\n");
    printf("    Bloque  0              → superbloque\n");
    printf("    Bloques %u..%u    → bitmap de inodos  (%u bloques)\n",
           inode_bitmap_start, block_bitmap_start - 1, inode_bitmap_blocks);
    printf("    Bloques %u..%u    → bitmap de bloques (%u bloques)\n",
           block_bitmap_start, inode_table_start - 1, block_bitmap_blocks);
    printf("    Bloques %u..%u   → tabla de inodos    (%u bloques)\n",
           inode_table_start, data_start - 1, inode_table_blocks);
    printf("    Bloques %u..%u → datos                (%u bloques libres)\n",
           data_start, total_blocks - 1, sb.free_blocks);
    printf("\n  Directorios creados: /");
    for (int d = 0; ZEROS_INITIAL_DIRS[d] != NULL; d++)
        printf(", %s", ZEROS_INITIAL_DIRS[d]);
    printf("\n\n");

    free(inode_bitmap);
    free(block_bitmap);
    close(fd);
    return 0;
}

/* ══════════════════════════════════════════════════════════
 * main — herramienta de línea de comandos
 * ══════════════════════════════════════════════════════════ */

/* Parsea tamaños humanos: "64M", "1G", "512K" o bytes */
static uint64_t parse_size(const char *s) {
    char *end;
    uint64_t val = strtoull(s, &end, 10);
    switch (*end) {
        case 'K': case 'k': val *= 1024ULL;                 break;
        case 'M': case 'm': val *= 1024ULL * 1024;          break;
        case 'G': case 'g': val *= 1024ULL * 1024 * 1024;   break;
    }
    return val;
}

int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Uso: %s <archivo> [<tamaño>]\n", argv[0]);
        fprintf(stderr, "  Para archivos:          %s disk.img 64M\n", argv[0]);
        fprintf(stderr, "  Para dispositivos:      %s /dev/sda\n",     argv[0]);
        fprintf(stderr, "  (sin tamaño: auto-detect via ioctl)\n");
        return 1;
    }

    uint64_t size = 0;
    if (argc == 3) {
        size = parse_size(argv[2]);
        if (size == 0) {
            fprintf(stderr, "zeros_format: tamaño inválido: '%s'\n", argv[2]);
            return 1;
        }
    }
    /* Si size == 0 y el path es un dispositivo de bloque,
     * zeros_format() detecta el tamaño automáticamente via BLKGETSIZE64. */

    return zeros_format(argv[1], size) == 0 ? 0 : 1;
}
