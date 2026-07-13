#include "acpi.h"
#include "debug.h"
#include "vmm.h"
#include "idt.h"

static int acpi_ok = 0;
static uint16_t pm1a_cnt_blk = 0;
static uint8_t sleep_type_a = 0;
static uint8_t has_reset_reg = 0;
static uint16_t reset_reg_addr = 0;
static uint8_t reset_value = 0;
static uint32_t smi_cmd = 0;
static uint8_t acpi_enable_val = 0;

static uint8_t acpi_checksum(void *ptr, uint32_t len) {
    uint8_t sum = 0;
    uint8_t *p = (uint8_t *)ptr;
    for (uint32_t i = 0; i < len; i++)
        sum += p[i];
    return sum;
}

static void read_phys(uint32_t phys, void *dst, uint32_t len) {
    uint32_t off = 0;
    while (off < len) {
        uint32_t page = (phys + off) & ~0xFFF;
        uint8_t *mapped = (uint8_t *)vmm_temp_map(page);
        uint32_t copy = 4096 - ((phys + off) - page);
        if (copy > len - off) copy = len - off;
        for (uint32_t i = 0; i < copy; i++)
            ((uint8_t *)dst)[off + i] = mapped[(phys + off) - page + i];
        vmm_temp_unmap();
        off += copy;
    }
}

