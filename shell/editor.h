#ifndef EDITOR_H
#define EDITOR_H

#define _POSIX_C_SOURCE 200809L
#include "../fs/vfs.h"

/*
 * editor_open — abre el editor de texto sobre un archivo del VFS.
 * Funciona tanto con el backend ZEROS como con el backend host.
 * Si el archivo no existe, empieza con un buffer vacío y lo crea al guardar.
 * Devuelve 0 al salir normalmente, -1 en error.
 */
int editor_open(vfs_t *vfs, const char *path);

#endif
