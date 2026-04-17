/*
 * vfs.c — Código común del VFS
 *
 * vfs_destroy es la única función aquí porque cerrar el backend
 * (ops->close) y liberar el propio vfs_t son dos pasos distintos
 * que no encajan en ningún backend concreto.
 */

#define _POSIX_C_SOURCE 200809L

#include "vfs.h"
#include <stdlib.h>

void vfs_destroy(vfs_t *v) {
    if (!v) return;
    v->ops->close(v->ctx);   /* el backend libera su ctx  */
    free(v);                  /* liberamos el vfs_t propio */
}
