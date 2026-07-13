#include "keyboard.h"
#include "idt.h"
#include "isr.h"
#include "spinlock.h"
#include "tty.h"
#include "debug.h"

#define LSHIFT_MAKE  0x2A
#define RSHIFT_MAKE  0x36
#define LSHIFT_BREAK 0xAA
#define RSHIFT_BREAK 0xB6
#define LCTRL_MAKE   0x1D
#define LCTRL_BREAK  0x9D

static const char scancode_ascii[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    0, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*',
    0, ' '
};

static const char scancode_shifted[] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    0, 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*',
    0, ' '
};

static volatile char key_buffer[256];
static volatile uint8_t key_buffer_head;
static volatile uint8_t key_buffer_tail;
static volatile uint8_t shift_pressed;
static volatile uint8_t ctrl_pressed;
static volatile uint8_t extended;
static spinlock_t keyboard_lock = SPINLOCK_INIT;

#define EXT_SCANCODE 0xE0

static void ps2_wait_input(void) {
    while (inb(0x64) & 0x02);
}

static void ps2_wait_output(void) {
    while (!(inb(0x64) & 0x01));
}

static void __attribute__((unused)) keyboard_push(char c) {
    uint8_t next_tail = (uint8_t)(key_buffer_tail + 1);
    if (next_tail == key_buffer_head) return;
    key_buffer[key_buffer_tail] = c;
    key_buffer_tail = next_tail;
}

static void serial_irq_handler(registers_t *r) {
    (void)r;
    uint8_t iir = inb(0x3F8 + 2);
    if (iir & 1) return;
    uint8_t cause = (iir >> 1) & 7;
    if (cause == 2 || cause == 6) {
        while (inb(0x3F8 + 5) & 1) {
            char c = inb(0x3F8);
            unsigned char uc = (unsigned char)c;
            if ((uc >= 0x20 && uc <= 0x7E) || (uc >= 0x01 && uc <= 0x1F))
                tty_input_byte(uc);
        }
    }
}

static void keyboard_irq_handler(registers_t *r) {
    uint8_t sc;
    char c;

    (void)r;

    sc = inb(0x60);

    if (sc == EXT_SCANCODE) {
        extended = 1;
        return;
    }

    if (sc == LSHIFT_MAKE || sc == RSHIFT_MAKE) {
        shift_pressed = 1;
        return;
    }
    if (sc == LSHIFT_BREAK || sc == RSHIFT_BREAK) {
        shift_pressed = 0;
        return;
    }
    if (sc == LCTRL_MAKE) {
        ctrl_pressed = 1;
        return;
    }
    if (sc == LCTRL_BREAK) {
        ctrl_pressed = 0;
        return;
    }

    if (sc & 0x80) {
        extended = 0;
        return;
    }

    if (extended) {
        extended = 0;
        if (sc == 0x48) { tty_input_byte(0x1B); tty_input_byte('['); tty_input_byte('A'); }
        else if (sc == 0x50) { tty_input_byte(0x1B); tty_input_byte('['); tty_input_byte('B'); }
        else if (sc == 0x4D) { tty_input_byte(0x1B); tty_input_byte('['); tty_input_byte('C'); }
        else if (sc == 0x4B) { tty_input_byte(0x1B); tty_input_byte('['); tty_input_byte('D'); }
        return;
    }

    if (sc >= sizeof(scancode_ascii))
        return;

    c = shift_pressed ? scancode_shifted[sc] : scancode_ascii[sc];
    if (!c)
        return;

    if (ctrl_pressed && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c = c & 0x1F;

    tty_input_byte((unsigned char)c);
}

void serial_input_init(void) {
    /* Drain stale bytes from serial receive buffer */
    while (inb(0x3F8 + 5) & 1)
        (void)inb(0x3F8);
    register_interrupt_handler(36, serial_irq_handler);
    outb(0x3F8 + 1, 1);
    outb(0x21, inb(0x21) & ~0x10);
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
    serial_input_init();
}

uint32_t keyboard_read(char *buf, uint32_t max) {
    uint32_t count = 0;
    uint32_t flags;
    spin_lock_irqsave(&keyboard_lock, &flags);
    while (count < max && key_buffer_head != key_buffer_tail) {
        buf[count++] = key_buffer[key_buffer_head];
        key_buffer_head = (uint8_t)(key_buffer_head + 1);
    }
    spin_unlock_irqrestore(&keyboard_lock, flags);
    return count;
}

char keyboard_getchar(void) {
    for (;;) {
        uint32_t flags;
        spin_lock_irqsave(&keyboard_lock, &flags);
        if (key_buffer_head != key_buffer_tail) {
            char c = key_buffer[key_buffer_head];
            key_buffer_head = (uint8_t)(key_buffer_head + 1);
            spin_unlock_irqrestore(&keyboard_lock, flags);
            return c;
        }
        spin_unlock_irqrestore(&keyboard_lock, flags);
        __asm__ __volatile__("hlt");
    }
}
