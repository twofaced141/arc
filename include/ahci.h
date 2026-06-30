#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

#define AHCI_SECTOR_SIZE 512

int ahci_init(void);
int ahci_read_sectors(uint32_t lba, int count, void *buf);
int ahci_write_sectors(uint32_t lba, int count, const void *buf);
int ahci_flush_cache(void);

#endif
