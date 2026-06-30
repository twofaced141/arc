#include "ahci.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "debug.h"
#include "idt.h"
#include "panic.h"

/* Memory-mapped virtual address for ABAR and DMA buffers */
#define AHCI_MMIO_BASE  0xFFB00000
#define ABAR_VADDR      (AHCI_MMIO_BASE)
#define BOUNCE_VADDR    (AHCI_MMIO_BASE + 0x10000)
#define CMDLIST_VADDR   (AHCI_MMIO_BASE + 0x11000)
#define RFIS_VADDR      (AHCI_MMIO_BASE + 0x12000)
#define CMDTABLE_VADDR  (AHCI_MMIO_BASE + 0x13000)

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
#define CMD_ICC_MASK (0xF << 28)
#define CMD_ASP    (1 << 27)
#define CMD_ALPE   (1 << 26)
#define CMD_DLAE   (1 << 25)
#define CMD_ATAPI  (1 << 24)
#define CMD_ESP    (1 << 21)
#define CMD_ESPMS  (1 << 22)
#define CMD_MPSP   (1 << 23)
#define CMD_HPCP   (1 << 18)
#define CMD_PMA    (1 << 17)
#define CMD_CPD    (1 << 16)
#define CMD_ICC_SHIFT 28

/* SSTS bits */
#define SSTS_DET_SHIFT 0
#define SSTS_DET_MASK  0xF
#define DET_NO_DEVICE    0
#define DET_PHY_EST      0x3

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

/* SERR bits to clear */
#define SERR_CLEAR 0xFFFFFFFF

/* TFD bits */
#define TFD_BSY (1 << 7)
#define TFD_DRQ (1 << 3)
#define TFD_ERR (1 << 0)

/* Command header */
typedef volatile struct {
    uint32_t dw0;
    uint32_t dw1;
    uint32_t dw2;
    uint32_t dw3;
    uint32_t reserved[4];
} __attribute__((packed)) ahci_cmd_header_t;

/* H2D Register FIS */
typedef volatile struct {
    uint8_t fis_type;
    uint8_t flags;
    uint8_t command;
    uint8_t features_low;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t features_high;
    uint8_t count_low;
    uint8_t count_high;
    uint8_t icc;
    uint8_t control;
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
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[0x80 - 0x50];
    ahci_prdt_entry_t prdt[1];
} __attribute__((packed)) ahci_cmd_table_t;

/* CMD header helpers */
#define CMD_HDR_CFL(n)     ((n) & 0x1F)
#define CMD_HDR_ATAPI      (1 << 5)
#define CMD_HDR_WRITE      (1 << 6)
#define CMD_HDR_PRDTL(n)   (((n) & 0xFFFF) << 16)

/* FIS helpers */
#define FIS_H2D_FLAG_C     (1 << 7)

/* PRDT helpers */
#define PRDT_DBC(n)        ((n) & 0x3FFFFF)
#define PRDT_IOC           (1U << 31)

/* ATA DMA commands */
#define ATA_CMD_READ_DMA   0x25
#define ATA_CMD_WRITE_DMA  0x35
#define ATA_CMD_FLUSH      0xE7

/* Timeouts */
#define AHCI_TIMEOUT 10000000

/* Device info */
typedef struct {
    int present;
    uint32_t abar_phys;
    volatile uint8_t *abar;
    int port_count;
    int active_port;
    uint64_t total_sectors;
    char model[41];
} ahci_device_t;

static ahci_device_t ahci_dev;

/* DMA buffer physical addresses (set during init) */
static uint32_t bounce_phys;
static uint32_t cmdlist_phys;
static uint32_t rfis_phys;
static uint32_t cmdtable_phys;

static uint32_t reg_read32(volatile uint8_t *base, uint32_t offset) {
    return *(volatile uint32_t *)(base + offset);
}

static void reg_write32(volatile uint8_t *base, uint32_t offset, uint32_t val) {
    *(volatile uint32_t *)(base + offset) = val;
}

static void reg_or32(volatile uint8_t *base, uint32_t offset, uint32_t val) {
    reg_write32(base, offset, reg_read32(base, offset) | val);
}

static void reg_and32(volatile uint8_t *base, uint32_t offset, uint32_t val) {
    reg_write32(base, offset, reg_read32(base, offset) & val);
}

static volatile uint8_t *port_base(int port) {
    return ahci_dev.abar + 0x100 + port * 0x80;
}

