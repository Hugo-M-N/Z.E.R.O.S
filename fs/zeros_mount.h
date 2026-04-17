/*
 * zeros_mount.h — API pública del filesystem ZEROS en tiempo de ejecución
 *
 * El tipo zeros_mount_t es opaco: la implementación vive en zeros_mount.c.
 * El código que use esta API no necesita conocer la estructura interna.
 */

#ifndef ZEROS_MOUNT_H
#define ZEROS_MOUNT_H

#define _POSIX_C_SOURCE 200809L

#include "zeros_fs.h"
#include <stdint.h>

typedef struct zeros_mount zeros_mount_t;

/* ── Montar / desmontar ──────────────────────────────────
 *
 * zeros_mount_open abre la imagen de disco y carga los
 * metadatos (superbloque, bitmaps) en memoria.
 * Devuelve NULL en error.
 *
 * zeros_mount_close escribe los metadatos pendientes
 * y cierra el descriptor.
 */
zeros_mount_t *zeros_mount_open(const char *disk_path);
void           zeros_mount_close(zeros_mount_t *mnt);

/* ── Lectura ─────────────────────────────────────────────*/
int         zeros_ls(zeros_mount_t *mnt, const char *path);
int         zeros_cat(zeros_mount_t *mnt, const char *path);
int         zeros_stat_path(zeros_mount_t *mnt, const char *path);

/* ── Navegación ──────────────────────────────────────────*/
int         zeros_cd(zeros_mount_t *mnt, const char *path);
const char *zeros_pwd(zeros_mount_t *mnt);

/* ── Funciones de bajo nivel para el driver FUSE ────────
 *
 * Estas dos funciones exponen lo mínimo que el driver FUSE necesita
 * para implementar getattr y readdir sin duplicar la lógica interna.
 * El resto de operaciones (read, write, mkdir, touch, rm) usan
 * directamente las funciones de escritura/lectura ya existentes.
 */

/* Rellena *out con el inodo correspondiente a path.
 * Devuelve 0 si existe, -1 si no. */
int zeros_getattr(zeros_mount_t *mnt, const char *path, zeros_inode *out);

/* Itera las entradas del directorio en path.
 * Llama a cb(nombre, nº_inodo, tipo, userdata) por cada entrada válida.
 * Devuelve 0 si ok, -1 si path no es un directorio. */
typedef int (*zeros_readdir_cb)(const char *name, uint32_t ino,
                                uint32_t type, void *userdata);
int zeros_readdir(zeros_mount_t *mnt, const char *path,
                  zeros_readdir_cb cb, void *userdata);

/* ── Lectura raw de archivos (para el editor) ───────────
 *
 * Devuelve un buffer malloc'd con el contenido del archivo
 * y escribe el tamaño en *out_len. El caller debe hacer free().
 * Devuelve NULL en error.
 */
uint8_t *zeros_read_file(zeros_mount_t *mnt, const char *path, uint32_t *out_len);

/* ── Autocompletado (para integración con readline) ─────
 *
 * Generador compatible con rl_completion_matches().
 * state=0 en la primera llamada, >0 en las siguientes.
 * Devuelve NULL cuando no hay más matches.
 */
char *zeros_path_complete(zeros_mount_t *mnt, const char *text, int state);

/* ── Escritura ───────────────────────────────────────────*/
int zeros_mkdir(zeros_mount_t *mnt, const char *path);
int zeros_touch(zeros_mount_t *mnt, const char *path);
int zeros_write_file(zeros_mount_t *mnt, const char *path,
                     const void *data, uint32_t len);
int zeros_rm(zeros_mount_t *mnt, const char *path);

#endif /* ZEROS_MOUNT_H */
