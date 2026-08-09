#ifndef BSD_BLOCK_H
#define BSD_BLOCK_H

#include <stdint.h>
#include <stddef.h>

struct block_dev;

typedef int (*block_read_t)(struct block_dev *dev, uint64_t lba, void *buf, size_t count);
typedef int (*block_write_t)(struct block_dev *dev, uint64_t lba, const void *buf, size_t count);

typedef struct block_dev {
    char name[16];
    uint32_t block_size;
    uint64_t num_blocks;
    block_read_t read;
    block_write_t write;
    void *priv;
    uint32_t cache_key;    /* stable per-device id for the block cache */
} block_dev_t;

int  blk_read(block_dev_t *dev, uint64_t lba, void *buf, size_t count);
int  blk_write(block_dev_t *dev, uint64_t lba, const void *buf, size_t count);

/* Write back all dirty cached blocks of a device. */
int  blk_sync(block_dev_t *dev);

void block_dev_register(block_dev_t *dev);
block_dev_t *block_dev_lookup(const char *name);
int  block_dev_get_count(void);
block_dev_t *block_dev_get(int index);

#endif
