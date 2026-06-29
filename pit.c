#include "pit.h"
#include "idt.h"
#include "isr.h"
#include "debug.h"

#define PIT_CMD   0x43
#define PIT_CH0   0x40

#define PIT_SEL_CH0    (0 << 6)
#define PIT_ACCESSLOB  (3 << 4)
#define PIT_MODE_RATE  (2 << 1)
#define PIT_BINARY     0

static volatile uint32_t pit_ticks = 0;

static void pit_callback(registers_t *r) {
    pit_ticks++;
    if (pit_ticks == 1)
        debug_print("pit: first tick\r\n");
}

uint32_t pit_get_ticks(void) {
    return pit_ticks;
}

void pit_init(void) {
    uint32_t divisor = PIT_BASE_FREQ / PIT_FREQUENCY;

    uint8_t cmd = PIT_SEL_CH0 | PIT_ACCESSLOB | PIT_MODE_RATE | PIT_BINARY;
    outb(PIT_CMD, cmd);

    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));

    register_interrupt_handler(32, pit_callback);
}
