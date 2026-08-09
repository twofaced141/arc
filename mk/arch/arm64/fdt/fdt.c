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


/* fdt.c — Minimal Flattened Device Tree parser for ARM64.
 *
 * Only supports what we need: find PCI ECAM base address from QEMU's DTB.
 * The DTB is placed in RAM by QEMU; its address is passed to the kernel
 * in x0 at boot.  We parse it *after* the MMU is on via identity-mapping
 * (the DTB sits in identity-mapped RAM at 0x40000000+).
 */

#include "fdt.h"
#include "debug.h"
#include "string.h"
#include "bus.h"
#include "device.h"
#include <arc/boot.h>

/* RAM bounds (must match vm/vmm.c). */
#define RAM_BASE  0x40000000ULL
#define RAM_SIZE  0x04000000ULL   /* 64 MB */

/* Snapshot of the parsed DTB (set by arc_boot_init_fdt, consumed by
 * the platform bus scan). */
static const uint8_t *saved_fdt;
static int saved_off_struct;
static int saved_off_strings;
static int saved_size_struct;


#define FDT_MAGIC          0xD00DFEED
#define FDT_BEGIN_NODE     0x00000001
#define FDT_END_NODE       0x00000002
#define FDT_PROP           0x00000003
#define FDT_NOP            0x00000004
#define FDT_END            0x00000009

/* Big-endian read helpers (DTB is always big-endian). */
static inline uint32_t be32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  | (uint32_t)b[3];
}

static inline uint64_t be64(const void *p) {
    return ((uint64_t)be32(p) << 32) | be32((const uint8_t *)p + 4);
}

/* Align offset up to next 4-byte boundary. */
static inline int align4(int off) {
    return (off + 3) & ~3;
}

/* Compare a null-terminated string against a buffer that may or may not
 * be null-terminated within the first `maxlen` bytes.
 * Returns 1 if equal, 0 otherwise. */
static int str_eq_buf(const char *s, const char *buf, int maxlen) {
    int i = 0;
    while (*s && i < maxlen) {
        if (*s != buf[i]) return 0;
        s++; i++;
    }
    /* If match string ended, we need either null in buf or exact length. */
    return *s == '\0' && (i < maxlen ? buf[i] == '\0' : 1);
}

/* Check if `compat` appears anywhere in a concatenated compatible string
 * property (sequence of NUL-terminated strings). */
static int compat_match(const char *prop, int prop_len, const char *compat) {
    int clen = (int)strlen(compat);
    int pos = 0;
    while (pos + clen <= prop_len) {
        if (prop[pos] == '\0') { pos++; continue; }
        if (str_eq_buf(compat, &prop[pos], prop_len - pos))
            return 1;
        /* skip to next null */
        while (pos < prop_len && prop[pos] != '\0') pos++;
    }
    return 0;
}

/* FDT traversal — find a node by compatible string, then read its
 * `reg` property.  Two-phase approach: first find the matching node,
 * then read reg.  This handles the case where 'reg' appears before
 * 'compatible' in the node's property list (QEMU virt's DTB does this). */
/* Phase 1: walk the structure block, find node with matching compatible,
 * return the offset right after its FDT_BEGIN_NODE token (its properties).
 * Scans for a property named 'compatible'; when found, calls compat_match.
 * Records both the matching node's `off` (after BEGIN_NODE) and its depth.
 *
 * Returns node_props_off on success, -1 on not found.
 */
static int fdt_find_node(const uint8_t *fdt,
                          int off_struct, int off_strings,
                          int struct_size,
                          const char *compat)
{
    int depth = 0;
    int off = off_struct;
    int node_props_off = -1; /* saved offset right after current node's BEGIN_NODE+name */

    while (off + 4 <= off_struct + struct_size) {
        uint32_t tok = be32(&fdt[off]);

        switch (tok) {
        case FDT_BEGIN_NODE: {
            depth++;
            const char *name = (const char *)&fdt[off + 4];
            int name_len = (int)strlen(name);
            node_props_off = align4(off + 4 + name_len + 1);
            off = node_props_off;
            break;
        }

        case FDT_PROP: {
            if (off + 12 > off_struct + struct_size)
                return -1;
            int len   = be32(&fdt[off + 4]);
            int name_off = be32(&fdt[off + 8]);
            int val_off = off + 12;
            int prop_end = align4(val_off + len);

            if (val_off + len > off_struct + struct_size)
                return -1;

            const char *prop_name = (name_off + off_strings < off_strings + struct_size)
                ? (const char *)&fdt[off_strings + name_off]
                : "";

            if (strcmp(prop_name, "compatible") == 0) {
                if (compat_match((const char *)&fdt[val_off], len, compat))
                    return node_props_off;
            }

            off = prop_end;
            break;
        }

        case FDT_END_NODE:
            depth--;
            off += 4;
            break;

        case FDT_END:
            return -1;

        case FDT_NOP:
            off += 4;
            break;

        default:
            off += 4;
            break;
        }
    }
    return -1;
}

