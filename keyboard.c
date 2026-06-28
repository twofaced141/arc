#include "keyboard.h"
#include "idt.h"
#include "isr.h"

static const char scancode_ascii[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0,
    0, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*',
    0, ' '
};

static volatile char key_buffer[256];
static volatile uint8_t key_buffer_head;
static volatile uint8_t key_buffer_tail;

static void ps2_wait_input(void) {
    while (inb(0x64) & 0x02);
}

static void ps2_wait_output(void) {
    while (!(inb(0x64) & 0x01));
}

static void keyboard_irq_handler(registers_t *r) {
    uint8_t sc;
    uint8_t next_tail;
    char c;

    (void)r;

    sc = inb(0x60);
    if (sc & 0x80)
        return;

    if (sc >= sizeof(scancode_ascii))
        return;

    c = scancode_ascii[sc];
    if (!c)
        return;

    next_tail = (uint8_t)(key_buffer_tail + 1);
    if (next_tail == key_buffer_head)
        return;

    key_buffer[key_buffer_tail] = c;
    key_buffer_tail = next_tail;
}

void keyboard_init(void) {
    key_buffer_head = 0;
    key_buffer_tail = 0;

    ps2_wait_input();
    outb(0x64, 0xAE);           // enable keyboard port

    ps2_wait_input();
    outb(0x64, 0x20);           // read controller config byte
    ps2_wait_output();
    uint8_t cmd = inb(0x60);

    cmd |= 0x01;                // enable IRQ1
    cmd |= 0x40;                // keep controller translation to set 1

    ps2_wait_input();
    outb(0x64, 0x60);           // write config byte
    ps2_wait_input();
    outb(0x60, cmd);

    ps2_wait_input();
    outb(0x60, 0xF4);           // enable keyboard scanning
    ps2_wait_output();
    (void)inb(0x60);            // discard ACK

    register_interrupt_handler(33, keyboard_irq_handler);
}

char keyboard_getchar(void) {
    while (key_buffer_head == key_buffer_tail)
        __asm__ __volatile__("hlt");

    {
        char c = key_buffer[key_buffer_head];
        key_buffer_head = (uint8_t)(key_buffer_head + 1);
        return c;
    }
}
