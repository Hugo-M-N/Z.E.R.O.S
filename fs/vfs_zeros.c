/*
 * vfs_zeros.c — Backend ZEROS para el VFS
 *
 * Envuelve la API de zeros_mount.h en el contrato vfs_ops_t.
 * Cada función simplemente hace cast de ctx a zeros_mount_t* y
 * delega en la función correspondiente de zeros_mount.c.
 *
 * Constructor: vfs_open_zeros(img_path)
 */

#define _POSIX_C_SOURCE 200809L

#include "vfs.h"
#include "zeros_mount.h"
#include <stdlib.h>

/* ── Operaciones ─────────────────────────────────────────── */

static void        z_close   (void *ctx)                              { zeros_mount_close   ((zeros_mount_t *)ctx); }
static int         z_cd      (void *ctx, const char *p)               { return zeros_cd         ((zeros_mount_t *)ctx, p); }
static const char *z_pwd     (void *ctx)                              { return zeros_pwd         ((zeros_mount_t *)ctx); }
static int         z_ls      (void *ctx, const char *p)               { return zeros_ls          ((zeros_mount_t *)ctx, p); }
static int         z_cat     (void *ctx, const char *p)               { return zeros_cat         ((zeros_mount_t *)ctx, p); }
static int         z_stat    (void *ctx, const char *p)               { return zeros_stat_path   ((zeros_mount_t *)ctx, p); }
static int         z_mkdir   (void *ctx, const char *p)               { return zeros_mkdir       ((zeros_mount_t *)ctx, p); }
static int         z_touch   (void *ctx, const char *p)               { return zeros_touch       ((zeros_mount_t *)ctx, p); }
static int         z_rm      (void *ctx, const char *p)               { return zeros_rm          ((zeros_mount_t *)ctx, p); }

static int z_write_file(void *ctx, const char *p, const void *d, uint32_t l) {
    return zeros_write_file((zeros_mount_t *)ctx, p, d, l);
}

static uint8_t *z_read_file(void *ctx, const char *p, uint32_t *l) {
    return zeros_read_file((zeros_mount_t *)ctx, p, l);
}

static char *z_path_complete(void *ctx, const char *t, int s) {
    return zeros_path_complete((zeros_mount_t *)ctx, t, s);
}

/* ── Vtable ──────────────────────────────────────────────── */

static const vfs_ops_t zeros_ops = {
    .close         = z_close,
    .cd            = z_cd,
    .pwd           = z_pwd,
    .ls            = z_ls,
    .cat           = z_cat,
    .stat_path     = z_stat,
    .mkdir         = z_mkdir,
    .touch         = z_touch,
    .write_file    = z_write_file,
    .rm            = z_rm,
    .read_file     = z_read_file,
    .path_complete = z_path_complete,
};

/* ── Constructor ─────────────────────────────────────────── */

vfs_t *vfs_open_zeros(const char *img_path) {
    zeros_mount_t *mnt = zeros_mount_open(img_path);
    if (!mnt) return NULL;

    vfs_t *v = malloc(sizeof(vfs_t));
    if (!v) { zeros_mount_close(mnt); return NULL; }

    v->ops     = &zeros_ops;
    v->ctx     = mnt;
    v->is_host = 0;
    return v;
}
