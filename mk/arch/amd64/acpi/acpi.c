#include "acpi.h"
#include "aml.h"
#include "debug.h"
#include "vmm.h"
#include "string.h"
#include "memory.h"
#include "idt.h"
#include <arc/boot.h>

/* The identity map covers the first 64MB of physical memory
   at KERNEL_BASE + phys. ACPI tables below this threshold
   are accessed directly; above it we use vmm_temp_map. */
#define IDENTITY_MAP_SIZE  0x4000000ULL  /* 64MB */

acpi_info_t acpi_info;


/* Map a physical address for reading. Returns a virtual pointer,
   or NULL if the address cannot be accessed.
   LIMITATION: temp_map uses a single 4K slot — the caller must
   finish reading before the next phys_ptr call. */
static const void *phys_ptr(uint64_t phys, uint32_t size) {
    if (phys < IDENTITY_MAP_SIZE && phys + size <= IDENTITY_MAP_SIZE)
        return (const void *)(KERNEL_BASE + phys);
    return vmm_temp_map(phys);
}

static void phys_ptr_done(const void *ptr) {
    uint64_t addr = (uint64_t)ptr;
    if (addr < KERNEL_BASE || addr >= KERNEL_BASE + IDENTITY_MAP_SIZE)
        vmm_temp_unmap();
}

int acpi_checksum(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += p[i];
    return sum == 0 ? 0 : -1;
}

/* Validate an RSDP at a physical address (signature + v1/v2
 * checksums) and copy it into *out.  The mapping is released before
 * returning, so the copy is safe to keep.  Returns 0 on success. */
static int rsdp_copy_from(uint64_t phys, rsdp_t *out) {
    if (!out)
        return -1;

    const rsdp_t *r = (const rsdp_t *)phys_ptr(phys, sizeof(rsdp_t));
    if (!r)
        return -1;

    if (memcmp(r->signature, RSDP_SIGNATURE, 8) != 0) {
        phys_ptr_done(r);
        return -1;
    }

    if (acpi_checksum(r, 20) != 0) {
        phys_ptr_done(r);
        return -1;
    }

    if (r->revision >= 2) {
        uint32_t len = r->length;
        if (len < sizeof(rsdp_t)) {
            phys_ptr_done(r);
            return -1;
        }
        if (acpi_checksum(r, len) != 0) {
            phys_ptr_done(r);
            return -1;
        }
    }

    memcpy(out, r, sizeof(rsdp_t));
    phys_ptr_done(r);
    return 0;
}

/* Legacy BIOS fallback: per the ACPI spec, search the first 1KB of
 * the EBDA (its segment is a 16-bit pointer in the BIOS data area at
 * 0x40:0x0E), then the main BIOS area 0xE0000 - 0xFFFFF. */
static int find_rsdp(rsdp_t *out) {
    const uint16_t *bda_ebda = (const uint16_t *)phys_ptr(0x40E, 2);
    if (bda_ebda) {
        uint64_t ebda = (uint64_t)*bda_ebda << 4;
        phys_ptr_done(bda_ebda);

        /* Sanity: the EBDA lives in the lower 1MB (usually 0x80000+). */
        if (ebda >= 0x80000 && ebda < 0x100000) {
            for (uint64_t addr = ebda; addr < ebda + 0x400; addr += 16) {
                if (rsdp_copy_from(addr, out) == 0) {
                    log_printf(LOG_LEVEL_INFO, "acpi: RSDP found in EBDA at 0x%08x\r\n",
                               (uint32_t)addr);
                    return 0;
                }
            }
        }
    }

    for (uint64_t addr = 0xE0000; addr <= 0xFFFFF; addr += 16) {
        if (rsdp_copy_from(addr, out) == 0) {
            log_printf(LOG_LEVEL_INFO, "acpi: RSDP found at 0x%08x\r\n",
                       (uint32_t)addr);
            return 0;
        }
    }
    return -1;
}

/* Walk RSDT (32-bit entries) or XSDT (64-bit entries) to find a
 * table with the given signature. */
