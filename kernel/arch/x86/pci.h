#pragma once

unsigned int pci_config_read(unsigned char bus, unsigned char slot,
                              unsigned char func, unsigned char offset);

/* Busca el primer controlador AHCI (clase 01h/06h/01h).
 * Devuelve el valor de BAR5 (MMIO base sin flags), o 0 si no encontrado. */
unsigned int pci_find_ahci(unsigned char *bus_out, unsigned char *slot_out);
