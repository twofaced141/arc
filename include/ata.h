#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include "block.h"

#define ATA_PRIMARY_DATA    0x1F0
#define ATA_PRIMARY_SCOUNT  0x1F2
#define ATA_PRIMARY_LBA0    0x1F3
#define ATA_PRIMARY_LBA1    0x1F4
#define ATA_PRIMARY_LBA2    0x1F5
#define ATA_PRIMARY_DRIVE   0x1F6
#define ATA_PRIMARY_CMD     0x1F7
#define ATA_PRIMARY_ALTSTAT 0x3F6

#define ATA_SECONDARY_DATA    0x170
#define ATA_SECONDARY_SCOUNT  0x172
#define ATA_SECONDARY_LBA0    0x173
#define ATA_SECONDARY_LBA1    0x174
#define ATA_SECONDARY_LBA2    0x175
#define ATA_SECONDARY_DRIVE   0x176
#define ATA_SECONDARY_CMD     0x177
#define ATA_SECONDARY_ALTSTAT 0x376

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

#define ATA_DRIVES_MAX 4

typedef struct {
    uint16_t data;
    uint16_t scount;
    uint16_t lba0;
    uint16_t lba1;
    uint16_t lba2;
    uint16_t drive;
    uint16_t cmd;
    uint16_t altstat;
} ata_channel_ports_t;

typedef struct {
    ata_channel_ports_t *chan;
    uint8_t drive_sel;
    uint32_t lba28_sectors;
    uint64_t lba48_sectors;
    char model[41];
    int present;
    block_device_t bdev;
} ata_drive_t;

int ata_init(void);
ata_drive_t *ata_get_drive(int index);
int ata_drive_count(void);

#endif
