#include <stddef.h>
#include "block.h"

static block_device_t *devices[BLOCK_DEV_MAX];
static int device_count;

void block_devices_init(void) {
    device_count = 0;
    for (int i = 0; i < BLOCK_DEV_MAX; i++)
        devices[i] = NULL;
}

int block_device_register(block_device_t *dev) {
    if (device_count >= BLOCK_DEV_MAX)
        return -1;
    devices[device_count++] = dev;
    return 0;
}

int block_device_count(void) {
    return device_count;
}

block_device_t *block_device_get(int index) {
    if (index < 0 || index >= device_count)
        return NULL;
    return devices[index];
}

int mbr_parse(block_device_t *dev, mbr_t *mbr) {
    if (dev->read(dev, mbr, 0, 1) < 0)
        return -1;
    if (mbr->signature != MBR_SIGNATURE)
        return -1;
    return 0;
}
