#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <stddef.h>

struct arc_boot_info;


#define RSDP_SIGNATURE "RSD PTR "

typedef struct {
    char     signature[8];
    uint8_t  checksum;
    char     oemid[6];
    uint8_t  revision;
    uint32_t rsdt_addr;         /* RSDT physical address (32-bit) */
} __attribute__((packed)) rsdp_v1_t;

typedef struct {
    char     signature[8];
    uint8_t  checksum;
    char     oemid[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    uint32_t length;            /* total RSDP length */
    uint64_t xsdt_addr;         /* XSDT physical address (64-bit) */
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed)) rsdp_t;


typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oemid[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) sdt_header_t;


typedef struct {
    sdt_header_t header;
    uint64_t     entries[0];    /* array of 64-bit table physical addresses */
} __attribute__((packed)) xsdt_t;


#define MADT_SIGNATURE "APIC"

typedef struct {
    sdt_header_t header;
    uint32_t     local_apic_addr;      /* physical base of local APIC */
    uint32_t     flags;                /* bit 0: PC-AT compatibility */
} __attribute__((packed)) madt_t;

/* MADT entry types */
#define MADT_ENTRY_LOCAL_APIC       0
#define MADT_ENTRY_IO_APIC          1
#define MADT_ENTRY_ISO              2   /* Interrupt Source Override */
#define MADT_ENTRY_NMI_SOURCE       3
#define MADT_ENTRY_LOCAL_APIC_NMI   4
#define MADT_ENTRY_LOCAL_APIC_OVR   5   /* Local APIC address override */
#define MADT_ENTRY_IO_SAPIC         6
#define MADT_ENTRY_LOCAL_SAPIC      7
#define MADT_ENTRY_PLATFORM_IS      8
#define MADT_ENTRY_GICC             10  /* Generic Interrupt Controller (ARM) */

typedef struct {
    uint8_t type;
    uint8_t length;
    uint8_t data[0];
} __attribute__((packed)) madt_entry_t;

/* I/O APIC entry (type 1) */
typedef struct {
    uint8_t  type;
    uint8_t  length;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_addr;       /* physical base of I/O APIC */
    uint32_t gsi_base;          /* Global System Interrupt base */
} __attribute__((packed)) madt_ioapic_t;

/* Interrupt Source Override (type 2) */
typedef struct {
    uint8_t  type;
    uint8_t  length;
    uint8_t  bus;               /* 0 = ISA */
    uint8_t  source;            /* IRQ number */
    uint32_t gsi;               /* Global System Interrupt */
    uint16_t flags;             /* MPS INTI flags (polarity/trigger) */
} __attribute__((packed)) madt_iso_t;


typedef struct {
    uint8_t  address_space_id;  /* 0=mem, 1=IO, 2=PCI, ... */
    uint8_t  register_bit_width;
    uint8_t  register_bit_offset;
    uint8_t  access_size;       /* 1=byte, 2=word, 3=dword, 4=qword */
    uint64_t address;
} __attribute__((packed)) gas_t;


#define FADT_SIGNATURE "FACP"

