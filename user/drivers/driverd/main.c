/* driverd — device enumeration daemon.
 *
 * Works with the kernel's generic driver framework (mk/dev): it walks
 * the global device list of every registered bus — PCI today, any
 * future bus (platform, USB, ...) automatically — prints a device
 * table (mini-lspci) and publishes each device as a service entry so
 * other processes can discover hardware and the drivers bound to it
 * via svc_lookup/svc_query("dev/<bus>:<name>").
 *
 * The daemon must stay alive: init's supervisor restarts it if it
 * exits (respawn on-failure).
 */

#include "libdriver.h"

/* ---- line-buffered output: one write syscall per line ---- */

static void fmt_hex(char *p, uint64_t v, int digits)
{
    const char hex[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) {
        p[i] = hex[v & 0xF];
        v >>= 4;
    }
}

static void fmt_dec(char *p, int digits, uint64_t v)
{
    for (int i = digits - 1; i >= 0; i--) {
        p[i] = '0' + (v % 10);
        v /= 10;
    }
}

static void out(const char *line, int len)
{
    driver_write(1, line, len);
}

/* ---- names for framework enums ---- */

static const char *type_name(uint32_t type)
{
    switch (type) {
    case 0x01: return "pci";
    case 0x02: return "ahci-port";
    case 0x03: return "ata";
    case 0x04: return "nvme";
    case 0x05: return "usb";
    case 0x06: return "block";
    case 0x07: return "net";
    case 0x08: return "platform";
    default:   return "unknown";
    }
}

static const char *state_name(uint32_t st)
{
    switch (st) {
    case DEV_STATE_NEW:        return "new";
    case DEV_STATE_REGISTERED: return "registered";
    case DEV_STATE_PROBED:     return "probed";
    case DEV_STATE_READY:      return "ready";
    case DEV_STATE_FAILED:     return "failed";
    default:                   return "?";
    }
}

static const char *class_name(uint8_t cls, uint8_t subcls)
{
    switch (cls) {
    case 0x01: return "mass storage";
    case 0x02: return "network";
    case 0x03: return "display";
    case 0x04: return "multimedia";
    case 0x06:
        switch (subcls) {
        case 0x00: return "host bridge";
        case 0x01: return "isa bridge";
        case 0x04: return "pci-to-pci bridge";
        case 0x80: return "other bridge";
        default:   return "bridge";
        }
    case 0x0C:
        switch (subcls) {
        case 0x03: return "usb controller";
        default:   return "serial bus";
        }
    default: return "unknown";
    }
}

/* Print one device row: bus name, device name, type, state, ids,
 * class, bound driver, first resources. */
static void print_device(const device_info_t *d)
{
    char line[160];
    int n = 0;

    line[n++] = ' ';
    line[n++] = ' ';
    const char *s;

    /* bus */
    s = d->bus;
    while (*s) line[n++] = *s++;
    line[n++] = ' ';
    while (n < 24) line[n++] = ' ';

    /* name */
    s = d->name;
    while (*s) line[n++] = *s++;
    while (n < 48) line[n++] = ' ';

    /* type + state */
    s = type_name(d->type);
    while (*s) line[n++] = *s++;
    line[n++] = ' ';
    s = state_name(d->state);
    while (*s) line[n++] = *s++;
    while (n < 72) line[n++] = ' ';

    /* vendor:device */
    fmt_hex(line + n, d->vendor, 4); n += 4;
    line[n++] = ':';
    fmt_hex(line + n, d->device, 4); n += 4;
    line[n++] = ' ';

    /* class */
    s = class_name(d->class_code, d->subclass);
    while (*s) line[n++] = *s++;
    line[n++] = ' ';
    line[n++] = '(';
    fmt_hex(line + n, d->class_code, 2); n += 2;
    line[n++] = '/';
    fmt_hex(line + n, d->subclass, 2); n += 2;
    line[n++] = ')';

    /* driver */
    line[n++] = ' ';
    if (d->driver[0]) {
        line[n++] = '[';
        s = d->driver;
        while (*s) line[n++] = *s++;
        line[n++] = ']';
    } else {
        line[n++] = '-';
    }

    /* resources: mmio=<start>+<size> irq=<n> */
    for (uint32_t i = 0; i < d->resource_count && i < DEV_MAX_RESOURCES; i++) {
        if (d->resources[i].type == DEV_RES_IRQ) {
            line[n++] = ' ';
            line[n++] = 'i'; line[n++] = 'r'; line[n++] = 'q';
            line[n++] = '=';
            fmt_dec(line + n, 2, d->resources[i].start); n += 2;
        } else if (d->resources[i].type == DEV_RES_MMIO) {
            line[n++] = ' ';
            line[n++] = 'm'; line[n++] = 'm'; line[n++] = 'i'; line[n++] = 'o';
            line[n++] = '=';
            fmt_hex(line + n, d->resources[i].start, 8); n += 8;
            line[n++] = '+';
            fmt_hex(line + n, d->resources[i].size, 8); n += 8;
        }
    }

    line[n++] = '\n';
    out(line, n);
}

