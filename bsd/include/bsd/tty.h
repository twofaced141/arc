#ifndef BSD_TTY_H
#define BSD_TTY_H

#include <stdint.h>
#include "vfs.h"

/* TTY minor numbers */
#define TTY_CONSOLE  0
#define TTY_SERIAL   1

/* ioctl commands */
#define TIOCGETA     0x5401
#define TIOCSETA     0x5402
#define TIOCGWINSZ   0x5413
#define TIOCGPGRP    0x540F
#define TIOCSPGRP    0x5410

/* termios (minimal, Linux-compatible flags) */
#define NCCS 20
#define ISIG   0x0001
#define ICANON 0x0002
#define ECHO   0x0008
#define VMIN   6
#define VTIME  5

typedef struct termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    char     c_cc[NCCS];
} termios_t;

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

/* Terminal line discipline */
typedef struct tty {
    int    minor;
    int    open_count;
    
    /* Input buffer (circular) */
    char   in_buf[256];
    int    in_head;
    int    in_tail;
    
    /* Line discipline state */
    int    echo;      /* echo input */
    int    icanon;    /* canonical (line-buffered) mode */
    int    isig;      /* signal on special chars */
    
    /* Terminal settings */
    termios_t term;
    struct winsize winsize;
    
    /* Process group */
    pid_t  pgrp;
    
    /* Current line (for canonical mode) */
    char   line_buf[256];
    int    line_pos;
    
    /* Wait queue for readers */
    int    readers_waiting;
    waitq_t waitq;
} tty_t;

/* TTY API */
void    tty_init(void);
tty_t  *tty_lookup(int minor);
int     tty_open(tty_t *t, int mode);
int     tty_close(tty_t *t);
int     tty_read(tty_t *t, void *buf, size_t count);
int     tty_write(tty_t *t, const void *buf, size_t count);
int     tty_ioctl(tty_t *t, int cmd, void *data);
int     tty_has_input(tty_t *t);   /* poll: data available */
void    tty_wake_readers(tty_t *t);
int     tty_poll(tty_t *t, int events);   /* poll mask (POLLIN/POLLOUT) */
waitq_t *tty_poll_waitq(tty_t *t);        /* waitq to block on in select */

/* Called by serial/keyboard drivers to inject input */
void    tty_input_char(int minor, char c);

#endif
