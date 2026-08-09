#include "debug.h"
#ifndef __aarch64__
#include "bsd/block.h"
#include "bsd/drivers/ata.h"
#include "pci.h"
#include "device.h"
#include "driver.h"
#include "idt.h"
#include "string.h"
#include "vmm.h"

typedef struct ata_drive {
    uint16_t io_base;         /* command block base */
    uint16_t ctrl_base;       /* control block base */
    int      drive_sel;       /* value for drive register */
    int      slave;           /* 0=master 1=slave */
    uint32_t total_sectors;
    char     model[41];
} ata_drive_t;

static ata_drive_t drives[ATA_MAX_DRIVES];
static int drive_count;


static int ata_wait_bsy(uint16_t io_base) {
    uint8_t s = inb(io_base + ATA_REG_STATUS);
    if (s == 0xFF)
        return -1;          /* floating bus — no controller */
    for (int i = 0; i < 10000000; i++) {
        if (!(inb(io_base + ATA_REG_STATUS) & ATA_SR_BSY))
            return 0;
    }
    return -1;
}

static int ata_wait_drq(uint16_t io_base) {
    for (int i = 0; i < 10000000; i++) {
        uint8_t s = inb(io_base + ATA_REG_STATUS);
        if (s & ATA_SR_ERR) return -1;
        if (s & ATA_SR_DRQ) return 0;
    }
    return -1;
}

static int ata_poll(uint16_t io_base) {
    if (ata_wait_bsy(io_base) < 0) return -1;
    uint8_t s = inb(io_base + ATA_REG_STATUS);
    if (s & ATA_SR_ERR) return -1;
    return 0;
}


static int ata_read_sectors(ata_drive_t *d, uint32_t lba, uint8_t count, void *buf) {
    uint16_t io = d->io_base;

    outb(io + ATA_REG_DRIVE, d->drive_sel | ((lba >> 24) & 0x0F));
    io_wait();

    outb(io + ATA_REG_FEATURES, 0);
    outb(io + ATA_REG_SECCOUNT, count);
    outb(io + ATA_REG_LBA_LO, lba & 0xFF);
    outb(io + ATA_REG_LBA_MI, (lba >> 8) & 0xFF);
    outb(io + ATA_REG_LBA_HI, (lba >> 16) & 0xFF);
    outb(io + ATA_REG_CMD, ATA_CMD_READ_PIO);

    uint16_t *ptr = (uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (ata_wait_drq(io) < 0)
            return -1;
        for (int i = 0; i < 256; i++)
            ptr[s * 256 + i] = inw(io + ATA_REG_DATA);
    }

    if (ata_poll(io) < 0)
        return -1;

    return 0;
}

static int ata_write_sectors(ata_drive_t *d, uint32_t lba, uint8_t count, const void *buf) {
    uint16_t io = d->io_base;

    outb(io + ATA_REG_DRIVE, d->drive_sel | ((lba >> 24) & 0x0F));
    io_wait();

    outb(io + ATA_REG_FEATURES, 0);
    outb(io + ATA_REG_SECCOUNT, count);
    outb(io + ATA_REG_LBA_LO, lba & 0xFF);
    outb(io + ATA_REG_LBA_MI, (lba >> 8) & 0xFF);
    outb(io + ATA_REG_LBA_HI, (lba >> 16) & 0xFF);
    outb(io + ATA_REG_CMD, ATA_CMD_WRITE_PIO);

    const uint16_t *ptr = (const uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (ata_wait_drq(io) < 0)
            return -1;
        for (int i = 0; i < 256; i++)
            outw(io + ATA_REG_DATA, ptr[s * 256 + i]);
    }

    /* Wait for write cache flush */
    io_wait();
    if (ata_wait_bsy(io) < 0)
        return -1;

    outb(io + ATA_REG_CMD, ATA_CMD_FLUSH);
    if (ata_poll(io) < 0)
        return -1;

    return 0;
}


static int ata_identify(ata_drive_t *d) {
    uint16_t io = d->io_base;

    /* Select drive */
    outb(io + ATA_REG_DRIVE, d->drive_sel);
    io_wait();
    if (ata_wait_bsy(io) < 0)
        return -1;

    /* Send IDENTIFY */
    outb(io + ATA_REG_SECCOUNT, 0);
    outb(io + ATA_REG_LBA_LO, 0);
    outb(io + ATA_REG_LBA_MI, 0);
    outb(io + ATA_REG_LBA_HI, 0);
    outb(io + ATA_REG_CMD, ATA_CMD_IDENTIFY);

    /* Check for device presence */
    uint8_t s = inb(io + ATA_REG_STATUS);
    if (s == 0)
        return -1; /* no device */

    if (ata_wait_bsy(io) < 0)
        return -1;

    s = inb(io + ATA_REG_STATUS);
    if (s & ATA_SR_ERR) {
        /* Might be ATAPI device */
        uint8_t err = inb(io + ATA_REG_ERROR);
        if (err & ATA_ER_ABRT)
            return -1;
        return -1;
    }

    /* Wait for DRQ */
    if (ata_wait_drq(io) < 0)
        return -1;

    /* Read identify data (256 words) */
    uint16_t id[256];
    for (int i = 0; i < 256; i++)
        id[i] = inw(io + ATA_REG_DATA);

    /* Extract model string (words 27-46, each word = 2 ASCII chars, big-endian) */
    for (int i = 0; i < 40; i += 2) {
        d->model[i]     = (id[27 + i/2] >> 8) & 0xFF;
        d->model[i + 1] = id[27 + i/2] & 0xFF;
    }
    d->model[40] = '\0';

    /* Trim trailing spaces */
    for (int i = 39; i >= 0 && d->model[i] == ' '; i--)
        d->model[i] = '\0';

    /* Total LBA sectors (words 60-61) */
    d->total_sectors = id[60] | ((uint32_t)id[61] << 16);

    log_printf(LOG_LEVEL_INFO, "ata: drive found: %s (%u sectors)\n", d->model, d->total_sectors);
    return 0;
}


