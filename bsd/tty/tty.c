#include "bsd/tty.h"
#include "bsd/errno.h"
#include "bsd/select.h"
#include "debug.h"
#include "string.h"

#define TTY_COUNT 2
static tty_t ttys[TTY_COUNT];

/* Keep the legacy echo/icanon/isig flags in sync with c_lflag. */
static void tty_apply_lflags(tty_t *t) {
    t->echo   = (t->term.c_lflag & ECHO)   ? 1 : 0;
    t->icanon = (t->term.c_lflag & ICANON) ? 1 : 0;
    t->isig   = (t->term.c_lflag & ISIG)   ? 1 : 0;
}

void tty_init(void) {
    memset(ttys, 0, sizeof(ttys));
    for (int i = 0; i < TTY_COUNT; i++) {
        ttys[i].minor   = i;
        ttys[i].pgrp    = 0;
        ttys[i].line_pos = 0;
        ttys[i].in_head = 0;
        ttys[i].in_tail = 0;
        ttys[i].open_count = 0;
        ttys[i].term.c_lflag = ISIG | ICANON | ECHO;
        ttys[i].winsize.ws_row = 25;
        ttys[i].winsize.ws_col = 80;
        tty_apply_lflags(&ttys[i]);
        waitq_init(&ttys[i].waitq);
    }
    log_print(LOG_LEVEL_DEBUG, "tty: init\r\n");
}

tty_t *tty_lookup(int minor) {
    if (minor < 0 || minor >= TTY_COUNT)
        return NULL;
    return &ttys[minor];
}

int tty_open(tty_t *t, int mode) {
    (void)mode;
    if (!t) return -ENXIO;
    t->open_count++;
    return 0;
}

int tty_close(tty_t *t) {
    if (!t) return -ENXIO;
    if (t->open_count > 0)
        t->open_count--;
    return 0;
}

int tty_has_input(tty_t *t) {
    return (t && t->in_head != t->in_tail) ? 1 : 0;
}

void tty_wake_readers(tty_t *t) {
    if (t)
        waitq_wake_all(&t->waitq);
}

/* Poll mask: readable when input is buffered (canonical mode needs a
 * full line), always writable (the console never blocks on output). */
int tty_poll(tty_t *t, int events) {
    int rev = 0;
    if (!t)
        return POLLNVAL;
    if (events & (POLLIN | POLLRDNORM)) {
        if (t->icanon) {
            int has_line = 0;
            for (int i = t->in_tail; i != t->in_head; i = (i + 1) % 256) {
                if (t->in_buf[i] == '\n') {
                    has_line = 1;
                    break;
                }
            }
            if (has_line)
                rev |= events & (POLLIN | POLLRDNORM);
        } else if (tty_has_input(t)) {
            rev |= events & (POLLIN | POLLRDNORM);
        }
    }
    if (events & (POLLOUT | POLLWRNORM))
        rev |= events & (POLLOUT | POLLWRNORM);
    return rev;
}

waitq_t *tty_poll_waitq(tty_t *t) {
    return t ? &t->waitq : NULL;
}

int tty_read(tty_t *t, void *buf, size_t count) {
    if (!t || !buf) return -ENXIO;

    char *dest = (char *)buf;
    size_t read_count = 0;

    for (;;) {
        /* Wait for input.  In canonical mode a read blocks until at
         * least one complete line is available; otherwise any byte. */
        if (!tty_has_input(t)) {
            int rc = waitq_sleep(&t->waitq);
            if (rc < 0)
                return -ERESTARTSYS;   /* interruptible: may re-run */
            continue;
        }

        if (t->icanon) {
            int has_line = 0;
            for (int i = t->in_tail; i != t->in_head; i = (i + 1) % 256) {
                if (t->in_buf[i] == '\n') {
                    has_line = 1;
                    break;
                }
            }
            if (!has_line) {
                int rc = waitq_sleep(&t->waitq);
                if (rc < 0)
                    return -ERESTARTSYS;
                continue;
            }
        }
        break;
    }

    while (read_count < count && t->in_head != t->in_tail) {
        char c = t->in_buf[t->in_tail];
        t->in_tail = (t->in_tail + 1) % 256;
        *dest++ = c;
        read_count++;

        /* In canonical mode, stop at newline */
        if (t->icanon && c == '\n')
            break;
    }

    return (int)read_count;
}

int tty_write(tty_t *t, const void *buf, size_t count) {
    (void)t;
    if (!buf) return -ENXIO;
    if (count > 4096)
        return -EINVAL;

    const char *src = (const char *)buf;
    for (size_t i = 0; i < count; i++)
        debug_putchar(src[i]);

    return (int)count;
}

int tty_ioctl(tty_t *t, int cmd, void *data) {
    if (!t) return -ENXIO;

    switch (cmd) {
    case TIOCGPGRP:
        if (data) *(pid_t *)data = t->pgrp;
        return 0;
    case TIOCSPGRP:
        if (data) t->pgrp = *(pid_t *)data;
        return 0;
    case TIOCGETA:
        if (data) *(termios_t *)data = t->term;
        return 0;
    case TIOCSETA:
        if (data) {
            t->term = *(termios_t *)data;
            tty_apply_lflags(t);
        }
        return 0;
    case TIOCGWINSZ:
        if (data) *(struct winsize *)data = t->winsize;
        return 0;
    default:
        return -ENOTTY;
    }
}

void tty_input_char(int minor, char c) {
    tty_t *t = tty_lookup(minor);
    if (!t) return;

    int next = (t->in_head + 1) % 256;
    if (next == t->in_tail)
        return; /* buffer full */

    if (t->echo) {
        debug_putchar(c);
    }

    t->in_buf[t->in_head] = c;
    t->in_head = next;

    tty_wake_readers(t);
}