static uint64_t find_table_in_sdt(uint64_t sdt_phys, const char *sig, int entry_size) {
    const sdt_header_t *hdr = (const sdt_header_t *)phys_ptr(sdt_phys, sizeof(sdt_header_t));
    if (!hdr) return 0;

    sdt_header_t hdr_copy;
    memcpy(&hdr_copy, hdr, sizeof(sdt_header_t));
    phys_ptr_done(hdr);

    /* Accept either RSDT or XSDT */
    if (memcmp(hdr_copy.signature, "RSDT", 4) != 0 &&
        memcmp(hdr_copy.signature, "XSDT", 4) != 0)
        return 0;

    int entry_count = (hdr_copy.length - sizeof(sdt_header_t)) / entry_size;
    uint32_t read_size = entry_count * entry_size;

    uint64_t result = 0;

    if (entry_size == 8) {
        /* XSDT: 64-bit entries */
        const uint64_t *entries = (const uint64_t *)phys_ptr(sdt_phys + sizeof(sdt_header_t), read_size);
        if (!entries) return 0;

        for (int i = 0; i < entry_count; i++) {
            if (entries[i] == 0) continue;

            const sdt_header_t *thdr = (const sdt_header_t *)phys_ptr(entries[i], sizeof(sdt_header_t));
            if (!thdr) continue;

            char sig_buf[5];
            memcpy(sig_buf, thdr->signature, 4);
            sig_buf[4] = '\0';
            phys_ptr_done(thdr);

            if (memcmp(sig_buf, sig, 4) == 0) {
                result = entries[i];
                break;
            }
        }
        phys_ptr_done(entries);
    } else {
        /* RSDT: 32-bit entries */
        const uint32_t *entries = (const uint32_t *)phys_ptr(sdt_phys + sizeof(sdt_header_t), read_size);
        if (!entries) return 0;

        for (int i = 0; i < entry_count; i++) {
            if (entries[i] == 0) continue;

            const sdt_header_t *thdr = (const sdt_header_t *)phys_ptr(entries[i], sizeof(sdt_header_t));
            if (!thdr) continue;

            char sig_buf[5];
            memcpy(sig_buf, thdr->signature, 4);
            sig_buf[4] = '\0';
            phys_ptr_done(thdr);

            if (memcmp(sig_buf, sig, 4) == 0) {
                result = entries[i];
                break;
            }
        }
        phys_ptr_done(entries);
    }

    return result;
}


