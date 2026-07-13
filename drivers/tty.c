#include "tty.h"
#include "terminal.h"
#include "signal.h"
#include "scheduler.h"
#include "vmm.h"
#include "spinlock.h"
#include "debug.h"

#define TTY_BUF_SIZE 4096

static struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    unsigned char c_cc[NCCS];

    volatile unsigned char buf[TTY_BUF_SIZE];
    volatile uint32_t head;
    volatile uint32_t tail;

    char line[256];
    int line_len;
    volatile int data_avail;
    volatile int eof_pending;

    spinlock_t lock;
} tty;

static const unsigned char default_cc[NCCS] = {
    [VINTR]  = 0x03,
    [VQUIT]  = 0x1C,
    [VERASE] = 0x7F,
    [VKILL]  = 0x15,
    [VEOF]   = 0x04,
    [VSTART] = 0x11,
    [VSTOP]  = 0x13,
    [VSUSP]  = 0x1A,
};

void tty_init(void) {
    tty.c_iflag = ICRNL;
    tty.c_oflag = OPOST | ONLCR;
    tty.c_cflag = CS8 | CREAD;
    tty.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK;
    for (int i = 0; i < NCCS; i++)
        tty.c_cc[i] = default_cc[i];
    tty.head = 0;
    tty.tail = 0;
    tty.line_len = 0;
    tty.data_avail = 0;
    tty.eof_pending = 0;
    tty.lock = SPINLOCK_INIT;
}

static void push_byte(unsigned char c) {
    uint32_t next = (tty.tail + 1) % TTY_BUF_SIZE;
    if (next == tty.head)
        return;
    tty.buf[tty.tail] = c;
    tty.tail = next;
    tty.data_avail = 1;
}

static void echo_char(unsigned char c) {
    if (c < 0x20) {
        if (c == '\n' || c == '\t') {
            terminal_putchar(c);
            debug_putchar(c);
        } else {
            terminal_putchar('^');
            terminal_putchar(c + 0x40);
            debug_putchar('^');
            debug_putchar(c + 0x40);
        }
    } else if (c == 0x7F) {
    } else {
        terminal_putchar(c);
        debug_putchar(c);
    }
}

void tty_input_byte(unsigned char c) {
    uint32_t flags;
    spin_lock_irqsave(&tty.lock, &flags);

    int isig = tty.c_lflag & ISIG;
    int icanon = tty.c_lflag & ICANON;
    int echo = tty.c_lflag & ECHO;

    if (isig) {
        if (c == tty.c_cc[VINTR]) {
            spin_unlock_irqrestore(&tty.lock, flags);
            sys_kill_pgid((int)foreground_pgid, SIGINT);
            return;
        }
        if (c == tty.c_cc[VSUSP]) {
            spin_unlock_irqrestore(&tty.lock, flags);
            sys_kill_pgid((int)foreground_pgid, SIGTSTP);
            return;
        }
    }

    if (tty.c_iflag & ICRNL && c == '\r')
        c = '\n';
    if (tty.c_iflag & INLCR && c == '\n')
        c = '\r';
    if (tty.c_iflag & IGNCR && c == '\r')
        goto done;

    if (icanon) {
        if (c == tty.c_cc[VERASE] || c == '\b') {
            if (tty.line_len > 0) {
                tty.line_len--;
                if (echo && (tty.c_lflag & ECHOE)) {
                    terminal_putchar('\b');
                    terminal_putchar(' ');
                    terminal_putchar('\b');
                    terminal_flush();
                    debug_putchar('\b');
                    debug_putchar(' ');
                    debug_putchar('\b');
                }
            }
            goto done;
        }
        if (c == tty.c_cc[VKILL]) {
            if (echo && (tty.c_lflag & ECHOK)) {
                terminal_write("^U", 2);
                terminal_putchar('\n');
                terminal_flush();
                debug_putchar('^');
                debug_putchar('U');
                debug_putchar('\n');
            }
            tty.line_len = 0;
            goto done;
        }
        if (c == tty.c_cc[VEOF]) {
            if (tty.line_len == 0) {
                tty.eof_pending = 1;
                tty.data_avail = 1;
            } else {
                for (int i = 0; i < tty.line_len; i++)
                    push_byte((unsigned char)tty.line[i]);
                tty.line_len = 0;
            }
            goto done;
        }
        if (c == '\n') {
            if (tty.line_len < 255)
                tty.line[tty.line_len++] = c;
            if (echo) {
                terminal_putchar('\n');
                terminal_flush();
                debug_putchar('\n');
            }
            for (int i = 0; i < tty.line_len; i++)
                push_byte((unsigned char)tty.line[i]);
            tty.line_len = 0;
            goto done;
        }
        if (tty.line_len < 255) {
            tty.line[tty.line_len++] = c;
        }
        if (echo) {
            echo_char(c);
            terminal_flush();
        }
    } else {
        if (echo) {
            terminal_putchar(c);
            terminal_flush();
        }
        push_byte(c);
    }

done:
    spin_unlock_irqrestore(&tty.lock, flags);
}

