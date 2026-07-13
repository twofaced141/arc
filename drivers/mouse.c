#include "mouse.h"
#include "idt.h"
#include "isr.h"
#include "spinlock.h"

static volatile int32_t mouse_x;
static volatile int32_t mouse_y;
static volatile uint8_t mouse_buttons;
static volatile int8_t  mouse_wheel;
static volatile uint8_t mouse_present;

static volatile uint8_t packet[4];
static volatile uint8_t packet_index;
static volatile uint8_t packet_size;

static spinlock_t mouse_lock = SPINLOCK_INIT;

static void ps2_wait_input(void) {
    while (inb(0x64) & 0x02);
}

static void ps2_wait_output(void) {
    while (!(inb(0x64) & 0x01));
}

static uint8_t ps2_send_to_mouse(uint8_t cmd) {
    ps2_wait_input();
    outb(0x64, 0xD4);
    ps2_wait_input();
    outb(0x60, cmd);
    ps2_wait_output();
    return inb(0x60);
}

static void mouse_irq_handler(registers_t *r) {
    uint8_t byte = inb(0x60);
    uint32_t flags;

    (void)r;

    if (packet_index == 0 && !(byte & 0x08))
        return;

    packet[packet_index++] = byte;

    if (packet_index < packet_size)
        return;

    packet_index = 0;

    uint8_t b = packet[0];
    int32_t dx, dy;

    dx = (int32_t)(int8_t)packet[1];
    dy = (int32_t)(int8_t)packet[2];

    spin_lock_irqsave((spinlock_t *)&mouse_lock, &flags);

    if (!(b & 0x40))
        mouse_x += dx;
    if (!(b & 0x80))
        mouse_y -= dy;

    mouse_buttons = b & 0x07;

    if (packet_size == 4)
        mouse_wheel = (int8_t)packet[3];

    spin_unlock_irqrestore((spinlock_t *)&mouse_lock, flags);
}

void mouse_init(void) {
    uint8_t ack;

    packet_index = 0;
    packet_size = 3;
    mouse_x = 0;
    mouse_y = 0;
    mouse_buttons = 0;
    mouse_wheel = 0;
    mouse_present = 0;

    ps2_wait_input();
    outb(0x64, 0xA8);

    ps2_wait_input();
    outb(0x64, 0x20);
    ps2_wait_output();
    uint8_t config = inb(0x60);

    config |= 0x02;
    config &= ~0x20;

    ps2_wait_input();
    outb(0x64, 0x60);
    ps2_wait_input();
    outb(0x60, config);

    ack = ps2_send_to_mouse(0xF5);
    if (ack != 0xFA)
        return;

    ps2_send_to_mouse(0xF3);
    ps2_wait_input();
    outb(0x64, 0xD4);
    ps2_wait_input();
    outb(0x60, 200);
    ps2_wait_output();
    inb(0x60);

    ps2_send_to_mouse(0xF3);
    ps2_wait_input();
    outb(0x64, 0xD4);
    ps2_wait_input();
    outb(0x60, 100);
    ps2_wait_output();
    inb(0x60);

    ps2_send_to_mouse(0xF3);
    ps2_wait_input();
    outb(0x64, 0xD4);
    ps2_wait_input();
    outb(0x60, 80);
    ps2_wait_output();
    inb(0x60);

    ps2_send_to_mouse(0xF2);
    ps2_wait_output();
    uint8_t dev_id = inb(0x60);

    if (dev_id == 3 || dev_id == 4)
        packet_size = 4;

    ack = ps2_send_to_mouse(0xE8);
    if (ack != 0xFA)
        return;

    ps2_wait_input();
    outb(0x64, 0xD4);
    ps2_wait_input();
    outb(0x60, 3);
    ps2_wait_output();
    inb(0x60);

    ack = ps2_send_to_mouse(0xF4);
    if (ack != 0xFA)
        return;

    mouse_present = 1;

    register_interrupt_handler(44, mouse_irq_handler);

    outb(0xA1, inb(0xA1) & ~0x10);
}

void mouse_get_state(mouse_state_t *state) {
    uint32_t flags;
    spin_lock_irqsave(&mouse_lock, &flags);
    state->x = mouse_x;
    state->y = mouse_y;
    state->buttons = mouse_buttons;
    state->wheel = mouse_wheel;
    state->present = mouse_present;
    spin_unlock_irqrestore(&mouse_lock, flags);
}