/* Phase 2: at a node (just past its FDT_BEGIN_NODE), walk properties
 * to find 'reg'.  Return parsed (address, size) using parent cells.
 * Returns 0 on success, -1 if reg not found or error.
 *
 * depth must be 0 when entering (we are at the node's first property)
 * and we leave when FDT_END_NODE is encountered (depth becomes -1).
 */
static int fdt_read_node_reg(const uint8_t *fdt,
                              int off_struct, int off_strings,
                              int struct_size,
                              int node_off,
                              uint64_t *out_addr, uint64_t *out_size,
                              int parent_addr_cells, int parent_size_cells)
{
    int depth = 0; /* we start at the node's property level */
    int off = node_off;

    while (off + 4 <= off_struct + struct_size) {
        uint32_t tok = be32(&fdt[off]);

        switch (tok) {
        case FDT_BEGIN_NODE: {
            depth++;
            const char *name = (const char *)&fdt[off + 4];
            int name_len = (int)strlen(name);
            off = align4(off + 4 + name_len + 1);
            break;
        }

        case FDT_PROP: {
            if (off + 12 > off_struct + struct_size)
                return -1;
            int len   = be32(&fdt[off + 4]);
            int name_off = be32(&fdt[off + 8]);
            int val_off = off + 12;
            int prop_end = align4(val_off + len);

            if (val_off + len > off_struct + struct_size)
                return -1;

            const char *prop_name = (name_off + off_strings < off_strings + struct_size)
                ? (const char *)&fdt[off_strings + name_off]
                : "";

            if (strcmp(prop_name, "reg") == 0) {
                int cell_bytes = (parent_addr_cells + parent_size_cells) * 4;
                if (len < cell_bytes)
                    return -1;

                const uint8_t *regp = &fdt[val_off];
                uint64_t addr = 0;
                uint64_t sz   = 0;

                if (parent_addr_cells == 2)
                    addr = be64(regp);
                else
                    addr = be32(regp);

                regp += parent_addr_cells * 4;

                if (parent_size_cells == 2)
                    sz = be64(regp);
                else
                    sz = be32(regp);

                *out_addr = addr;
                *out_size = sz;
                return 0;
            }

            off = prop_end;
            break;
        }

        case FDT_END_NODE: {
            if (depth == 0)
                return -1;
            depth--;
            off += 4;
            break;
        }

        case FDT_END:
            return -1;

        case FDT_NOP:
            off += 4;
            break;

        default:
            off += 4;
            break;
        }
    }
    return -1;
}

/* Combined: find node by compatible, then reg. */
static int fdt_get_reg_by_compat(const uint8_t *fdt,
                                  int off_struct, int off_strings,
                                  int struct_size,
                                  const char *compat,
                                  uint64_t *out_addr, uint64_t *out_size,
                                  int parent_addr_cells, int parent_size_cells)
{
    int node_off = fdt_find_node(fdt, off_struct, off_strings, struct_size, compat);
    if (node_off < 0)
        return -1;

    return fdt_read_node_reg(fdt, off_struct, off_strings, struct_size,
                             node_off, out_addr, out_size,
                             parent_addr_cells, parent_size_cells);
}


/* Scan physical RAM for a valid FDT blob.  Returns its address, or 0.
 * Scans from kernel_end upward to the end of RAM (identity-mapped area).
 * Checks only page-aligned addresses for speed. */
const void *fdt_scan_ram(uint64_t kernel_end) {
    uint64_t start = (kernel_end + 0xFFF) & ~0xFFFULL;
    uint64_t end   = RAM_BASE + RAM_SIZE;

    /* Also scan from the very start of RAM — QEMU may place the DTB
     * before the kernel load point (e.g. at 0x40000000). */
    if (start > RAM_BASE) start = RAM_BASE;
    if (start >= end) return NULL;

    debug_printf("fdt: scanning RAM from 0x%lx to 0x%lx\n", start, end);

    const void *found = NULL;
    for (uint64_t addr = start; addr < end; addr += 0x1000) {
        const volatile uint8_t *p = (const volatile uint8_t *)addr;
        /* FDT_MAGIC = 0xD00DFEED, stored big-endian in memory. */
        if (p[0] == 0xD0 && p[1] == 0x0D && p[2] == 0xFE && p[3] == 0xED) {
            uint32_t totalsize = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16)
                               | ((uint32_t)p[6] << 8)  | p[7];
            if (totalsize >= 256 && totalsize <= RAM_SIZE) {
                found = (const void *)(uintptr_t)addr;
                debug_printf("fdt: found at phys 0x%lx (size %u)\n", addr, totalsize);
                break;
            }
        }
    }

    if (!found)
        debug_print("fdt: DTB not found in RAM\n");
    return found;
}