static void ahci_map_phys(uint32_t phys, uint32_t virt, int pages) {
    page_directory_t *kd = vmm_get_kernel_directory();
    for (int i = 0; i < pages; i++) {
        if (vmm_map_page(kd, phys + i * 4096, virt + i * 4096,
                         VMM_PRESENT | VMM_WRITABLE | VMM_CACHE_DISABLE) < 0)
            panic_simple("ahci: vmm_map_page failed");
    }
}

static void ahci_map_phys_wb(uint32_t phys, uint32_t virt, int pages) {
    page_directory_t *kd = vmm_get_kernel_directory();
    for (int i = 0; i < pages; i++) {
        if (vmm_map_page(kd, phys + i * 4096, virt + i * 4096,
                         VMM_PRESENT | VMM_WRITABLE) < 0)
            panic_simple("ahci: vmm_map_page (wb) failed");
    }
}

static int ahci_pci_find_abar(uint32_t *abar_phys_out) {
    for (uint8_t bus = 0; bus < 255; bus++) {
        uint32_t id0 = pci_config_read(bus, 0, 0, 0);
        if ((id0 & 0xFFFF) == 0xFFFF) {
            if (bus == 0) continue;
            break;
        }
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id = pci_config_read(bus, slot, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF) continue;

            uint32_t hdr = pci_config_read(bus, slot, 0, 0x0C);
            uint8_t max_func = (hdr & 0x800000) ? 8 : 1;

            for (uint8_t func = 0; func < max_func; func++) {
                uint32_t dev_id = pci_config_read(bus, slot, func, 0);
                if ((dev_id & 0xFFFF) == 0xFFFF) continue;

                uint32_t class_rev = pci_config_read(bus, slot, func, 8);
                uint8_t cls = class_rev >> 24;
                uint8_t sub = (class_rev >> 16) & 0xFF;
                uint8_t prog_if = (class_rev >> 8) & 0xFF;

                /* Mass storage controller / SATA / AHCI */
                if (cls == 0x01 && sub == 0x06 && prog_if == 0x01) {
                    uint32_t bar5 = pci_config_read(bus, slot, func, 0x24);
                    uint32_t abar = bar5 & ~0xF;
                    debug_printf("ahci: found at %x:%x.%d bar5=0x%x\r\n", bus, slot, func, abar);

                    /* Enable bus mastering and memory space */
                    uint32_t cmd = pci_config_read(bus, slot, func, 0x04);
                    pci_config_write(bus, slot, func, 0x04, cmd | 0x06);

                    *abar_phys_out = abar;
                    return 0;
                }

                /* Track PCI-to-PCI bridges for secondary bus scan */
                if (cls == 0x06 && sub == 0x04 && func == 0) {
                    uint32_t bus_reg = pci_config_read(bus, slot, 0, 0x18);
                    uint8_t secondary = (bus_reg >> 8) & 0xFF;
                    if (secondary != bus && secondary != 0 && secondary > bus) {
                        /* Will be scanned by outer loop */
                    }
                }
            }
        }
    }
    return -1;
}

static int ahci_hba_reset(volatile uint8_t *abar) {
    /* Set HBA reset */
    reg_or32(abar, HBA_GHC, GHC_HR);
    int timeout = AHCI_TIMEOUT;
    while (reg_read32(abar, HBA_GHC) & GHC_HR) {
        if (--timeout == 0) return -1;
    }

    /* Enable AHCI mode */
    reg_or32(abar, HBA_GHC, GHC_AE);
    return 0;
}

static void ahci_port_stop(volatile uint8_t *p) {
    /* Clear ST and FRE */
    reg_and32(p, PORT_CMD, ~(CMD_ST | CMD_FRE));

    /* Wait for CR and FR to clear */
    int timeout = AHCI_TIMEOUT;
    while (reg_read32(p, PORT_CMD) & (CMD_CR | CMD_FR)) {
        if (--timeout == 0) break;
    }
}

static void ahci_port_start(volatile uint8_t *p) {
    /* Clear SERR */
    reg_write32(p, PORT_SERR, SERR_CLEAR);

    /* Set FRE (FIS Receive Enable) */
    reg_or32(p, PORT_CMD, CMD_FRE);

    /* Set ST (Start) */
    reg_or32(p, PORT_CMD, CMD_ST);
}

