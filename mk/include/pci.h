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


#ifndef PCI_H
#define PCI_H

#include <stdint.h>

#define PCI_VENDOR_ID      0x00
#define PCI_DEVICE_ID      0x02
#define PCI_COMMAND        0x04
#define PCI_STATUS         0x06
#define PCI_REVISION_ID    0x08
#define PCI_PROG_IF        0x09
#define PCI_SUBCLASS       0x0A
#define PCI_CLASS          0x0B
#define PCI_CACHE_LINE     0x0C
#define PCI_LATENCY_TIMER  0x0D
#define PCI_HEADER_TYPE    0x0E
#define PCI_BIST           0x0F
#define PCI_BAR0           0x10
#define PCI_BAR1           0x14
#define PCI_BAR2           0x18
#define PCI_BAR3           0x1C
#define PCI_BAR4           0x20
#define PCI_BAR5           0x24
#define PCI_CAP_PTR        0x34
#define PCI_INTERRUPT_LINE 0x3C
#define PCI_INTERRUPT_PIN  0x3D
#define PCI_SECONDARY_BUS  0x19

#define PCI_CLASS_MASS_STORAGE   0x01
#define PCI_CLASS_NETWORK        0x02
#define PCI_CLASS_DISPLAY        0x03
#define PCI_CLASS_MULTIMEDIA     0x04
#define PCI_CLASS_MEMORY         0x05
#define PCI_CLASS_BRIDGE         0x06
#define PCI_CLASS_SIMPLE_COMM    0x07
#define PCI_CLASS_SERIAL         0x0C

#define PCI_SUBCLASS_IDE         0x01
#define PCI_SUBCLASS_AHCI        0x06
#define PCI_SUBCLASS_NVME        0x08
#define PCI_SUBCLASS_ETHERNET    0x00
#define PCI_SUBCLASS_USB         0x03
#define PCI_SUBCLASS_SATA        0x06

#define PCI_HEADER_TYPE_BRIDGE   0x01
#define PCI_HEADER_TYPE_MULTIFN  0x80

#define PCI_BAR_TYPE_MEM(x)      (!((x) & 1))
#define PCI_BAR_TYPE_IO(x)       ((x) & 1)
#define PCI_BAR_ADDR_MEM(x)      ((x) & ~0xF)
#define PCI_BAR_ADDR_IO(x)       ((x) & ~0x3)
#define PCI_BAR_64BIT(x)         (((x) & 0x6) == 0x4)

typedef struct pci_addr {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
} pci_addr_t;

typedef struct pci_device {
    pci_addr_t addr;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  header_type;
    uint32_t bars[6];
    uint32_t bar_sizes[6];
    uint8_t  irq_line;
    char     name_buf[20];
} pci_device_t;

/* Userspace-friendly device info (no kernel-internal addr type) */
typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  irq_line;
    uint64_t bar[6];
} pci_device_info_t;

/* Look up the Nth PCI device matching class/subclass (0xFF = any).
 * Fills *info and returns 0 on success, -1 if not found. */
int pci_device_info(uint8_t cls, uint8_t subclass, int index, pci_device_info_t *info);

uint32_t pci_config_read(pci_addr_t addr, uint8_t offset);
void pci_config_write(pci_addr_t addr, uint8_t offset, uint32_t val);
void pci_init(void);
int pci_device_count(void);
const pci_device_t *pci_device_get(int index);
int pci_find_device(uint8_t cls, uint8_t subclass, pci_device_t *out);

#endif
