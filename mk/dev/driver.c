#include "driver.h"
#include "debug.h"

/* Drivers are static, compile-time objects registered at boot; the list
 * has no fixed cap and nothing to free.  Registration order is kept
 * (append at tail), so the first driver to claim a device stays the
 * first one tried when several drivers match the same id table. */

static struct list_node driver_list;
static int driver_list_ready;

static void driver_list_init(void) {
    if (!driver_list_ready) {
        list_init(&driver_list);
        driver_list_ready = 1;
    }
}

void arc_driver_register(struct arc_driver *drv) {
    if (!drv) return;
    driver_list_init();
    list_append(&driver_list, &drv->drv_node);
    log_printf(LOG_LEVEL_DEBUG, "driver: '%s' registered\n", drv->name);
    arc_device_reprobe(drv);
}

struct list_node *arc_driver_list(void) {
    driver_list_init();
    return &driver_list;
}
