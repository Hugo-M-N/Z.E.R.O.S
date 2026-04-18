/*
 * pci.c (x86) — Acceso al espacio de configuración PCI
 *
 * El acceso se hace via los puertos 0xCF8 (dirección) y 0xCFC (datos).
 * El registro de dirección codifica bus/slot/función/offset en 32 bits:
 *
 *   bit 31     : enable (siempre 1)
 *   bits 23-16 : número de bus (0-255)
 *   bits 15-11 : número de slot/device (0-31)
 *   bits 10-8  : número de función (0-7)
 *   bits 7-2   : registro (offset / 4)
 *   bits 1-0   : siempre 0 (acceso a dword completo)
 */

#include "pci.h"
#include "io.h"

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

unsigned int pci_config_read(unsigned char bus, unsigned char slot,
                              unsigned char func, unsigned char offset) {
    unsigned int addr = (1u << 31)
                      | ((unsigned int)bus  << 16)
                      | ((unsigned int)slot << 11)
                      | ((unsigned int)func <<  8)
                      | (offset & 0xFC);
    outl(PCI_ADDR, addr);
    return inl(PCI_DATA);
}

unsigned int pci_find_ahci(unsigned char *bus_out, unsigned char *slot_out) {
    for (int bus = 0; bus < 4; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            unsigned int id = pci_config_read(bus, slot, 0, 0x00);
            if ((id & 0xFFFF) == 0xFFFF) continue;

            /* Clase: storage (01h), subclase: SATA (06h), prog-if: AHCI (01h) */
            unsigned int cls = pci_config_read(bus, slot, 0, 0x08);
            if (((cls >> 24) & 0xFF) != 0x01) continue;
            if (((cls >> 16) & 0xFF) != 0x06) continue;
            if (((cls >>  8) & 0xFF) != 0x01) continue;

            *bus_out  = (unsigned char)bus;
            *slot_out = (unsigned char)slot;

            /* Activar bus mastering (bit 2 del Command register PCI).
             * Sin esto el controlador no puede hacer DMA a la RAM. */
            unsigned int cmd_reg = pci_config_read(bus, slot, 0, 0x04);
            unsigned int new_cmd = (cmd_reg & 0xFFFF) | (1u << 2) | (1u << 1);
            unsigned int addr = (1u << 31) | ((unsigned int)bus << 16)
                              | ((unsigned int)slot << 11) | 0x04;
            outl(PCI_ADDR, addr);
            outl(PCI_DATA, new_cmd);

            unsigned int bar5 = pci_config_read(bus, slot, 0, 0x24);
            return bar5 & ~0xFu;
        }
    }
    return 0;
}
