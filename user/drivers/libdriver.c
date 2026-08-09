/* libdriver.c — Userspace driver library implementation
 *
 * Compiled with: gcc -ffreestanding -nostdlib -nostartfiles ... -c
 * Linked with driver ELF via driver.ld.
 */

#include "libdriver.h"


int pci_find(uint8_t cls, uint8_t subcls, int idx, pci_dev_t *dev)
{
    return (int)syscall5(BSD_SYS(SYS_PCI_DEVICE_INFO),
                         cls, subcls, idx, (long)dev);
}


int dev_enum(int index, device_info_t *dev)
{
    return (int)syscall3(BSD_SYS(SYS_DEVICE_INFO), index, (long)dev);
}

int dev_open(const char *bus, const char *name)
{
    return (int)syscall3(BSD_SYS(SYS_DEV_OPEN), (long)bus, (long)name);
}

int dev_close(int handle)
{
    return (int)syscall2(BSD_SYS(SYS_DEV_CLOSE), handle);
}

int dev_info(int handle, device_info_t *dev)
{
    return (int)syscall3(BSD_SYS(SYS_DEV_INFO), handle, (long)dev);
}


void puts(const char *s)
{
    unsigned long len = 0;
    while (s[len]) len++;
    driver_write(1, s, len);
}

void puthex(uint64_t v)
{
    char buf[19] = "0x0000000000000000";
    for (int i = 17; i >= 2; i--) {
        unsigned d = (unsigned)(v & 0xF);
        buf[i] = d < 10 ? '0' + d : 'a' + d - 10;
        v >>= 4;
    }
    driver_write(1, buf, 18);
}

void putdec(int64_t v)
{
    char buf[21];
    int i = 20;
    buf[20] = '\n';
    if (v < 0) {
        driver_write(1, "-", 1);
        v = -v;
    }
    do {
        buf[--i] = '0' + (v % 10);
        v /= 10;
    } while (v);
    driver_write(1, buf + i, 21 - i);
}


void *memset(void *dst, int c, size_t n)
{
    unsigned char *p = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)c;
    return dst;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    for (size_t i = 0; i < n; i++)
        ((unsigned char *)dest)[i] = ((const unsigned char *)src)[i];
    return dest;
}
