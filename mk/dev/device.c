#include <stddef.h>
#include "device.h"
#include "driver.h"
#include "bus.h"
#include "string.h"
#include "debug.h"

static struct list_node device_list;
static int device_list_ready;

static void device_list_init(void) {
    if (!device_list_ready) {
        list_init(&device_list);
        device_list_ready = 1;
    }
}

static int arc_device_id_match(const struct arc_device_id *id, struct arc_device *dev) {
    if (id->compatible) {
        if (!dev->compatible) return 0;
        return strcmp(id->compatible, dev->compatible) == 0;
    }
    if (id->vendor   && id->vendor   != dev->id.vendor)   return 0;
    if (id->device   && id->device   != dev->id.device)   return 0;
    if (id->class    && id->class    != dev->id.class)    return 0;
    if (id->subclass && id->subclass != dev->id.subclass) return 0;
    if (id->prog_if  && id->prog_if  != dev->id.prog_if)  return 0;
    return 1;
}

/* Try to bind a single driver to a device.
 * Returns: 0 = bound (READY), 1 = matched but probe failed (FAILED),
 *          -1 = no id match or wrong bus type (state untouched). */
static int arc_device_try_bind(struct arc_device *dev, struct arc_driver *drv) {
    if (drv->type != ARC_DEV_UNKNOWN && drv->type != dev->type)
        return -1;

    for (const struct arc_device_id *id = drv->id_table;
         id->compatible || id->vendor || id->device || id->class || id->subclass || id->prog_if;
         id++) {
        if (!arc_device_id_match(id, dev))
            continue;
        log_printf(LOG_LEVEL_DEBUG, "dev: '%s' matches driver '%s'\n", dev->name, drv->name);
        dev->state = ARC_DEV_PROBED;
        if (!drv->probe || drv->probe(dev) == 0) {
            dev->driver = drv;
            dev->state = ARC_DEV_READY;
            log_printf(LOG_LEVEL_DEBUG, "dev: '%s' probed by '%s'\n", dev->name, drv->name);
            return 0;
        }
        dev->state = ARC_DEV_FAILED;
        return 1;
    }
    return -1;
}

int arc_device_register(struct arc_device *dev) {
    if (!dev) return -1;

    device_list_init();

    list_init(&dev->children);
    list_init(&dev->child_node);
    list_init(&dev->dev_node);
    dev->parent = NULL;
    dev->driver = NULL;
    dev->state = ARC_DEV_REGISTERED;

    list_insert(&device_list, &dev->dev_node);

    struct list_node *dl = arc_driver_list();
    list_for_each(pos, dl) {
        struct arc_driver *drv = list_entry(pos, struct arc_driver, drv_node);
        if (!drv->id_table) continue;
        int rc = arc_device_try_bind(dev, drv);
        if (rc == 0) return 0;
        if (rc == 1) continue;   /* first probe failed — try next matching driver */
    }

#ifdef CONFIG_DEBUG
    log_printf(LOG_LEVEL_WARN, "dev: '%s' has no driver\n", dev->name);
#endif
    return -1;
}

int arc_device_reprobe(struct arc_driver *drv) {
    int bound = 0;

    if (!drv || !drv->id_table) return 0;
    device_list_init();

    for (struct list_node *pos = device_list.next; pos != &device_list; pos = pos->next) {
        struct arc_device *dev = list_entry(pos, struct arc_device, dev_node);
        if (dev->state != ARC_DEV_REGISTERED)
            continue;   /* already bound or terminal */
        if (arc_device_try_bind(dev, drv) == 0)
            bound++;
    }
    return bound;
}

int arc_device_remove(struct arc_device *dev) {
    if (!dev || dev->state < ARC_DEV_READY) return -1;

    if (dev->driver && dev->driver->remove)
        dev->driver->remove(dev);

    /* Remove from parent's children list */
    if (dev->parent)
        list_remove(&dev->child_node);

    /* Remove from global device list */
    list_remove(&dev->dev_node);

    dev->driver = NULL;
    dev->state = ARC_DEV_REGISTERED;
    return 0;
}

int arc_device_add_child(struct arc_device *parent, struct arc_device *child) {
    if (!parent || !child) return -1;
    child->parent = parent;
    list_insert(&parent->children, &child->child_node);
    return 0;
}

/* Fill *out with the info of a single device. */
int arc_device_fill_info(const struct arc_device *dev, arc_device_info_t *out) {
    if (!dev || !out) return -1;

    memset(out, 0, sizeof(*out));
    strncpy(out->bus, (dev->bus && dev->bus->name) ? dev->bus->name : "",
            sizeof(out->bus) - 1);
    strncpy(out->name, dev->name ? dev->name : "",
            sizeof(out->name) - 1);
    out->type  = dev->type;
    out->state = dev->state;
    out->vendor    = dev->id.vendor;
    out->device    = dev->id.device;
    out->class_code = dev->id.class;
    out->subclass  = dev->id.subclass;
    out->prog_if   = dev->id.prog_if;
    if (dev->driver && dev->driver->name)
        strncpy(out->driver, dev->driver->name, sizeof(out->driver) - 1);

    size_t rc = dev->resource_count;
    if (rc > ARC_DEV_MAX_RESOURCES)
        rc = ARC_DEV_MAX_RESOURCES;
    out->resource_count = rc;
    for (size_t r = 0; r < rc; r++) {
        out->resources[r].type  = dev->resources[r].type;
        out->resources[r].start = dev->resources[r].start;
        out->resources[r].size  = dev->resources[r].size;
    }
    return 0;
}

/* Fill *out with the info of the index-th device of the global device
 * list (all buses).  Returns 0 on success, -1 when index is out of
 * range. */
int arc_device_info(int index, arc_device_info_t *out) {
    if (!out) return -1;
    device_list_init();

    int i = 0;
    list_for_each(pos, &device_list) {
        struct arc_device *dev = list_entry(pos, struct arc_device, dev_node);
        if (i != index) {
            i++;
            continue;
        }
        return arc_device_fill_info(dev, out);
    }
    return -1;
}

/* Look up a device by owning bus name + device name. */
struct arc_device *arc_device_find(const char *bus, const char *name) {
    if (!bus || !name) return NULL;
    device_list_init();

    list_for_each(pos, &device_list) {
        struct arc_device *dev = list_entry(pos, struct arc_device, dev_node);
        const char *dev_bus = (dev->bus && dev->bus->name) ? dev->bus->name : "";
        if (strcmp(dev_bus, bus) == 0 && dev->name &&
            strcmp(dev->name, name) == 0)
            return dev;
    }
    return NULL;
}