/* Publish each device as a service entry so other processes can find
 * hardware by name ("dev/<bus>:<name>").  Returns the number of
 * entries registered (the registry has a fixed size — full is not an
 * error). */
static int register_devices(const device_info_t *all, int count)
{
    int registered = 0;
    char svc[DEV_INFO_NAME_MAX + DEV_INFO_BUS_MAX + 8];

    for (int i = 0; i < count; i++) {
        /* svc name: "dev/" + bus + ":" + name, e.g. dev/pci:pci0000:00:1f.2 */
        int n = 0;
        const char *s = "dev/";
        while (*s) svc[n++] = *s++;
        s = all[i].bus;
        while (*s && n < (int)sizeof(svc) - 1) svc[n++] = *s++;
        svc[n++] = ':';
        s = all[i].name;
        while (*s && n < (int)sizeof(svc) - 1) svc[n++] = *s++;
        svc[n] = '\0';

        /* data = framework index, so a client can dev_enum() back */
        if (svc_register(svc, (uint64_t)i) == 0)
            registered++;
    }
    return registered;
}

/* Verify the opaque-handle API: open a session for every enumerated
 * device, read info back through the handle, compare with the
 * enumeration data, then close.  Returns the number of devices that
 * passed. */
static int check_handles(const device_info_t *all, int count)
{
    int ok = 0;
    for (int i = 0; i < count; i++) {
        int h = dev_open(all[i].bus, all[i].name);
        if (h <= 0)
            continue;

        device_info_t via;
        if (dev_info(h, &via) < 0) {
            dev_close(h);
            continue;
        }

        int same = 1;
        for (int c = 0; c < DEV_INFO_BUS_MAX; c++)
            if (via.bus[c] != all[i].bus[c]) { same = 0; break; }
        for (int c = 0; c < DEV_INFO_NAME_MAX; c++)
            if (via.name[c] != all[i].name[c]) { same = 0; break; }
        if (via.type != all[i].type ||
            via.state != all[i].state ||
            via.vendor != all[i].vendor ||
            via.device != all[i].device ||
            via.resource_count != all[i].resource_count)
            same = 0;

        if (dev_close(h) == 0 && same)
            ok++;
    }
    return ok;
}

void _start(void)
{
    puts("driverd: starting\n");

    /* Walk the whole framework device list (all buses). */
    device_info_t dev;
    device_info_t all[64];
    int count = 0;
    for (int idx = 0; idx < 64; idx++) {
        if (dev_enum(idx, &dev) < 0)
            break;
        all[count++] = dev;
    }

    for (int i = 0; i < count; i++)
        print_device(&all[i]);

    out("driverd: scan done, ", 20);
    char buf[16];
    fmt_dec(buf, 2, (uint64_t)count);
    out(buf, 2);
    out(" device(s)\n", 11);

    /* Register the daemon itself, then every device as a service. */
    const char *desc = "driverd: device enumeration service";
    if (svc_register_desc("driverd", desc, 0) < 0) {
        puts("driverd: svc_register failed\n");
        driver_exit(1);
    }

    int reg = register_devices(all, count);
    puts("driverd: service registered, ready\n");
    out("driverd: published ", 19);
    fmt_dec(buf, 2, (uint64_t)reg);
    out(buf, 2);
    out(" device service(s)\n", 19);

    /* Exercise the opaque-handle API end to end. */
    int ok = check_handles(all, count);
    out("driverd: handle check: ", 23);
    fmt_dec(buf, 2, (uint64_t)ok);
    out(buf, 2);
    out("/", 1);
    fmt_dec(buf, 2, (uint64_t)count);
    out(buf, 2);
    out(" ok\n", 4);

    /* Negative paths: unknown device must fail, closed handle is dead. */
    if (dev_open("pci", "no-such-device") > 0) {
        puts("driverd: handle check: FAIL (open of unknown device)\n");
    }
    int h = dev_open(all[0].bus, all[0].name);
    device_info_t dead;
    if (h > 0) {
        dev_close(h);
        if (dev_info(h, &dead) == 0)
            puts("driverd: handle check: FAIL (info on closed handle)\n");
    }

    /* Stay alive forever; init respawns us on unexpected death. */
    for (;;) {
        driver_sleep(60);
    }
}
