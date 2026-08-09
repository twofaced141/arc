#ifndef ARC_BUS_H
#define ARC_BUS_H

#include "list.h"

struct arc_device;

struct arc_bus {
    const char *name;
    int (*scan)(struct arc_bus *);

    /* Registration list node (mk/dev/bus.c) */
    struct list_node bus_node;
};

void arc_bus_register(struct arc_bus *bus);
void arc_bus_scan_all(void);

int  arc_device_register(struct arc_device *dev);
int  arc_device_remove(struct arc_device *dev);
int  arc_device_add_child(struct arc_device *parent, struct arc_device *child);

#endif
