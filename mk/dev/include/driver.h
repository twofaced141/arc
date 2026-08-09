#ifndef ARC_DRIVER_H
#define ARC_DRIVER_H

#include "device.h"

struct arc_device;

struct arc_driver {
    const char *name;

    /* Bus type this driver handles; ARC_DEV_UNKNOWN matches any bus. */
    enum arc_device_type type;

    const struct arc_device_id *id_table;

    int (*probe)(struct arc_device *);
    int (*remove)(struct arc_device *);

    int (*open)(struct arc_device *);
    int (*close)(struct arc_device *);

    int (*suspend)(struct arc_device *);
    int (*resume)(struct arc_device *);

    /* Registration list node (mk/dev/driver.c) */
    struct list_node drv_node;
};

void arc_driver_register(struct arc_driver *drv);
struct list_node *arc_driver_list(void);

#endif