static int ahci_port_init(volatile uint8_t *p, int port_num) {
    /* Stop port */
    ahci_port_stop(p);

    /* Set command list base */
    reg_write32(p, PORT_CLB, cmdlist_phys);
    reg_write32(p, PORT_CLBU, 0);

    /* Set received FIS base */
    reg_write32(p, PORT_FB, rfis_phys);
    reg_write32(p, PORT_FBU, 0);

    /* Clear interrupt status */
    reg_write32(p, PORT_IS, 0xFFFFFFFF);

    /* Start port */
    ahci_port_start(p);

    debug_printf("ahci: port %d ssts=", port_num);
    debug_print_hex8(reg_read32(p, PORT_SSTS) & 0xF);
    debug_print("\r\n");

    /* Check if device is present and PHY is established */
    uint32_t ssts = reg_read32(p, PORT_SSTS);
    if ((ssts & SSTS_DET_MASK) != DET_PHY_EST) {
        debug_print("ahci: no device on port\r\n");
        return -1;
    }

    /* Check signature (0x00000101 = ATA) */
    uint32_t sig = reg_read32(p, PORT_SIG);
    if (sig != 0x00000101) {
        debug_printf("ahci: non-ATA signature 0x%x\r\n", sig);
        return -1;
    }

    /* Clear SERR */
    reg_write32(p, PORT_SERR, SERR_CLEAR);

    return 0;
}

static int ahci_identify_device(volatile uint8_t *p) {
    /* Clear command header slot 0 */
    ahci_cmd_header_t *clb = (ahci_cmd_header_t *)CMDLIST_VADDR;
    for (int i = 0; i < 8; i++)
        ((volatile uint32_t *)&clb[0])[i] = 0;

    /* Setup command header */
    clb[0].dw0 = CMD_HDR_CFL(5) | CMD_HDR_PRDTL(1);
    clb[0].dw2 = cmdtable_phys;
    clb[0].dw3 = 0;

    /* Clear command table */
    ahci_cmd_table_t *ct = (ahci_cmd_table_t *)CMDTABLE_VADDR;
    for (int i = 0; i < (int)sizeof(ahci_cmd_table_t); i++)
        ((volatile uint8_t *)ct)[i] = 0;

    /* Setup FIS: IDENTIFY DEVICE */
    fis_h2d_t *fis = (fis_h2d_t *)ct->cfis;
    fis->fis_type = 0x27;
    fis->flags = FIS_H2D_FLAG_C;
    fis->command = 0xEC;
    fis->device = 0x00;

    /* Setup PRDT: point to bounce buffer */
    ct->prdt[0].dba = bounce_phys;
    ct->prdt[0].dbau = 0;
    ct->prdt[0].dbc = PRDT_DBC(511) | PRDT_IOC;

    /* Clear interrupt status */
    reg_write32(p, PORT_IS, 0xFFFFFFFF);

    /* Issue command */
    reg_write32(p, PORT_CI, 1);

    /* Wait for completion */
    int timeout = AHCI_TIMEOUT;
    while (reg_read32(p, PORT_CI) & 1) {
        if (--timeout == 0) {
            debug_print("ahci: identify timeout\r\n");
            return -1;
        }
    }

    /* Check for errors */
    uint32_t is = reg_read32(p, PORT_IS);
    if (is & (PORT_IS_TFES | PORT_IS_HBFS | PORT_IS_HBDS | PORT_IS_IFS | PORT_IS_INFS | PORT_IS_OFS)) {
        debug_printf("ahci: identify error is=0x%x tfd=0x%x\r\n", is, reg_read32(p, PORT_TFD));
        reg_write32(p, PORT_IS, 0xFFFFFFFF);
        return -1;
    }

    /* Check TFD error */
    if (reg_read32(p, PORT_TFD) & TFD_ERR) {
        debug_print("ahci: identify TFD error\r\n");
        reg_write32(p, PORT_IS, 0xFFFFFFFF);
        return -1;
    }

    reg_write32(p, PORT_IS, 0xFFFFFFFF);

    /* Parse identify data */
    uint16_t *id = (uint16_t *)BOUNCE_VADDR;

    ahci_dev.total_sectors = (uint32_t)id[61] << 16 | id[60];

    for (int i = 0; i < 40; i += 2) {
        ahci_dev.model[i] = id[27 + i/2] >> 8;
        ahci_dev.model[i + 1] = id[27 + i/2] & 0xFF;
    }
    ahci_dev.model[40] = '\0';

    debug_printf("ahci: model='%s' sectors=%u\r\n", ahci_dev.model, (uint32_t)ahci_dev.total_sectors);
    return 0;
}

