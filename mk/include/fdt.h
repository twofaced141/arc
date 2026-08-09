#ifndef FDT_H
#define FDT_H

#include <stdint.h>

/* FDT (Flattened Device Tree) parser — minimal, read-only.
 * Finds PCI ECAM base address via compatible = "pci-host-ecam-generic". */

/* Parse DTB blob and locate PCI ECAM config space base + size.
 * Returns 0 on success, -1 if not found / bad FDT.
 * dtb — pointer to DTB in memory (must be accessible, identity-mapped). */
int fdt_get_pci_ecam(const void *dtb, uint64_t *base, uint64_t *size);

/* Search RAM for FDT blob when x0 is not passed by bootloader.
 * Scans page-aligned addresses from kernel_end to end of RAM.
 * Returns pointer to DTB, or NULL. */
const void *fdt_scan_ram(uint64_t kernel_end);

/* Register the "platform" bus: it enumerates DT nodes (compatible,
 * reg, interrupts) as arc_devices.  Call once after arc_boot_init_fdt. */
int fdt_platform_init(void);

/* Collect the MPIDR values of all /cpus/cpu@* nodes into mpidrs[].
 * Returns the number of CPUs found (up to max), or 0 on failure. */
int fdt_get_cpus(const void *dtb, uint64_t *mpidrs, int max);

#endif
