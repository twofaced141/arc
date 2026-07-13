#include <stddef.h>
#include "e1000.h"
#include "pci.h"
#include "debug.h"
#include "vmm.h"
#include "pmm.h"

#define E1000_BAR_SIZE  0x20000  /* 128 KB for 82540EM/82545EM/82544GC */
#define E1000_MMIO_VADDR 0xE0000000

static volatile uint8_t *mmio_base;
static uint8_t irq_num;
static int e1000_ok;
static uint8_t e1000_mac[6];

/* TX/RX descriptor rings */
static e1000_rx_desc_t *rx_descs;
static uint8_t *rx_bufs[E1000_NUM_RX_DESC];
static uint32_t rx_cur;

static e1000_tx_desc_t *tx_descs;
static uint8_t *tx_bufs[E1000_NUM_TX_DESC];
static uint32_t tx_cur;

static inline uint32_t e1000_read(uint16_t reg) {
    return *(volatile uint32_t *)(mmio_base + reg);
}

static inline void e1000_write(uint16_t reg, uint32_t val) {
    *(volatile uint32_t *)(mmio_base + reg) = val;
}

int e1000_probe(void) {
    /* Probe all PCI buses for an Intel 8254x */
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t id0 = pci_config_read(bus, slot, 0, 0);
            uint16_t vendor = id0 & 0xFFFF;
            uint16_t device = id0 >> 16;
            if (vendor == E1000_VENDOR_INTEL &&
                (device == E1000_DEV_82540EM ||
                 device == E1000_DEV_82545EM ||
                 device == E1000_DEV_82544GC)) {
                debug_printf("e1000: found at %x:%x.%d (dev %x)\r\n", bus, slot, 0, device);
                uint32_t bar0 = pci_config_read(bus, slot, 0, 0x10);
                uint32_t irq_reg = pci_config_read(bus, slot, 0, 0x3C);
                uint8_t irq = irq_reg & 0xFF;
                uint32_t bar0_phys = bar0 & ~0xF;
                debug_printf("e1000: BAR0=0x%x IRQ=%d\r\n", bar0_phys, irq);

                pci_config_write(bus, slot, 0, 0x04, 0x7);
                return e1000_init(bar0_phys, irq);
            }
        }
    }
    debug_print("e1000: not found\r\n");
    return -1;
}

static int e1000_ring_init(void) {
    /* Allocate RX descriptor ring (phys contiguous) */
    uint32_t rx_ring_pages = (E1000_NUM_RX_DESC * E1000_DESC_SIZE + 4095) / 4096;
    uint32_t rx_ring_phys = 0;
    for (uint32_t i = 0; i < rx_ring_pages; i++) {
        uint32_t p = (uint32_t)pmm_alloc_page();
        if (!p) return -1;
        if (i == 0) rx_ring_phys = p;
    }
    rx_descs = (e1000_rx_desc_t *)vmm_temp_map(rx_ring_phys);
    for (uint32_t i = 0; i < E1000_NUM_RX_DESC; i++) {
        rx_descs[i].addr = 0;
        rx_descs[i].length = 0;
        rx_descs[i].status = 0;
    }
    vmm_temp_unmap();

    /* Allocate RX buffers */
    for (uint32_t i = 0; i < E1000_NUM_RX_DESC; i++) {
        uint32_t phys = (uint32_t)pmm_alloc_page();
        if (!phys) return -1;
        rx_bufs[i] = (uint8_t *)(uint32_t)phys;
        rx_descs = (e1000_rx_desc_t *)vmm_temp_map(rx_ring_phys);
        rx_descs[i].addr = phys;
        rx_descs[i].length = 0;
        rx_descs[i].status = 0;
        vmm_temp_unmap();
    }

    /* Allocate TX descriptor ring */
    uint32_t tx_ring_pages = (E1000_NUM_TX_DESC * E1000_DESC_SIZE + 4095) / 4096;
    uint32_t tx_ring_phys = 0;
    for (uint32_t i = 0; i < tx_ring_pages; i++) {
        uint32_t p = (uint32_t)pmm_alloc_page();
        if (!p) return -1;
        if (i == 0) tx_ring_phys = p;
    }
    tx_descs = (e1000_tx_desc_t *)vmm_temp_map(tx_ring_phys);
    for (uint32_t i = 0; i < E1000_NUM_TX_DESC; i++) {
        tx_descs[i].addr = 0;
        tx_descs[i].cmd = 0;
        tx_descs[i].status = 0;
    }
    vmm_temp_unmap();

    /* Allocate TX buffers */
    for (uint32_t i = 0; i < E1000_NUM_TX_DESC; i++) {
        uint32_t phys = (uint32_t)pmm_alloc_page();
        if (!phys) return -1;
        tx_bufs[i] = (uint8_t *)(uint32_t)phys;
        tx_descs = (e1000_tx_desc_t *)vmm_temp_map(tx_ring_phys);
        tx_descs[i].addr = phys;
        tx_descs[i].cmd = 0;
        tx_descs[i].status = 0;
        vmm_temp_unmap();
    }

    /* Program RX ring registers (RDT set after RCTL.EN below) */
    e1000_write(E1000_RDBAL, rx_ring_phys);
    e1000_write(E1000_RDBAH, 0);
    e1000_write(E1000_RDLEN, E1000_NUM_RX_DESC * E1000_DESC_SIZE);
    e1000_write(E1000_RDH, 0);

    /* Program TX ring registers */
    e1000_write(E1000_TDBAL, tx_ring_phys);
    e1000_write(E1000_TDBAH, 0);
    e1000_write(E1000_TDLEN, E1000_NUM_TX_DESC * E1000_DESC_SIZE);
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);

    rx_cur = 0;
    tx_cur = 0;
    return 0;
}

