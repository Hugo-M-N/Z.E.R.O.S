#pragma once

/* Interfaz arch-independiente de acceso al disco.
 * x86: implementada por ahci.c (SATA DMA).
 * ARM: implementar con eMMC/SD.
 *
 * Un sector = 512 bytes.
 * El zeros_fs usa bloques de 1024 bytes = 2 sectores. */

void disk_init(void);
int  disk_read_sector (unsigned int lba, void *buf);
int  disk_write_sector(unsigned int lba, const void *buf);
