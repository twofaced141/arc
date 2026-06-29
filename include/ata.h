#ifndef ATA_H
#define ATA_H

#include <stdint.h>

#define ATA_PRIMARY_DATA    0x1F0
#define ATA_PRIMARY_ERROR   0x1F1
#define ATA_PRIMARY_SCOUNT  0x1F2
#define ATA_PRIMARY_LBA0    0x1F3
#define ATA_PRIMARY_LBA1    0x1F4
#define ATA_PRIMARY_LBA2    0x1F5
#define ATA_PRIMARY_DRIVE   0x1F6
#define ATA_PRIMARY_CMD     0x1F7
#define ATA_PRIMARY_ALTSTAT 0x3F6

#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CMD_FLUSH       0xE7

#define ATA_STATUS_ERR   (1 << 0)
#define ATA_STATUS_DRQ   (1 << 3)
#define ATA_STATUS_SRV   (1 << 4)
#define ATA_STATUS_DF    (1 << 5)
#define ATA_STATUS_RDY   (1 << 6)
#define ATA_STATUS_BSY   (1 << 7)

#define ATA_DRIVE_MASTER 0xE0
#define ATA_DRIVE_SLAVE  0xF0
#define ATA_LBA_BIT      (1 << 6)

#define SECTOR_SIZE 512

typedef struct {
    uint32_t lba28_sectors;
    uint64_t lba48_sectors;
    char model[41];
    int present;
} ata_drive_t;

int ata_init(void);
int ata_read_sectors(uint32_t lba, int count, void *buf);
int ata_write_sectors(uint32_t lba, int count, const void *buf);
int ata_flush_cache(void);

#endif
