/*
 * zeros_fuse.c — Driver FUSE para el filesystem ZEROS
 *
 * Monta un disco con formato ZEROS como un directorio Linux normal,
 * de modo que el kernel pueda hacer exec(), open(), write()... sobre él
 * sin necesidad de una API propietaria.
 *
 * Uso:
 *   zeros_fuse <disco> <punto_de_montaje>
 *
 * Ejemplo:
 *   zeros_fuse /dev/sda /z
 *
 * Una vez montado, /z se comporta como cualquier directorio Linux:
 *   ls /z/bin/          → lista los binarios del disco ZEROS
 *   tcc prog.c -o /z/bin/prog  → compila y guarda en el disco ZEROS
 *   /z/bin/prog         → el kernel ejecuta el binario directamente
 *
 * Nota: corre en modo monohilo (-s) para simplificar el acceso al disco.
 * Cuando se integre en un kernel propio, este código se convierte
 * directamente en el driver de filesystem del kernel.
 */

#define FUSE_USE_VERSION 31
#define _POSIX_C_SOURCE 200809L

#include <fuse.h>
#include "zeros_mount.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

/* Montaje global — seguro sin mutex gracias al modo monohilo (-s) */
static zeros_mount_t *g_mnt;

/* ── Conversión de modo ZEROS → modo Linux ───────────────*/
static mode_t zeros_to_linux_mode(uint32_t mode) {
    mode_t m = mode & 0777;
    return ZEROS_IS_DIR(mode) ? (m | S_IFDIR) : (m | S_IFREG);
}

/* ══════════════════════════════════════════════════════════
 * getattr — equivale a stat(2)
 * ══════════════════════════════════════════════════════════ */
static int zf_getattr(const char *path, struct stat *st,
                       struct fuse_file_info *fi) {
    (void)fi;
    zeros_inode ino;
    if (zeros_getattr(g_mnt, path, &ino) < 0) return -ENOENT;

    memset(st, 0, sizeof(*st));
    st->st_mode  = zeros_to_linux_mode(ino.mode);
    st->st_nlink = ZEROS_IS_DIR(ino.mode) ? 2 : 1;
    st->st_size  = (off_t)ino.size;
    st->st_ctime = (time_t)ino.ctime;
    st->st_mtime = (time_t)ino.mtime;
    st->st_atime = (time_t)ino.atime;
    st->st_uid   = 0;
    st->st_gid   = 0;
    st->st_blksize = 1024;
    st->st_blocks  = (st->st_size + 511) / 512;
    return 0;
}

/* ══════════════════════════════════════════════════════════
 * readdir — listar un directorio
 * ══════════════════════════════════════════════════════════ */
struct readdir_ctx { void *buf; fuse_fill_dir_t filler; };

static int fill_entry(const char *name, uint32_t ino_num, uint32_t type,
                       void *ud) {
    (void)ino_num; (void)type;
    struct readdir_ctx *ctx = ud;
    ctx->filler(ctx->buf, name, NULL, 0, 0);
    return 0;
}

static int zf_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi,
                       enum fuse_readdir_flags flags) {
    (void)offset; (void)fi; (void)flags;
    filler(buf, ".",  NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    struct readdir_ctx ctx = { buf, filler };
    if (zeros_readdir(g_mnt, path, fill_entry, &ctx) < 0) return -ENOENT;
    return 0;
}

/* ══════════════════════════════════════════════════════════
 * open — comprobar existencia y tipo
 * ══════════════════════════════════════════════════════════ */
static int zf_open(const char *path, struct fuse_file_info *fi) {
    (void)fi;
    zeros_inode ino;
    if (zeros_getattr(g_mnt, path, &ino) < 0) return -ENOENT;
    if (ZEROS_IS_DIR(ino.mode)) return -EISDIR;
    return 0;
}

/* ══════════════════════════════════════════════════════════
 * read — leer contenido (con soporte de offset)
 * ══════════════════════════════════════════════════════════ */
static int zf_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi) {
    (void)fi;
    uint32_t len;
    uint8_t *data = zeros_read_file(g_mnt, path, &len);
    if (!data) return -ENOENT;

    if (offset >= (off_t)len) { free(data); return 0; }
    size_t n = (size_t)((off_t)len - offset);
    if (n > size) n = size;
    memcpy(buf, data + offset, n);
    free(data);
    return (int)n;
}

