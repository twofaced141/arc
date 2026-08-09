/* host_rcparse_test.c — host-side tests for user/rc/rcparse.c.
 *
 * Compiled with the other host tests; rcparse.c is added to
 * HOST_TEST_SRCS and compiled with -DHOST_TEST (skips the syscall-
 * dependent directory scan).  Covers the s-expression lexer/parser
 * and unit field extraction.
 */

#include <stdio.h>
#include <string.h>

#include "user/rc/rcparse.h"

static int failures = 0;
static int total = 0;

#define TEST(name, cond) do { \
    total++; \
    if (!(cond)) { \
        printf("  FAIL: %s\n", name); \
        failures++; \
    } else { \
        printf("  PASS: %s\n", name); \
    } \
} while (0)

static void t_service_basic(void) {
    const char *src =
        "; comment line\n"
        "(unit configd\n"
        "  (type service)\n"
        "  (exec /sbin/configd)\n"
        "  (deps driverd notifyd)\n"
        "  (respawn on-failure)\n"
        "  (uid 3))\n";
    rc_unit_t u[8];
    int n = rc_parse_buf(src, strlen(src), u, 8, "configd.rc");
    TEST("service: parsed", n == 1);
    if (n != 1)
        return;
    TEST("service: name", strcmp(u[0].name, "configd") == 0);
    TEST("service: type", u[0].type == RC_T_SERVICE);
    TEST("service: exec", strcmp(u[0].exec, "/sbin/configd") == 0);
    TEST("service: deps", u[0].ndeps == 2 &&
         strcmp(u[0].deps[0], "driverd") == 0 &&
         strcmp(u[0].deps[1], "notifyd") == 0);
    TEST("service: respawn", u[0].respawn == RC_R_ON_FAILURE);
    TEST("service: uid", u[0].uid == 3);
    TEST("service: line", u[0].line == 2);
}

static void t_service_defaults(void) {
    const char *src = "(unit x (type service) (exec /bin/x))";
    rc_unit_t u[4];
    int n = rc_parse_buf(src, strlen(src), u, 4, "x.rc");
    TEST("defaults: parsed", n == 1);
    if (n != 1)
        return;
    TEST("defaults: respawn", u[0].respawn == RC_R_ON_FAILURE);
    TEST("defaults: stdio console", u[0].stdio == RC_IO_CONSOLE);
    TEST("defaults: no deps", u[0].ndeps == 0);
    TEST("defaults: no env", u[0].nenv == 0);
}

static void t_mount_unit(void) {
    const char *src =
        "(unit bootfs (type mount) (device /dev/hd0s1) (fstype ext2)\n"
        "       (mountpoint /boot) (flags ro))\n";
    rc_unit_t u[4];
    int n = rc_parse_buf(src, strlen(src), u, 4, "boot.rc");
    TEST("mount: parsed", n == 1);
    if (n != 1)
        return;
    TEST("mount: type", u[0].type == RC_T_MOUNT);
    TEST("mount: device", strcmp(u[0].device, "/dev/hd0s1") == 0);
    TEST("mount: fstype", strcmp(u[0].fstype, "ext2") == 0);
    TEST("mount: mountpoint", strcmp(u[0].mountpoint, "/boot") == 0);
    TEST("mount: flags", strcmp(u[0].mflags, "ro") == 0);
}

static void t_oneshot_unit(void) {
    const char *src =
        "(unit fsck-root (type oneshot)\n"
        "  (exec /sbin/fsck /dev/hd0s2)\n"
        "  (deps mounted)\n"
        "  (stdio silent))\n";
    rc_unit_t u[4];
    int n = rc_parse_buf(src, strlen(src), u, 4, "f.rc");
    TEST("oneshot: parsed", n == 1);
    if (n != 1)
        return;
    TEST("oneshot: type", u[0].type == RC_T_ONESHOT);
    TEST("oneshot: argc", u[0].argc == 1);
    TEST("oneshot: argv[0]", strcmp(u[0].argv[0], "/dev/hd0s2") == 0);
    TEST("oneshot: silent stdio", u[0].stdio == RC_IO_SILENT);
}

