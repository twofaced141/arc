#ifndef TTY_H
#define TTY_H

#include <stdint.h>

#define NCCS 32

/* ioctl requests for TTY */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404

/* c_cc indices */
#define VINTR   0
#define VQUIT   1
#define VERASE  2
#define VKILL   3
#define VEOF    4
#define VTIME   5
#define VMIN    6
#define VSWTC   7
#define VSTART  8
#define VSTOP   9
#define VSUSP   10
#define VEOL    11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE 14
#define VLNEXT  15
#define VEOL2   16

/* c_iflag */
#define IGNBRK  0x00000001
#define BRKINT  0x00000002
#define IGNPAR  0x00000004
#define PARMRK  0x00000008
#define INPCK   0x00000010
#define ISTRIP  0x00000020
#define INLCR   0x00000040
#define IGNCR   0x00000080
#define ICRNL   0x00000100
#define IUCLC   0x00000200
#define IXON    0x00000400
#define IXANY   0x00000800
#define IXOFF   0x00001000
#define IMAXBEL 0x00002000
#define IUTF8   0x00004000

/* c_cflag */
#define CSIZE   0x00000030
#define CS5     0x00000010
#define CS6     0x00000020
#define CS7     0x00000030
#define CS8     0x00000040
#define CSTOPB  0x00000080
#define CREAD   0x00000100
#define PARENB  0x00000200
#define PARODD  0x00000400
#define HUPCL   0x00000800
#define CLOCAL  0x00001000

/* c_oflag */
#define OPOST   0x00000001
#define ONLCR   0x00000004
#define OCRNL   0x00000008
#define ONOCR   0x00000010
#define ONLRET  0x00000020
#define OFILL   0x00000040

/* c_lflag */
#define ISIG    0x00000001
#define ICANON  0x00000002
#define XCASE   0x00000004
#define ECHO    0x00000008
#define ECHOE   0x00000010
#define ECHOK   0x00000020
#define ECHONL  0x00000040
#define NOFLSH  0x00000080
#define TOSTOP  0x00000100
#define IEXTEN  0x00000200
#define ECHOCTL 0x00000400
#define ECHOPRT 0x00000800
#define ECHOKE  0x00001000
#define FLUSHO  0x00002000
#define PENDIN  0x00004000
#define EXTPROC 0x00010000

struct termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    unsigned char c_cc[NCCS];
};

void tty_init(void);
int tty_read(char *buf, uint32_t count);
int tty_write(const char *buf, uint32_t count);
int tty_ioctl(int request, void *argp);
void tty_input_byte(unsigned char c);

#endif