/* Collect the MPIDR values of all /cpus/cpu@* nodes into mpidrs[].
 * Returns the number of CPUs found (up to max), or 0 on failure. */
int fdt_get_cpus(const void *dtb, uint64_t *mpidrs, int max) {
    if (!dtb || !mpidrs || max <= 0) return 0;

    const uint8_t *fdt = (const uint8_t *)dtb;
    if (be32(&fdt[0]) != FDT_MAGIC) return 0;

    uint32_t off_dt_struct  = be32(&fdt[8]);
    uint32_t off_dt_strings = be32(&fdt[12]);
    uint32_t version        = be32(&fdt[20]);
    uint32_t size_dt_struct = (version >= 17) ? be32(&fdt[0x24])
                                              : be32(&fdt[0x10]);

    int off = (int)off_dt_struct;
    int depth = 0;
    int cpus_off = -1;          /* /cpus props offset */
    int cpus_addr_cells = 1;

    /* Pass 1: find /cpus at depth 1 and read its #address-cells. */
    while (off + 4 <= (int)off_dt_struct + (int)size_dt_struct) {
        uint32_t tok = be32(&fdt[off]);
        if (tok == FDT_BEGIN_NODE) {
            depth++;
            const char *name = (const char *)&fdt[off + 4];
            int name_len = (int)strlen(name);
            int props_off = align4(off + 4 + name_len + 1);
            if (depth == 1 && cpus_off < 0) {
                if (strncmp(name, "cpus", 4) == 0)
                    cpus_off = props_off;
            }
            off = props_off;
        } else if (tok == FDT_PROP) {
            int len = be32(&fdt[off + 4]);
            int name_off = be32(&fdt[off + 8]);
            int val_off = off + 12;
            if (cpus_off >= 0 && off >= cpus_off) {
                const char *pn = (const char *)&fdt[off_dt_strings + name_off];
                if (strcmp(pn, "#address-cells") == 0 && len >= 4)
                    cpus_addr_cells = (int)be32(&fdt[val_off]);
            }
            off = align4(val_off + len);
        } else if (tok == FDT_END_NODE) {
            depth--;
            off += 4;
            if (depth == 0)
                break;          /* left /cpus — done with pass 1 */
        } else if (tok == FDT_END) {
            break;
        } else {
            off += 4;
        }
    }

    if (cpus_off < 0)
        return 0;

    /* Pass 2: walk the children of /cpus (depth 2), read reg of
     * cpu@* nodes as the MPIDR. */
    int count = 0;
    off = cpus_off;
    while (off + 4 <= (int)off_dt_struct + (int)size_dt_struct) {
        uint32_t tok = be32(&fdt[off]);
        if (tok == FDT_BEGIN_NODE) {
            depth++;
            const char *name = (const char *)&fdt[off + 4];
            int name_len = (int)strlen(name);
            int props_off = align4(off + 4 + name_len + 1);
            if (depth == 2 && strncmp(name, "cpu", 3) == 0 && count < max) {
                uint64_t mpidr = 0;
                int po = props_off;
                /* read reg within this node */
                while (po + 4 <= (int)off_dt_struct + (int)size_dt_struct) {
                    uint32_t t = be32(&fdt[po]);
                    if (t == FDT_PROP) {
                        int len = be32(&fdt[po + 4]);
                        int name_off = be32(&fdt[po + 8]);
                        int val_off = po + 12;
                        const char *pn = (const char *)&fdt[off_dt_strings + name_off];
                        if (strcmp(pn, "reg") == 0) {
                            for (int i = 0; i < cpus_addr_cells; i++)
                                mpidr = (mpidr << 32) | be32(&fdt[val_off + i * 4]);
                            break;
                        }
                        po = align4(val_off + len);
                    } else if (t == FDT_END_NODE) {
                        break;
                    } else {
                        po += 4;
                    }
                }
                mpidrs[count++] = mpidr;
            }
            off = props_off;
        } else if (tok == FDT_PROP) {
            int len = be32(&fdt[off + 4]);
            int val_off = off + 12;
            off = align4(val_off + len);
        } else if (tok == FDT_END_NODE) {
            depth--;
            off += 4;
            if (depth == 1)
                break;
        } else if (tok == FDT_END) {
            break;
        } else {
            off += 4;
        }
    }

    return count;
}

