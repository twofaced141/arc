#include "ata.h"
#include "ahci.h"
#include "idt.h"
#include "debug.h"
#include "terminal.h"

static ata_drive_t primary_master;
static int use_ahci = 0;

static int ata_wait_bsy(uint16_t alt_status) {
    for (int tries = 0; tries < 100000; tries++) {
        if (!(inb(alt_status) & ATA_STATUS_BSY))
            return 0;
    }
    return -1;
}

static int ata_wait_drq(uint16_t alt_status) {
    uint8_t st;
    for (int tries = 0; tries < 100000; tries++) {
        st = inb(alt_status);
        if (st & ATA_STATUS_DRQ)
            return 0;
        if (st & ATA_STATUS_ERR)
            return -1;
        if (!(st & ATA_STATUS_BSY))
            return -1;
    }
    return -1;
}

static void ata_io_wait(void) {
    inb(ATA_PRIMARY_ALTSTAT);
    inb(ATA_PRIMARY_ALTSTAT);
    inb(ATA_PRIMARY_ALTSTAT);
    inb(ATA_PRIMARY_ALTSTAT);
}

static int ata_identify_master(void) {
    if (ata_wait_bsy(ATA_PRIMARY_ALTSTAT) < 0)
        return -1;

    outb(ATA_PRIMARY_DRIVE, ATA_DRIVE_MASTER);
    ata_io_wait();

    outb(ATA_PRIMARY_SCOUNT, 0);
    outb(ATA_PRIMARY_LBA0, 0);
    outb(ATA_PRIMARY_LBA1, 0);
    outb(ATA_PRIMARY_LBA2, 0);

    outb(ATA_PRIMARY_CMD, ATA_CMD_IDENTIFY);
    ata_io_wait();

    uint8_t st = inb(ATA_PRIMARY_CMD);
    if (st == 0)
        return -1;

    if (ata_wait_bsy(ATA_PRIMARY_ALTSTAT) < 0)
        return -1;

    st = inb(ATA_PRIMARY_CMD);
    if (st & ATA_STATUS_ERR)
        return -1;

    if (!(st & ATA_STATUS_DRQ)) {
        for (int tries = 0; tries < 100000; tries++) {
            st = inb(ATA_PRIMARY_CMD);
            if (st & ATA_STATUS_DRQ) break;
            if (st & ATA_STATUS_ERR) return -1;
        }
        if (!(st & ATA_STATUS_DRQ))
            return -1;
    }

    uint16_t buf[256];
    for (int i = 0; i < 256; i++)
        buf[i] = inw(ATA_PRIMARY_DATA);

    primary_master.lba28_sectors = (uint32_t)buf[61] << 16 | buf[60];
    primary_master.lba48_sectors = (uint64_t)buf[101] << 48 | (uint64_t)buf[100] << 32 |
                                  (uint64_t)buf[103] << 16 | buf[102];

    for (int i = 0; i < 40; i += 2) {
        primary_master.model[i] = buf[27 + i/2] >> 8;
        primary_master.model[i + 1] = buf[27 + i/2] & 0xFF;
    }
    primary_master.model[40] = '\0';

    primary_master.present = 1;
    return 0;
}

int ata_init(void) {
    if (ahci_init() == 0) {
        use_ahci = 1;
        return 0;
    }
    use_ahci = 0;

    primary_master.present = 0;

    if (ata_identify_master() == 0) {
        debug_printf("ata: master model='%s' lba28=%u lba48=", primary_master.model, primary_master.lba28_sectors);
        debug_printf("%u", (uint32_t)(primary_master.lba48_sectors >> 32));
        debug_printf("%u\r\n", (uint32_t)primary_master.lba48_sectors);
    } else {
        debug_print("ata: no master drive\r\n");
    }

    return primary_master.present ? 0 : -1;
}

