/* Activa extensiones POSIX con -std=c11 */
#define _POSIX_C_SOURCE 200809L

#ifndef ZEROS_FS_H
#define ZEROS_FS_H

#include <stdint.h>
#include <stddef.h>     /* NULL */

/*
 * zeros_fs.h — Estructuras y constantes del filesystem ZEROS
 *
 * Layout del disco (todos los offsets se calculan al formatear
 * y se guardan en el superbloque — nada está hardcodeado):
 *
 *  ┌──────────────────┐ bloque 0
 *  │   Superbloque    │ (siempre bloque 0)
 *  ├──────────────────┤ bloque 1
 *  │  Bitmap inodos   │ (≥1 bloque según total_inodes)
 *  ├──────────────────┤
 *  │  Bitmap bloques  │ (≥1 bloque según total_blocks)
 *  ├──────────────────┤
 *  │  Tabla de inodos │ (10% del tamaño del disco)
 *  ├──────────────────┤
 *  │  Bloques de datos│ (el resto)
 *  └──────────────────┘
 */

/* ═══════════════════════════════════════════════════════
 * Constantes de geometría
 * ═══════════════════════════════════════════════════════ */

#define ZEROS_BLOCK_SIZE        1024    /* bytes por bloque */
#define ZEROS_INODE_RATIO         10    /* % del disco reservado para inodos */
#define ZEROS_INODE_SIZE         128    /* bytes por inodo (8 inodos/bloque) */
#define ZEROS_MAX_EXTENTS          8    /* extents inline por inodo */
#define ZEROS_DIRENT_SIZE        128    /* bytes por entrada de directorio */
#define ZEROS_NAME_MAX           119    /* máx. chars en un nombre (+ '\0' = 120) */

/* Cuántas entradas caben en un bloque de datos de directorio */
#define ZEROS_DIRENTS_PER_BLOCK  (ZEROS_BLOCK_SIZE / ZEROS_DIRENT_SIZE)   /* = 8 */

/* Cuántos inodos caben en un bloque */
#define ZEROS_INODES_PER_BLOCK   (ZEROS_BLOCK_SIZE / ZEROS_INODE_SIZE)    /* = 8 */

/* Cuántos bits (bloques/inodos) representa un bloque de bitmap */
#define ZEROS_BITS_PER_BLOCK     (ZEROS_BLOCK_SIZE * 8)                   /* = 8192 */

/* ═══════════════════════════════════════════════════════
 * Identificación del filesystem
 * ═══════════════════════════════════════════════════════ */

/* Número mágico: "ZERO" en ASCII → 0x5A45524F
 * Se escribe en el superbloque para identificar el filesystem.
 * Si al montar el valor no coincide, el disco no es ZEROS. */
#define ZEROS_MAGIC             0x5A45524F

#define ZEROS_FS_VERSION        1       /* versión del formato en disco */

/* Inodo reservado para el directorio raíz */
#define ZEROS_ROOT_INODE        0

/* ═══════════════════════════════════════════════════════
 * Tipos de archivo (bits altos de zeros_inode.mode)
 * ═══════════════════════════════════════════════════════ */

#define ZEROS_FT_REG    0x1000          /* archivo regular */
#define ZEROS_FT_DIR    0x2000          /* directorio */
#define ZEROS_FT_MASK   0xF000          /* máscara para aislar el tipo */

/* ═══════════════════════════════════════════════════════
 * Permisos (bits bajos de zeros_inode.mode)
 * Misma convención que POSIX para no reinventar la rueda.
 * ═══════════════════════════════════════════════════════ */