int fdt_get_pci_ecam(const void *dtb, uint64_t *base, uint64_t *size) {
    if (!dtb || !base || !size) return -1;

    const uint8_t *fdt = (const uint8_t *)dtb;

    /* Validate FDT magic. */
    if (be32(&fdt[0]) != FDT_MAGIC) {
        debug_print("fdt: bad magic — not a device tree\n");
        return -1;
    }

    uint32_t totalsize      = be32(&fdt[4]);
    uint32_t off_dt_struct  = be32(&fdt[8]);
    uint32_t off_dt_strings = be32(&fdt[12]);
    uint32_t version        = be32(&fdt[20]);

    /* We need version >= 16 for size_dt_strings / size_dt_struct. */
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;

    if (version >= 17) {
        size_dt_strings = be32(&fdt[0x20]);
        size_dt_struct  = be32(&fdt[0x24]);
    } else if (version >= 16) {
        size_dt_strings = be32(&fdt[0x20]);
        size_dt_struct  = totalsize - off_dt_struct;
    } else {
        debug_print("fdt: unsupported version\n");
        return -1;
    }

    debug_printf("fdt: totalsize=%u, struct at %u (%u bytes), "
                 "strings at %u (%u bytes)\n",
                 totalsize, off_dt_struct, size_dt_struct,
                 off_dt_strings, size_dt_strings);

    /* Sanity bounds. */
    if (off_dt_struct + size_dt_struct > totalsize ||
        off_dt_strings + size_dt_strings > totalsize) {
        debug_print("fdt: header offsets exceed totalsize\n");
        return -1;
    }

    /* Search for pci-host-ecam-generic in the structure block.
     * Default root address-cells=2, size-cells=2 (ARM64 QEMU virt). */
    int ret = fdt_get_reg_by_compat(fdt, (int)off_dt_struct, (int)off_dt_strings,
                                     (int)size_dt_struct,
                                     "pci-host-ecam-generic",
                                     base, size, 2, 2);
    if (ret < 0) {
        debug_print("fdt: no pci-host-ecam-generic node found\n");
        return -1;
    }

    debug_printf("fdt: PCI ECAM at 0x%lx size 0x%lx\n", *base, *size);
    return 0;
}


/* Find a node whose name starts with `prefix` (e.g. "memory@").
 * Returns the offset right after its FDT_BEGIN_NODE + name (its
 * properties), or -1 if not found. */
static int fdt_find_node_by_name(const uint8_t *fdt,
                                 int off_struct, int off_strings,
                                 int struct_size,
                                 const char *prefix)
{
    int depth = 0;
    int off = off_struct;
    size_t plen = strlen(prefix);

    while (off + 4 <= off_struct + struct_size) {
        uint32_t tok = be32(&fdt[off]);

        switch (tok) {
        case FDT_BEGIN_NODE: {
            depth++;
            const char *name = (const char *)&fdt[off + 4];
            int name_len = (int)strlen(name);
            int props_off = align4(off + 4 + name_len + 1);
            if (strncmp(name, prefix, plen) == 0)
                return props_off;
            off = props_off;
            break;
        }

        case FDT_PROP: {
            if (off + 12 > off_struct + struct_size)
                return -1;
            int len   = be32(&fdt[off + 4]);
            int val_off = off + 12;
            off = align4(val_off + len);
            break;
        }

        case FDT_END_NODE:
            depth--;
            off += 4;
            break;

        case FDT_END:
            return -1;

        case FDT_NOP:
            off += 4;
            break;

        default:
            off += 4;
            break;
        }
    }
    return -1;
}

/* Read a string property of the node at node_off (props offset).
 * Returns 0 on success (copied, NUL-terminated), -1 if absent. */
static int fdt_node_prop_str(const uint8_t *fdt,
                             int off_struct, int off_strings,
                             int struct_size,
                             int node_off,
                             const char *prop, char *out, int maxlen)
{
    int depth = 0;
    int off = node_off;

    while (off + 4 <= off_struct + struct_size) {
        uint32_t tok = be32(&fdt[off]);

        switch (tok) {
        case FDT_BEGIN_NODE:
            depth++;
            off = align4(off + 4 + (int)strlen((const char *)&fdt[off + 4]) + 1);
            break;

        case FDT_PROP: {
            if (off + 12 > off_struct + struct_size)
                return -1;
            int len      = be32(&fdt[off + 4]);
            int name_off = be32(&fdt[off + 8]);
            int val_off  = off + 12;
            int prop_end = align4(val_off + len);

            if (val_off + len > off_struct + struct_size)
                return -1;

            const char *prop_name = (name_off + off_strings < off_strings + struct_size)
                ? (const char *)&fdt[off_strings + name_off]
                : "";

            if (strcmp(prop_name, prop) == 0) {
                if (len > maxlen - 1) len = maxlen - 1;
                memcpy(out, &fdt[val_off], (size_t)len);
                out[len] = '\0';
                return 0;
            }

            off = prop_end;
            break;
        }

        case FDT_END_NODE: {
            if (depth == 0)
                return -1;
            depth--;
            off += 4;
            break;
        }

        case FDT_END:
            return -1;

        case FDT_NOP:
            off += 4;
            break;

        default:
            off += 4;
            break;
        }
    }
    return -1;
}

