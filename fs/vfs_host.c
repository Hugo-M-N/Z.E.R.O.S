/*
 * vfs_host.c — Backend POSIX para el VFS
 *
 * Implementa vfs_ops_t usando syscalls estándar de Linux/POSIX.
 * Cuando Z.E.R.O.S arranque sin imagen de disco, la shell opera
 * sobre el filesystem del sistema a través de este backend.
 *
 * Constructor: vfs_open_host()
 *
 * Nota sobre path_complete:
 *   Devuelve NULL para que readline use su completado de archivos
 *   por defecto en lugar del nuestro. El backend ZEROS tiene su
 *   propio completado porque opera sobre una imagen binaria que
 *   readline no puede ver.
 */

#define _POSIX_C_SOURCE 200809L

#include "vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

/* ── Estado interno ──────────────────────────────────────── */

typedef struct {
    char cwd[4096];
} vfs_host_ctx_t;

/* ── Operaciones ─────────────────────────────────────────── */

static void h_close(void *ctx) {
    free(ctx);
}

static int h_cd(void *ctx, const char *path) {
    vfs_host_ctx_t *h = (vfs_host_ctx_t *)ctx;
    if (chdir(path) < 0) { perror("zeros: cd"); return -1; }
    if (getcwd(h->cwd, sizeof(h->cwd)) == NULL) {
        perror("zeros: pwd"); return -1;
    }
    return 0;
}

static const char *h_pwd(void *ctx) {
    return ((vfs_host_ctx_t *)ctx)->cwd;
}

static int h_ls(void *ctx, const char *path) {
    vfs_host_ctx_t *h = (vfs_host_ctx_t *)ctx;
    const char *target = (path && path[0]) ? path : h->cwd;

    DIR *d = opendir(target);
    if (!d) { perror("zeros: ls"); return -1; }

    printf("  %-20s  %-5s  %s\n", "nombre", "tipo", "tamaño");
    printf("  %-20s  %-5s  %s\n", "──────────────────", "────", "──────");

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;

        char full[4096 + 256 + 1];
        snprintf(full, sizeof(full), "%s/%s", target, entry->d_name);

        struct stat st;
        if (stat(full, &st) < 0) continue;

        const char *type = S_ISDIR(st.st_mode) ? "dir" : "reg";
        printf("  %-20s  %-5s  %lld\n",
               entry->d_name, type, (long long)st.st_size);
    }
    closedir(d);
    return 0;
}

static int h_cat(void *ctx, const char *path) {
    (void)ctx;
    FILE *f = fopen(path, "r");
    if (!f) { perror("zeros: cat"); return -1; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        fwrite(buf, 1, n, stdout);
    fclose(f);
    return 0;
}

static int h_stat_path(void *ctx, const char *path) {
    (void)ctx;
    struct stat st;
    if (stat(path, &st) < 0) { perror("zeros: stat"); return -1; }

    printf("  tipo     : %s\n", S_ISDIR(st.st_mode) ? "directorio" : "archivo");
    printf("  tamaño   : %lld bytes\n", (long long)st.st_size);
    printf("  permisos : %04o\n", st.st_mode & 0777);

    char buf[64];
    struct tm *tm = localtime(&st.st_mtime);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
    printf("  modificado: %s\n", buf);
    return 0;
}

static int h_mkdir(void *ctx, const char *path) {
    (void)ctx;
    if (mkdir(path, 0755) < 0) { perror("zeros: mkdir"); return -1; }
    return 0;
}

static int h_touch(void *ctx, const char *path) {
    (void)ctx;
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) { perror("zeros: touch"); return -1; }
    close(fd);
    return 0;
}

static int h_write_file(void *ctx, const char *path,
                        const void *data, uint32_t len) {
    (void)ctx;
    FILE *f = fopen(path, "wb");
    if (!f) { perror("zeros: write"); return -1; }
    fwrite(data, 1, len, f);
    fclose(f);
    return 0;
}

static int h_rm(void *ctx, const char *path) {
    (void)ctx;
    /* Intentamos unlink (archivo), si falla rmdir (directorio vacío) */
    if (unlink(path) < 0 && rmdir(path) < 0) {
        perror("zeros: rm"); return -1;
    }
    return 0;
}

static uint8_t *h_read_file(void *ctx, const char *path, uint32_t *out_len) {
    (void)ctx;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);

    uint8_t *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);

    *out_len = (uint32_t)len;
    return buf;
}

/* El backend host usa el completado de archivos nativo de readline */
static char *h_path_complete(void *ctx, const char *text, int state) {
    (void)ctx; (void)text; (void)state;
    return NULL;
}

/* ── Vtable ──────────────────────────────────────────────── */

static const vfs_ops_t host_ops = {
    .close         = h_close,
    .cd            = h_cd,
    .pwd           = h_pwd,
    .ls            = h_ls,
    .cat           = h_cat,
    .stat_path     = h_stat_path,
    .mkdir         = h_mkdir,
    .touch         = h_touch,
    .write_file    = h_write_file,
    .rm            = h_rm,
    .read_file     = h_read_file,
    .path_complete = h_path_complete,
};

/* ── Constructor ─────────────────────────────────────────── */

vfs_t *vfs_open_host(void) {
    vfs_host_ctx_t *h = malloc(sizeof(vfs_host_ctx_t));
    if (!h) return NULL;

    if (getcwd(h->cwd, sizeof(h->cwd)) == NULL) {
        free(h); return NULL;
    }

    vfs_t *v = malloc(sizeof(vfs_t));
    if (!v) { free(h); return NULL; }

    v->ops     = &host_ops;
    v->ctx     = h;
    v->is_host = 1;
    return v;
}
