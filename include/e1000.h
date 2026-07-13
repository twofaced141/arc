#ifndef E1000_H
#define E1000_H

#include <stdint.h>

#define E1000_VENDOR_INTEL    0x8086
#define E1000_DEV_82540EM     0x100E
#define E1000_DEV_82545EM     0x100F
#define E1000_DEV_82544GC     0x1004

#define E1000_NUM_RX_DESC     32
#define E1000_NUM_TX_DESC     8
#define E1000_DESC_SIZE       16
#define E1000_RX_BUF_SIZE     2048

/* PCI BAR0 MMIO registers */
#define E1000_CTRL       0x0000
#define E1000_STATUS     0x0008
#define E1000_EECD       0x0010
#define E1000_EERD       0x0014
#define E1000_ICT        0x00E0
#define E1000_ICS        0x00E4
#define E1000_IMS        0x00E8
#define E1000_IMC        0x00EC
#define E1000_RCTL       0x0100
#define E1000_TCTL       0x0400
#define E1000_TIPG       0x0410
#define E1000_RDBAL      0x2800
#define E1000_RDBAH      0x2804
#define E1000_RDLEN      0x2808
#define E1000_RDH        0x2810
#define E1000_RDT        0x2818
#define E1000_TDBAL      0x3800
#define E1000_TDBAH      0x3804
#define E1000_TDLEN      0x3808
#define E1000_TDH        0x3810
#define E1000_TDT        0x3818
#define E1000_RAL        0x5400
#define E1000_RAH        0x5404
#define E1000_MTA        0x5200

/* CTRL bits */
#define E1000_CTRL_FD      (1 << 0)
#define E1000_CTRL_LRST    (1 << 3)
#define E1000_CTRL_ASDE    (1 << 5)
#define E1000_CTRL_SLU     (1 << 6)
#define E1000_CTRL_ILOS    (1 << 7)
#define E1000_CTRL_RST     (1 << 26)
#define E1000_CTRL_VME     (1 << 30)
#define E1000_CTRL_PHY_RST (1 << 31)

/* STATUS bits */
#define E1000_STATUS_FD     (1 << 0)
#define E1000_STATUS_LU     (1 << 1)
#define E1000_STATUS_TXOFF  (1 << 4)

/* RCTL bits */
#define E1000_RCTL_EN       (1 << 1)
#define E1000_RCTL_SBP      (1 << 2)
#define E1000_RCTL_UPE      (1 << 3)
#define E1000_RCTL_MPE      (1 << 4)
#define E1000_RCTL_LPE      (1 << 5)
#define E1000_RCTL_LBM_NONE (0 << 6)
#define E1000_RCTL_RDMTS_HALF (0 << 8)
#define E1000_RCTL_BAM      (1 << 15)
#define E1000_RCTL_BSIZE_2048 (0 << 16)
#define E1000_RCTL_BSIZE_4096 (3 << 16)
#define E1000_RCTL_SECRC    (1 << 26)

/* TCTL bits */
#define E1000_TCTL_EN       (1 << 1)
#define E1000_TCTL_PSP      (1 << 3)
#define E1000_TCTL_CT       (0x10 << 4)
#define E1000_TCTL_COLD     (0x40 << 12)
#define E1000_TCTL_SWXOFF   (1 << 22)

/* Descriptor command bits */
#define E1000_CMD_EOP       (1 << 0)
#define E1000_CMD_IFCS      (1 << 1)
#define E1000_CMD_RS        (1 << 3)
#define E1000_CMD_RPS       (1 << 4)

/* Descriptor status bits */
#define E1000_RXD_STAT_DD   (1 << 0)
#define E1000_RXD_STAT_EOP  (1 << 1)

typedef struct {
    uint64_t addr;      /* bytes 0-7:  buffer address */
    uint16_t length;    /* bytes 8-9:  packet length */
    uint16_t cksum;     /* bytes 10-11: checksum */
    uint8_t  status;    /* byte 12:    status (DD=E1000_RXD_STAT_DD bit 0) */
    uint8_t  errors;    /* byte 13:    errors */
    uint16_t special;   /* bytes 14-15: special */
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

int e1000_probe(void);
int e1000_init(uint32_t bar0_phys, uint8_t irq);
void e1000_get_mac(uint8_t *mac);
int e1000_send(const void *buf, uint16_t len);
void *e1000_poll_rx(uint16_t *len);
void e1000_free_rx(void *buf);
void e1000_poll(void);

#endif