/* Read a 32-bit property of the node at node_off. */
static int fdt_node_prop_u32(const uint8_t *fdt,
                             int off_struct, int off_strings,
                             int struct_size,
                             int node_off,
                             const char *prop, uint32_t *out)
{
    int depth = 0;
    int off = node_off;

    while (off + 4 <= off_struct + struct_size) {
        uint32_t tok = be32(&fdt[off]);

        switch (tok) {
        case FDT_BEGIN_NODE:
            depth++;
            off = align4(off + 4 + (int)strlen((const char *)&fdt[off + 4]) + 1);
            break;

        case FDT_PROP: {
            if (off + 12 > off_struct + struct_size)
                return -1;
            int len      = be32(&fdt[off + 4]);
            int name_off = be32(&fdt[off + 8]);
            int val_off  = off + 12;
            int prop_end = align4(val_off + len);

            if (val_off + len > off_struct + struct_size)
                return -1;

            const char *prop_name = (name_off + off_strings < off_strings + struct_size)
                ? (const char *)&fdt[off_strings + name_off]
                : "";

            if (strcmp(prop_name, prop) == 0) {
                if (len < 4)
                    return -1;
                *out = be32(&fdt[val_off]);
                return 0;
            }

            off = prop_end;
            break;
        }

        case FDT_END_NODE: {
            if (depth == 0)
                return -1;
            depth--;
            off += 4;
            break;
        }

        case FDT_END:
            return -1;

        case FDT_NOP:
            off += 4;
            break;

        default:
            off += 4;
            break;
        }
    }
    return -1;
}

/* Boot info storage (same pattern as the multiboot2 frontend in boot/). */
#define FDT_BOOT_MAX_REGIONS 16
#define FDT_BOOT_CMDLINE_MAX 512

static struct arc_memory_region fdt_boot_regions[FDT_BOOT_MAX_REGIONS];
static struct arc_boot_info   fdt_boot_info;
static char                   fdt_boot_cmdline[FDT_BOOT_CMDLINE_MAX];

/* Parse the FDT header.  Returns 0 on success with structure block
 * bounds filled in, -1 on error. */
static int fdt_header_bounds(const uint8_t *fdt,
                             int *off_struct, int *off_strings,
                             int *size_struct)
{
    if (!fdt || be32(&fdt[0]) != FDT_MAGIC)
        return -1;

    uint32_t totalsize      = be32(&fdt[4]);
    uint32_t off_dt_struct  = be32(&fdt[8]);
    uint32_t off_dt_strings = be32(&fdt[12]);
    uint32_t version        = be32(&fdt[20]);

    uint32_t size_dt_strings;
    uint32_t size_dt_struct;

    if (version >= 17) {
        size_dt_strings = be32(&fdt[0x20]);
        size_dt_struct  = be32(&fdt[0x24]);
    } else if (version >= 16) {
        size_dt_strings = be32(&fdt[0x20]);
        size_dt_struct  = totalsize - off_dt_struct;
    } else {
        return -1;
    }

    if (off_dt_struct + size_dt_struct > totalsize ||
        off_dt_strings + size_dt_strings > totalsize)
        return -1;

    *off_struct  = (int)off_dt_struct;
    *off_strings = (int)off_dt_strings;
    *size_struct = (int)size_dt_struct;
    return 0;
}

struct arc_boot_info *arc_boot_init_fdt(const void *dtb) {
    memset(&fdt_boot_info, 0, sizeof(fdt_boot_info));
    fdt_boot_info.magic   = ARC_BOOT_MAGIC;
    fdt_boot_info.version = ARC_BOOT_VERSION;
    fdt_boot_info.memory_map = fdt_boot_regions;

    const uint8_t *fdt = (const uint8_t *)dtb;

    int off_struct, off_strings, size_struct;
    if (fdt_header_bounds(fdt, &off_struct, &off_strings, &size_struct) < 0) {
        debug_print("fdt: arc_boot_init_fdt: no valid DTB\n");
        return &fdt_boot_info;
    }