int ata_read_sectors(uint32_t lba, int count, void *buf) {
    if (use_ahci)
        return ahci_read_sectors(lba, count, buf);
    if (!primary_master.present)
        return -1;

    if (count <= 0 || count > 256)
        return -1;

    uint8_t scount = count == 256 ? 0 : (uint8_t)count;

    if (ata_wait_bsy(ATA_PRIMARY_ALTSTAT) < 0)
        return -1;

    outb(ATA_PRIMARY_DRIVE, ATA_DRIVE_MASTER | ATA_LBA_BIT | ((lba >> 24) & 0x0F));
    ata_io_wait();

    outb(ATA_PRIMARY_SCOUNT, scount);
    outb(ATA_PRIMARY_LBA0, lba & 0xFF);
    outb(ATA_PRIMARY_LBA1, (lba >> 8) & 0xFF);
    outb(ATA_PRIMARY_LBA2, (lba >> 16) & 0xFF);

    outb(ATA_PRIMARY_CMD, ATA_CMD_READ_PIO);
    ata_io_wait();

    uint16_t *ptr = (uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (ata_wait_bsy(ATA_PRIMARY_ALTSTAT) < 0)
            return -1;

        uint8_t st = inb(ATA_PRIMARY_CMD);
        if (st & ATA_STATUS_ERR)
            return -1;

        if (!(st & ATA_STATUS_DRQ)) {
            if (ata_wait_drq(ATA_PRIMARY_ALTSTAT) < 0)
                return -1;
        }

        for (int i = 0; i < 256; i++)
            ptr[s * 256 + i] = inw(ATA_PRIMARY_DATA);
    }

    return count * SECTOR_SIZE;
}

int ata_write_sectors(uint32_t lba, int count, const void *buf) {
    if (use_ahci)
        return ahci_write_sectors(lba, count, buf);
    if (!primary_master.present)
        return -1;

    if (count <= 0 || count > 256)
        return -1;

    uint8_t scount = count == 256 ? 0 : (uint8_t)count;

    if (ata_wait_bsy(ATA_PRIMARY_ALTSTAT) < 0)
        return -1;

    outb(ATA_PRIMARY_DRIVE, ATA_DRIVE_MASTER | ATA_LBA_BIT | ((lba >> 24) & 0x0F));
    ata_io_wait();

    outb(ATA_PRIMARY_SCOUNT, scount);
    outb(ATA_PRIMARY_LBA0, lba & 0xFF);
    outb(ATA_PRIMARY_LBA1, (lba >> 8) & 0xFF);
    outb(ATA_PRIMARY_LBA2, (lba >> 16) & 0xFF);

    outb(ATA_PRIMARY_CMD, ATA_CMD_WRITE_PIO);
    ata_io_wait();

    const uint16_t *ptr = (const uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (ata_wait_bsy(ATA_PRIMARY_ALTSTAT) < 0)
            return -1;

        uint8_t st = inb(ATA_PRIMARY_CMD);
        if (st & ATA_STATUS_ERR)
            return -1;

        if (!(st & ATA_STATUS_DRQ)) {
            if (ata_wait_drq(ATA_PRIMARY_ALTSTAT) < 0)
                return -1;
        }

        for (int i = 0; i < 256; i++)
            outw(ATA_PRIMARY_DATA, ptr[s * 256 + i]);
    }

    if (ata_wait_bsy(ATA_PRIMARY_ALTSTAT) < 0)
        return -1;

    return count * SECTOR_SIZE;
}

int ata_flush_cache(void) {
    if (use_ahci)
        return ahci_flush_cache();
    if (!primary_master.present)
        return -1;

    if (ata_wait_bsy(ATA_PRIMARY_ALTSTAT) < 0)
        return -1;

    outb(ATA_PRIMARY_CMD, ATA_CMD_FLUSH);
    ata_io_wait();

    if (ata_wait_bsy(ATA_PRIMARY_ALTSTAT) < 0)
        return -1;

    return 0;
}
