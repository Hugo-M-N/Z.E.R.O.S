#pragma once

int ahci_init(void);
int ahci_read_sector (unsigned int lba, void *buf);
int ahci_write_sector(unsigned int lba, const void *buf);
