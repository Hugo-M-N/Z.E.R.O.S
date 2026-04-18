/* Implementación de disk.h para x86 — delega en el driver AHCI */
#include "ahci.h"
#include "../../disk.h"

void disk_init(void)                              { ahci_init(); }
int  disk_read_sector (unsigned int lba, void *b) { return ahci_read_sector(lba, b); }
int  disk_write_sector(unsigned int lba, const void *b) { return ahci_write_sector(lba, b); }
