/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026, fierce
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS AS IS AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */


#include "bsd/drivers/ahci.h"
#include "bsd/block.h"
#include "pci.h"
#include "device.h"
#include "driver.h"
#include "vmm.h"
#include "pmm.h"
#include "debug.h"
#include "string.h"

/* Memory-mapped virtual address layout for ABAR and DMA buffers */
#define AHCI_MMIO_BASE   0xFFB00000
#define ABAR_VADDR       (AHCI_MMIO_BASE)
#define BOUNCE_VADDR     (AHCI_MMIO_BASE + 0x10000)
#define CMDLIST_VADDR    (AHCI_MMIO_BASE + 0x11000)
#define RFIS_VADDR       (AHCI_MMIO_BASE + 0x12000)
#define CMDTABLE_VADDR   (AHCI_MMIO_BASE + 0x13000)

/* HBA registers */
#define HBA_CAP   0x00
#define HBA_GHC   0x04
#define HBA_IS    0x08
#define HBA_PI    0x0C
#define HBA_VER   0x10
#define HBA_CAP2  0x24
#define HBA_BOHC  0x28

/* Port registers (per-port stride 0x80) */
#define PORT_CLB   0x00
#define PORT_CLBU  0x04
#define PORT_FB    0x08
#define PORT_FBU   0x0C
#define PORT_IS    0x10
#define PORT_IE    0x14
#define PORT_CMD   0x18
#define PORT_TFD   0x20
#define PORT_SIG   0x24
#define PORT_SSTS  0x28
#define PORT_SCTL  0x2C
#define PORT_SERR  0x30
#define PORT_SACT  0x34
#define PORT_CI    0x38

/* GHC bits */
#define GHC_HR    (1 << 0)
#define GHC_IE    (1 << 1)
#define GHC_AE    (1U << 31)

/* CAP bits */
#define CAP_NP_SHIFT  0
#define CAP_NP_MASK   0x1F
#define CAP_SAM        (1 << 18)
#define CAP_SSS        (1 << 20)
#define CAP_S64A       (1 << 31)

/* CMD bits */
#define CMD_ST    (1 << 0)
#define CMD_SUD   (1 << 1)
#define CMD_POD   (1 << 2)
#define CMD_CLI   (1 << 3)
#define CMD_FRE   (1 << 4)
#define CMD_CR    (1 << 15)
#define CMD_FR    (1 << 14)

/* SSTS bits */
#define SSTS_DET_SHIFT 0
#define SSTS_DET_MASK  0xF
#define DET_PHY_EST    0x3

/* Port interrupt status bits */
#define PORT_IS_DHRS  (1 << 0)
#define PORT_IS_PSS   (1 << 1)
#define PORT_IS_DSS   (1 << 2)
#define PORT_IS_SDBS  (1 << 3)
#define PORT_IS_UFS   (1 << 4)
#define PORT_IS_DPS   (1 << 5)
#define PORT_IS_PCS   (1 << 6)
#define PORT_IS_DMPS  (1 << 7)
#define PORT_IS_PRCS  (1 << 22)
#define PORT_IS_IPMS  (1 << 23)
#define PORT_IS_OFS   (1 << 24)
#define PORT_IS_INFS  (1 << 26)
#define PORT_IS_IFS   (1 << 27)
#define PORT_IS_HBDS  (1 << 28)
#define PORT_IS_HBFS  (1 << 29)
#define PORT_IS_TFES  (1 << 30)
#define PORT_IS_CPDS  (1U << 31)

/* SERR clear */
#define SERR_CLEAR 0xFFFFFFFF

/* TFD bits */
#define TFD_BSY (1 << 7)
#define TFD_DRQ (1 << 3)
#define TFD_ERR (1 << 0)

/* ATA DMA commands */
#define ATA_CMD_READ_DMA   0x25
#define ATA_CMD_WRITE_DMA  0x35
#define ATA_CMD_FLUSH      0xE7

/* Timeout loops */
#define AHCI_TIMEOUT 10000000