typedef struct {
    sdt_header_t header;        /* 0-35 */
    uint32_t     firmware_ctrl; /* 36 — FACS phys, legacy */
    uint32_t     dsdt_addr;     /* 40 — DSDT phys, legacy */
    uint8_t      reserved;      /* 44 — was int_model */
    uint8_t      preferred_pm_profile; /* 45 */
    uint16_t     sci_int;       /* 46 */
    uint32_t     smi_cmd;       /* 48 */
    uint8_t      acpi_enable;   /* 52 */
    uint8_t      acpi_disable;  /* 53 */
    uint8_t      s4bios_req;    /* 54 */
    uint8_t      pstate_cnt;    /* 55 */
    uint32_t     pm1a_evt_blk;  /* 56 */
    uint32_t     pm1b_evt_blk;  /* 60 */
    uint32_t     pm1a_cnt_blk;  /* 64 */
    uint32_t     pm1b_cnt_blk;  /* 68 */
    uint32_t     pm2_cnt_blk;   /* 72 */
    uint32_t     pm_tmr_blk;    /* 76 */
    uint32_t     gpe0_blk;      /* 80 */
    uint32_t     gpe1_blk;      /* 84 */
    uint8_t      pm1_evt_len;   /* 88 */
    uint8_t      pm1_cnt_len;   /* 89 */
    uint8_t      pm2_cnt_len;   /* 90 */
    uint8_t      pm_tmr_len;    /* 91 */
    uint8_t      gpe0_blk_len;  /* 92 */
    uint8_t      gpe1_blk_len;  /* 93 */
    uint8_t      gpe1_base;     /* 94 */
    uint8_t      _cst_cnt;      /* 95 */
    uint16_t     p_lvl2_lat;    /* 96 */
    uint16_t     p_lvl3_lat;    /* 98 */
    uint16_t     flush_size;    /* 100 */
    uint16_t     flush_stride;  /* 102 */
    uint8_t      duty_offset;   /* 104 */
    uint8_t      duty_width;    /* 105 */
    uint8_t      day_alrm;      /* 106 */
    uint8_t      mon_alrm;      /* 107 */
    uint8_t      century;       /* 108 */
    uint16_t     iapc_boot_arch; /* 109 — ACPI 2.0+ */
    uint8_t      reserved2;     /* 111 */
    uint32_t     flags;         /* 112 — ACPI 2.0+ */
    gas_t        reset_reg;     /* 116 — ACPI 2.0+ */
    uint8_t      reset_value;   /* 128 */
    uint8_t      reserved3[3];  /* 129-131 */
    uint64_t     x_firmware_ctrl; /* 132 — 64-bit FACS */
    uint64_t     x_dsdt;        /* 140 — 64-bit DSDT */
    /* Extended register blocks follow in ACPI 2.0+ */
} __attribute__((packed)) fadt_t;


#define DSDT_SIGNATURE "DSDT"
#define SSDT_SIGNATURE "SSDT"


typedef struct {
    /* MADT / APIC */
    int      valid;
    uint32_t lapic_addr;        /* local APIC physical base */
    uint32_t ioapic_addr;       /* I/O APIC physical base */
    uint32_t ioapic_gsi_base;   /* GSI base for this I/O APIC */

    int      iso_count;
    struct {
        uint8_t  source;        /* ISA IRQ */
        uint32_t gsi;
        uint16_t flags;
    } isos[16];

    int      lapic_count;
    struct {
        uint8_t  apic_id;
        uint8_t  flags;         /* bit 0 = enabled */
    } lapics[64];

    /* FADT / DSDT */
    uint32_t dsdt_addr;         /* DSDT physical address (legacy) */
    uint64_t x_dsdt;            /* DSDT physical address (64-bit) 0 if none */
    uint32_t dsdt_length;       /* DSDT total length */

    uint32_t facs_addr;         /* FACS physical address (legacy) */
    uint64_t x_facs;            /* FACS physical address (64-bit) */

    gas_t    reset_reg;         /* reset register descriptor */
    uint8_t  reset_value;       /* value to write for reset */
    uint8_t  has_reset;         /* nonzero if reset_reg is valid */

    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm_tmr_blk;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;
    uint8_t  pm_tmr_len;

    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;

    uint16_t sci_int;           /* system control interrupt */

    /* _S5 sleep state for shutdown */
    uint8_t  s5_slp_typa;
    uint8_t  s5_slp_typb;
    uint8_t  s5_valid;
} acpi_info_t;

extern acpi_info_t acpi_info;


int  acpi_init(struct arc_boot_info *boot);
int  acpi_find_madt(void);
void acpi_shutdown(void);

/* Physical read helpers (used during early init before full VMM) */
uint8_t  acpi_read8(uint64_t phys);
uint16_t acpi_read16(uint64_t phys);
uint32_t acpi_read32(uint64_t phys);
uint64_t acpi_read64(uint64_t phys);
int      acpi_checksum(const void *data, size_t len);

#endif
