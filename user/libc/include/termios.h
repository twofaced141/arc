#ifndef _TERMIOS_H
#define _TERMIOS_H

#include <sys/types.h>

#define NCCS 32

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

/* ioctl requests */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404

#define TCSANOW     0
#define TCSADRAIN   1
#define TCSAFLUSH   2

#define TCIFLUSH    0
#define TCOFLUSH    1
#define TCIOFLUSH   2

/* c_cflag */
#define CSIZE       0x0030
#define CS5         0x0000
#define CS6         0x0010
#define CS7         0x0020
#define CS8         0x0030
#define CSTOPB      0x0040
#define CREAD       0x0080
#define PARENB      0x0100
#define PARODD      0x0200
#define HUPCL       0x0400
#define CLOCAL      0x0800

typedef unsigned int speed_t;

#define B0       0
#define B50      50
#define B75      75
#define B110     110
#define B134     134
#define B150     150
#define B200     200
#define B300     300
#define B600     600
#define B1200    1200
#define B1800    1800
#define B2400    2400
#define B4800    4800
#define B9600    9600
#define B19200   19200
#define B38400   38400
#define B57600   57600
#define B115200  115200

struct termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    unsigned char c_cc[NCCS];
};

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int action, const struct termios *t);
int tcflush(int fd, int queue);
int cfsetspeed(struct termios *tio, speed_t speed);
void cfmakeraw(struct termios *tio);
int tcsendbreak(int fd, int duration);

#endif
