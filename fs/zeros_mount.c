/*
 * zeros_mount.c — Operaciones sobre el filesystem ZEROS en tiempo de ejecución
 *
 * Implementa la API declarada en zeros_mount.h:
 * abrir/cerrar disco, leer directorios y archivos, crear y borrar entradas.
 *
 * Cada operación de escritura termina con flush_metadata() para mantener
 * los bitmaps y el superbloque consistentes en disco.
 */

#define _POSIX_C_SOURCE 200809L

#include "zeros_mount.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

/* ── Estructura interna ──────────────────────────────────
 *
 * Opaca para los usuarios de la API.
 * Contiene todo lo necesario para operar sobre el disco.
 */
struct zeros_mount {
    int              fd;
    zeros_superblock sb;
    uint8_t         *inode_bitmap;
    uint8_t         *block_bitmap;
    uint32_t         cwd_ino;           /* inodo del directorio actual */
    char             cwd_path[1024];    /* path del directorio actual (para el prompt) */
};

/* ══════════════════════════════════════════════════════════
 * Helpers internos — I/O
 * ══════════════════════════════════════════════════════════ */

static int write_block(int fd, uint32_t blk, const void *data) {
    if (pwrite(fd, data, ZEROS_BLOCK_SIZE, (off_t)blk * ZEROS_BLOCK_SIZE)
            != ZEROS_BLOCK_SIZE) {
        perror("zeros_mount: write_block"); return -1;
    }
    return 0;
}

static int read_block(int fd, uint32_t blk, void *data) {
    if (pread(fd, data, ZEROS_BLOCK_SIZE, (off_t)blk * ZEROS_BLOCK_SIZE)
            != ZEROS_BLOCK_SIZE) {
        perror("zeros_mount: read_block"); return -1;
    }
    return 0;
}

static int write_inode(zeros_mount_t *mnt, uint32_t ino, const zeros_inode *inode) {
    uint32_t blk = mnt->sb.inode_table_start + ino / ZEROS_INODES_PER_BLOCK;
    uint32_t off = (ino % ZEROS_INODES_PER_BLOCK) * ZEROS_INODE_SIZE;
    uint8_t buf[ZEROS_BLOCK_SIZE];
    if (read_block(mnt->fd, blk, buf)  < 0) return -1;
    memcpy(buf + off, inode, ZEROS_INODE_SIZE);
    return write_block(mnt->fd, blk, buf);
}

static int read_inode(zeros_mount_t *mnt, uint32_t ino, zeros_inode *inode) {
    uint32_t blk = mnt->sb.inode_table_start + ino / ZEROS_INODES_PER_BLOCK;
    uint32_t off = (ino % ZEROS_INODES_PER_BLOCK) * ZEROS_INODE_SIZE;
    uint8_t buf[ZEROS_BLOCK_SIZE];
    if (read_block(mnt->fd, blk, buf) < 0) return -1;
    memcpy(inode, buf + off, ZEROS_INODE_SIZE);
    return 0;
}

/* ══════════════════════════════════════════════════════════
 * Helpers internos — Bitmap
 * ══════════════════════════════════════════════════════════ */

static int  bm_test(const uint8_t *bm, uint32_t bit) {
    return (bm[bit / 8] >> (bit % 8)) & 1;
}
static void bm_set(uint8_t *bm, uint32_t bit) {
    bm[bit / 8] |=  (1u << (bit % 8));
}
static void bm_clear(uint8_t *bm, uint32_t bit) {
    bm[bit / 8] &= ~(1u << (bit % 8));
}

static uint32_t alloc_inode(zeros_mount_t *mnt) {
    for (uint32_t i = 0; i < mnt->sb.total_inodes; i++) {
        if (!bm_test(mnt->inode_bitmap, i)) {
            bm_set(mnt->inode_bitmap, i);
            mnt->sb.free_inodes--;
            return i;
        }
    }
    return UINT32_MAX;
}

static uint32_t alloc_block(zeros_mount_t *mnt) {
    /* Empezamos desde data_start para no tocar metadatos */
    for (uint32_t i = mnt->sb.data_start; i < mnt->sb.total_blocks; i++) {
        if (!bm_test(mnt->block_bitmap, i)) {
            bm_set(mnt->block_bitmap, i);
            mnt->sb.free_blocks--;
            return i;
        }
    }
    return UINT32_MAX;
}

static void free_inode(zeros_mount_t *mnt, uint32_t ino) {
    bm_clear(mnt->inode_bitmap, ino);
    mnt->sb.free_inodes++;
}

static void free_block(zeros_mount_t *mnt, uint32_t blk) {
    bm_clear(mnt->block_bitmap, blk);
    mnt->sb.free_blocks++;
}