#define ZEROS_S_IRUSR   0400    /* lectura propietario */
#define ZEROS_S_IWUSR   0200    /* escritura propietario */
#define ZEROS_S_IXUSR   0100    /* ejecución propietario */
#define ZEROS_S_IRGRP   0040    /* lectura grupo */
#define ZEROS_S_IWGRP   0020    /* escritura grupo */
#define ZEROS_S_IXGRP   0010    /* ejecución grupo */
#define ZEROS_S_IROTH   0004    /* lectura otros */
#define ZEROS_S_IWOTH   0002    /* escritura otros */
#define ZEROS_S_IXOTH   0001    /* ejecución otros */

/* Macros para comprobar el tipo desde el campo mode */
#define ZEROS_IS_REG(mode)  (((mode) & ZEROS_FT_MASK) == ZEROS_FT_REG)
#define ZEROS_IS_DIR(mode)  (((mode) & ZEROS_FT_MASK) == ZEROS_FT_DIR)

/* ═══════════════════════════════════════════════════════
 * Extent
 *
 * Un extent describe un rango contiguo de bloques de datos.
 * En lugar de tener un puntero por bloque (como en los FS
 * clásicos), un extent cubre N bloques de golpe:
 *
 *   start_block=10, length=4  →  bloques 10, 11, 12, 13
 *
 * Ventaja: archivos contiguos se describen con un solo extent.
 * Un inodo ZEROS tiene hasta ZEROS_MAX_EXTENTS extents inline.
 * ═══════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint32_t start_block;   /* primer bloque del extent */
    uint32_t length;        /* número de bloques contiguos */
} zeros_extent;             /* 8 bytes */

/* ═══════════════════════════════════════════════════════
 * Inodo
 *
 * Almacena los metadatos de un archivo o directorio.
 * El contenido (datos) vive en los bloques apuntados
 * por los extents — el inodo solo guarda la estructura.
 *
 * Tamaño fijo: ZEROS_INODE_SIZE (128 bytes).
 * ═══════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint64_t    size;           /*  8 bytes — tamaño en bytes del archivo */
    int64_t     ctime;          /*  8 bytes — timestamp de creación (Unix epoch) */
    int64_t     mtime;          /*  8 bytes — timestamp de última modificación */
    int64_t     atime;          /*  8 bytes — timestamp de último acceso */
    uint32_t    mode;           /*  4 bytes — tipo (bits altos) + permisos (bits bajos) */
    uint16_t    extent_count;   /*  2 bytes — cuántos extents están en uso (0..ZEROS_MAX_EXTENTS) */
    uint16_t    link_count;     /*  2 bytes — número de enlaces al inodo (para saber cuándo borrarlo) */
    /* 40 bytes hasta aquí */
    zeros_extent extents[ZEROS_MAX_EXTENTS]; /* 8 * 8 = 64 bytes */
    /* 104 bytes hasta aquí */
    uint8_t     reserved[24];   /* 24 bytes — padding hasta 128, reservado para versiones futuras */
} zeros_inode;                  /* total: 128 bytes == ZEROS_INODE_SIZE */

/* Comprobación en tiempo de compilación: el inodo debe medir exactamente ZEROS_INODE_SIZE */
_Static_assert(sizeof(zeros_inode) == ZEROS_INODE_SIZE,
               "zeros_inode no mide ZEROS_INODE_SIZE bytes — revisa el padding");