int e1000_init(uint32_t bar0_phys, uint8_t irq) {
    irq_num = irq;

    uint32_t bar0_page = bar0_phys & ~0xFFF;
    uint32_t bar0_offset = bar0_phys & 0xFFF;
    page_directory_t *kdir = vmm_get_kernel_directory();
    uint32_t pages = E1000_BAR_SIZE / 4096;
    for (uint32_t i = 0; i < pages; i++) {
        vmm_map_page(kdir, bar0_page + i * 4096,
                     E1000_MMIO_VADDR + i * 4096,
                     VMM_PRESENT | VMM_WRITABLE | VMM_CACHE_DISABLE);
    }
    mmio_base = (volatile uint8_t *)(E1000_MMIO_VADDR + bar0_offset);
    debug_printf("e1000: MMIO at 0x%x -> 0x%x (%d pages)\r\n", bar0_phys, (uint32_t)mmio_base, pages);

    /* Reset */
    e1000_write(E1000_CTRL, E1000_CTRL_RST);
    for (volatile int i = 0; i < 100000; i++);
    debug_print("e1000: reset done\r\n");

    if (e1000_ring_init() < 0) {
        debug_print("e1000: ring init failed\r\n");
        return -1;
    }

    /* Configure CTRL */
    uint32_t ctrl = e1000_read(E1000_CTRL);
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE;
    ctrl &= ~E1000_CTRL_LRST;
    e1000_write(E1000_CTRL, ctrl);

    /* Read MAC from RAL/RAH */
    uint32_t ral = e1000_read(E1000_RAL);
    uint32_t rah = e1000_read(E1000_RAH);
    e1000_mac[0] = ral & 0xFF;
    e1000_mac[1] = (ral >> 8) & 0xFF;
    e1000_mac[2] = (ral >> 16) & 0xFF;
    e1000_mac[3] = (ral >> 24) & 0xFF;
    e1000_mac[4] = rah & 0xFF;
    e1000_mac[5] = (rah >> 8) & 0xFF;
    debug_printf("e1000: MAC %x:%x:%x:%x:%x:%x\r\n",
        e1000_mac[0], e1000_mac[1], e1000_mac[2],
        e1000_mac[3], e1000_mac[4], e1000_mac[5]);

    /* Enable RX */
    uint32_t rctl = E1000_RCTL_EN | E1000_RCTL_SBP | E1000_RCTL_UPE | E1000_RCTL_MPE |
                    E1000_RCTL_BAM | E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC;
    e1000_write(E1000_RCTL, rctl);
    e1000_write(E1000_RDT, E1000_NUM_RX_DESC - 1);

    /* Enable TX */
    uint32_t tctl = E1000_TCTL_EN | E1000_TCTL_PSP | E1000_TCTL_CT | E1000_TCTL_COLD;
    e1000_write(E1000_TCTL, tctl);
    e1000_write(E1000_TIPG, 0x0060200A);

    /* Initialize Multicast Table Array (128 entries, accept all) */
    for (int i = 0; i < 128; i++)
        e1000_write(E1000_MTA + i * 4, 0xFFFFFFFF);

    /* Program MAC into Receive Address registers */
    uint32_t ra_low = (e1000_mac[3] << 24) | (e1000_mac[2] << 16) |
                      (e1000_mac[1] << 8) | e1000_mac[0];
    uint32_t ra_high = (e1000_mac[5] << 8) | e1000_mac[4] | (1 << 31);
    e1000_write(E1000_RAL, ra_low);
    e1000_write(E1000_RAH, ra_high);

    /* Mask all interrupts */
    e1000_write(E1000_IMC, 0xFFFFFFFF);
    e1000_write(E1000_IMS, 0);
    e1000_write(E1000_ICT, 0xFFFFFFFF);

    uint32_t status = e1000_read(E1000_STATUS);
    debug_printf("e1000: STATUS=0x%x (LU=%d)\r\n", status,
                 (status & E1000_STATUS_LU) ? 1 : 0);
    debug_printf("e1000: RCTL=0x%x FCTRL=0x%x\r\n", e1000_read(E1000_RCTL),
                 e1000_read(0x058));

    e1000_ok = 1;
    debug_print("e1000: initialized\r\n");
    return 0;
}