    /* Keep a snapshot so the platform bus can enumerate DT devices. */
    saved_fdt           = fdt;
    saved_off_struct    = off_struct;
    saved_off_strings   = off_strings;
    saved_size_struct   = size_struct;

    /* --- Memory map: /memory@... (reg = addr-cells/size-cells 2/2
     * on QEMU virt, same assumption as fdt_get_pci_ecam). --- */
    int off = fdt_find_node_by_name(fdt, off_struct, off_strings,
                                    size_struct, "memory@");
    if (off >= 0) {
        uint64_t base, size;
        if (fdt_read_node_reg(fdt, off_struct, off_strings, size_struct,
                              off, &base, &size, 2, 2) == 0 &&
            size > 0 && fdt_boot_info.memory_entries < FDT_BOOT_MAX_REGIONS) {
            fdt_boot_regions[fdt_boot_info.memory_entries].base   = base;
            fdt_boot_regions[fdt_boot_info.memory_entries].length = size;
            fdt_boot_regions[fdt_boot_info.memory_entries].type   = ARC_MEM_USABLE;
            fdt_boot_info.memory_entries++;
        }
    }

    /* --- Command line: /chosen/bootargs --- */
    off = fdt_find_node_by_name(fdt, off_struct, off_strings,
                                size_struct, "chosen");
    if (off >= 0) {
        if (fdt_node_prop_str(fdt, off_struct, off_strings, size_struct,
                              off, "bootargs",
                              fdt_boot_cmdline, FDT_BOOT_CMDLINE_MAX) == 0) {
            fdt_boot_info.flags |= ARC_BOOT_HAS_CMDLINE;
            fdt_boot_info.cmdline = fdt_boot_cmdline;
        }
    }

    /* --- Framebuffer: /chosen/framebuffer@... (QEMU virt) --- */
    off = fdt_find_node_by_name(fdt, off_struct, off_strings,
                                size_struct, "framebuffer@");
    if (off >= 0) {
        uint64_t fb_addr = 0, fb_size = 0;
        if (fdt_read_node_reg(fdt, off_struct, off_strings, size_struct,
                              off, &fb_addr, &fb_size, 2, 2) == 0 &&
            fb_size > 0) {
            fdt_boot_info.framebuffer.address = fb_addr;
            fdt_boot_info.framebuffer.format   = 0;
            fdt_node_prop_u32(fdt, off_struct, off_strings, size_struct,
                              off, "width", &fdt_boot_info.framebuffer.width);
            fdt_node_prop_u32(fdt, off_struct, off_strings, size_struct,
                              off, "height", &fdt_boot_info.framebuffer.height);
            fdt_node_prop_u32(fdt, off_struct, off_strings, size_struct,
                              off, "stride", &fdt_boot_info.framebuffer.pitch);
            fdt_boot_info.flags |= ARC_BOOT_HAS_FB;
        }
    }

    return &fdt_boot_info;
}


#define FDT_PLATFORM_MAX_DEVICES 64
#define FDT_PLATFORM_MAX_NAME    40
#define FDT_PLATFORM_MAX_COMPAT  64
#define FDT_WALK_MAX_DEPTH       16

struct fdt_platform_device {
    struct arc_device dev;
    char name[FDT_PLATFORM_MAX_NAME];
    char compatible[FDT_PLATFORM_MAX_COMPAT];
    struct arc_resource resources[ARC_DEV_MAX_RESOURCES];
};

static struct fdt_platform_device fdt_platform_devices[FDT_PLATFORM_MAX_DEVICES];
static int  fdt_platform_device_count;
static int  fdt_platform_bus_registered;

/* Find a property of the node at node_off; returns a pointer to its
 * value in the FDT blob, or NULL.  *out_len gets the value length. */
static const uint8_t *fdt_node_find_prop(const uint8_t *fdt,
                                         int off_struct, int off_strings,
                                         int struct_size,
                                         int node_off,
                                         const char *prop, int *out_len)
{
    int depth = 0;
    int off = node_off;

    while (off + 4 <= off_struct + struct_size) {
        uint32_t tok = be32(&fdt[off]);

        switch (tok) {
        case FDT_BEGIN_NODE:
            depth++;
            off = align4(off + 4 + (int)strlen((const char *)&fdt[off + 4]) + 1);
            break;

        case FDT_PROP: {
            if (off + 12 > off_struct + struct_size)
                return NULL;
            int len      = be32(&fdt[off + 4]);
            int name_off = be32(&fdt[off + 8]);
            int val_off  = off + 12;
            int prop_end = align4(val_off + len);

            if (val_off + len > off_struct + struct_size)
                return NULL;

            const char *prop_name = (name_off + off_strings < off_strings + struct_size)
                ? (const char *)&fdt[off_strings + name_off]
                : "";

            if (strcmp(prop_name, prop) == 0) {
                if (out_len) *out_len = len;
                return &fdt[val_off];
            }

            off = prop_end;
            break;
        }

        case FDT_END_NODE: {
            if (depth == 0)
                return NULL;
            depth--;
            off += 4;
            break;
        }

        case FDT_END:
            return NULL;

        case FDT_NOP:
            off += 4;
            break;

        default:
            off += 4;
            break;
        }
    }
    return NULL;
}