static int ahci_dma_transfer(uint32_t lba, int count, uint32_t buf_phys, int is_write) {
    if (count <= 0 || count > 256) return -1;
    if (!ahci_dev.present) return -1;

    volatile uint8_t *p = port_base(ahci_dev.active_port);

    /* Wait for BSY and DRQ to clear */
    int timeout = AHCI_TIMEOUT;
    while (reg_read32(p, PORT_TFD) & (TFD_BSY | TFD_DRQ)) {
        if (--timeout == 0) return -1;
    }

    /* Clear command header */
    ahci_cmd_header_t *clb = (ahci_cmd_header_t *)CMDLIST_VADDR;
    for (int i = 0; i < 8; i++)
        ((volatile uint32_t *)&clb[0])[i] = 0;

    /* Setup command header */
    int prdtl = 1;

    clb[0].dw0 = CMD_HDR_CFL(5) | (is_write ? CMD_HDR_WRITE : 0) | CMD_HDR_PRDTL(prdtl);
    clb[0].dw2 = cmdtable_phys;
    clb[0].dw3 = 0;

    /* Clear command table */
    ahci_cmd_table_t *ct = (ahci_cmd_table_t *)CMDTABLE_VADDR;
    for (int i = 0; i < (int)sizeof(ahci_cmd_table_t); i++)
        ((volatile uint8_t *)ct)[i] = 0;

    /* Setup FIS */
    fis_h2d_t *fis = (fis_h2d_t *)ct->cfis;
    fis->fis_type = 0x27;
    fis->flags = FIS_H2D_FLAG_C;
    fis->command = is_write ? ATA_CMD_WRITE_DMA : ATA_CMD_READ_DMA;
    fis->device = 0x40 | ((lba >> 24) & 0x0F);

    fis->lba0 = lba & 0xFF;
    fis->lba1 = (lba >> 8) & 0xFF;
    fis->lba2 = (lba >> 16) & 0xFF;
    fis->lba3 = (lba >> 24) & 0xFF;
    fis->lba4 = 0;
    fis->lba5 = 0;

    uint8_t scount = (count == 256) ? 0 : (uint8_t)count;
    fis->count_low = scount;
    fis->count_high = 0;

    /* Setup PRDT */
    ct->prdt[0].dba = buf_phys;
    ct->prdt[0].dbau = 0;
    ct->prdt[0].dbc = PRDT_DBC(count * 512 - 1) | PRDT_IOC;

    /* Clear interrupt status */
    reg_write32(p, PORT_IS, 0xFFFFFFFF);

    /* Issue command */
    reg_write32(p, PORT_CI, 1);

    /* Wait for completion */
    timeout = AHCI_TIMEOUT;
    while (reg_read32(p, PORT_CI) & 1) {
        if (--timeout == 0) return -1;
    }

    /* Check for errors */
    uint32_t is = reg_read32(p, PORT_IS);
    if (is & (PORT_IS_TFES | PORT_IS_HBFS | PORT_IS_HBDS | PORT_IS_IFS | PORT_IS_INFS | PORT_IS_OFS)) {
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

int ahci_init(void) {
    ahci_dev.present = 0;

    uint32_t abar_phys;
    if (ahci_pci_find_abar(&abar_phys) < 0) {
        debug_print("ahci: controller not found\r\n");
        return -1;
    }

    ahci_dev.abar_phys = abar_phys;

    /* Allocate DMA-safe physical memory */
    bounce_phys = (uint32_t)pmm_alloc_pages(2);
    cmdlist_phys = (uint32_t)pmm_alloc_page();
    rfis_phys = (uint32_t)pmm_alloc_page();
    cmdtable_phys = (uint32_t)pmm_alloc_page();
    if (!bounce_phys || !cmdlist_phys || !rfis_phys || !cmdtable_phys) {
        debug_print("ahci: DMA alloc failed\r\n");
        return -1;
    }

    debug_printf("ahci: bounce=0x%x clb=0x%x fb=0x%x ct=0x%x\r\n",
                 bounce_phys, cmdlist_phys, rfis_phys, cmdtable_phys);

    /* Map MMIO and DMA buffers into virtual address space */
    ahci_map_phys(abar_phys, ABAR_VADDR, 8);
    ahci_map_phys_wb(bounce_phys, BOUNCE_VADDR, 2);
    ahci_map_phys_wb(cmdlist_phys, CMDLIST_VADDR, 1);
    ahci_map_phys_wb(rfis_phys, RFIS_VADDR, 1);
    ahci_map_phys_wb(cmdtable_phys, CMDTABLE_VADDR, 1);

    ahci_dev.abar = (volatile uint8_t *)ABAR_VADDR;

    /* Reset HBA and enable AHCI */
    if (ahci_hba_reset(ahci_dev.abar) < 0) {
        debug_print("ahci: HBA reset failed\r\n");
        return -1;
    }

    /* Find implemented ports */
    uint32_t pi = reg_read32(ahci_dev.abar, HBA_PI);
    debug_printf("ahci: ports implemented=0x%x\r\n", pi);

    uint32_t cap = reg_read32(ahci_dev.abar, HBA_CAP);
    int np = (cap & CAP_NP_MASK) + 1;

    debug_printf("ahci: np=%d cap=0x%x\r\n", np, cap);

    ahci_dev.active_port = -1;

    for (int i = 0; i < np && i < 32; i++) {
        if (!(pi & (1 << i))) continue;

        volatile uint8_t *p = port_base(i);
        debug_printf("ahci: trying port %d\r\n", i);

        if (ahci_port_init(p, i) < 0) continue;

        if (ahci_identify_device(p) < 0) {
            ahci_port_stop(p);
            continue;
        }

        ahci_dev.active_port = i;
        ahci_dev.present = 1;
        debug_printf("ahci: device found on port %d\r\n", i);
        break;
    }

    if (ahci_dev.active_port < 0) {
        debug_print("ahci: no usable device found\r\n");
        return -1;
    }

    debug_print("ahci: init OK\r\n");
    return 0;
}

int ahci_read_sectors(uint32_t lba, int count, void *buf) {
    if (!ahci_dev.present) return -1;

    /* Use bounce buffer: DMA into it, then copy to caller */
    int ret = ahci_dma_transfer(lba, count, bounce_phys, 0);
    if (ret < 0) return ret;

    uint8_t *src = (uint8_t *)BOUNCE_VADDR;
    for (int i = 0; i < count * AHCI_SECTOR_SIZE; i++)
        ((uint8_t *)buf)[i] = src[i];

    return ret;
}

int ahci_write_sectors(uint32_t lba, int count, const void *buf) {
    if (!ahci_dev.present) return -1;

    /* Copy caller data to bounce buffer */
    uint8_t *dst = (uint8_t *)BOUNCE_VADDR;
    for (int i = 0; i < count * AHCI_SECTOR_SIZE; i++)
        dst[i] = ((const uint8_t *)buf)[i];

    return ahci_dma_transfer(lba, count, bounce_phys, 1);
}

int ahci_flush_cache(void) {
    if (!ahci_dev.present) return -1;

    volatile uint8_t *p = port_base(ahci_dev.active_port);

    int timeout = AHCI_TIMEOUT;
    while (reg_read32(p, PORT_TFD) & (TFD_BSY | TFD_DRQ)) {
        if (--timeout == 0) return -1;
    }

    ahci_cmd_header_t *clb = (ahci_cmd_header_t *)CMDLIST_VADDR;
    for (int i = 0; i < 8; i++)
        ((volatile uint32_t *)&clb[0])[i] = 0;

    clb[0].dw0 = CMD_HDR_CFL(5) | CMD_HDR_PRDTL(0);
    clb[0].dw2 = cmdtable_phys;
    clb[0].dw3 = 0;

    ahci_cmd_table_t *ct = (ahci_cmd_table_t *)CMDTABLE_VADDR;
    for (int i = 0; i < (int)sizeof(ahci_cmd_table_t); i++)
        ((volatile uint8_t *)ct)[i] = 0;

    fis_h2d_t *fis = (fis_h2d_t *)ct->cfis;
    fis->fis_type = 0x27;
    fis->flags = FIS_H2D_FLAG_C;
    fis->command = ATA_CMD_FLUSH;
    fis->device = 0x40;

    reg_write32(p, PORT_IS, 0xFFFFFFFF);
    reg_write32(p, PORT_CI, 1);

    timeout = AHCI_TIMEOUT;
    while (reg_read32(p, PORT_CI) & 1) {
        if (--timeout == 0) return -1;
    }

    uint32_t is = reg_read32(p, PORT_IS);
    if (is & (PORT_IS_TFES | PORT_IS_HBFS | PORT_IS_HBDS | PORT_IS_IFS | PORT_IS_INFS | PORT_IS_OFS)) {
        reg_write32(p, PORT_IS, 0xFFFFFFFF);
        return -1;
    }

    reg_write32(p, PORT_IS, 0xFFFFFFFF);
    return 0;
}