int e1000_send(const void *buf, uint16_t len) {
    if (!e1000_ok) return -1;
    if (len > 2048) len = 2048;
    static int d;
    if (++d <= 5) debug_printf("e1000: TX len=%u\r\n", len);

    uint32_t head = e1000_read(E1000_TDH);
    uint32_t next = (tx_cur + 1) % E1000_NUM_TX_DESC;
    if (next == head) return -1;

    uint32_t tx_ring_phys = e1000_read(E1000_TDBAL);
    tx_descs = (e1000_tx_desc_t *)vmm_temp_map(tx_ring_phys);
    uint32_t buf_phys = tx_descs[tx_cur].addr;
    vmm_temp_unmap();

    uint8_t *page = (uint8_t *)vmm_temp_map(buf_phys);
    for (uint16_t i = 0; i < len; i++)
        page[i] = ((const uint8_t *)buf)[i];
    vmm_temp_unmap();

    tx_descs = (e1000_tx_desc_t *)vmm_temp_map(tx_ring_phys);
    tx_descs[tx_cur].length = len;
    tx_descs[tx_cur].cmd = E1000_CMD_EOP | E1000_CMD_IFCS | E1000_CMD_RS;
    tx_descs[tx_cur].status = 0;
    vmm_temp_unmap();

    (void)tx_cur;
    tx_cur = next;
    e1000_write(E1000_TDT, tx_cur);

    for (volatile int i = 0; i < 100000; i++) {
        head = e1000_read(E1000_TDH);
        if (head == tx_cur) break;
    }

    return len;
}

void *e1000_poll_rx(uint16_t *len) {
    if (!e1000_ok) return NULL;
    static int r;
    uint32_t rdh = e1000_read(E1000_RDH);
    if (++r <= 5)
        debug_printf("e1000: poll_rx cur=%u RDH=%u RDT=%u\r\n", rx_cur, rdh,
                     e1000_read(E1000_RDT));

    uint32_t rx_ring_phys = e1000_read(E1000_RDBAL);
    rx_descs = (e1000_rx_desc_t *)vmm_temp_map(rx_ring_phys);

    if (!(rx_descs[rx_cur].status & E1000_RXD_STAT_DD)) {
        vmm_temp_unmap();
        return NULL;
    }
    debug_printf("e1000: RX packet at desc %u len=%u\r\n", rx_cur,
                 rx_descs[rx_cur].length);

    uint32_t buf_phys = (uint32_t)rx_descs[rx_cur].addr;
    uint16_t pkt_len = rx_descs[rx_cur].length;
    rx_descs[rx_cur].status = 0;
    vmm_temp_unmap();

    if (pkt_len <= 200) {
        uint8_t *rd = (uint8_t *)vmm_temp_map(buf_phys);
        debug_printf("e1000: packet bytes: ");
        for (uint16_t i = 0; i < pkt_len; i++)
            debug_printf("%x ", rd[i]);
        debug_printf("\r\n");
        vmm_temp_unmap();
    }

    uint8_t *buf = (uint8_t *)vmm_temp_map(buf_phys);
    uint8_t *copy = (uint8_t *)kmalloc(pkt_len);
    if (copy) {
        for (uint16_t i = 0; i < pkt_len; i++)
            copy[i] = buf[i];
    }
    vmm_temp_unmap();

    uint32_t done = rx_cur;

    /* Restart RX descriptor */
    rx_descs = (e1000_rx_desc_t *)vmm_temp_map(rx_ring_phys);
    rx_descs[done].length = 0;
    rx_descs[done].status = 0;
    vmm_temp_unmap();

    rx_cur = (rx_cur + 1) % E1000_NUM_RX_DESC;
    e1000_write(E1000_RDT, done);

    if (len) *len = pkt_len;
    return copy;
}

void e1000_free_rx(void *buf) {
    if (buf) kfree(buf);
}

void e1000_get_mac(uint8_t *mac) {
    for (int i = 0; i < 6; i++) mac[i] = e1000_mac[i];
}

void e1000_poll(void) {
    uint16_t len;
    void *pkt;
    while ((pkt = e1000_poll_rx(&len)) != NULL) {
        extern void e1000if_input(void *pkt, uint16_t len);
        e1000if_input(pkt, len);
    }
}
