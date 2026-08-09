#include "bsd/block.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "debug.h"

typedef struct {
    void *data;
    size_t size;
} ramdisk_priv_t;

static int ramdisk_read(block_dev_t *dev, uint64_t lba, void *buf, size_t count) {
    ramdisk_priv_t *priv = (ramdisk_priv_t *)dev->priv;
    uint64_t offset = lba * dev->block_size;
    size_t len = count * dev->block_size;
    if (offset + len > priv->size)
        return -1;
    memcpy(buf, (uint8_t *)priv->data + offset, len);
    return (int)count;
}

static int ramdisk_write(block_dev_t *dev, uint64_t lba, const void *buf, size_t count) {
    ramdisk_priv_t *priv = (ramdisk_priv_t *)dev->priv;
    uint64_t offset = lba * dev->block_size;
    size_t len = count * dev->block_size;
    if (offset + len > priv->size)
        return -1;
    memcpy((uint8_t *)priv->data + offset, buf, len);
    return (int)count;
}

block_dev_t *ramdisk_create(size_t size) {
    block_dev_t *dev = (block_dev_t *)kmalloc(sizeof(block_dev_t));
    if (!dev) return NULL;
    memset(dev, 0, sizeof(block_dev_t));

    ramdisk_priv_t *priv = (ramdisk_priv_t *)kmalloc(sizeof(ramdisk_priv_t));
    if (!priv) { kfree(dev); return NULL; }

    uint32_t num_pages = (size + 0xFFF) >> 12;
    priv->data = pmm_alloc_pages(num_pages);
    if (!priv->data) { kfree(priv); kfree(dev); return NULL; }
    memset(priv->data, 0, size);

    priv->size = size;

    memcpy(dev->name, "ramdisk", 8);
    dev->block_size = 512;
    dev->num_blocks = size / 512;
    dev->read = ramdisk_read;
    dev->write = ramdisk_write;
    dev->priv = priv;

    log_printf(LOG_LEVEL_DEBUG, "ramdisk: created %u KB\n", (unsigned)(size / 1024));

    block_dev_register(dev);
    return dev;
}

block_dev_t *ramdisk_create_from(void *data, size_t size) {
    block_dev_t *dev = (block_dev_t *)kmalloc(sizeof(block_dev_t));
    if (!dev) return NULL;
    memset(dev, 0, sizeof(block_dev_t));

    ramdisk_priv_t *priv = (ramdisk_priv_t *)kmalloc(sizeof(ramdisk_priv_t));
    if (!priv) { kfree(dev); return NULL; }

    priv->data = data;
    priv->size = size;

    memcpy(dev->name, "ramdisk", 8);
    dev->block_size = 512;
    dev->num_blocks = size / 512;
    dev->read = ramdisk_read;
    dev->write = ramdisk_write;
    dev->priv = priv;

    log_printf(LOG_LEVEL_INFO, "ramdisk: created from external image, %u KB\n", (unsigned)(size / 1024));

    block_dev_register(dev);
    return dev;
}
