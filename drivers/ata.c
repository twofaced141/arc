#include "ata.h"
#include "idt.h"
#include "debug.h"
#include "string.h"

static ata_channel_ports_t primary_chan = {
    .data    = ATA_PRIMARY_DATA,
    .scount  = ATA_PRIMARY_SCOUNT,
    .lba0    = ATA_PRIMARY_LBA0,
    .lba1    = ATA_PRIMARY_LBA1,
    .lba2    = ATA_PRIMARY_LBA2,
    .drive   = ATA_PRIMARY_DRIVE,
    .cmd     = ATA_PRIMARY_CMD,
    .altstat = ATA_PRIMARY_ALTSTAT,
};

static ata_channel_ports_t secondary_chan = {
    .data    = ATA_SECONDARY_DATA,
    .scount  = ATA_SECONDARY_SCOUNT,
    .lba0    = ATA_SECONDARY_LBA0,
    .lba1    = ATA_SECONDARY_LBA1,
    .lba2    = ATA_SECONDARY_LBA2,
    .drive   = ATA_SECONDARY_DRIVE,
    .cmd     = ATA_SECONDARY_CMD,
    .altstat = ATA_SECONDARY_ALTSTAT,
};

static ata_drive_t ata_drives[ATA_DRIVES_MAX];
static int ata_count;

static int ata_wait_bsy(ata_channel_ports_t *chan) {
    for (int tries = 0; tries < 100000; tries++) {
        if (!(inb(chan->altstat) & ATA_STATUS_BSY))
            return 0;
    }
    return -1;
}

static int ata_wait_drq(ata_channel_ports_t *chan) {
    uint8_t st;
    for (int tries = 0; tries < 100000; tries++) {
        st = inb(chan->altstat);
        if (st & ATA_STATUS_DRQ)
            return 0;
        if (st & ATA_STATUS_ERR)
            return -1;
        if (!(st & ATA_STATUS_BSY))
            return -1;
    }
    return -1;
}

static void ata_io_wait(ata_channel_ports_t *chan) {
    inb(chan->altstat);
    inb(chan->altstat);
    inb(chan->altstat);
    inb(chan->altstat);
}

static int ata_identify(ata_drive_t *drive) {
    ata_channel_ports_t *chan = drive->chan;

    if (ata_wait_bsy(chan) < 0)
        return -1;

    outb(chan->drive, drive->drive_sel);
    ata_io_wait(chan);

    outb(chan->scount, 0);
    outb(chan->lba0, 0);
    outb(chan->lba1, 0);
    outb(chan->lba2, 0);

    outb(chan->cmd, ATA_CMD_IDENTIFY);
    ata_io_wait(chan);

    uint8_t st = inb(chan->cmd);
    if (st == 0)
        return -1;

    if (ata_wait_bsy(chan) < 0)
        return -1;

    st = inb(chan->cmd);
    if (st & ATA_STATUS_ERR)
        return -1;

    if (!(st & ATA_STATUS_DRQ)) {
        for (int tries = 0; tries < 100000; tries++) {
            st = inb(chan->cmd);
            if (st & ATA_STATUS_DRQ) break;
            if (st & ATA_STATUS_ERR) return -1;
        }
        if (!(st & ATA_STATUS_DRQ))
            return -1;
    }

    uint16_t buf[256];
    for (int i = 0; i < 256; i++)
        buf[i] = inw(chan->data);

    drive->lba28_sectors = (uint32_t)buf[61] << 16 | buf[60];
    drive->lba48_sectors = (uint64_t)buf[101] << 48 | (uint64_t)buf[100] << 32 |
                           (uint64_t)buf[103] << 16 | buf[102];

    for (int i = 0; i < 40; i += 2) {
        drive->model[i] = buf[27 + i/2] >> 8;
        drive->model[i + 1] = buf[27 + i/2] & 0xFF;
    }
    drive->model[40] = '\0';

    drive->present = 1;
    return 0;
}

static int ata_drive_read(block_device_t *bdev, void *buf, uint32_t lba, int count) {
    ata_drive_t *drive = (ata_drive_t *)bdev->priv;
    ata_channel_ports_t *chan = drive->chan;

    if (!drive->present)
        return -1;
    if (count <= 0 || count > 256)
        return -1;

    uint8_t scount = count == 256 ? 0 : (uint8_t)count;

    if (ata_wait_bsy(chan) < 0)
        return -1;

    outb(chan->drive, drive->drive_sel | ATA_LBA_BIT | ((lba >> 24) & 0x0F));
    ata_io_wait(chan);

    outb(chan->scount, scount);
    outb(chan->lba0, lba & 0xFF);
    outb(chan->lba1, (lba >> 8) & 0xFF);
    outb(chan->lba2, (lba >> 16) & 0xFF);

    outb(chan->cmd, ATA_CMD_READ_PIO);
    ata_io_wait(chan);

    uint16_t *ptr = (uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (ata_wait_bsy(chan) < 0)
            return -1;

        uint8_t st = inb(chan->cmd);
        if (st & ATA_STATUS_ERR)
            return -1;

        if (!(st & ATA_STATUS_DRQ)) {
            if (ata_wait_drq(chan) < 0)
                return -1;
        }

        for (int i = 0; i < 256; i++)
            ptr[s * 256 + i] = inw(chan->data);
    }

    return count * SECTOR_SIZE;
}

