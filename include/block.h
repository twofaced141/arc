#ifndef BLOCK_H
#define BLOCK_H

#include <stdint.h>

#define SECTOR_SIZE 512

typedef struct block_device {
    const char *name;
    uint32_t lba_count;
    int (*read)(struct block_device *dev, void *buf, uint32_t lba, int count);
    int (*write)(struct block_device *dev, const void *buf, uint32_t lba, int count);
    void *priv;
} block_device_t;

#define BLOCK_DEV_MAX 16

void block_devices_init(void);
int block_device_register(block_device_t *dev);
int block_device_count(void);
block_device_t *block_device_get(int index);

typedef struct {
    uint8_t boot_indicator;
    uint8_t start_chs[3];
    uint8_t type;
    uint8_t end_chs[3];
    uint32_t lba_start;
    uint32_t lba_count;
} __attribute__((packed)) mbr_entry_t;

typedef struct {
    uint8_t code[446];
    mbr_entry_t partitions[4];
    uint16_t signature;
} __attribute__((packed)) mbr_t;

#define MBR_SIGNATURE 0xAA55

int mbr_parse(block_device_t *dev, mbr_t *mbr);

#endif