/* Create + register an arc_device for one DT node.  Resources are
 * copied into the device's own static storage. */
static void fdt_platform_add_device(const char *name, const char *compat,
                                    uint32_t phandle,
                                    const struct arc_resource *res, size_t nres,
                                    struct arc_device *parent)
{
    if (fdt_platform_device_count >= FDT_PLATFORM_MAX_DEVICES)
        return;

    struct fdt_platform_device *pd = &fdt_platform_devices[fdt_platform_device_count];
    memset(pd, 0, sizeof(*pd));

    int name_len = (int)strlen(name);
    if (name_len >= (int)sizeof(pd->name)) name_len = (int)sizeof(pd->name) - 1;
    memcpy(pd->name, name, (size_t)name_len);
    pd->name[name_len] = '\0';

    int compat_len = (int)strlen(compat);
    if (compat_len >= (int)sizeof(pd->compatible)) compat_len = (int)sizeof(pd->compatible) - 1;
    memcpy(pd->compatible, compat, (size_t)compat_len);
    pd->compatible[compat_len] = '\0';

    struct arc_device *dev = &pd->dev;
    dev->name       = pd->name;
    dev->compatible = pd->compatible;
    dev->phandle    = phandle;
    dev->type       = ARC_DEV_PLATFORM;
    dev->resources  = pd->resources;
    dev->resource_count = (nres < ARC_DEV_MAX_RESOURCES) ? nres : ARC_DEV_MAX_RESOURCES;
    for (size_t i = 0; i < dev->resource_count; i++)
        pd->resources[i] = res[i];

    fdt_platform_device_count++;
    if (parent)
        arc_device_add_child(parent, dev);
    arc_device_register(dev);
}