/* ══════════════════════════════════════════════════════════
 * Helpers internos — Directorio
 * ══════════════════════════════════════════════════════════ */

/* Busca 'name' en el directorio dir_ino. Devuelve su inodo o UINT32_MAX. */
static uint32_t dir_lookup(zeros_mount_t *mnt, uint32_t dir_ino, const char *name) {
    zeros_inode dir;
    if (read_inode(mnt, dir_ino, &dir) < 0) return UINT32_MAX;

    uint8_t buf[ZEROS_BLOCK_SIZE];
    for (uint16_t e = 0; e < dir.extent_count; e++) {
        for (uint32_t b = 0; b < dir.extents[e].length; b++) {
            if (read_block(mnt->fd, dir.extents[e].start_block + b, buf) < 0)
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

/* Añade una entrada al directorio dir_ino. Asigna nuevo bloque si hace falta. */
static int dir_add_entry(zeros_mount_t *mnt, uint32_t dir_ino,
                         const char *name, uint32_t child_ino, uint32_t type) {
    zeros_inode dir;
    if (read_inode(mnt, dir_ino, &dir) < 0) return -1;

    uint8_t buf[ZEROS_BLOCK_SIZE];

    for (uint16_t e = 0; e < dir.extent_count; e++) {
        for (uint32_t b = 0; b < dir.extents[e].length; b++) {
            uint32_t blk = dir.extents[e].start_block + b;
            if (read_block(mnt->fd, blk, buf) < 0) return -1;
            zeros_dirent *entries = (zeros_dirent *)buf;
            for (int i = 0; i < ZEROS_DIRENTS_PER_BLOCK; i++) {
                if (entries[i].inode != ZEROS_DIRENT_FREE) continue;
                entries[i].inode = child_ino;
                entries[i].type  = type;
                strncpy(entries[i].name, name, ZEROS_NAME_MAX);
                entries[i].name[ZEROS_NAME_MAX] = '\0';
                if (write_block(mnt->fd, blk, buf) < 0) return -1;
                dir.size += sizeof(zeros_dirent);
                dir.mtime = (int64_t)time(NULL);
                return write_inode(mnt, dir_ino, &dir);
            }
        }
    }

    /* Sin hueco: nuevo bloque */
    if (dir.extent_count >= ZEROS_MAX_EXTENTS) {
        fprintf(stderr, "zeros_mount: directorio lleno\n"); return -1;
    }
    uint32_t new_blk = alloc_block(mnt);
    if (new_blk == UINT32_MAX) {
        fprintf(stderr, "zeros_mount: disco lleno\n"); return -1;
    }
    memset(buf, 0xFF, ZEROS_BLOCK_SIZE);
    zeros_dirent *entries = (zeros_dirent *)buf;
    entries[0].inode = child_ino;
    entries[0].type  = type;
    strncpy(entries[0].name, name, ZEROS_NAME_MAX);
    entries[0].name[ZEROS_NAME_MAX] = '\0';
    if (write_block(mnt->fd, new_blk, buf) < 0) return -1;

    dir.extents[dir.extent_count].start_block = new_blk;
    dir.extents[dir.extent_count].length      = 1;
    dir.extent_count++;
    dir.size  += sizeof(zeros_dirent);
    dir.mtime  = (int64_t)time(NULL);
    return write_inode(mnt, dir_ino, &dir);
}

/* Elimina la entrada con 'name' del directorio dir_ino. */
static int dir_remove_entry(zeros_mount_t *mnt, uint32_t dir_ino, const char *name) {
    zeros_inode dir;
    if (read_inode(mnt, dir_ino, &dir) < 0) return -1;

    uint8_t buf[ZEROS_BLOCK_SIZE];
    for (uint16_t e = 0; e < dir.extent_count; e++) {
        for (uint32_t b = 0; b < dir.extents[e].length; b++) {
            uint32_t blk = dir.extents[e].start_block + b;
            if (read_block(mnt->fd, blk, buf) < 0) return -1;
            zeros_dirent *entries = (zeros_dirent *)buf;
            for (int i = 0; i < ZEROS_DIRENTS_PER_BLOCK; i++) {
                if (entries[i].inode == ZEROS_DIRENT_FREE) continue;
                if (strcmp(entries[i].name, name) != 0) continue;
                entries[i].inode = ZEROS_DIRENT_FREE;
                if (write_block(mnt->fd, blk, buf) < 0) return -1;
                dir.mtime = (int64_t)time(NULL);
                return write_inode(mnt, dir_ino, &dir);
            }
        }
    }
    return -1;  /* no encontrado */
}

/* Devuelve 1 si el directorio solo tiene "." y "..". */
static int dir_is_empty(zeros_mount_t *mnt, uint32_t dir_ino) {
    zeros_inode dir;
    if (read_inode(mnt, dir_ino, &dir) < 0) return 0;

    uint8_t buf[ZEROS_BLOCK_SIZE];
    for (uint16_t e = 0; e < dir.extent_count; e++) {
        for (uint32_t b = 0; b < dir.extents[e].length; b++) {
            if (read_block(mnt->fd, dir.extents[e].start_block + b, buf) < 0) return 0;
            zeros_dirent *entries = (zeros_dirent *)buf;
            for (int i = 0; i < ZEROS_DIRENTS_PER_BLOCK; i++) {
                if (entries[i].inode == ZEROS_DIRENT_FREE) continue;
                if (strcmp(entries[i].name, ".") == 0) continue;
                if (strcmp(entries[i].name, "..") == 0) continue;
                return 0;  /* tiene contenido */
            }
        }
    }
    return 1;
}

/* ══════════════════════════════════════════════════════════
 * Helpers internos — Rutas
 * ══════════════════════════════════════════════════════════ */

/*
 * Resuelve un path (absoluto o relativo) a un número de inodo.
 * Absoluto si empieza por '/', relativo al cwd si no.
 * Devuelve UINT32_MAX si no existe.
 */
static uint32_t resolve_path(zeros_mount_t *mnt, const char *path) {
    if (path == NULL || path[0] == '\0') return mnt->cwd_ino;
    if (strcmp(path, "/") == 0) return ZEROS_ROOT_INODE;

    uint32_t cur = (path[0] == '/') ? ZEROS_ROOT_INODE : mnt->cwd_ino;

    char copy[1024];
    strncpy(copy, path, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    char *tok = strtok(copy, "/");
    while (tok) {
        uint32_t next = dir_lookup(mnt, cur, tok);
        if (next == UINT32_MAX) return UINT32_MAX;
        cur = next;
        tok = strtok(NULL, "/");
    }
    return cur;
}

/*
 * Separa el path en directorio padre y nombre final.
 *   "/sys/kernel" → parent_ino = ino("/sys"), name_out = "kernel"
 *   "docs"        → parent_ino = cwd_ino,    name_out = "docs"
 */
static uint32_t resolve_parent(zeros_mount_t *mnt, const char *path, char *name_out) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        /* Sin barra: el padre es el cwd */
        strncpy(name_out, path, ZEROS_NAME_MAX);
        name_out[ZEROS_NAME_MAX] = '\0';
        return mnt->cwd_ino;
    }
    if (slash == path) {
        /* Path como "/foo": padre es root */
        strncpy(name_out, slash + 1, ZEROS_NAME_MAX);
        name_out[ZEROS_NAME_MAX] = '\0';
        return ZEROS_ROOT_INODE;
    }
    /* Caso general "/foo/bar": padre es "/foo" */
    char parent_path[1024];
    size_t len = (size_t)(slash - path);
    strncpy(parent_path, path, len);
    parent_path[len] = '\0';
    strncpy(name_out, slash + 1, ZEROS_NAME_MAX);
    name_out[ZEROS_NAME_MAX] = '\0';
    return resolve_path(mnt, parent_path);
}

/* ══════════════════════════════════════════════════════════
 * Flush — escribe los metadatos modificados de vuelta a disco
 * ══════════════════════════════════════════════════════════ */

static int flush_metadata(zeros_mount_t *mnt) {
    /* Superbloque */
    uint8_t sb_buf[ZEROS_BLOCK_SIZE];
    memset(sb_buf, 0, sizeof(sb_buf));
    memcpy(sb_buf, &mnt->sb, sizeof(mnt->sb));
    if (write_block(mnt->fd, 0, sb_buf) < 0) return -1;

    /* Bitmap de inodos */
    for (uint32_t i = 0; i < mnt->sb.inode_bitmap_blocks; i++) {
        if (write_block(mnt->fd, mnt->sb.inode_bitmap_start + i,
                        mnt->inode_bitmap + i * ZEROS_BLOCK_SIZE) < 0) return -1;
    }
    /* Bitmap de bloques */
    for (uint32_t i = 0; i < mnt->sb.block_bitmap_blocks; i++) {
        if (write_block(mnt->fd, mnt->sb.block_bitmap_start + i,
                        mnt->block_bitmap + i * ZEROS_BLOCK_SIZE) < 0) return -1;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════
 * API pública — Montar / desmontar
 * ══════════════════════════════════════════════════════════ */

zeros_mount_t *zeros_mount_open(const char *disk_path) {
    zeros_mount_t *mnt = calloc(1, sizeof(zeros_mount_t));
    if (!mnt) return NULL;

    mnt->fd = open(disk_path, O_RDWR);
    if (mnt->fd < 0) { perror("zeros_mount: open"); free(mnt); return NULL; }

    /* Leer superbloque */
    uint8_t sb_buf[ZEROS_BLOCK_SIZE];
    if (read_block(mnt->fd, 0, sb_buf) < 0) { close(mnt->fd); free(mnt); return NULL; }
    memcpy(&mnt->sb, sb_buf, sizeof(mnt->sb));

    if (mnt->sb.magic != ZEROS_MAGIC) {
        fprintf(stderr, "zeros_mount: numero magico invalido — ejecuta zeros_fsck\n");
        close(mnt->fd); free(mnt); return NULL;
    }

    /* Cargar bitmaps en memoria */
    uint32_t ibm_bytes = mnt->sb.inode_bitmap_blocks * ZEROS_BLOCK_SIZE;
    uint32_t bbm_bytes = mnt->sb.block_bitmap_blocks * ZEROS_BLOCK_SIZE;
    mnt->inode_bitmap = malloc(ibm_bytes);
    mnt->block_bitmap = malloc(bbm_bytes);
    if (!mnt->inode_bitmap || !mnt->block_bitmap) {
        free(mnt->inode_bitmap); free(mnt->block_bitmap);
        close(mnt->fd); free(mnt); return NULL;
    }
    for (uint32_t i = 0; i < mnt->sb.inode_bitmap_blocks; i++)
        read_block(mnt->fd, mnt->sb.inode_bitmap_start + i,
                   mnt->inode_bitmap + i * ZEROS_BLOCK_SIZE);
    for (uint32_t i = 0; i < mnt->sb.block_bitmap_blocks; i++)
        read_block(mnt->fd, mnt->sb.block_bitmap_start + i,
                   mnt->block_bitmap + i * ZEROS_BLOCK_SIZE);

    /* Empezamos en root */
    mnt->cwd_ino = ZEROS_ROOT_INODE;
    strcpy(mnt->cwd_path, "/");

    /* Actualizamos el timestamp de montaje */
    mnt->sb.mounted_at = (int64_t)time(NULL);
    flush_metadata(mnt);

    return mnt;
}

void zeros_mount_close(zeros_mount_t *mnt) {
    if (!mnt) return;
    flush_metadata(mnt);
    free(mnt->inode_bitmap);
    free(mnt->block_bitmap);
    close(mnt->fd);
    free(mnt);
}

/* ══════════════════════════════════════════════════════════
 * API pública — Navegación
 * ══════════════════════════════════════════════════════════ */

const char *zeros_pwd(zeros_mount_t *mnt) {
    return mnt->cwd_path;
}

int zeros_cd(zeros_mount_t *mnt, const char *path) {
    const char *target = (path && path[0]) ? path : "/";

    uint32_t ino = resolve_path(mnt, target);
    if (ino == UINT32_MAX) {
        fprintf(stderr, "zeros: cd: '%s': no existe\n", target); return -1;
    }
    zeros_inode inode;
    if (read_inode(mnt, ino, &inode) < 0) return -1;
    if (!ZEROS_IS_DIR(inode.mode)) {
        fprintf(stderr, "zeros: cd: '%s': no es un directorio\n", target); return -1;
    }

    mnt->cwd_ino = ino;

    /* Actualizamos cwd_path */
    if (target[0] == '/') {
        strncpy(mnt->cwd_path, target, sizeof(mnt->cwd_path) - 1);
        mnt->cwd_path[sizeof(mnt->cwd_path) - 1] = '\0';
    } else if (strcmp(target, "..") == 0) {
        char *slash = strrchr(mnt->cwd_path, '/');
        if (slash && slash != mnt->cwd_path)
            *slash = '\0';
        else
            strcpy(mnt->cwd_path, "/");
    } else {
        /* Ruta relativa: añadir componente */
        size_t len = strlen(mnt->cwd_path);
        if (mnt->cwd_path[len - 1] != '/')
            strncat(mnt->cwd_path, "/", sizeof(mnt->cwd_path) - len - 1);
        strncat(mnt->cwd_path, target, sizeof(mnt->cwd_path) - strlen(mnt->cwd_path) - 1);
    }
    /* Normalizar trailing slash */
    size_t l = strlen(mnt->cwd_path);
    if (l > 1 && mnt->cwd_path[l - 1] == '/')
        mnt->cwd_path[l - 1] = '\0';

    return 0;
}

/* ══════════════════════════════════════════════════════════
 * API pública — Lectura
 * ══════════════════════════════════════════════════════════ */

int zeros_ls(zeros_mount_t *mnt, const char *path) {
    const char *target = (path && path[0]) ? path : mnt->cwd_path;
    uint32_t dir_ino = resolve_path(mnt, target);
    if (dir_ino == UINT32_MAX) {
        fprintf(stderr, "zeros: ls: '%s': no existe\n", target); return -1;
    }
    zeros_inode dir;
    if (read_inode(mnt, dir_ino, &dir) < 0) return -1;
    if (!ZEROS_IS_DIR(dir.mode)) {
        fprintf(stderr, "zeros: ls: '%s': no es un directorio\n", target); return -1;
    }

    printf("  %-20s  %-5s  %s\n", "nombre", "tipo", "tamaño");
    printf("  %-20s  %-5s  %s\n", "──────────────────", "────", "──────");

    uint8_t buf[ZEROS_BLOCK_SIZE];
    for (uint16_t e = 0; e < dir.extent_count; e++) {
        for (uint32_t b = 0; b < dir.extents[e].length; b++) {
            if (read_block(mnt->fd, dir.extents[e].start_block + b, buf) < 0) return -1;
            zeros_dirent *entries = (zeros_dirent *)buf;
            for (int i = 0; i < ZEROS_DIRENTS_PER_BLOCK; i++) {
                if (entries[i].inode == ZEROS_DIRENT_FREE) continue;
                zeros_inode child;
                if (read_inode(mnt, entries[i].inode, &child) < 0) continue;
                const char *type = ZEROS_IS_DIR(child.mode) ? "dir" : "reg";
                printf("  %-20s  %-5s  %llu\n",
                       entries[i].name, type,
                       (unsigned long long)child.size);
            }
        }
    }
    return 0;
}

int zeros_cat(zeros_mount_t *mnt, const char *path) {
    uint32_t ino = resolve_path(mnt, path);
    if (ino == UINT32_MAX) {
        fprintf(stderr, "zeros: cat: '%s': no existe\n", path); return -1;
    }
    zeros_inode inode;
    if (read_inode(mnt, ino, &inode) < 0) return -1;
    if (ZEROS_IS_DIR(inode.mode)) {
        fprintf(stderr, "zeros: cat: '%s': es un directorio\n", path); return -1;
    }

    uint64_t remaining = inode.size;
    uint8_t buf[ZEROS_BLOCK_SIZE];
    for (uint16_t e = 0; e < inode.extent_count && remaining > 0; e++) {
        for (uint32_t b = 0; b < inode.extents[e].length && remaining > 0; b++) {
            if (read_block(mnt->fd, inode.extents[e].start_block + b, buf) < 0) return -1;
            uint32_t chunk = (remaining < ZEROS_BLOCK_SIZE)
                           ? (uint32_t)remaining : ZEROS_BLOCK_SIZE;
            fwrite(buf, 1, chunk, stdout);
            remaining -= chunk;
        }
    }
    if (inode.size > 0) printf("\n");
    return 0;
}

int zeros_stat_path(zeros_mount_t *mnt, const char *path) {
    uint32_t ino = resolve_path(mnt, path);
    if (ino == UINT32_MAX) {
        fprintf(stderr, "zeros: stat: '%s': no existe\n", path); return -1;
    }
    zeros_inode inode;
    if (read_inode(mnt, ino, &inode) < 0) return -1;

    printf("  inodo    : %u\n", ino);
    printf("  tipo     : %s\n", ZEROS_IS_DIR(inode.mode) ? "directorio" : "archivo");
    printf("  tamaño   : %llu bytes\n", (unsigned long long)inode.size);
    printf("  links    : %u\n", inode.link_count);
    printf("  extents  : %u\n", inode.extent_count);
    for (uint16_t e = 0; e < inode.extent_count; e++)
        printf("    [%u] bloque %u, longitud %u\n",
               e, inode.extents[e].start_block, inode.extents[e].length);
    return 0;
}

/* ══════════════════════════════════════════════════════════
 * API pública — Escritura
 * ══════════════════════════════════════════════════════════ */

int zeros_mkdir(zeros_mount_t *mnt, const char *path) {
    char name[ZEROS_NAME_MAX + 1];
    uint32_t parent_ino = resolve_parent(mnt, path, name);
    if (parent_ino == UINT32_MAX) {
        fprintf(stderr, "zeros: mkdir: directorio padre no existe\n"); return -1;
    }
    if (dir_lookup(mnt, parent_ino, name) != UINT32_MAX) {
        fprintf(stderr, "zeros: mkdir: '%s': ya existe\n", name); return -1;
    }

    uint32_t ino = alloc_inode(mnt);
    if (ino == UINT32_MAX) { fprintf(stderr, "zeros: mkdir: inodos agotados\n"); return -1; }
    uint32_t blk = alloc_block(mnt);
    if (blk == UINT32_MAX) { fprintf(stderr, "zeros: mkdir: disco lleno\n"); return -1; }

    int64_t now = (int64_t)time(NULL);
    zeros_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.mode  = ZEROS_FT_DIR | ZEROS_S_IRUSR | ZEROS_S_IWUSR | ZEROS_S_IXUSR
                               | ZEROS_S_IRGRP | ZEROS_S_IXGRP
                               | ZEROS_S_IROTH | ZEROS_S_IXOTH;
    inode.ctime = inode.mtime = inode.atime = now;
    inode.link_count   = 2;
    inode.extent_count = 1;
    inode.extents[0].start_block = blk;
    inode.extents[0].length      = 1;

    /* Bloque de datos vacío */
    uint8_t buf[ZEROS_BLOCK_SIZE];
    memset(buf, 0xFF, sizeof(buf));
    if (write_block(mnt->fd, blk, buf) < 0) return -1;
    if (write_inode(mnt, ino, &inode) < 0) return -1;

    /* Entradas . y .. */
    if (dir_add_entry(mnt, ino, ".",  ino,        ZEROS_FT_DIR) < 0) return -1;
    if (dir_add_entry(mnt, ino, "..", parent_ino, ZEROS_FT_DIR) < 0) return -1;

    /* Entrada en el padre */
    if (dir_add_entry(mnt, parent_ino, name, ino, ZEROS_FT_DIR) < 0) return -1;

    return flush_metadata(mnt);
}

int zeros_touch(zeros_mount_t *mnt, const char *path) {
    /* Si ya existe, solo actualizamos atime */
    uint32_t existing = resolve_path(mnt, path);
    if (existing != UINT32_MAX) {
        zeros_inode inode;
        if (read_inode(mnt, existing, &inode) < 0) return -1;
        inode.atime = (int64_t)time(NULL);
        return write_inode(mnt, existing, &inode);
    }

    char name[ZEROS_NAME_MAX + 1];
    uint32_t parent_ino = resolve_parent(mnt, path, name);
    if (parent_ino == UINT32_MAX) {
        fprintf(stderr, "zeros: touch: directorio padre no existe\n"); return -1;
    }

    uint32_t ino = alloc_inode(mnt);
    if (ino == UINT32_MAX) { fprintf(stderr, "zeros: touch: inodos agotados\n"); return -1; }

    int64_t now = (int64_t)time(NULL);
    zeros_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.mode  = ZEROS_FT_REG | ZEROS_S_IRUSR | ZEROS_S_IWUSR
                               | ZEROS_S_IRGRP
                               | ZEROS_S_IROTH;  /* 0644 */
    inode.ctime = inode.mtime = inode.atime = now;
    inode.link_count   = 1;
    inode.extent_count = 0;
    inode.size         = 0;

    if (write_inode(mnt, ino, &inode) < 0) return -1;
    if (dir_add_entry(mnt, parent_ino, name, ino, ZEROS_FT_REG) < 0) return -1;
    return flush_metadata(mnt);
}

int zeros_write_file(zeros_mount_t *mnt, const char *path,
                     const void *data, uint32_t len) {
    /* Creamos el archivo si no existe, lo sobreescribimos si existe */
    uint32_t ino = resolve_path(mnt, path);
    zeros_inode inode;

    if (ino == UINT32_MAX) {
        /* No existe: lo creamos */
        if (zeros_touch(mnt, path) < 0) return -1;
        ino = resolve_path(mnt, path);
        if (ino == UINT32_MAX) return -1;
    }
    if (read_inode(mnt, ino, &inode) < 0) return -1;
    if (ZEROS_IS_DIR(inode.mode)) {
        fprintf(stderr, "zeros: write: '%s': es un directorio\n", path); return -1;
    }

    /* Liberamos bloques anteriores si los había */
    for (uint16_t e = 0; e < inode.extent_count; e++)
        for (uint32_t b = 0; b < inode.extents[e].length; b++)
            free_block(mnt, inode.extents[e].start_block + b);
    inode.extent_count = 0;
    inode.size = 0;

    /* Escribimos los datos bloque a bloque */
    const uint8_t *src = (const uint8_t *)data;
    uint32_t written = 0;
    uint8_t buf[ZEROS_BLOCK_SIZE];

    while (written < len) {
        if (inode.extent_count >= ZEROS_MAX_EXTENTS) {
            fprintf(stderr, "zeros: write: archivo demasiado grande\n"); return -1;
        }
        uint32_t blk = alloc_block(mnt);
        if (blk == UINT32_MAX) { fprintf(stderr, "zeros: write: disco lleno\n"); return -1; }

        uint32_t chunk = len - written;
        if (chunk > ZEROS_BLOCK_SIZE) chunk = ZEROS_BLOCK_SIZE;

        memset(buf, 0, sizeof(buf));
        memcpy(buf, src + written, chunk);
        if (write_block(mnt->fd, blk, buf) < 0) return -1;

        /* Intentamos extender el extent anterior si el bloque es contiguo */
        if (inode.extent_count > 0) {
            zeros_extent *prev = &inode.extents[inode.extent_count - 1];
            if (prev->start_block + prev->length == blk) {
                prev->length++;
                written += chunk;
                inode.size += chunk;
                continue;
            }
        }
        inode.extents[inode.extent_count].start_block = blk;
        inode.extents[inode.extent_count].length      = 1;
        inode.extent_count++;
        written += chunk;
        inode.size += chunk;
    }

    inode.mtime = inode.atime = (int64_t)time(NULL);
    if (write_inode(mnt, ino, &inode) < 0) return -1;
    return flush_metadata(mnt);
}

int zeros_rm(zeros_mount_t *mnt, const char *path) {
    uint32_t ino = resolve_path(mnt, path);
    if (ino == UINT32_MAX) {
        fprintf(stderr, "zeros: rm: '%s': no existe\n", path); return -1;
    }
    if (ino == ZEROS_ROOT_INODE) {
        fprintf(stderr, "zeros: rm: no se puede borrar root\n"); return -1;
    }
    if (ino == mnt->cwd_ino) {
        fprintf(stderr, "zeros: rm: no se puede borrar el directorio actual\n"); return -1;
    }

    zeros_inode inode;
    if (read_inode(mnt, ino, &inode) < 0) return -1;

    if (ZEROS_IS_DIR(inode.mode) && !dir_is_empty(mnt, ino)) {
        fprintf(stderr, "zeros: rm: '%s': el directorio no esta vacio\n", path); return -1;
    }

    /* Liberar bloques de datos */
    for (uint16_t e = 0; e < inode.extent_count; e++)
        for (uint32_t b = 0; b < inode.extents[e].length; b++)
            free_block(mnt, inode.extents[e].start_block + b);
    free_inode(mnt, ino);

    /* Eliminar la entrada del directorio padre */
    char name[ZEROS_NAME_MAX + 1];
    uint32_t parent_ino = resolve_parent(mnt, path, name);
    if (parent_ino != UINT32_MAX)
        dir_remove_entry(mnt, parent_ino, name);

    return flush_metadata(mnt);
}

/* ══════════════════════════════════════════════════════════
 * zeros_read_file — lee el contenido completo de un archivo
 * ══════════════════════════════════════════════════════════ */

uint8_t *zeros_read_file(zeros_mount_t *mnt, const char *path, uint32_t *out_len) {
    uint32_t ino = resolve_path(mnt, path);
    if (ino == UINT32_MAX) return NULL;

    zeros_inode inode;
    if (read_inode(mnt, ino, &inode) < 0) return NULL;
    if (ZEROS_IS_DIR(inode.mode)) return NULL;

    uint32_t len = (uint32_t)inode.size;
    uint8_t *buf = malloc(len + 1);
    if (!buf) return NULL;

    uint8_t block_buf[ZEROS_BLOCK_SIZE];
    uint32_t remaining = len;
    uint32_t offset    = 0;

    for (uint16_t e = 0; e < inode.extent_count && remaining > 0; e++) {
        for (uint32_t b = 0; b < inode.extents[e].length && remaining > 0; b++) {
            if (read_block(mnt->fd, inode.extents[e].start_block + b, block_buf) < 0) {
                free(buf); return NULL;
            }
            uint32_t chunk = remaining < ZEROS_BLOCK_SIZE ? remaining : ZEROS_BLOCK_SIZE;
            memcpy(buf + offset, block_buf, chunk);
            offset    += chunk;
            remaining -= chunk;
        }
    }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

/* ══════════════════════════════════════════════════════════
 * zeros_path_complete — generador de completado para readline
 *
 * readline llama a esta función repetidamente con state=0, 1, 2...
 * hasta que devuelve NULL. Cada llamada devuelve el siguiente match.
 *
 * Dado un texto parcial como "/sys/k", separa el directorio ("/sys/")
 * del prefijo ("k"), busca en ese directorio las entradas que empiecen
 * por el prefijo y las devuelve una a una.
 *
 * Si el match es un directorio añade '/' al final para que el usuario
 * pueda seguir completando sin pulsar Tab de nuevo.
 * ══════════════════════════════════════════════════════════ */

char *zeros_path_complete(zeros_mount_t *mnt, const char *text, int state) {
    /* Variables estáticas: persisten entre llamadas sucesivas de readline */
    static uint32_t    dir_ino;
    static char        prefix[ZEROS_NAME_MAX + 1];
    static char        dir_text[1024];   /* parte del texto hasta el último '/' */
    static zeros_inode dir_inode;
    static uint16_t    ext_idx;
    static uint32_t    blk_idx;
    static int         entry_idx;
    static uint8_t     buf[ZEROS_BLOCK_SIZE];
    static int         buf_valid;

    /* ── Primera llamada: inicializar el estado ── */
    if (state == 0) {
        const char *last_slash = strrchr(text, '/');

        if (last_slash == NULL) {
            /* Sin barra: buscar en el directorio actual */
            strncpy(prefix, text, ZEROS_NAME_MAX);
            prefix[ZEROS_NAME_MAX] = '\0';
            dir_ino = mnt->cwd_ino;
            dir_text[0] = '\0';
        } else if (last_slash == text) {
            /* Empieza con '/': buscar en root */
            strncpy(prefix, last_slash + 1, ZEROS_NAME_MAX);
            prefix[ZEROS_NAME_MAX] = '\0';
            dir_ino  = ZEROS_ROOT_INODE;
            dir_text[0] = '/'; dir_text[1] = '\0';
        } else {
            /* "/foo/bar" o "foo/bar": separar directorio y prefijo */
            strncpy(prefix, last_slash + 1, ZEROS_NAME_MAX);
            prefix[ZEROS_NAME_MAX] = '\0';

            size_t dir_len = (size_t)(last_slash - text + 1); /* incluye la '/' */
            strncpy(dir_text, text, dir_len);
            dir_text[dir_len] = '\0';

            /* Resolver el directorio sin la barra final */
            char dir_path[1024];
            strncpy(dir_path, text, last_slash - text);
            dir_path[last_slash - text] = '\0';
            dir_ino = resolve_path(mnt, dir_path);
        }

        if (dir_ino == UINT32_MAX) return NULL;
        if (read_inode(mnt, dir_ino, &dir_inode) < 0) return NULL;
        if (!ZEROS_IS_DIR(dir_inode.mode)) return NULL;

        ext_idx    = 0;
        blk_idx    = 0;
        entry_idx  = 0;
        buf_valid  = 0;
    }

    size_t prefix_len = strlen(prefix);

    /* ── Iterar por las entradas del directorio ── */
    while (ext_idx < dir_inode.extent_count && ext_idx < ZEROS_MAX_EXTENTS) {
        if (!buf_valid) {
            uint32_t blk = dir_inode.extents[ext_idx].start_block + blk_idx;
            if (read_block(mnt->fd, blk, buf) < 0) return NULL;
            buf_valid = 1;
        }

        zeros_dirent *entries = (zeros_dirent *)buf;
        while (entry_idx < ZEROS_DIRENTS_PER_BLOCK) {
            zeros_dirent *d = &entries[entry_idx++];
            if (d->inode == ZEROS_DIRENT_FREE) continue;
            if (strcmp(d->name, ".") == 0 || strcmp(d->name, "..") == 0) continue;
            if (strncmp(d->name, prefix, prefix_len) != 0) continue;

            /* Match encontrado: devolvemos "dir_text + nombre [+ '/']" */
            char result[1024 + ZEROS_NAME_MAX + 2];
            snprintf(result, sizeof(result), "%s%s", dir_text, d->name);
            if (d->type == ZEROS_FT_DIR)
                strncat(result, "/", sizeof(result) - strlen(result) - 1);
            return strdup(result);  /* readline libera esta memoria */
        }

        /* Fin del bloque: avanzamos */
        entry_idx = 0;
        buf_valid = 0;
        blk_idx++;
        if (blk_idx >= dir_inode.extents[ext_idx].length) {
            blk_idx = 0;
            ext_idx++;
        }
    }

    return NULL;
}

/* ══════════════════════════════════════════════════════════
 * API para el driver FUSE
 * ══════════════════════════════════════════════════════════ */

int zeros_getattr(zeros_mount_t *mnt, const char *path, zeros_inode *out) {
    uint32_t ino = resolve_path(mnt, path);
    if (ino == UINT32_MAX) return -1;
    return read_inode(mnt, ino, out);
}

int zeros_readdir(zeros_mount_t *mnt, const char *path,
                  zeros_readdir_cb cb, void *userdata) {
    uint32_t ino = resolve_path(mnt, path);
    if (ino == UINT32_MAX) return -1;

    zeros_inode dir;
    if (read_inode(mnt, ino, &dir) < 0) return -1;
    if (!ZEROS_IS_DIR(dir.mode)) return -1;

    uint8_t buf[ZEROS_BLOCK_SIZE];
    for (uint16_t e = 0; e < dir.extent_count; e++) {
        for (uint32_t b = 0; b < dir.extents[e].length; b++) {
            if (read_block(mnt->fd, dir.extents[e].start_block + b, buf) < 0)
                return -1;
            zeros_dirent *entries = (zeros_dirent *)buf;
            for (int i = 0; i < ZEROS_DIRENTS_PER_BLOCK; i++) {
                if (entries[i].inode == ZEROS_DIRENT_FREE) continue;
                if (cb(entries[i].name, entries[i].inode,
                       entries[i].type, userdata) != 0)
                    return 0;
            }
        }
    }
    return 0;
}