static void parse_madt(uint64_t madt_phys) {
    const sdt_header_t *hdr = (const sdt_header_t *)phys_ptr(madt_phys, sizeof(sdt_header_t));
    if (!hdr) return;

    sdt_header_t hdr_copy;
    memcpy(&hdr_copy, hdr, sizeof(sdt_header_t));
    phys_ptr_done(hdr);

    if (memcmp(hdr_copy.signature, MADT_SIGNATURE, 4) != 0) {
        log_print(LOG_LEVEL_ERROR, "acpi: MADT signature mismatch\r\n");
        return;
    }

    uint32_t madt_size = hdr_copy.length;
    if (madt_size < sizeof(madt_t)) return;

    const madt_t *madt = (const madt_t *)phys_ptr(madt_phys, madt_size);
    if (!madt) return;

    uint32_t lapic_addr = madt->local_apic_addr;
    int entry_offset = sizeof(madt_t);
    uint32_t ioapic_addr = 0;
    uint32_t ioapic_gsi_base = 0;

    while (entry_offset + 2 <= (int)madt_size) {
        const madt_entry_t *mentry = (const madt_entry_t *)((const uint8_t *)madt + entry_offset);
        uint8_t type = mentry->type;
        uint8_t len  = mentry->length;

        if (len < 2 || entry_offset + len > (int)madt_size)
            break;

        switch (type) {
        case MADT_ENTRY_IO_APIC: {
            const madt_ioapic_t *ioapic = (const madt_ioapic_t *)mentry;
            ioapic_addr     = ioapic->ioapic_addr;
            ioapic_gsi_base = ioapic->gsi_base;
            log_printf(LOG_LEVEL_DEBUG, "acpi: I/O APIC id=%u addr=0x%x gsi_base=%u\r\n",
                         ioapic->ioapic_id, ioapic_addr, ioapic_gsi_base);
            break;
        }
        case MADT_ENTRY_ISO: {
            const madt_iso_t *iso = (const madt_iso_t *)mentry;
            if (acpi_info.iso_count < 16) {
                int idx = acpi_info.iso_count++;
                acpi_info.isos[idx].source = iso->source;
                acpi_info.isos[idx].gsi    = iso->gsi;
                acpi_info.isos[idx].flags  = iso->flags;
            }
            log_printf(LOG_LEVEL_DEBUG, "acpi: ISA IRQ %u -> GSI %u flags=0x%x\r\n",
                         iso->source, iso->gsi, iso->flags);
            break;
        }
        case MADT_ENTRY_LOCAL_APIC: {
            /* Layout: type(1), len(1), acpi_id(1), apic_id(1), flags(4) */
            if (len >= 8 && acpi_info.lapic_count < 64) {
                uint8_t apic_id = ((const uint8_t *)mentry)[3];
                uint32_t lapic_flags = *(const uint32_t *)(((const uint8_t *)mentry) + 4);
                int idx = acpi_info.lapic_count++;
                acpi_info.lapics[idx].apic_id = apic_id;
                acpi_info.lapics[idx].flags   = (uint8_t)lapic_flags;
            }
            break;
        }
        case MADT_ENTRY_LOCAL_APIC_OVR: {
            if (len >= 12) {
                uint64_t ovr_addr = *(const uint64_t *)(((const uint8_t *)mentry) + 4);
                lapic_addr = (uint32_t)ovr_addr;
                log_printf(LOG_LEVEL_DEBUG, "acpi: LAPIC address override -> 0x%x\r\n", lapic_addr);
            }
            break;
        }
        }

        entry_offset += len;
    }

    phys_ptr_done(madt);

    /* Store parsed info (acpi_info already zeroed in acpi_init) */
    acpi_info.valid           = 1;
    acpi_info.lapic_addr      = lapic_addr;
    acpi_info.ioapic_addr     = ioapic_addr;
    acpi_info.ioapic_gsi_base = ioapic_gsi_base;

    log_printf(LOG_LEVEL_INFO, "acpi: MADT parsed: LAPIC @ 0x%x I/O APIC @ 0x%x GSI=%u\r\n",
                 lapic_addr, ioapic_addr, ioapic_gsi_base);
}


static void parse_fadt(uint64_t fadt_phys) {
    const sdt_header_t *hdr = (const sdt_header_t *)phys_ptr(fadt_phys, sizeof(sdt_header_t));
    if (!hdr) return;

    sdt_header_t hdr_copy;
    memcpy(&hdr_copy, hdr, sizeof(sdt_header_t));
    phys_ptr_done(hdr);

    if (memcmp(hdr_copy.signature, FADT_SIGNATURE, 4) != 0)
        return;

    uint32_t fadt_len = hdr_copy.length;
    if (fadt_len < sizeof(fadt_t))
        return;

    const fadt_t *fadt = (const fadt_t *)phys_ptr(fadt_phys, fadt_len);
    if (!fadt) return;

    /* DSDT address — prefer 64-bit if available */
    if (fadt->x_dsdt != 0) {
        acpi_info.x_dsdt = fadt->x_dsdt;
        acpi_info.dsdt_addr = (uint32_t)fadt->x_dsdt;
    } else {
        acpi_info.dsdt_addr = fadt->dsdt_addr;
    }

    /* FACS address */
    if (fadt->x_firmware_ctrl != 0) {
        acpi_info.x_facs = fadt->x_firmware_ctrl;
        acpi_info.facs_addr = (uint32_t)fadt->x_firmware_ctrl;
    } else {
        acpi_info.facs_addr = fadt->firmware_ctrl;
    }

    /* PM blocks */
    acpi_info.pm1a_evt_blk = fadt->pm1a_evt_blk;
    acpi_info.pm1b_evt_blk = fadt->pm1b_evt_blk;
    acpi_info.pm1a_cnt_blk = fadt->pm1a_cnt_blk;
    acpi_info.pm1b_cnt_blk = fadt->pm1b_cnt_blk;
    acpi_info.pm_tmr_blk   = fadt->pm_tmr_blk;
    acpi_info.pm1_evt_len  = fadt->pm1_evt_len;
    acpi_info.pm1_cnt_len  = fadt->pm1_cnt_len;
    acpi_info.pm_tmr_len   = fadt->pm_tmr_len;

    /* SMI command port for ACPI enable/disable */
    acpi_info.smi_cmd      = fadt->smi_cmd;
    acpi_info.acpi_enable  = fadt->acpi_enable;
    acpi_info.acpi_disable = fadt->acpi_disable;

    /* SCI interrupt */
    acpi_info.sci_int = fadt->sci_int;

    /* Reset register (ACPI 2.0+) */
    if (fadt_len >= offsetof(fadt_t, reset_reg) + sizeof(gas_t)) {
        acpi_info.reset_reg = fadt->reset_reg;
        acpi_info.reset_value = fadt->reset_value;
        /* Valid if address space is system memory (0) or system IO (1)
           and the address is non-zero. */
        if ((fadt->reset_reg.address_space_id == 0 ||
             fadt->reset_reg.address_space_id == 1) &&
            fadt->reset_reg.address != 0) {
            acpi_info.has_reset = 1;
        }
    }

    phys_ptr_done(fadt);

    log_printf(LOG_LEVEL_INFO, "acpi: FADT parsed DSDT=0x%x reset={reg=0x%lx,val=0x%x} pmtmr=0x%x/%u\r\n",
                 acpi_info.dsdt_addr,
                 (unsigned long)acpi_info.reset_reg.address,
                 acpi_info.reset_value,
                 acpi_info.pm_tmr_blk, acpi_info.pm_tmr_len);
}