static int fdt_platform_bus_scan(struct arc_bus *bus) {
    (void)bus;
    const uint8_t *fdt = saved_fdt;
    if (!fdt) return 0;

    int off_struct    = saved_off_struct;
    int off_strings   = saved_off_strings;
    int size_struct   = saved_size_struct;

    /* Walk stack: per-depth inherited #address-cells/#size-cells and
     * the nearest ancestor that became an arc_device. */
    struct {
        uint32_t addr_cells;
        uint32_t size_cells;
        struct arc_device *dev;
    } walk[FDT_WALK_MAX_DEPTH];
    int depth = 0;
    int off = off_struct;

    walk[0].addr_cells = 2; /* arm64 default (root usually overrides) */
    walk[0].size_cells = 2;
    walk[0].dev = NULL;

    while (off + 4 <= off_struct + size_struct) {
        uint32_t tok = be32(&fdt[off]);

        if (tok == FDT_BEGIN_NODE) {
            depth++;
            if (depth >= FDT_WALK_MAX_DEPTH) return -1;

            const char *name = (const char *)&fdt[off + 4];
            /* QEMU emits an empty name for the root node. */
            if (name[0] == '\0')
                name = "/";
            int node_props = align4(off + 4 + (int)strlen(name) + 1);

            walk[depth].addr_cells = walk[depth - 1].addr_cells;
            walk[depth].size_cells = walk[depth - 1].size_cells;
            walk[depth].dev = NULL;

            /* Parse this node's properties. */
            uint32_t addr_cells = 0, size_cells = 0;
            int len = 0;
            const uint8_t *v = fdt_node_find_prop(fdt, off_struct, off_strings, size_struct,
                                                  node_props, "#address-cells", &len);
            if (v && len >= 4) addr_cells = be32(v);
            v = fdt_node_find_prop(fdt, off_struct, off_strings, size_struct,
                                   node_props, "#size-cells", &len);
            if (v && len >= 4) size_cells = be32(v);

            if (addr_cells >= 1 && addr_cells <= 2)
                walk[depth].addr_cells = addr_cells;
            if (size_cells >= 1 && size_cells <= 2)
                walk[depth].size_cells = size_cells;

            v = fdt_node_find_prop(fdt, off_struct, off_strings, size_struct,
                                   node_props, "compatible", &len);
            if (v && len > 0) {
                /* First NUL-terminated string of the compatible list. */
                const char *compat = (const char *)v;
                int clen = (int)strlen(compat);
                if (clen > len) clen = len;

                int disabled = 0;
                const uint8_t *sv = fdt_node_find_prop(fdt, off_struct, off_strings, size_struct,
                                                       node_props, "status", &len);
                if (sv && strncmp((const char *)sv, "disabled", 8) == 0)
                    disabled = 1;

                if (!disabled) {
                    struct arc_resource res[ARC_DEV_MAX_RESOURCES];
                    size_t nres = 0;

                    uint64_t reg_base = 0, reg_size = 0;
                    int has_reg = (fdt_read_node_reg(fdt, off_struct, off_strings, size_struct,
                                                     node_props, &reg_base, &reg_size,
                                                     (int)walk[depth - 1].addr_cells,
                                                     (int)walk[depth - 1].size_cells) == 0);
                    if (has_reg && nres < ARC_DEV_MAX_RESOURCES) {
                        res[nres].type  = ARC_RES_MMIO;
                        res[nres].start = reg_base;
                        res[nres].size  = reg_size;
                        nres++;
                    }

                    uint32_t irq = 0;
                    int has_irq = 0;
                    v = fdt_node_find_prop(fdt, off_struct, off_strings, size_struct,
                                           node_props, "interrupts", &len);
                    if (v && len >= 12) {
                        /* GICv2/v3: <type number flags>; SPI => irq 32+number,
                         * PPI => irq 16+number. */
                        uint32_t type   = be32(v);
                        uint32_t number = be32(v + 4);
                        if (type == 0)
                            irq = 32 + number;
                        else if (type == 1)
                            irq = 16 + number;
                        has_irq = 1;
                    }
                    if (has_irq && nres < ARC_DEV_MAX_RESOURCES) {
                        res[nres].type  = ARC_RES_IRQ;
                        res[nres].start = irq;
                        res[nres].size  = 0;
                        nres++;
                    }

                    /* clocks: list of <phandle [specifier...]>.  Stored
                     * verbatim as ARC_RES_CLOCK (no #clock-cells
                     * resolution yet) — the first cell of each entry is
                     * the provider phandle. */
                    v = fdt_node_find_prop(fdt, off_struct, off_strings, size_struct,
                                           node_props, "clocks", &len);
                    if (v && len >= 4) {
                        int ncells = len / 4;
                        for (int i = 0; i < ncells && nres < ARC_DEV_MAX_RESOURCES; i++) {
                            res[nres].type  = ARC_RES_CLOCK;
                            res[nres].start = be32(v + i * 4);
                            res[nres].size  = 0;
                            nres++;
                        }
                    }

                    /* phandle: this node's own handle (0 if absent). */
                    uint32_t phandle = 0;
                    v = fdt_node_find_prop(fdt, off_struct, off_strings, size_struct,
                                           node_props, "phandle", &len);
                    if (v && len >= 4)
                        phandle = be32(v);

                    char node_name[FDT_PLATFORM_MAX_NAME];
                    int nl = (int)strlen(name);
                    if (nl >= (int)sizeof(node_name)) nl = (int)sizeof(node_name) - 1;
                    memcpy(node_name, name, (size_t)nl);
                    node_name[nl] = '\0';

#ifdef CONFIG_DEBUG
                    debug_printf("fdt: node '%s' (%d chars) -> device\n", node_name, nl);
#endif

                    fdt_platform_add_device(node_name, compat, phandle, res, nres,
                                            walk[depth - 1].dev);
                    walk[depth].dev = walk[depth - 1].dev
                        ? walk[depth - 1].dev
                        : &fdt_platform_devices[fdt_platform_device_count - 1].dev;
                }
            }

            off = node_props;
            continue;
        }

        if (tok == FDT_PROP) {
            if (off + 12 > off_struct + size_struct)
                return -1;
            int len = be32(&fdt[off + 4]);
            int val_off = off + 12;
            off = align4(val_off + len);
            continue;
        }

        if (tok == FDT_END_NODE) {
            depth--;
            off += 4;
            continue;
        }

        if (tok == FDT_NOP) {
            off += 4;
            continue;
        }

        if (tok == FDT_END)
            break;

        off += 4;
    }

    debug_printf("platform: %d device(s) from DT\n", fdt_platform_device_count);
    return 0;
}

static struct arc_bus fdt_platform_bus = {
    .name = "platform",
    .scan = fdt_platform_bus_scan,
};

int fdt_platform_init(void) {
    if (fdt_platform_bus_registered)
        return 0;
    fdt_platform_bus_registered = 1;
    arc_bus_register(&fdt_platform_bus);
    return 0;
}