/* ══════════════════════════════════════════════════════════
 * write — escribir contenido (leer-modificar-escribir)
 *
 * ZEROS solo tiene escritura completa del archivo, así que
 * para escrituras parciales (offset > 0) leemos el contenido
 * actual, modificamos el rango y reescribimos todo.
 * ══════════════════════════════════════════════════════════ */
static int zf_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi) {
    (void)fi;

    uint32_t old_len = 0;
    uint8_t *old = zeros_read_file(g_mnt, path, &old_len);

    size_t new_len = (size_t)offset + size;
    if ((size_t)old_len > new_len) new_len = (size_t)old_len;

    uint8_t *data = calloc(1, new_len);
    if (!data) { free(old); return -ENOMEM; }
    if (old) { memcpy(data, old, old_len); free(old); }
    memcpy(data + offset, buf, size);

    int r = zeros_write_file(g_mnt, path, data, (uint32_t)new_len);
    free(data);
    return (r < 0) ? -EIO : (int)size;
}

/* ══════════════════════════════════════════════════════════
 * create — crear archivo vacío
 * ══════════════════════════════════════════════════════════ */
static int zf_create(const char *path, mode_t mode,
                      struct fuse_file_info *fi) {
    (void)mode; (void)fi;
    /* Si ya existe, no fallamos — open con O_TRUNC llamará a truncate */
    zeros_inode ino;
    if (zeros_getattr(g_mnt, path, &ino) == 0) return 0;
    return zeros_touch(g_mnt, path) < 0 ? -EIO : 0;
}

/* ══════════════════════════════════════════════════════════
 * truncate — cambiar el tamaño del archivo
 * ══════════════════════════════════════════════════════════ */
static int zf_truncate(const char *path, off_t size,
                        struct fuse_file_info *fi) {
    (void)fi;
    if (size < 0) return -EINVAL;

    uint32_t old_len = 0;
    uint8_t *old = zeros_read_file(g_mnt, path, &old_len);

    size_t sz = (size_t)size;
    uint8_t *data = calloc(1, sz > 0 ? sz : 1);
    if (!data) { free(old); return -ENOMEM; }
    if (old) {
        size_t copy = sz < (size_t)old_len ? sz : (size_t)old_len;
        memcpy(data, old, copy);
        free(old);
    }

    int r = zeros_write_file(g_mnt, path, data, (uint32_t)sz);
    free(data);
    return (r < 0) ? -EIO : 0;
}

/* ══════════════════════════════════════════════════════════
 * mkdir / unlink / rmdir
 * ══════════════════════════════════════════════════════════ */
static int zf_mkdir(const char *path, mode_t mode) {
    (void)mode;
    return zeros_mkdir(g_mnt, path) < 0 ? -EIO : 0;
}

static int zf_unlink(const char *path) {
    return zeros_rm(g_mnt, path) < 0 ? -ENOENT : 0;
}

static int zf_rmdir(const char *path) {
    return zeros_rm(g_mnt, path) < 0 ? -ENOENT : 0;
}

/* utimens: ignoramos timestamps por ahora */
static int zf_utimens(const char *path, const struct timespec tv[2],
                       struct fuse_file_info *fi) {
    (void)path; (void)tv; (void)fi;
    return 0;
}

/* ── Tabla de operaciones ────────────────────────────────*/
static const struct fuse_operations z_ops = {
    .getattr  = zf_getattr,
    .readdir  = zf_readdir,
    .open     = zf_open,
    .read     = zf_read,
    .write    = zf_write,
    .create   = zf_create,
    .truncate = zf_truncate,
    .mkdir    = zf_mkdir,
    .unlink   = zf_unlink,
    .rmdir    = zf_rmdir,
    .utimens  = zf_utimens,
};

/* ── Main ────────────────────────────────────────────────*/
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <disco> <mountpoint>\n", argv[0]);
        return 1;
    }

    g_mnt = zeros_mount_open(argv[1]);
    if (!g_mnt) {
        fprintf(stderr, "zeros_fuse: no se puede abrir '%s'\n", argv[1]);
        return 1;
    }

    /* Construir argv para FUSE: [programa, mountpoint, -s] */
    char *fuse_argv[] = { argv[0], argv[2], "-s", NULL };
    int   fuse_argc   = 3;

    return fuse_main(fuse_argc, fuse_argv, &z_ops, NULL);
}
