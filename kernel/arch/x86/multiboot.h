#pragma once

/* Estructura que el bootloader deja en memoria antes de saltar al kernel.
 * Solo definimos los campos que usamos — el spec completo tiene muchos más. */
struct multiboot_info {
    unsigned int flags;
    unsigned int mem_lower;   /* KB por debajo de 1 MB (típicamente 640) */
    unsigned int mem_upper;   /* KB por encima de 1 MB */
    unsigned int boot_device;
    unsigned int cmdline;
    unsigned int mods_count;
    unsigned int mods_addr;
    unsigned int syms[4];
    unsigned int mmap_length; /* longitud del mapa de memoria en bytes */
    unsigned int mmap_addr;   /* dirección del mapa de memoria */
} __attribute__((packed));

/* Cada entrada del mapa de memoria multiboot */
struct mmap_entry {
    unsigned int size;                    /* tamaño de esta entrada - 4 */
    unsigned int base_low, base_high;     /* dirección base (64-bit) */
    unsigned int len_low,  len_high;      /* longitud (64-bit) */
    unsigned int type;                    /* 1 = disponible, resto = reservado */
} __attribute__((packed));

#define MULTIBOOT_FLAG_MMAP (1 << 6)   /* el campo mmap_* es válido */