static int ata_drive_write(block_device_t *bdev, const void *buf, uint32_t lba, int count) {
    ata_drive_t *drive = (ata_drive_t *)bdev->priv;
    ata_channel_ports_t *chan = drive->chan;

    if (!drive->present)
        return -1;
    if (count <= 0 || count > 256)
        return -1;

    uint8_t scount = count == 256 ? 0 : (uint8_t)count;

    if (ata_wait_bsy(chan) < 0)
        return -1;

    outb(chan->drive, drive->drive_sel | ATA_LBA_BIT | ((lba >> 24) & 0x0F));
    ata_io_wait(chan);

    outb(chan->scount, scount);
    outb(chan->lba0, lba & 0xFF);
    outb(chan->lba1, (lba >> 8) & 0xFF);
    outb(chan->lba2, (lba >> 16) & 0xFF);

    outb(chan->cmd, ATA_CMD_WRITE_PIO);
    ata_io_wait(chan);

    const uint16_t *ptr = (const uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (ata_wait_bsy(chan) < 0)
            return -1;

        uint8_t st = inb(chan->cmd);
        if (st & ATA_STATUS_ERR)
            return -1;

        if (!(st & ATA_STATUS_DRQ)) {
            if (ata_wait_drq(chan) < 0)
                return -1;
        }

        for (int i = 0; i < 256; i++)
            outw(chan->data, ptr[s * 256 + i]);
    }

    if (ata_wait_bsy(chan) < 0)
        return -1;

    return count * SECTOR_SIZE;
}

static void ata_register_drive(ata_drive_t *drive, const char *name) {
    drive->bdev.name = name;
    drive->bdev.lba_count = drive->lba28_sectors;
    drive->bdev.read = ata_drive_read;
    drive->bdev.write = ata_drive_write;
    drive->bdev.priv = drive;
    block_device_register(&drive->bdev);
}

int ata_init(void) {
    ata_count = 0;
    for (int i = 0; i < ATA_DRIVES_MAX; i++) {
        ata_drives[i].present = 0;
        ata_drives[i].bdev.name = NULL;
    }

    /* Primary master */
    ata_drives[ata_count].chan = &primary_chan;
    ata_drives[ata_count].drive_sel = ATA_DRIVE_MASTER;
    if (ata_identify(&ata_drives[ata_count]) == 0) {
        debug_printf("ata0: model='%s' lba28=%u\r\n",
                     ata_drives[ata_count].model,
                     ata_drives[ata_count].lba28_sectors);
        ata_register_drive(&ata_drives[ata_count], "ata0");
        ata_count++;
    }

    /* Primary slave */
    ata_drives[ata_count].chan = &primary_chan;
    ata_drives[ata_count].drive_sel = ATA_DRIVE_SLAVE;
    if (ata_identify(&ata_drives[ata_count]) == 0) {
        debug_printf("ata1: model='%s' lba28=%u\r\n",
                     ata_drives[ata_count].model,
                     ata_drives[ata_count].lba28_sectors);
        ata_register_drive(&ata_drives[ata_count], "ata1");
        ata_count++;
    }

    /* Secondary master */
    ata_drives[ata_count].chan = &secondary_chan;
    ata_drives[ata_count].drive_sel = ATA_DRIVE_MASTER;
    if (ata_identify(&ata_drives[ata_count]) == 0) {
        debug_printf("ata2: model='%s' lba28=%u\r\n",
                     ata_drives[ata_count].model,
                     ata_drives[ata_count].lba28_sectors);
        ata_register_drive(&ata_drives[ata_count], "ata2");
        ata_count++;
    }

    /* Secondary slave */
    ata_drives[ata_count].chan = &secondary_chan;
    ata_drives[ata_count].drive_sel = ATA_DRIVE_SLAVE;
    if (ata_identify(&ata_drives[ata_count]) == 0) {
        debug_printf("ata3: model='%s' lba28=%u\r\n",
                     ata_drives[ata_count].model,
                     ata_drives[ata_count].lba28_sectors);
        ata_register_drive(&ata_drives[ata_count], "ata3");
        ata_count++;
    }

    debug_printf("ata: %d drive(s) found\r\n", ata_count);
    return ata_count > 0 ? 0 : -1;
}

ata_drive_t *ata_get_drive(int index) {
    if (index < 0 || index >= ata_count)
        return NULL;
    return &ata_drives[index];
}

int ata_drive_count(void) {
    return ata_count;
}
