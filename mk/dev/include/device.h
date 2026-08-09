#ifndef ARC_DEVICE_H
#define ARC_DEVICE_H

#include <stddef.h>
#include <stdint.h>
#include "list.h"

enum arc_device_type {
    ARC_DEV_PCI       = 0x01,
    ARC_DEV_AHCI_PORT = 0x02,
    ARC_DEV_ATA       = 0x03,
    ARC_DEV_NVME      = 0x04,
    ARC_DEV_USB       = 0x05,
    ARC_DEV_BLOCK     = 0x06,
    ARC_DEV_NET       = 0x07,
    ARC_DEV_PLATFORM  = 0x08,
    ARC_DEV_UNKNOWN   = 0xFF,
};

enum arc_device_state {
    ARC_DEV_NEW        = 0,
    ARC_DEV_REGISTERED = 1,
    ARC_DEV_PROBED     = 2,
    ARC_DEV_READY      = 3,
    ARC_DEV_FAILED     = 4,
};

enum arc_resource_type {
    ARC_RES_MMIO  = 0,
    ARC_RES_IRQ   = 1,
    ARC_RES_DMA   = 2,
    ARC_RES_CLOCK = 3,  /* DT clock reference (start = provider phandle) */
};

struct arc_resource {
    enum arc_resource_type type;
    uint64_t start;
    uint64_t size;
};

struct arc_bus;
struct arc_driver;

struct arc_device_id {
    const char *compatible;  /* DT compatible string (platform devices) */
    uint16_t vendor;
    uint16_t device;
    uint8_t  class;
    uint8_t  subclass;
    uint8_t  prog_if;
};

#define ARC_DEVICE_ID_END { NULL, 0, 0, 0, 0, 0 }
#define ARC_DEV_MAX_RESOURCES 8

struct arc_device {
    const char          *name;
    const char          *compatible; /* first DT compatible string */
    enum arc_device_type type;
    enum arc_device_state state;

    uint32_t             phandle;   /* DT phandle of the node, 0 if none */

    struct arc_bus      *bus;
    struct arc_driver   *driver;

    struct arc_device_id id;

    struct arc_device   *parent;
    struct list_node     children;   /* head of children list */
    struct list_node     child_node; /* node in parent's children list */
    struct list_node     dev_node;   /* node in global device list */

    /* Resource list (MMIO/IRQ/CLOCK).  Storage is owned by the bus that
     * created the device (static arrays in fdt.c / pci.c). */
    struct arc_resource *resources;
    size_t               resource_count;

    void   *priv;
    uint32_t flags;
};

#define ARC_DEV_ENABLED  (1 << 0)

/* ---- Userspace-facing device descriptor ----
 *
 * Bus-agnostic snapshot of one entry of the global device list.
 * Mirrored in user/libdriver.h (device_info_t) — keep in sync. */
#define ARC_DEVINFO_BUS_MAX    16
#define ARC_DEVINFO_NAME_MAX   48
#define ARC_DEVINFO_DRV_MAX    32

typedef struct {
    uint32_t type;                   /* enum arc_resource_type      */
    uint64_t start;
    uint64_t size;
} arc_device_resource_info_t;

typedef struct {
    char     bus[ARC_DEVINFO_BUS_MAX];   /* owning bus name, e.g. "pci" */
    char     name[ARC_DEVINFO_NAME_MAX]; /* e.g. "pci0000:00:1f.2"      */
    uint32_t type;                       /* enum arc_device_type        */
    uint32_t state;                      /* enum arc_device_state       */
    uint16_t vendor;                     /* arc_device_id.vendor        */
    uint16_t device;                     /* arc_device_id.device        */
    uint8_t  class_code;                 /* arc_device_id.class         */
    uint8_t  subclass;                   /* arc_device_id.subclass      */
    uint8_t  prog_if;                    /* arc_device_id.prog_if       */
    char     driver[ARC_DEVINFO_DRV_MAX]; /* bound driver name, "" if none */
    uint32_t resource_count;
    arc_device_resource_info_t resources[ARC_DEV_MAX_RESOURCES];
} arc_device_info_t;

/* Fill *out with the info of the index-th device of the global device
 * list (all buses).  Returns 0 on success, -1 when index is out of
 * range. */
int  arc_device_info(int index, arc_device_info_t *out);

/* Fill *out with the info of a single device. */
int  arc_device_fill_info(const struct arc_device *dev, arc_device_info_t *out);

/* Look up a device by owning bus name + device name.  Returns the
 * device or NULL.  The caller must NOT keep the pointer across calls
 * that can remove devices (no refcounting in the framework yet). */
struct arc_device *arc_device_find(const char *bus, const char *name);

/* device.c */
int  arc_device_register(struct arc_device *dev);
int  arc_device_remove(struct arc_device *dev);
int  arc_device_add_child(struct arc_device *parent, struct arc_device *child);
int  arc_device_reprobe(struct arc_driver *drv);

/* i8042.c — PS/2 keyboard controller platform device (x86 only). */
void i8042_init(void);

#endif
