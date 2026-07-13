#include "lwip/arch.h"
#include "pit.h"

u32_t sys_now(void) {
    return pit_get_ticks() * 10;
}
