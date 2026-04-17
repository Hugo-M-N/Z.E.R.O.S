/*
 * vfs.h — Interfaz del Virtual File System de Z.E.R.O.S
 *
 * Define el contrato entre la shell (y cualquier otro consumidor)
 * y las implementaciones concretas del filesystem:
 *
 *   Backend ZEROS  (vfs_zeros.c) — imagen de disco propia
 *   Backend host   (vfs_host.c)  — filesystem POSIX del sistema
 *
 * Para cambiar de kernel o de filesystem solo hay que escribir
 * un nuevo backend que implemente vfs_ops_t; el resto del código
 * no cambia.
 *
 * Uso típico:
 *   vfs_t *vfs = vfs_open_zeros("disco.img");  // o vfs_open_host()
 *   vfs_ls(vfs, "/");
 *   vfs_destroy(vfs);
 */

#ifndef ZEROS_VFS_H
#define ZEROS_VFS_H

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>

/* ── Tipo opaco ──────────────────────────────────────────── */
typedef struct vfs vfs_t;

/* ── Tabla de operaciones (vtable) ───────────────────────────
 *
 * Cada backend rellena esta estructura con sus propias funciones.
 * El primer parámetro 'ctx' es el estado interno del backend
 * (zeros_mount_t* para ZEROS, vfs_host_ctx_t* para host).
 */
typedef struct {
    /* Ciclo de vida */
    void        (*close)        (void *ctx);

    /* Navegación */
    int         (*cd)           (void *ctx, const char *path);
    const char *(*pwd)          (void *ctx);

    /* Lectura */
    int         (*ls)           (void *ctx, const char *path);
    int         (*cat)          (void *ctx, const char *path);
    int         (*stat_path)    (void *ctx, const char *path);

    /* Escritura */
    int         (*mkdir)        (void *ctx, const char *path);
    int         (*touch)        (void *ctx, const char *path);
    int         (*write_file)   (void *ctx, const char *path,
                                 const void *data, uint32_t len);
    int         (*rm)           (void *ctx, const char *path);

    /* I/O raw (editor, cc, ejecución de binarios) */
    uint8_t    *(*read_file)    (void *ctx, const char *path, uint32_t *out_len);

    /* Autocompletado para readline — devuelve NULL para usar el default del host */
    char       *(*path_complete)(void *ctx, const char *text, int state);
} vfs_ops_t;

/* ── Estructura pública ──────────────────────────────────────
 *
 * is_host == 1 indica backend POSIX; la shell lo usa para
 * ajustar el completado de readline y el cd sin argumento.
 */
struct vfs {
    const vfs_ops_t *ops;
    void            *ctx;
    int              is_host;
};

/* ── Constructores ───────────────────────────────────────── */
vfs_t *vfs_open_zeros(const char *img_path);   /* imagen de disco ZEROS   */
vfs_t *vfs_open_host(void);                    /* filesystem del sistema  */
void   vfs_destroy(vfs_t *v);                  /* flush + cierra + libera */

/* ── Wrappers inline ─────────────────────────────────────────
 *
 * Sintaxis cómoda: vfs_ls(v, "/") en lugar de v->ops->ls(v->ctx, "/").
 */
static inline int
vfs_cd(vfs_t *v, const char *p)
{ return v->ops->cd(v->ctx, p); }

static inline const char *
vfs_pwd(vfs_t *v)
{ return v->ops->pwd(v->ctx); }

static inline int
vfs_ls(vfs_t *v, const char *p)
{ return v->ops->ls(v->ctx, p); }

static inline int
vfs_cat(vfs_t *v, const char *p)
{ return v->ops->cat(v->ctx, p); }

static inline int
vfs_stat_path(vfs_t *v, const char *p)
{ return v->ops->stat_path(v->ctx, p); }

static inline int
vfs_mkdir(vfs_t *v, const char *p)
{ return v->ops->mkdir(v->ctx, p); }

static inline int
vfs_touch(vfs_t *v, const char *p)
{ return v->ops->touch(v->ctx, p); }

static inline int
vfs_write_file(vfs_t *v, const char *p, const void *d, uint32_t l)
{ return v->ops->write_file(v->ctx, p, d, l); }

static inline int
vfs_rm(vfs_t *v, const char *p)
{ return v->ops->rm(v->ctx, p); }

static inline uint8_t *
vfs_read_file(vfs_t *v, const char *p, uint32_t *l)
{ return v->ops->read_file(v->ctx, p, l); }

static inline char *
vfs_path_complete(vfs_t *v, const char *t, int s)
{ return v->ops->path_complete(v->ctx, t, s); }

#endif /* ZEROS_VFS_H */