static void probe_dsdt(void) {
    uint64_t dsdt_phys = acpi_info.x_dsdt ? acpi_info.x_dsdt : (uint64_t)acpi_info.dsdt_addr;
    if (!dsdt_phys) return;

    const sdt_header_t *hdr = (const sdt_header_t *)phys_ptr(dsdt_phys, sizeof(sdt_header_t));
    if (!hdr) return;

    sdt_header_t hdr_copy;
    memcpy(&hdr_copy, hdr, sizeof(sdt_header_t));
    phys_ptr_done(hdr);

    if (memcmp(hdr_copy.signature, DSDT_SIGNATURE, 4) != 0) {
        log_print(LOG_LEVEL_ERROR, "acpi: DSDT signature mismatch\r\n");
        return;
    }

    acpi_info.dsdt_length = hdr_copy.length;

    if (acpi_checksum(hdr, hdr_copy.length) != 0) {
        log_print(LOG_LEVEL_ERROR, "acpi: DSDT checksum failed\r\n");
        return;
    }

    log_printf(LOG_LEVEL_DEBUG, "acpi: DSDT at 0x%lx length=%u\r\n",
                 (unsigned long)dsdt_phys, acpi_info.dsdt_length);
}


int acpi_init(struct arc_boot_info *boot) {
    memset(&acpi_info, 0, sizeof(acpi_info));

    /* Prefer the bootloader-provided RSDP (multiboot2 ACPI tag, EFI
     * configuration table, ...); fall back to legacy BIOS scanning. */
    rsdp_t rsdp;
    int have_rsdp = 0;

    if (boot && (boot->flags & ARC_BOOT_HAS_ACPI) && boot->acpi_rsdp) {
        if (rsdp_copy_from((uintptr_t)boot->acpi_rsdp, &rsdp) == 0) {
            log_printf(LOG_LEVEL_INFO, "acpi: RSDP from bootloader at 0x%lx\r\n",
                       (unsigned long)(uintptr_t)boot->acpi_rsdp);
            have_rsdp = 1;
        } else {
            log_printf(LOG_LEVEL_WARN, "acpi: bootloader RSDP invalid, "
                       "falling back to BIOS scan\r\n");
        }
    }

    if (!have_rsdp && find_rsdp(&rsdp) == 0)
        have_rsdp = 1;

    if (!have_rsdp) {
        log_print(LOG_LEVEL_ERROR, "acpi: RSDP not found\r\n");
        return -1;
    }

    uint64_t sdt_phys;
    int entry_size;
    if (rsdp.revision >= 2) {
        sdt_phys  = rsdp.xsdt_addr;
        entry_size = 8;
        log_print(LOG_LEVEL_INFO, "acpi: using XSDT (v2)\r\n");
    } else {
        sdt_phys  = (uint64_t)rsdp.rsdt_addr;
        entry_size = 4;
        log_print(LOG_LEVEL_INFO, "acpi: using RSDT (v1)\r\n");
    }

    if (!sdt_phys) {
        log_print(LOG_LEVEL_ERROR, "acpi: no RSDT/XSDT\r\n");
        return -1;
    }

    /* Find and parse MADT */
    uint64_t madt_phys = find_table_in_sdt(sdt_phys, MADT_SIGNATURE, entry_size);
    if (!madt_phys) {
        log_print(LOG_LEVEL_ERROR, "acpi: MADT not found\r\n");
        return -1;
    }
    parse_madt(madt_phys);

    if (!acpi_info.ioapic_addr) {
        log_print(LOG_LEVEL_ERROR, "acpi: no I/O APIC found in MADT\r\n");
        return -1;
    }

    /* Find and parse FADT */
    uint64_t fadt_phys = find_table_in_sdt(sdt_phys, FADT_SIGNATURE, entry_size);
    if (fadt_phys) {
        parse_fadt(fadt_phys);
    } else {
        log_print(LOG_LEVEL_ERROR, "acpi: FADT not found\r\n");
    }

    /* Probe DSDT */
    probe_dsdt();

    /* Parse AML namespace from DSDT */
    if (aml_init() == 0) {
        /* Extract _S5 sleep states for ACPI shutdown */
        uint64_t s5_vals[2];

        /* Try Name(_S5, Package(...)) via namespace lookup */
        int found = (aml_get_package_int("\\_S5_", 0, &s5_vals[0]) == 0 &&
                     aml_get_package_int("\\_S5_", 1, &s5_vals[1]) == 0);

        /* Fallback: raw AML byte scan (handles cases where _S5 is
         * behind a broken PkgLength chain in the namespace builder) */
        if (!found) {
            uint64_t raw_vals[4];
            int ne = aml_raw_scan_s5(raw_vals, 4);
            if (ne >= 2) {
                s5_vals[0] = raw_vals[0];
                s5_vals[1] = raw_vals[1];
                found = 1;
            }
        }

        if (found) {
            acpi_info.s5_slp_typa = (uint8_t)s5_vals[0];
            acpi_info.s5_slp_typb = (uint8_t)s5_vals[1];
            acpi_info.s5_valid = 1;
            log_printf(LOG_LEVEL_DEBUG, "acpi: _S5 = {%u, %u}\r\n",
                         (unsigned int)s5_vals[0],
                         (unsigned int)s5_vals[1]);
        } else {
            log_print(LOG_LEVEL_WARN, "acpi: _S5 not found\n");
        }
    }

    return 0;
}

