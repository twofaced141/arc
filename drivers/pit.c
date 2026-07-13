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

volatile uint32_t system_ticks = 0;

static void pit_callback(registers_t *r) {
    (void)r;
    system_ticks++;
    if (system_ticks == 1)
        debug_print("pit: first tick\r\n");
}

uint32_t pit_get_ticks(void) {
    return system_ticks;
}

void pit_init(void) {
    uint32_t divisor = PIT_BASE_FREQ / PIT_FREQUENCY;

    uint8_t cmd = PIT_SEL_CH0 | PIT_ACCESSLOB | PIT_MODE_RATE | PIT_BINARY;
    outb(PIT_CMD, cmd);

    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));

    debug_printf("pit: divisor=%u cmd=0x%x\r\n", divisor, cmd);

    /* Verify PIT is counting by reading counter after a short delay */
    outb(0x43, 0x00);  /* latch counter 0 */
    uint32_t cnt1 = inb(0x40) | (inb(0x40) << 8);
    for (volatile int d = 0; d < 100000; d++);
    outb(0x43, 0x00);
    uint32_t cnt2 = inb(0x40) | (inb(0x40) << 8);
    debug_printf("pit: counter %u -> %u (delta=%d)\r\n", cnt1, cnt2, (int)(cnt1 - cnt2));

    register_interrupt_handler(32, pit_callback);
}