int acpi_init(void) {
    debug_printf("acpi: probing...\r\n");

    uint32_t rsdt_phys = 0;
    int found = 0;

    for (uint32_t addr = 0xE0000; addr < 0x100000; addr += 16) {
        uint8_t buf[20];
        read_phys(addr, buf, 20);
        if (buf[0] == 'R' && buf[1] == 'S' && buf[2] == 'D' &&
            buf[3] == ' ' && buf[4] == 'P' && buf[5] == 'T' &&
            buf[6] == 'R' && buf[7] == ' ') {
            if (acpi_checksum(buf, 20) == 0) {
                rsdt_phys = (uint32_t)buf[16] | ((uint32_t)buf[17] << 8) |
                            ((uint32_t)buf[18] << 16) | ((uint32_t)buf[19] << 24);
                debug_printf("acpi: RSDP at 0x%x rev=%d\r\n", addr, buf[8]);
                found = 1;
                break;
            }
        }
    }

    if (!found) {
        debug_print("acpi: RSDP not found\r\n");
        return -1;
    }

    debug_printf("acpi: RSDT at 0x%x\r\n", rsdt_phys);

    uint8_t rsdt_hdr_buf[36];
    read_phys(rsdt_phys, rsdt_hdr_buf, 36);
    uint32_t rsdt_len = (uint32_t)rsdt_hdr_buf[4] | ((uint32_t)rsdt_hdr_buf[5] << 8) |
                        ((uint32_t)rsdt_hdr_buf[6] << 16) | ((uint32_t)rsdt_hdr_buf[7] << 24);
    uint32_t num_entries = (rsdt_len - 36) / 4;

    for (uint32_t i = 0; i < num_entries; i++) {
        uint32_t entry_addr = rsdt_phys + 36 + i * 4;
        uint8_t ebuf[4];
        read_phys(entry_addr, ebuf, 4);
        uint32_t entry_phys = (uint32_t)ebuf[0] | ((uint32_t)ebuf[1] << 8) |
                              ((uint32_t)ebuf[2] << 16) | ((uint32_t)ebuf[3] << 24);

        uint8_t sig[4];
        read_phys(entry_phys, sig, 4);
        uint8_t len_buf[4];
        read_phys(entry_phys + 4, len_buf, 4);
        uint32_t entry_len = (uint32_t)len_buf[0] | ((uint32_t)len_buf[1] << 8) |
                             ((uint32_t)len_buf[2] << 16) | ((uint32_t)len_buf[3] << 24);

        if (sig[0] == 'F' && sig[1] == 'A' && sig[2] == 'C' && sig[3] == 'P') {
            debug_printf("acpi: FADT at 0x%x len=%d\r\n", entry_phys, entry_len);

            uint8_t fadt_buf[244];
            uint32_t fadt_size = entry_len < 244 ? entry_len : 244;
            read_phys(entry_phys, fadt_buf, fadt_size);

            pm1a_cnt_blk = (uint16_t)((uint32_t)fadt_buf[64] | ((uint32_t)fadt_buf[65] << 8) |
                                      ((uint32_t)fadt_buf[66] << 16) | ((uint32_t)fadt_buf[67] << 24));
            debug_printf("acpi: PM1a_CNT_BLK=0x%x\r\n", pm1a_cnt_blk);

            uint32_t dsdt_phys = (uint32_t)fadt_buf[40] | ((uint32_t)fadt_buf[41] << 8) |
                                 ((uint32_t)fadt_buf[42] << 16) | ((uint32_t)fadt_buf[43] << 24);
            debug_printf("acpi: DSDT at 0x%x\r\n", dsdt_phys);

            uint8_t dsdt_hdr[36];
            read_phys(dsdt_phys, dsdt_hdr, 36);
            uint32_t dsdt_len = (uint32_t)dsdt_hdr[4] | ((uint32_t)dsdt_hdr[5] << 8) |
                                ((uint32_t)dsdt_hdr[6] << 16) | ((uint32_t)dsdt_hdr[7] << 24);

            uint32_t dsdt_search = dsdt_len > 32768 ? 32768 : dsdt_len;
            uint8_t *dsdt_copy = (uint8_t *)kmalloc(dsdt_search);
            if (dsdt_copy) {
                read_phys(dsdt_phys, dsdt_copy, dsdt_search);
                debug_printf("acpi: DSDT first 32:");
                for (uint32_t dd = 0; dd < 32; dd++) debug_printf(" %x", dsdt_copy[dd]);
                debug_printf("\r\n");
                for (uint32_t j = 24; j < dsdt_search - 7; j++) {
                    if (dsdt_copy[j] == 0x08 &&
                        dsdt_copy[j+1] == 0x5F &&
                        dsdt_copy[j+2] == 0x53 &&
                        dsdt_copy[j+3] == 0x35) {
                        debug_printf("acpi: _S5 at+%d:", j);
                        for (uint32_t d = 0; d < 16 && j+d < dsdt_search; d++)
                            debug_printf(" %x", dsdt_copy[j+d]);
                        debug_printf("\r\n");
                        if (j+5 < dsdt_search && dsdt_copy[j+5] == 0x12) {
                            sleep_type_a = 7;
                            uint32_t elem = j + 8;
                            int ei = 0;
                            while (elem < dsdt_search - 1 && ei < 4) {
                                debug_printf("acpi: _S5 elem[%d]=%x\r\n", ei, dsdt_copy[elem]);
                                if (dsdt_copy[elem] == 0x0A) {
                                    if (ei == 1) { sleep_type_a = dsdt_copy[elem + 1]; debug_printf("acpi: _S5 val=%d\r\n", sleep_type_a); }
                                    elem += 2; ei++;
                                } else if (dsdt_copy[elem] == 0x00 || dsdt_copy[elem] == 0x01) {
                                    if (ei == 1) { sleep_type_a = dsdt_copy[elem]; debug_printf("acpi: _S5 val=%d\r\n", sleep_type_a); }
                                    elem += 1; ei++;
                                } else {
                                    debug_printf("acpi: _S5 unknown elem byte %x\r\n", dsdt_copy[elem]);
                                    break;
                                }
                            }
                        }
                        break;
                    }
                }
                kfree(dsdt_copy);
            }

            if (sleep_type_a == 0) {
                debug_print("acpi: _S5 not found, using QEMU default 7\r\n");
                sleep_type_a = 7;
            }

            smi_cmd = (uint32_t)fadt_buf[48] | ((uint32_t)fadt_buf[49] << 8) |
                       ((uint32_t)fadt_buf[50] << 16) | ((uint32_t)fadt_buf[51] << 24);
            acpi_enable_val = fadt_buf[52];
            if (smi_cmd) debug_printf("acpi: SMI_CMD=0x%x ACPI_ENABLE=0x%x\r\n", smi_cmd, acpi_enable_val);

            if (fadt_size >= 128) {
                uint8_t rasc = fadt_buf[119];
                uint8_t rlen = fadt_buf[120];
                has_reset_reg = (rasc == 1 && rlen == 1);
                if (has_reset_reg) {
                    reset_reg_addr = (uint16_t)fadt_buf[116] | ((uint16_t)fadt_buf[117] << 8);
                    reset_value = fadt_buf[121];
                    debug_printf("acpi: ResetReg at 0x%x val=0x%x\r\n", reset_reg_addr, reset_value);
                }
            }

            acpi_ok = 1;
            debug_print("acpi: initialized\r\n");
            return 0;
        }
    }

    debug_print("acpi: FADT not found\r\n");
    return -1;
}

int acpi_available(void) {
    return acpi_ok;
}

void acpi_poweroff(void) {
    if (!acpi_ok || !pm1a_cnt_blk) {
        debug_print("acpi: cannot poweroff (not initialized)\r\n");
        return;
    }
    debug_print("acpi: poweroff (stub)\r\n");
}

void acpi_reboot(void) {
    if (has_reset_reg && reset_reg_addr) {
        debug_printf("acpi: reboot via reset reg 0x%x\r\n", reset_reg_addr);
        __asm__ __volatile__("cli");
        outb(reset_reg_addr, reset_value);
        for (int i = 0; i < 100000; i++) __asm__ __volatile__("pause");
    }
    debug_print("acpi: reboot via keyboard controller\r\n");
    __asm__ __volatile__("cli");
    for (volatile int i = 0; i < 100000; i++);
    outb(0x64, 0xFE);
    for (;;) __asm__ __volatile__("hlt");
}