void acpi_shutdown(void) {
    if (!acpi_info.s5_valid || !acpi_info.pm1a_cnt_blk) {
        log_print(LOG_LEVEL_WARN, "acpi: no _S5 data, can't shutdown\n");
        return;
    }

    log_printf(LOG_LEVEL_DEBUG, "acpi: shutdown (SLP_TYPa=%u SLP_TYPb=%u PM1a=0x%x PM1b=0x%x)\r\n",
                 acpi_info.s5_slp_typa, acpi_info.s5_slp_typb,
                 acpi_info.pm1a_cnt_blk, acpi_info.pm1b_cnt_blk);

    __asm__ __volatile__("cli");

    uint16_t pm1a_val = ((uint16_t)acpi_info.s5_slp_typa << 10) | (1 << 13);
    outw(acpi_info.pm1a_cnt_blk, pm1a_val);

    if (acpi_info.pm1b_cnt_blk) {
        uint16_t pm1b_val = ((uint16_t)acpi_info.s5_slp_typb << 10) | (1 << 13);
        outw(acpi_info.pm1b_cnt_blk, pm1b_val);
    }

    for (volatile int i = 0; i < 100000000; i++)
        __asm__ __volatile__("pause");

    log_print(LOG_LEVEL_ERROR, "acpi: shutdown failed, halting\r\n");
    for (;;)
        __asm__ __volatile__("hlt");
}
