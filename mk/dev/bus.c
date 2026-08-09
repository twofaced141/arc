#include "bus.h"
#include "debug.h"

/* Buses are static, compile-time objects registered at boot; the list
 * has no fixed cap and nothing to free. */

static struct list_node bus_list;
static int bus_list_ready;

static void bus_list_init(void) {
    if (!bus_list_ready) {
        list_init(&bus_list);
        bus_list_ready = 1;
    }
}

void arc_bus_register(struct arc_bus *bus) {
    if (!bus) return;
    bus_list_init();
    list_append(&bus_list, &bus->bus_node);
    log_printf(LOG_LEVEL_DEBUG, "bus: '%s' registered\n", bus->name);
}

void arc_bus_scan_all(void) {
    bus_list_init();
    list_for_each(pos, &bus_list) {
        struct arc_bus *bus = list_entry(pos, struct arc_bus, bus_node);
        log_printf(LOG_LEVEL_DEBUG, "bus: scanning '%s'\n", bus->name);
        if (bus->scan)
            bus->scan(bus);
    }
}