/* ═══════════════════════════════════════════════════════
 * Superbloque
 *
 * Siempre ocupa el bloque 0. Contiene toda la información
 * necesaria para leer y escribir el filesystem.
 *
 * Tamaño: ZEROS_BLOCK_SIZE (1024 bytes).
 * ═══════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint32_t    magic;                  /*  4 — debe ser ZEROS_MAGIC */
    uint32_t    version;                /*  4 — versión del formato (ZEROS_FS_VERSION) */
    uint32_t    block_size;             /*  4 — bytes por bloque (siempre ZEROS_BLOCK_SIZE) */

    uint32_t    total_blocks;           /*  4 — total de bloques en el disco */
    uint32_t    total_inodes;           /*  4 — total de inodos (= 10% del disco en inodos) */
    uint32_t    free_blocks;            /*  4 — bloques de datos libres ahora mismo */
    uint32_t    free_inodes;            /*  4 — inodos libres ahora mismo */

    /* Offsets del layout — calculados al formatear, nunca hardcodeados */
    uint32_t    inode_bitmap_start;     /*  4 — bloque donde empieza el bitmap de inodos */
    uint32_t    inode_bitmap_blocks;    /*  4 — cuántos bloques ocupa ese bitmap */
    uint32_t    block_bitmap_start;     /*  4 — bloque donde empieza el bitmap de bloques */
    uint32_t    block_bitmap_blocks;    /*  4 — cuántos bloques ocupa ese bitmap */
    uint32_t    inode_table_start;      /*  4 — bloque donde empieza la tabla de inodos */
    uint32_t    inode_table_blocks;     /*  4 — cuántos bloques ocupa la tabla */
    uint32_t    data_start;             /*  4 — bloque donde empiezan los datos */

    int64_t     created_at;             /*  8 — timestamp de creación del filesystem */
    int64_t     mounted_at;             /*  8 — timestamp del último montaje */

    /* 72 bytes hasta aquí */
    uint8_t     reserved[952];          /* 952 bytes — pad hasta 1024 */
} zeros_superblock;                     /* total: 1024 bytes == ZEROS_BLOCK_SIZE */

_Static_assert(sizeof(zeros_superblock) == ZEROS_BLOCK_SIZE,
               "zeros_superblock no mide ZEROS_BLOCK_SIZE bytes — revisa el padding");

/* ═══════════════════════════════════════════════════════
 * Entrada de directorio
 *
 * Un directorio es un archivo cuyo contenido son entradas
 * de este tipo. Tamaño fijo: ZEROS_DIRENT_SIZE (128 bytes),
 * lo que da ZEROS_DIRENTS_PER_BLOCK (8) entradas por bloque.
 *
 * Una entrada con inode == ZEROS_DIRENT_FREE está vacía.
 * ═══════════════════════════════════════════════════════ */

#define ZEROS_DIRENT_FREE   UINT32_MAX  /* inodo = 0xFFFFFFFF → entrada libre */

typedef struct __attribute__((packed)) {
    uint32_t    inode;                  /*  4 — número de inodo (ZEROS_DIRENT_FREE = libre) */
    uint32_t    type;                   /*  4 — ZEROS_FT_REG o ZEROS_FT_DIR (copia rápida del modo) */
    char        name[ZEROS_NAME_MAX + 1]; /* 120 — nombre con terminador '\0' */
    /* 128 bytes total */
} zeros_dirent;                         /* total: 128 bytes == ZEROS_DIRENT_SIZE */

_Static_assert(sizeof(zeros_dirent) == ZEROS_DIRENT_SIZE,
               "zeros_dirent no mide ZEROS_DIRENT_SIZE bytes — revisa el padding");

/* ═══════════════════════════════════════════════════════
 * Estructura de directorios inicial
 *
 * zeros_format() debe crear estos directorios al inicializar
 * un disco nuevo. Se definen aquí para que zeros_format y
 * cualquier herramienta de diagnóstico usen la misma lista.
 * ═══════════════════════════════════════════════════════ */

static const char *ZEROS_INITIAL_DIRS[] __attribute__((unused)) = {
    "/sys",
    "/sys/kernel",
    "/sys/boot",
    "/sys/model",
    "/sys/model/weights",
    "/sys/model/cache",
    "/bin",
    "/etc",
    "/home",
    "/dock",
    "/operator",
    "/operator/learned",
    "/operator/history",
    "/log",
    "/log/system",
    "/log/dock",
    "/tmp",
    "/doc",
    "/sys/src",
    "/sys/src/shell",
    "/sys/src/fs",
    "/sys/src/vm",
    NULL   /* centinela — fin del array */
};

#endif /* ZEROS_FS_H */