static void t_ready_env(void) {
    const char *src =
        "(unit syslogd (type service) (exec /sbin/syslogd)\n"
        "  (ready port syslog)\n"
        "  (env DEBUG=0 PATH=/bin)\n"
        "  (gid 3))\n"
        "(unit sync (type oneshot) (exec /sbin/sync) (ready sleep 30))\n";
    rc_unit_t u[8];
    int n = rc_parse_buf(src, strlen(src), u, 8, "s.rc");
    TEST("ready/env: two units", n == 2);
    if (n != 2)
        return;
    TEST("ready: port kind", u[0].ready_kind == RC_READY_PORT);
    TEST("ready: port name", strcmp(u[0].ready_port, "syslog") == 0);
    TEST("ready: env count", u[0].nenv == 2);
    TEST("ready: env[0]", strcmp(u[0].env[0], "DEBUG=0") == 0);
    TEST("ready: gid", u[0].gid == 3);
    TEST("ready: sleep kind", u[1].ready_kind == RC_READY_SLEEP);
    TEST("ready: sleep secs", u[1].ready_secs == 30);
    TEST("ready: no ready by default", u[1].ready_port[0] == '\0');
}

static void t_invalid_units(void) {
    rc_unit_t u[8];

    const char *bad_type = "(unit a (type wat) (exec /bin/a))";
    int n = rc_parse_buf(bad_type, strlen(bad_type), u, 8, "bad.rc");
    TEST("invalid: bad type dropped", n == 0);
    TEST("invalid: error set", rc_last_error()[0] != '\0');

    const char *no_exec = "(unit b (type service))";
    n = rc_parse_buf(no_exec, strlen(no_exec), u, 8, "b.rc");
    TEST("invalid: service w/o exec dropped", n == 0);

    const char *bad_mount = "(unit m (type mount) (device /dev/x))";
    n = rc_parse_buf(bad_mount, strlen(bad_mount), u, 8, "m.rc");
    TEST("invalid: mount w/o fstype dropped", n == 0);

    const char *unbalanced = "(unit c (type service) (exec /bin/c)";
    n = rc_parse_buf(unbalanced, strlen(unbalanced), u, 8, "u.rc");
    TEST("invalid: unbalanced parens -> 0", n == 0);
}

static void t_unknown_field_tolerant(void) {
    const char *src =
        "(unit ok (type service) (exec /bin/ok) (mystery value))\n"
        "(unit good (type oneshot) (exec /bin/good))\n";
    rc_unit_t u[8];
    int n = rc_parse_buf(src, strlen(src), u, 8, "t.rc");
    TEST("tolerant: unknown field kept, units load", n == 2);
    TEST("tolerant: names", strcmp(u[0].name, "ok") == 0 &&
         strcmp(u[1].name, "good") == 0);
}

static void t_noise(void) {
    /* Stray atoms, a stray ')' and a non-unit list must not stop
     * parsing; an unclosed '(' at top level is genuinely fatal. */
    const char *src = "\n;;; \n  ) ( stray atoms ) (garbage)\n  (unit z (type oneshot) (exec /bin/z))\n";
    rc_unit_t u[8];
    int n = rc_parse_buf(src, strlen(src), u, 8, "n.rc");
    TEST("noise: valid unit survives junk", n == 1);
    TEST("noise: name", n == 1 && strcmp(u[0].name, "z") == 0);
}

static void t_limits(void) {
    /* more args than RC_ARG_MAX: truncated, unit still valid */
    char src[512];
    strcpy(src, "(unit w (type oneshot) (exec /bin/w");
    for (int i = 0; i < 12; i++)
        strcat(src, " argN");
    strcat(src, "))");
    rc_unit_t u[8];
    int n = rc_parse_buf(src, strlen(src), u, 8, "l.rc");
    TEST("limits: unit parsed", n == 1);
    TEST("limits: argc capped", n == 1 && u[0].argc == RC_ARG_MAX);
}

void host_rcparse_tests(void) {
    printf("host rcparse tests\n");
    t_service_basic();
    t_service_defaults();
    t_mount_unit();
    t_oneshot_unit();
    t_ready_env();
    t_invalid_units();
    t_unknown_field_tolerant();
    t_noise();
    t_limits();
    printf("%d tests, %d failures\n", total, failures);
}
