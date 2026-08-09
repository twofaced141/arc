#ifndef __aarch64__
/* serial.c — x86 (amd64/i386) only; arm64 has its own UART in mk/arch/arm64/kern/main.c */

#include "serial.h"
#include "idt.h"

#define SERIAL_PORT 0x3F8

void serial_init(void) {
    outb(SERIAL_PORT + 1, 0x00);  // Disable interrupts
    outb(SERIAL_PORT + 3, 0x80);  // Enable DLAB (set baud rate divisor)
    outb(SERIAL_PORT + 0, 0x03);  // Divisor = 3 (38400 baud)
    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x03);  // 8 bits, no parity, 1 stop bit
    outb(SERIAL_PORT + 2, 0xC7);  // Enable FIFO, clear, 14-byte threshold
    outb(SERIAL_PORT + 4, 0x0B);  // IRQs enabled, RTS/DSR set
}

static int serial_is_transmit_empty(void) {
    return inb(SERIAL_PORT + 5) & 0x20;
}

void serial_putchar(char c) {
    while (!serial_is_transmit_empty());
    outb(SERIAL_PORT, (uint8_t)c);
}

void serial_write(const char *buf, unsigned int count) {
    for (unsigned int i = 0; i < count; i++)
        serial_putchar(buf[i]);
}

#endif /* !__aarch64__ */