/* Max sectors per DMA transfer (fits in 2-page bounce buffer) */
#define MAX_DMA_SECTORS 16


/* Command header (32 bytes) */
typedef volatile struct {
    uint32_t dw0;
    uint32_t dw1;
    uint32_t dw2;
    uint32_t dw3;
    uint32_t reserved[4];
} __attribute__((packed)) ahci_cmd_header_t;

/* H2D Register FIS */
typedef volatile struct {
    uint8_t  fis_type;     /* 0x27 */
    uint8_t  flags;
    uint8_t  command;
    uint8_t  features_low;
    uint8_t  lba0;
    uint8_t  lba1;
    uint8_t  lba2;
    uint8_t  device;
    uint8_t  lba3;
    uint8_t  lba4;
    uint8_t  lba5;
    uint8_t  features_high;
    uint8_t  count_low;
    uint8_t  count_high;
    uint8_t  icc;
    uint8_t  control;
    uint32_t reserved;
} __attribute__((packed)) fis_h2d_t;

/* PRDT entry */
typedef volatile struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc;
} __attribute__((packed)) ahci_prdt_entry_t;

/* Command table (256 bytes + PRDT entries) */
typedef volatile struct {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  reserved[0x80 - 0x50];
    ahci_prdt_entry_t prdt[1];
} __attribute__((packed)) ahci_cmd_table_t;

/* CMD header helpers */
#define CMD_HDR_CFL(n)     ((n) & 0x1F)
#define CMD_HDR_WRITE      (1 << 6)
#define CMD_HDR_PRDTL(n)   (((n) & 0xFFFF) << 16)

/* FIS helpers */
#define FIS_H2D_FLAG_C     (1 << 7)

/* PRDT helpers */
#define PRDT_DBC(n)        ((n) & 0x3FFFFF)
#define PRDT_IOC           (1U << 31)


typedef struct {
    int             present;
    uint64_t        abar_phys;
    volatile uint8_t *abar;
    int             port_count;
    int             active_port;
    uint64_t        total_sectors;
    char            model[41];
    block_dev_t     bdev;
} ahci_device_t;

static ahci_device_t ahci_dev;

/* DMA buffer physical addresses */
static uint64_t bounce_phys;
static uint64_t cmdlist_phys;
static uint64_t rfis_phys;
static uint64_t cmdtable_phys;


