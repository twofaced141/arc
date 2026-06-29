#include <stddef.h>
#include "pci.h"
#include "idt.h"
#include "debug.h"

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ __volatile__("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = 0x80000000 | ((uint32_t)bus << 16) | ((uint32_t)slot << 11)
                     | ((uint32_t)func << 8) | (offset & 0xFC);
    outl(0xCF8, address);
    return inl(0xCFC);
}

struct pci_class {
    uint8_t class;
    const char *name;
};

static const struct pci_class classes[] = {
    {0x00, "Pre-2.0"},    {0x01, "Mass Storage"}, {0x02, "Network"},
    {0x03, "Display"},     {0x04, "Multimedia"},   {0x05, "Memory"},
    {0x06, "Bridge"},      {0x07, "Comm"},          {0x08, "System Periph"},
    {0x09, "Input"},       {0x0A, "Docking"},       {0x0B, "Processor"},
    {0x0C, "Serial Bus"},  {0x0D, "Wireless"},      {0x0E, "I2O"},
    {0x0F, "Satellite"},   {0x10, "Crypto"},        {0x11, "Data/Acq"},
};

static const char *class_name(uint8_t c) {
    for (size_t i = 0; i < sizeof(classes)/sizeof(classes[0]); i++)
        if (classes[i].class == c) return classes[i].name;
    return "Unknown";
}

static void pci_scan_bus(uint16_t pbus) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        uint32_t id0 = pci_read(pbus, slot, 0, 0);
        if ((id0 & 0xFFFF) == 0xFFFF)
            continue;

        uint32_t hdr = pci_read(pbus, slot, 0, 0x0C);
        uint8_t max_func = (hdr & 0x800000) ? 8 : 1;

        for (uint8_t func = 0; func < max_func; func++) {
            uint32_t id = pci_read(pbus, slot, func, 0);
            uint16_t vendor = id & 0xFFFF;
            uint16_t device = id >> 16;
            if (vendor == 0xFFFF)
                continue;

            uint32_t class_rev = pci_read(pbus, slot, func, 8);
            uint8_t cls = class_rev >> 24;
            uint8_t sub = (class_rev >> 16) & 0xFF;
            uint8_t prog_if = (class_rev >> 8) & 0xFF;
            uint8_t rev = class_rev & 0xFF;

            debug_printf("PCI: %x:%x.%d %x:%x %s (%x/%x/%x) rev=%x\r\n",
                pbus, slot, func, vendor, device,
                class_name(cls), cls, sub, prog_if, rev);

            if (cls == 0x06 && sub == 0x04) {
                uint32_t bus_reg = pci_read(pbus, slot, func, 0x18);
                uint8_t secondary = (bus_reg >> 8) & 0xFF;
                if (secondary != pbus && secondary != 0) {
                    debug_printf("PCI: scanning secondary bus %x\r\n", secondary);
                    pci_scan_bus(secondary);
                }
            }
        }
    }
}

void pci_scan(void) {
    debug_print("PCI: scan start\r\n");
    pci_scan_bus(0);
    debug_print("PCI: scan done\r\n");
}