static int ata_block_read(block_dev_t *dev, uint64_t lba, void *buf, size_t count) {
    ata_drive_t *d = (ata_drive_t *)dev->priv;
    uint8_t *ptr = (uint8_t *)buf;

    while (count > 0) {
        uint8_t batch = (count > 255) ? 255 : (uint8_t)count;
        if (ata_read_sectors(d, (uint32_t)lba, batch, ptr) < 0)
            return -1;
        lba += batch;
        ptr += batch * ATA_SECTOR_SIZE;
        count -= batch;
    }
    return (int)count;
}

static int ata_block_write(block_dev_t *dev, uint64_t lba, const void *buf, size_t count) {
    ata_drive_t *d = (ata_drive_t *)dev->priv;
    const uint8_t *ptr = (const uint8_t *)buf;

    while (count > 0) {
        uint8_t batch = (count > 255) ? 255 : (uint8_t)count;
        if (ata_write_sectors(d, (uint32_t)lba, batch, ptr) < 0)
            return -1;
        lba += batch;
        ptr += batch * ATA_SECTOR_SIZE;
        count -= batch;
    }
    return (int)count;
}


static void ata_probe_channel(uint16_t io_base, uint16_t ctrl_base, int channel) {
    for (int slave = 0; slave <= 1; slave++) {
        if (drive_count >= ATA_MAX_DRIVES) return;

        ata_drive_t *d = &drives[drive_count];
        memset(d, 0, sizeof(ata_drive_t));
        d->io_base = io_base;
        d->ctrl_base = ctrl_base;
        d->slave = slave;
        d->drive_sel = ATA_DRIVE_LBA | (slave ? (1 << 4) : 0);

        if (ata_wait_bsy(io_base) < 0)
            continue;

        if (ata_identify(d) == 0) {
            block_dev_t *bdev = (block_dev_t *)kmalloc(sizeof(block_dev_t));
            if (bdev) {
                memset(bdev, 0, sizeof(block_dev_t));
                bdev->name[0] = 'a'; bdev->name[1] = 't'; bdev->name[2] = 'a';
                bdev->name[3] = '0' + (char)drive_count;
                bdev->name[4] = '\0';
                bdev->block_size = ATA_SECTOR_SIZE;
                bdev->num_blocks = d->total_sectors;
                bdev->read = ata_block_read;
                bdev->write = ata_block_write;
                bdev->priv = d;
                block_dev_register(bdev);
                log_printf(LOG_LEVEL_INFO, "ata: %s registered (channel=%d slave=%d)\n",
                             bdev->name, channel, slave);
            }
            drive_count++;
        }
    }
}


static int ata_pci_probe(struct arc_device *adev) {
    pci_device_t *dev = (pci_device_t *)adev->priv;
    log_printf(LOG_LEVEL_DEBUG, "ata: probing %s (%02x:%02x.%x prog_if=0x%02x)\n",
                 adev->name, dev->addr.bus, dev->addr.slot, dev->addr.func, dev->prog_if);

    /* Determine which channels this device controls based on prog_if
     * Bit 0: primary channel mode (0=legacy, 1=native PCI)
     * Bit 2: secondary channel mode (0=legacy, 1=native PCI) */
    if (!(dev->prog_if & 0x01)) {
        log_print(LOG_LEVEL_DEBUG, "ata: probing primary channel (legacy)\n");
        ata_probe_channel(ATA_PRIMARY_IO, ATA_PRIMARY_CTRL, 0);
    } else if (dev->bars[0] || dev->bars[1]) {
        /* Native mode — try to use BARs (command block at BAR0, control at BAR1) */
        uint16_t io = (uint16_t)PCI_BAR_ADDR_IO(dev->bars[0]);
        uint16_t ctrl = (uint16_t)PCI_BAR_ADDR_IO(dev->bars[1]);
        ata_probe_channel(io, ctrl, 0);
    }

    if (!(dev->prog_if & 0x04)) {
        log_print(LOG_LEVEL_DEBUG, "ata: probing secondary channel (legacy)\n");
        ata_probe_channel(ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 1);
    } else if (dev->bars[2] || dev->bars[3]) {
        uint16_t io = (uint16_t)PCI_BAR_ADDR_IO(dev->bars[2]);
        uint16_t ctrl = (uint16_t)PCI_BAR_ADDR_IO(dev->bars[3]);
        ata_probe_channel(io, ctrl, 1);
    }

    return 0;
}

static const struct arc_device_id ata_ids[] = {
    { .class = PCI_CLASS_MASS_STORAGE, .subclass = PCI_SUBCLASS_IDE },
    ARC_DEVICE_ID_END
};

static struct arc_driver ata_driver = {
    .name     = "ata-pio",
    .type     = ARC_DEV_PCI,
    .id_table = ata_ids,
    .probe    = ata_pci_probe,
};


void ata_init(void) {
    log_print(LOG_LEVEL_DEBUG, "ata: init\n");

    /* Also try legacy ports directly in case PCI is not available */
    ata_probe_channel(ATA_PRIMARY_IO, ATA_PRIMARY_CTRL, 0);
    ata_probe_channel(ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 1);

    arc_driver_register(&ata_driver);
}
#else
void ata_init(void) {
    log_print(LOG_LEVEL_DEBUG, "ata: init (not available on this arch)\n");
}
#endif