int tty_read(char *buf, uint32_t count) {
    if (count == 0) return 0;
    uint32_t flags;
    spin_lock_irqsave(&tty.lock, &flags);

    while (tty.head == tty.tail) {
        if (tty.eof_pending) {
            tty.eof_pending = 0;
            tty.data_avail = 0;
            spin_unlock_irqrestore(&tty.lock, flags);
            return 0;
        }
        spin_unlock_irqrestore(&tty.lock, flags);
        __asm__ __volatile__("sti\n\t"
                             "hlt\n\t"
                             "cli");
        spin_lock_irqsave(&tty.lock, &flags);
    }

    int read_count = 0;
    while (read_count < (int)count && tty.head != tty.tail) {
        buf[read_count++] = (char)tty.buf[tty.head];
        tty.head = (tty.head + 1) % TTY_BUF_SIZE;
    }
    if (tty.head == tty.tail)
        tty.data_avail = 0;

    spin_unlock_irqrestore(&tty.lock, flags);
    return read_count;
}

int tty_write(const char *buf, uint32_t count) {
    char serbuf[256];
    uint32_t serpos = 0;
    if (tty.c_oflag & OPOST) {
        for (uint32_t i = 0; i < count; i++) {
            char c = buf[i];
            if (c == '\n' && (tty.c_oflag & ONLCR)) {
                terminal_putchar('\r');
                serbuf[serpos++] = '\r';
                if (serpos >= 256) { debug_write(serbuf, serpos); serpos = 0; }
            }
            terminal_putchar(c);
            serbuf[serpos++] = c;
            if (serpos >= 256) { debug_write(serbuf, serpos); serpos = 0; }
        }
    } else {
        for (uint32_t i = 0; i < count; i++) {
            terminal_putchar(buf[i]);
            serbuf[serpos++] = buf[i];
            if (serpos >= 256) { debug_write(serbuf, serpos); serpos = 0; }
        }
    }
    if (serpos) debug_write(serbuf, serpos);
    terminal_flush();
    return (int)count;
}

int tty_ioctl(int request, void *argp) {
    uint32_t flags;
    switch (request) {
    case TCGETS: {
        struct termios t;
        spin_lock_irqsave(&tty.lock, &flags);
        t.c_iflag = tty.c_iflag;
        t.c_oflag = tty.c_oflag;
        t.c_cflag = tty.c_cflag;
        t.c_lflag = tty.c_lflag;
        for (int i = 0; i < NCCS; i++)
            t.c_cc[i] = tty.c_cc[i];
        spin_unlock_irqrestore(&tty.lock, flags);
        if (copy_to_user(argp, &t, sizeof(t)) < 0)
            return -1;
        return 0;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        struct termios t;
        if (copy_from_user(&t, argp, sizeof(t)) < 0)
            return -1;
        spin_lock_irqsave(&tty.lock, &flags);
        tty.c_iflag = t.c_iflag;
        tty.c_oflag = t.c_oflag;
        tty.c_cflag = t.c_cflag;
        tty.c_lflag = t.c_lflag;
        for (int i = 0; i < NCCS; i++)
            tty.c_cc[i] = t.c_cc[i];
        spin_unlock_irqrestore(&tty.lock, flags);
        return 0;
    }
    default:
        return -1;
    }
}