static inline uint32_t reg_read32(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static inline void reg_write32(volatile uint8_t *base, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(base + off) = v;
}

static inline void reg_or32(volatile uint8_t *base, uint32_t off, uint32_t v) {
    reg_write32(base, off, reg_read32(base, off) | v);
}

static inline void reg_and32(volatile uint8_t *base, uint32_t off, uint32_t v) {
    reg_write32(base, off, reg_read32(base, off) & v);
}

static inline volatile uint8_t *port_base(int port) {
    return ahci_dev.abar + 0x100 + port * 0x80;
}


static void ahci_map_phys(uint64_t phys, uint64_t virt, int pages, uint32_t flags) {
    page_directory_t *kd = vmm_get_kernel_directory();
    for (int i = 0; i < pages; i++) {
        if (vmm_map_page(kd, phys + i * PAGE_SIZE, virt + i * PAGE_SIZE, flags) < 0) {
            log_print(LOG_LEVEL_ERROR, "ahci: vmm_map_page failed\r\n");
        }
    }
}


static int ahci_hba_reset(volatile uint8_t *abar) {
    reg_or32(abar, HBA_GHC, GHC_HR);
    int timeout = AHCI_TIMEOUT;
    while (reg_read32(abar, HBA_GHC) & GHC_HR) {
        if (--timeout == 0) return -1;
    }
    reg_or32(abar, HBA_GHC, GHC_AE);
    return 0;
}


static void ahci_port_stop(volatile uint8_t *p) {
    reg_and32(p, PORT_CMD, ~(CMD_ST | CMD_FRE));
    int timeout = AHCI_TIMEOUT;
    while (reg_read32(p, PORT_CMD) & (CMD_CR | CMD_FR)) {
        if (--timeout == 0) break;
    }
}

static void ahci_port_start(volatile uint8_t *p) {
    reg_write32(p, PORT_SERR, SERR_CLEAR);
    reg_or32(p, PORT_CMD, CMD_FRE);
    reg_or32(p, PORT_CMD, CMD_ST);
}

static int ahci_port_init(volatile uint8_t *p, int port_num) {
    ahci_port_stop(p);

    reg_write32(p, PORT_CLB, (uint32_t)cmdlist_phys);
    reg_write32(p, PORT_CLBU, (uint32_t)(cmdlist_phys >> 32));
    reg_write32(p, PORT_FB, (uint32_t)rfis_phys);
    reg_write32(p, PORT_FBU, (uint32_t)(rfis_phys >> 32));
    reg_write32(p, PORT_IS, 0xFFFFFFFF);

    ahci_port_start(p);

    uint32_t ssts = reg_read32(p, PORT_SSTS);
    log_printf(LOG_LEVEL_DEBUG, "ahci: port %d ssts=0x%x det=%u\r\n",
                 port_num, ssts, ssts & SSTS_DET_MASK);

    if ((ssts & SSTS_DET_MASK) != DET_PHY_EST) {
        log_print(LOG_LEVEL_DEBUG, "ahci: no device on port\r\n");
        return -1;
    }

    uint32_t sig = reg_read32(p, PORT_SIG);
    if (sig != 0x00000101) {
        log_printf(LOG_LEVEL_WARN, "ahci: non-ATA signature 0x%x\r\n", sig);
        return -1;
    }

    reg_write32(p, PORT_SERR, SERR_CLEAR);
    return 0;
}


static int ahci_identify(volatile uint8_t *p) {
    ahci_cmd_header_t *clb = (ahci_cmd_header_t *)CMDLIST_VADDR;
    memset((void *)clb, 0, 8 * sizeof(uint32_t));

    clb[0].dw0 = CMD_HDR_CFL(5) | CMD_HDR_PRDTL(1);
    clb[0].dw2 = (uint32_t)cmdtable_phys;
    clb[0].dw3 = (uint32_t)(cmdtable_phys >> 32);

    ahci_cmd_table_t *ct = (ahci_cmd_table_t *)CMDTABLE_VADDR;
    memset((void *)ct, 0, sizeof(ahci_cmd_table_t));

    fis_h2d_t *fis = (fis_h2d_t *)ct->cfis;
    fis->fis_type = 0x27;
    fis->flags    = FIS_H2D_FLAG_C;
    fis->command  = 0xEC;   /* IDENTIFY DEVICE */
    fis->device   = 0;

    ct->prdt[0].dba  = (uint32_t)bounce_phys;
    ct->prdt[0].dbau = (uint32_t)(bounce_phys >> 32);
    ct->prdt[0].dbc  = PRDT_DBC(511) | PRDT_IOC;

    reg_write32(p, PORT_IS, 0xFFFFFFFF);
    reg_write32(p, PORT_CI, 1);

    int timeout = AHCI_TIMEOUT;
    while (reg_read32(p, PORT_CI) & 1) {
        if (--timeout == 0) {
            log_print(LOG_LEVEL_ERROR, "ahci: identify timeout\r\n");
            return -1;
        }
    }

    uint32_t is = reg_read32(p, PORT_IS);
    if (is & (PORT_IS_TFES | PORT_IS_HBFS | PORT_IS_HBDS |
              PORT_IS_IFS | PORT_IS_INFS | PORT_IS_OFS)) {
        log_printf(LOG_LEVEL_ERROR, "ahci: identify error is=0x%x tfd=0x%x\r\n",
                     is, reg_read32(p, PORT_TFD));
        reg_write32(p, PORT_IS, 0xFFFFFFFF);
        return -1;
    }

    if (reg_read32(p, PORT_TFD) & TFD_ERR) {
        log_print(LOG_LEVEL_ERROR, "ahci: identify TFD error\r\n");
        reg_write32(p, PORT_IS, 0xFFFFFFFF);
        return -1;
    }

    reg_write32(p, PORT_IS, 0xFFFFFFFF);

    /* Parse IDENTIFY data in bounce buffer */
    uint16_t *id = (uint16_t *)BOUNCE_VADDR;

    ahci_dev.total_sectors = (uint64_t)id[61] << 48 |
                             (uint64_t)id[60] << 32 |
                             (uint64_t)id[61] << 16 |
                             (uint64_t)id[60];
    /* ATA identify word 60 = LBA low, 61 = LBA high (32-bit total) */
    ahci_dev.total_sectors = (uint32_t)id[61] << 16 | id[60];

    for (int i = 0; i < 40; i += 2) {
        ahci_dev.model[i]     = (uint8_t)(id[27 + i/2] >> 8);
        ahci_dev.model[i + 1] = (uint8_t)(id[27 + i/2] & 0xFF);
    }
    ahci_dev.model[40] = '\0';

    log_printf(LOG_LEVEL_INFO, "ahci: model='%s' sectors=%lx\r\n",
                 ahci_dev.model, ahci_dev.total_sectors);
    return 0;
}


static int ahci_dma_transfer(uint32_t lba, int count, uint64_t buf_phys, int is_write) {
    if (count <= 0 || count > 256) return -1;
    if (!ahci_dev.present) return -1;

    volatile uint8_t *p = port_base(ahci_dev.active_port);

    int timeout = AHCI_TIMEOUT;
    while (reg_read32(p, PORT_TFD) & (TFD_BSY | TFD_DRQ)) {
        if (--timeout == 0) return -1;
    }

    ahci_cmd_header_t *clb = (ahci_cmd_header_t *)CMDLIST_VADDR;
    memset((void *)clb, 0, 8 * sizeof(uint32_t));

    int prdtl = 1;
    clb[0].dw0 = CMD_HDR_CFL(5) | (is_write ? CMD_HDR_WRITE : 0) | CMD_HDR_PRDTL(prdtl);
    clb[0].dw2 = (uint32_t)cmdtable_phys;
    clb[0].dw3 = (uint32_t)(cmdtable_phys >> 32);

    ahci_cmd_table_t *ct = (ahci_cmd_table_t *)CMDTABLE_VADDR;
    memset((void *)ct, 0, sizeof(ahci_cmd_table_t));

    fis_h2d_t *fis = (fis_h2d_t *)ct->cfis;
    fis->fis_type  = 0x27;
    fis->flags     = FIS_H2D_FLAG_C;
    fis->command   = is_write ? ATA_CMD_WRITE_DMA : ATA_CMD_READ_DMA;
    fis->device    = 0x40 | ((lba >> 24) & 0x0F);

    fis->lba0 = (uint8_t)(lba);
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = 0;
    fis->lba5 = 0;

    uint8_t scount = (count == 256) ? 0 : (uint8_t)count;
    fis->count_low  = scount;
    fis->count_high = 0;

    ct->prdt[0].dba  = (uint32_t)buf_phys;
    ct->prdt[0].dbau = (uint32_t)(buf_phys >> 32);
    ct->prdt[0].dbc  = PRDT_DBC(count * 512 - 1) | PRDT_IOC;

    reg_write32(p, PORT_IS, 0xFFFFFFFF);
    reg_write32(p, PORT_CI, 1);

    timeout = AHCI_TIMEOUT;
    while (reg_read32(p, PORT_CI) & 1) {
        if (--timeout == 0) return -1;
    }

    uint32_t is = reg_read32(p, PORT_IS);
    if (is & (PORT_IS_TFES | PORT_IS_HBFS | PORT_IS_HBDS |
              PORT_IS_IFS | PORT_IS_INFS | PORT_IS_OFS)) {
        reg_write32(p, PORT_IS, 0xFFFFFFFF);
        return -1;
    }

    if (reg_read32(p, PORT_TFD) & TFD_ERR) {
        reg_write32(p, PORT_IS, 0xFFFFFFFF);
        return -1;
    }

    reg_write32(p, PORT_IS, 0xFFFFFFFF);
    return count * AHCI_SECTOR_SIZE;
}


static int ahci_read_sectors(uint32_t lba, int count, void *buf) {
    if (!ahci_dev.present) return -1;
    if (count * AHCI_SECTOR_SIZE > MAX_DMA_SECTORS * AHCI_SECTOR_SIZE) return -1;

    int ret = ahci_dma_transfer(lba, count, bounce_phys, 0);
    if (ret < 0) return ret;

    uint8_t *src = (uint8_t *)BOUNCE_VADDR;
    uint8_t *dst = (uint8_t *)buf;
    for (int i = 0; i < count * AHCI_SECTOR_SIZE; i++)
        dst[i] = src[i];

    return ret;
}

static int ahci_write_sectors(uint32_t lba, int count, const void *buf) {
    if (!ahci_dev.present) return -1;
    if (count * AHCI_SECTOR_SIZE > MAX_DMA_SECTORS * AHCI_SECTOR_SIZE) return -1;

    uint8_t *dst = (uint8_t *)BOUNCE_VADDR;
    const uint8_t *src = (const uint8_t *)buf;
    for (int i = 0; i < count * AHCI_SECTOR_SIZE; i++)
        dst[i] = src[i];

    return ahci_dma_transfer(lba, count, bounce_phys, 1);
}


static int ahci_block_read(block_dev_t *dev, uint64_t lba, void *buf, size_t count) {
    (void)dev;
    uint8_t *ptr = (uint8_t *)buf;

    while (count > 0) {
        size_t batch = (count > MAX_DMA_SECTORS) ? MAX_DMA_SECTORS : count;
        if (ahci_read_sectors((uint32_t)lba, (int)batch, ptr) < 0)
            return -1;
        lba += batch;
        ptr += batch * AHCI_SECTOR_SIZE;
        count -= batch;
    }
    return 0;
}

static int ahci_block_write(block_dev_t *dev, uint64_t lba, const void *buf, size_t count) {
    (void)dev;
    const uint8_t *ptr = (const uint8_t *)buf;

    while (count > 0) {
        size_t batch = (count > MAX_DMA_SECTORS) ? MAX_DMA_SECTORS : count;
        if (ahci_write_sectors((uint32_t)lba, (int)batch, ptr) < 0)
            return -1;
        lba += batch;
        ptr += batch * AHCI_SECTOR_SIZE;
        count -= batch;
    }
    return 0;
}


static int ahci_pci_probe(struct arc_device *adev) {
    pci_device_t *dev = (pci_device_t *)adev->priv;
    log_printf(LOG_LEVEL_DEBUG, "ahci: probing %s (%02x:%02x.%x prog_if=0x%x)\r\n",
                 adev->name, dev->addr.bus, dev->addr.slot, dev->addr.func, dev->prog_if);

    /* Only AHCI controllers (prog_if == 0x01) */
    if (dev->prog_if != 0x01) {
        log_print(LOG_LEVEL_WARN, "ahci: not AHCI (prog_if != 0x01)\r\n");
        return -1;
    }

    uint64_t abar = PCI_BAR_ADDR_MEM(dev->bars[5]); /* BAR5 = ABAR */
    if (!abar) {
        log_print(LOG_LEVEL_WARN, "ahci: ABAR (BAR5) is zero\r\n");
        return -1;
    }

    log_printf(LOG_LEVEL_DEBUG, "ahci: ABAR at 0x%lx\r\n", abar);

    /* Enable bus mastering + memory space */
    uint32_t cmd = pci_config_read(dev->addr, PCI_COMMAND);
    pci_config_write(dev->addr, PCI_COMMAND, cmd | 0x06);

    ahci_dev.abar_phys = abar;
    ahci_dev.present   = 0;
    ahci_dev.active_port = -1;

    /* Allocate DMA-safe physical memory */
    bounce_phys   = (uint64_t)(uintptr_t)pmm_alloc_pages(2);
    cmdlist_phys  = (uint64_t)(uintptr_t)pmm_alloc_page();
    rfis_phys     = (uint64_t)(uintptr_t)pmm_alloc_page();
    cmdtable_phys = (uint64_t)(uintptr_t)pmm_alloc_page();

    if (!bounce_phys || !cmdlist_phys || !rfis_phys || !cmdtable_phys) {
        log_print(LOG_LEVEL_ERROR, "ahci: DMA alloc failed\r\n");
        return -1;
    }

    /* Map MMIO uncacheable, DMA buffers write-back */
    ahci_map_phys(abar, ABAR_VADDR, 8, VMM_PRESENT | VMM_WRITABLE | VMM_CACHE_DISABLE);
    ahci_map_phys(bounce_phys, BOUNCE_VADDR, 2, VMM_PRESENT | VMM_WRITABLE);
    ahci_map_phys(cmdlist_phys, CMDLIST_VADDR, 1, VMM_PRESENT | VMM_WRITABLE);
    ahci_map_phys(rfis_phys, RFIS_VADDR, 1, VMM_PRESENT | VMM_WRITABLE);
    ahci_map_phys(cmdtable_phys, CMDTABLE_VADDR, 1, VMM_PRESENT | VMM_WRITABLE);

    ahci_dev.abar = (volatile uint8_t *)ABAR_VADDR;

    /* Reset HBA and enable AHCI */
    if (ahci_hba_reset(ahci_dev.abar) < 0) {
        log_print(LOG_LEVEL_ERROR, "ahci: HBA reset failed\r\n");
        return -1;
    }

    /* Check port map */
    uint32_t cap = reg_read32(ahci_dev.abar, HBA_CAP);
    int np = (cap & CAP_NP_MASK) + 1;
    uint32_t pi = reg_read32(ahci_dev.abar, HBA_PI);

    for (int i = 0; i < np && i < 32; i++) {
        if (!(pi & (1 << i))) continue;
        if (ahci_port_init(port_base(i), i) < 0) continue;

        if (ahci_identify(port_base(i)) < 0) {
            ahci_port_stop(port_base(i));
            continue;
        }

        ahci_dev.active_port = i;
        ahci_dev.present     = 1;
        log_printf(LOG_LEVEL_INFO, "ahci: device found on port %d\r\n", i);
        break;
    }

    if (ahci_dev.active_port < 0) {
        log_print(LOG_LEVEL_WARN, "ahci: no usable device found\r\n");
        return -1;
    }

    /* Register block device */
    block_dev_t *bdev = &ahci_dev.bdev;
    memset(bdev, 0, sizeof(block_dev_t));
    bdev->name[0] = 'a'; bdev->name[1] = 'h'; bdev->name[2] = 'c';
    bdev->name[3] = 'i'; bdev->name[4] = '0'; bdev->name[5] = '\0';
    bdev->block_size  = AHCI_SECTOR_SIZE;
    bdev->num_blocks  = ahci_dev.total_sectors;
    bdev->read   = ahci_block_read;
    bdev->write  = ahci_block_write;
    bdev->priv   = &ahci_dev;
    block_dev_register(bdev);

    log_print(LOG_LEVEL_INFO, "ahci: init OK\r\n");
    return 0;
}


static const struct arc_device_id ahci_ids[] = {
    { .class = PCI_CLASS_MASS_STORAGE, .subclass = PCI_SUBCLASS_AHCI },
    ARC_DEVICE_ID_END
};

static struct arc_driver ahci_driver = {
    .name     = "ahci",
    .type     = ARC_DEV_PCI,
    .id_table = ahci_ids,
    .probe    = ahci_pci_probe,
};


void ahci_init(void) {
    log_print(LOG_LEVEL_DEBUG, "ahci: registering driver\r\n");
    arc_driver_register(&ahci_driver);
}

