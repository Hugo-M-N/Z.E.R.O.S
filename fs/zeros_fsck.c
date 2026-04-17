/*
 * zeros_fsck.c — Verificador del filesystem ZEROS
 *
 * Realiza cuatro comprobaciones en orden:
 *
 *   1. Superbloque   — número mágico, versión, layout coherente
 *   2. Inodos        — modo válido, extents dentro del área de datos
 *   3. Bitmaps       — reconstruye los bitmaps reales desde los inodos
 *                      y los compara con los escritos en disco
 *   4. Directorios   — recorre el árbol desde root, verifica "." y "..",
 *                      detecta inodos huérfanos (usados pero inalcanzables)
 *
 * Uso: ./zeros_fsck <imagen>
 */

#define _POSIX_C_SOURCE 200809L

#include "zeros_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* ── Estado global del chequeo ──────────────────────────── */

typedef struct {
    int              fd;
    zeros_superblock sb;
    uint8_t         *inode_bitmap;   /* bitmap leído del disco */
    uint8_t         *block_bitmap;   /* bitmap leído del disco */
    int              errors;
    int              warnings;
} fsck_t;

/* ── Reporting ───────────────────────────────────────────── */

static void fsck_error(fsck_t *f, const char *fmt, ...) {
    va_list ap;
    printf("  [ERROR] ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    f->errors++;
}

static void fsck_warn(fsck_t *f, const char *fmt, ...) {
    va_list ap;
    printf("  [WARN]  ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    f->warnings++;
}

/* ── Helpers de bitmap ───────────────────────────────────── */

static int bitmap_test(const uint8_t *bm, uint32_t bit) {
    return (bm[bit / 8] >> (bit % 8)) & 1;
}

static void bitmap_set(uint8_t *bm, uint32_t bit) {
    bm[bit / 8] |= (1u << (bit % 8));
}

/* ── I/O ─────────────────────────────────────────────────── */

static int read_block(int fd, uint32_t block_num, void *data) {
    off_t off = (off_t)block_num * ZEROS_BLOCK_SIZE;
    if (pread(fd, data, ZEROS_BLOCK_SIZE, off) != ZEROS_BLOCK_SIZE) {
        perror("zeros_fsck: read_block");
        return -1;
    }
    return 0;
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
 * CHECK 1 — Superbloque
 * ══════════════════════════════════════════════════════════ */

static int check_superblock(fsck_t *f) {
    printf("[ 1/4 ] Superbloque\n");

    if (f->sb.magic != ZEROS_MAGIC) {
        fsck_error(f, "numero magico incorrecto: 0x%08X (esperado 0x%08X)",
                   f->sb.magic, ZEROS_MAGIC);
        /* Sin magic válido no tiene sentido continuar */
        return -1;
    }

    if (f->sb.version != ZEROS_FS_VERSION)
        fsck_warn(f, "version %u desconocida (esta herramienta conoce la %u)",
                  f->sb.version, ZEROS_FS_VERSION);

    if (f->sb.block_size != ZEROS_BLOCK_SIZE)
        fsck_error(f, "block_size=%u pero ZEROS_BLOCK_SIZE=%u",
                   f->sb.block_size, ZEROS_BLOCK_SIZE);

    if (f->sb.total_blocks == 0)
        fsck_error(f, "total_blocks == 0");

    if (f->sb.total_inodes == 0)
        fsck_error(f, "total_inodes == 0");

    /* Los offsets deben estar en orden y dentro del disco */
    if (f->sb.inode_bitmap_start == 0 || f->sb.inode_bitmap_start >= f->sb.total_blocks)
        fsck_error(f, "inode_bitmap_start=%u fuera de rango", f->sb.inode_bitmap_start);

    if (f->sb.block_bitmap_start <= f->sb.inode_bitmap_start)
        fsck_error(f, "block_bitmap_start no es posterior a inode_bitmap_start");

    if (f->sb.inode_table_start <= f->sb.block_bitmap_start)
        fsck_error(f, "inode_table_start no es posterior a block_bitmap_start");

    if (f->sb.data_start <= f->sb.inode_table_start)
        fsck_error(f, "data_start no es posterior a inode_table_start");

    if (f->sb.data_start >= f->sb.total_blocks)
        fsck_error(f, "data_start=%u >= total_blocks=%u (sin area de datos)",
                   f->sb.data_start, f->sb.total_blocks);

    if (f->sb.free_blocks > f->sb.total_blocks)
        fsck_error(f, "free_blocks=%u > total_blocks=%u",
                   f->sb.free_blocks, f->sb.total_blocks);

    if (f->sb.free_inodes > f->sb.total_inodes)
        fsck_error(f, "free_inodes=%u > total_inodes=%u",
                   f->sb.free_inodes, f->sb.total_inodes);

    if (f->errors == 0 && f->warnings == 0)
        printf("         OK\n");

    return 0;
}

/* ══════════════════════════════════════════════════════════
 * CHECK 2 — Inodos
 *
 * Para cada inodo marcado como usado en el bitmap del disco:
 *   - El tipo debe ser REG o DIR
 *   - extent_count <= ZEROS_MAX_EXTENTS
 *   - link_count > 0
 *   - Cada extent debe estar dentro del área de datos
 *
 * De paso construimos:
 *   real_inode_bitmap — qué inodos están realmente en uso
 *   real_block_bitmap — qué bloques apuntan los inodos
 * Estos se comparan con los del disco en el check 3.
 * ══════════════════════════════════════════════════════════ */

static void check_inodes(fsck_t *f, uint8_t *real_ibm, uint8_t *real_bbm) {
    printf("[ 2/4 ] Inodos\n");

    int errors_before = f->errors;

    for (uint32_t ino = 0; ino < f->sb.total_inodes; ino++) {
        if (!bitmap_test(f->inode_bitmap, ino)) continue;

        /* Este inodo está marcado como usado en el bitmap del disco */
        bitmap_set(real_ibm, ino);

        zeros_inode inode;
        if (read_inode(f->fd, &f->sb, ino, &inode) < 0) {
            fsck_error(f, "inodo %u: error de lectura", ino);
            continue;
        }

        uint32_t type = inode.mode & ZEROS_FT_MASK;
        if (type != ZEROS_FT_REG && type != ZEROS_FT_DIR)
            fsck_error(f, "inodo %u: tipo invalido en mode=0x%04X", ino, inode.mode);

        if (inode.extent_count > ZEROS_MAX_EXTENTS)
            fsck_error(f, "inodo %u: extent_count=%u > MAX=%u",
                       ino, inode.extent_count, ZEROS_MAX_EXTENTS);

        if (inode.link_count == 0)
            fsck_warn(f, "inodo %u: link_count=0 (candidato a huerfano)", ino);

        /* Verificamos cada extent */
        uint16_t ext_limit = inode.extent_count < ZEROS_MAX_EXTENTS
                           ? inode.extent_count : ZEROS_MAX_EXTENTS;
        for (uint16_t e = 0; e < ext_limit; e++) {
            uint32_t start = inode.extents[e].start_block;
            uint32_t len   = inode.extents[e].length;

            if (start < f->sb.data_start)
                fsck_error(f, "inodo %u extent %u: start_block=%u dentro del area de metadatos",
                           ino, e, start);
            else if (start + len > f->sb.total_blocks)
                fsck_error(f, "inodo %u extent %u: bloques %u..%u fuera del disco (%u bloques total)",
                           ino, e, start, start + len - 1, f->sb.total_blocks);
            else {
                /* Marcamos los bloques como usados en el bitmap real */
                for (uint32_t b = start; b < start + len; b++)
                    bitmap_set(real_bbm, b);
            }
        }
    }

    if (f->errors == errors_before)
        printf("         OK\n");
}

/* ══════════════════════════════════════════════════════════
 * CHECK 3 — Bitmaps
 *
 * Comparamos los bitmaps reconstruidos (real_*) con los
 * escritos en disco. Cualquier diferencia es una inconsistencia:
 *
 *   Bit en disco=1, real=0 → bloque/inodo marcado como usado
 *                             pero ningún inodo lo referencia
 *                             (bloque perdido / lost+found)
 *
 *   Bit en disco=0, real=1 → bloque/inodo en uso pero marcado
 *                             como libre — podría sobreescribirse
 *                             (corrupción grave)
 * ══════════════════════════════════════════════════════════ */

static void check_bitmaps(fsck_t *f, const uint8_t *real_ibm, const uint8_t *real_bbm) {
    printf("[ 3/4 ] Bitmaps\n");

    int errors_before = f->errors;

    /* ── Bitmap de inodos ── */
    for (uint32_t i = 0; i < f->sb.total_inodes; i++) {
        int on_disk = bitmap_test(f->inode_bitmap, i);
        int real    = bitmap_test(real_ibm, i);
        if (on_disk && !real)
            fsck_error(f, "inodo %u: marcado como usado en disco pero no referenciado", i);
        else if (!on_disk && real)
            fsck_error(f, "inodo %u: en uso pero marcado como libre en el bitmap", i);
    }

    /* ── Bitmap de bloques ──
     * Solo comprobamos bloques del área de datos (data_start en adelante).
     * Los bloques de metadatos deben estar marcados como usados en ambos. */
    for (uint32_t b = 0; b < f->sb.total_blocks; b++) {
        int on_disk = bitmap_test(f->block_bitmap, b);
        int real    = bitmap_test(real_bbm, b);

        if (b < f->sb.data_start) {
            /* Bloque de metadatos: debe estar marcado como usado en disco */
            if (!on_disk)
                fsck_error(f, "bloque %u (metadatos): no esta marcado como usado", b);
        } else {
            if (on_disk && !real)
                fsck_error(f, "bloque %u: marcado como usado en disco pero sin inodo que lo apunte", b);
            else if (!on_disk && real)
                fsck_error(f, "bloque %u: apuntado por un inodo pero marcado como libre", b);
        }
    }

    if (f->errors == errors_before)
        printf("         OK\n");
}

/* ══════════════════════════════════════════════════════════
 * CHECK 4 — Árbol de directorios
 *
 * Recorre el árbol desde root en profundidad (DFS).
 * Para cada directorio comprueba:
 *   - "." apunta al propio directorio
 *   - ".." apunta al padre
 *   - Cada entrada apunta a un inodo válido y marcado como usado
 *   - Los nombres no contienen '/' ni están vacíos
 *
 * Al terminar, compara los inodos alcanzados con el inode_bitmap
 * para detectar inodos huérfanos.
 * ══════════════════════════════════════════════════════════ */

/* reachable: bitmap de inodos alcanzados desde root */
static void check_dir_recursive(fsck_t *f, uint8_t *reachable,
                                 uint32_t dir_ino, uint32_t parent_ino,
                                 const char *path) {
    zeros_inode dir;
    if (read_inode(f->fd, &f->sb, dir_ino, &dir) < 0) {
        fsck_error(f, "directorio '%s' (ino=%u): error de lectura", path, dir_ino);
        return;
    }

    if (!ZEROS_IS_DIR(dir.mode)) {
        fsck_error(f, "'%s' (ino=%u): se esperaba DIR pero mode=0x%04X",
                   path, dir_ino, dir.mode);
        return;
    }

    bitmap_set(reachable, dir_ino);

    int found_dot    = 0;
    int found_dotdot = 0;

    uint8_t buf[ZEROS_BLOCK_SIZE];
    for (uint16_t e = 0; e < dir.extent_count && e < ZEROS_MAX_EXTENTS; e++) {
        for (uint32_t b = 0; b < dir.extents[e].length; b++) {
            if (read_block(f->fd, dir.extents[e].start_block + b, buf) < 0) {
                fsck_error(f, "directorio '%s': error leyendo bloque de datos", path);
                return;
            }
            zeros_dirent *entries = (zeros_dirent *)buf;
            for (int i = 0; i < ZEROS_DIRENTS_PER_BLOCK; i++) {
                zeros_dirent *d = &entries[i];
                if (d->inode == ZEROS_DIRENT_FREE) continue;

                /* El inodo referenciado debe ser válido */
                if (d->inode >= f->sb.total_inodes) {
                    fsck_error(f, "'%s': entrada '%s' apunta a inodo %u fuera de rango",
                               path, d->name, d->inode);
                    continue;
                }
                if (!bitmap_test(f->inode_bitmap, d->inode)) {
                    fsck_error(f, "'%s': entrada '%s' apunta a inodo %u marcado como libre",
                               path, d->name, d->inode);
                    continue;
                }

                /* Nombre no puede estar vacío ni contener '/' */
                if (d->name[0] == '\0')
                    fsck_error(f, "'%s': entrada con inodo=%u tiene nombre vacio", path, d->inode);
                if (strchr(d->name, '/'))
                    fsck_error(f, "'%s': nombre '%s' contiene '/'", path, d->name);

                /* Comprobaciones específicas de "." y ".." */
                if (strcmp(d->name, ".") == 0) {
                    found_dot = 1;
                    if (d->inode != dir_ino)
                        fsck_error(f, "'%s': '.' apunta a ino=%u, se esperaba %u",
                                   path, d->inode, dir_ino);

                } else if (strcmp(d->name, "..") == 0) {
                    found_dotdot = 1;
                    if (d->inode != parent_ino)
                        fsck_error(f, "'%s': '..' apunta a ino=%u, se esperaba %u",
                                   path, d->inode, parent_ino);

                } else if (d->type == ZEROS_FT_DIR) {
                    /* Subdirectorio: recursamos solo si no fue visitado aún.
                     * La función recursiva hace bitmap_set al inicio — no lo
                     * marcamos aquí para no romper la condición de ciclo. */
                    if (!bitmap_test(reachable, d->inode)) {
                        char child_path[1024];
                        if (strcmp(path, "/") == 0)
                            snprintf(child_path, sizeof(child_path), "/%s", d->name);
                        else
                            snprintf(child_path, sizeof(child_path), "%s/%s", path, d->name);
                        check_dir_recursive(f, reachable, d->inode, dir_ino, child_path);
                    }
                } else {
                    /* Archivo regular u otro tipo: simplemente marcamos como alcanzable */
                    bitmap_set(reachable, d->inode);
                }
            }
        }
    }

    if (!found_dot)
        fsck_error(f, "'%s' (ino=%u): falta la entrada '.'", path, dir_ino);
    if (!found_dotdot)
        fsck_error(f, "'%s' (ino=%u): falta la entrada '..'", path, dir_ino);
}

static void check_directories(fsck_t *f) {
    printf("[ 4/4 ] Directorios\n");

    int errors_before = f->errors;

    uint32_t ibm_bytes = f->sb.inode_bitmap_blocks * ZEROS_BLOCK_SIZE;
    uint8_t *reachable = calloc(1, ibm_bytes);
    if (!reachable) {
        fsck_error(f, "sin memoria para bitmap de alcanzabilidad");
        return;
    }

    /* El inodo raíz debe existir y estar marcado como usado */
    if (!bitmap_test(f->inode_bitmap, ZEROS_ROOT_INODE))
        fsck_error(f, "inodo raiz (%u) no esta marcado como usado", ZEROS_ROOT_INODE);
    else
        check_dir_recursive(f, reachable, ZEROS_ROOT_INODE, ZEROS_ROOT_INODE, "/");

    /* Buscamos inodos huérfanos: marcados como usados pero no alcanzables desde root */
    for (uint32_t i = 0; i < f->sb.total_inodes; i++) {
        if (bitmap_test(f->inode_bitmap, i) && !bitmap_test(reachable, i))
            fsck_error(f, "inodo %u: en uso pero no alcanzable desde root (huerfano)", i);
    }

    free(reachable);

    if (f->errors == errors_before)
        printf("         OK\n");
}

/* ══════════════════════════════════════════════════════════
 * zeros_fsck — función principal
 * ══════════════════════════════════════════════════════════ */

int zeros_fsck(const char *path) {
    fsck_t f;
    memset(&f, 0, sizeof(f));

    f.fd = open(path, O_RDONLY);
    if (f.fd < 0) { perror("zeros_fsck: open"); return -1; }

    printf("zeros_fsck: verificando '%s'\n\n", path);

    /* Leemos el superbloque (siempre bloque 0) */
    if (pread(f.fd, &f.sb, sizeof(f.sb), 0) != sizeof(f.sb)) {
        perror("zeros_fsck: leyendo superbloque");
        close(f.fd); return -1;
    }

    /* Check 1: superbloque — si falla no podemos continuar */
    if (check_superblock(&f) < 0) {
        printf("\nAbortando: superbloque invalido.\n");
        close(f.fd); return -1;
    }
    if (f.errors > 0) {
        printf("\nAbortando: errores criticos en el superbloque.\n");
        close(f.fd); return -1;
    }

    /* Cargamos los bitmaps del disco en memoria */
    uint32_t ibm_bytes = f.sb.inode_bitmap_blocks * ZEROS_BLOCK_SIZE;
    uint32_t bbm_bytes = f.sb.block_bitmap_blocks * ZEROS_BLOCK_SIZE;
    f.inode_bitmap = malloc(ibm_bytes);
    f.block_bitmap = malloc(bbm_bytes);
    if (!f.inode_bitmap || !f.block_bitmap) {
        fprintf(stderr, "zeros_fsck: sin memoria para bitmaps\n");
        free(f.inode_bitmap); free(f.block_bitmap); close(f.fd); return -1;
    }
    for (uint32_t i = 0; i < f.sb.inode_bitmap_blocks; i++)
        read_block(f.fd, f.sb.inode_bitmap_start + i, f.inode_bitmap + i * ZEROS_BLOCK_SIZE);
    for (uint32_t i = 0; i < f.sb.block_bitmap_blocks; i++)
        read_block(f.fd, f.sb.block_bitmap_start + i, f.block_bitmap + i * ZEROS_BLOCK_SIZE);

    /* Bitmaps reconstruidos desde los inodos (para comparar en check 3) */
    uint8_t *real_ibm = calloc(1, ibm_bytes);
    uint8_t *real_bbm = calloc(1, bbm_bytes);
    if (!real_ibm || !real_bbm) {
        fprintf(stderr, "zeros_fsck: sin memoria para bitmaps reales\n");
        free(f.inode_bitmap); free(f.block_bitmap);
        free(real_ibm); free(real_bbm); close(f.fd); return -1;
    }
    /* Marcamos los bloques de metadatos como usados en el bitmap real */
    for (uint32_t i = 0; i < f.sb.data_start; i++)
        bitmap_set(real_bbm, i);

    /* Checks 2, 3, 4 */
    check_inodes(&f, real_ibm, real_bbm);
    check_bitmaps(&f, real_ibm, real_bbm);
    check_directories(&f);

    /* Resumen */
    printf("\nResultado: %d error(es), %d aviso(s)\n",
           f.errors, f.warnings);
    if (f.errors == 0)
        printf("El filesystem esta limpio.\n\n");
    else
        printf("El filesystem tiene errores — se recomienda reformatear.\n\n");

    free(f.inode_bitmap); free(f.block_bitmap);
    free(real_ibm); free(real_bbm);
    close(f.fd);
    return f.errors == 0 ? 0 : 1;
}

/* ══════════════════════════════════════════════════════════
 * main
 * ══════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <imagen>\n", argv[0]);
        return 1;
    }
    return zeros_fsck(argv[1]);
}
