#ifndef USER_RC_RCPARSE_H
#define USER_RC_RCPARSE_H

#include <stddef.h>
#include <stdint.h>

/* rcparse.h — /etc/rc unit parser for the arc init (freestanding, no libc).
 *
 * Syntax (s-expressions; ';' starts a comment to end of line):
 *
 *   (unit NAME
 *     (type mount|oneshot|service)
 *     (device /dev/name) (fstype ufs|ext2) (mountpoint /path) (flags rw|ro)
 *     (exec /path/to/prog arg1 arg2 ...)
 *     (stdio console|silent)
 *     (respawn never|on-failure|always)
 *     (deps a b c) (after a b)
 *     (ready port NAME | sleep SECS)
 *     (env K=V ...) (uid N) (gid N))
 *
 * Semantics:
 *   - mount    — filesystem mount (declarative fstab), runs before services
 *   - oneshot  — run to completion (fsck, mkdir ...); deps must succeed
 *   - service  — long-running daemon; deps must be healthy
 *   - unknown fields are ignored (a warning goes to rc_last_error())
 *   - malformed or invalid units are skipped, the rest still load
 */

#define RC_UNIT_NAME_MAX  32
#define RC_EXEC_MAX       64
#define RC_ARG_MAX        8
#define RC_ARG_LEN        32
#define RC_DEPS_MAX       8
#define RC_ENV_MAX        4
#define RC_ENV_LEN        32
#define RC_PATH_MAX       32
#define RC_UNITS_MAX      64
#define RC_ERR_MAX        128

enum rc_unit_type { RC_T_MOUNT = 1, RC_T_ONESHOT, RC_T_SERVICE };
enum rc_stdio    { RC_IO_CONSOLE = 1, RC_IO_SILENT };
enum rc_respawn  { RC_R_NEVER = 0, RC_R_ON_FAILURE, RC_R_ALWAYS };
enum rc_ready    { RC_READY_NONE = 0, RC_READY_PORT, RC_READY_SLEEP };

typedef struct rc_unit {
    char name[RC_UNIT_NAME_MAX];
    int  type;
    char exec[RC_EXEC_MAX];
    char argv[RC_ARG_MAX][RC_ARG_LEN];
    int  argc;
    int  stdio;
    int  respawn;
    char deps[RC_DEPS_MAX][RC_UNIT_NAME_MAX];
    int  ndeps;
    char after[RC_DEPS_MAX][RC_UNIT_NAME_MAX];
    int  nafter;
    int  ready_kind;
    char ready_port[RC_UNIT_NAME_MAX];
    int  ready_secs;
    char env[RC_ENV_MAX][RC_ENV_LEN];
    int  nenv;
    int  uid;
    int  gid;
    char device[RC_PATH_MAX];
    char fstype[16];
    char mountpoint[RC_PATH_MAX];
    char mflags[16];
    int  line;              /* source line of the (unit ...) for diagnostics */
} rc_unit_t;

/* Parse one buffer (one .rc file) into units[].  Returns the number of
 * valid units (0 if none were valid); never returns negative for
 * ordinary input.  srcname is used in error messages. */
int rc_parse_buf(const char *buf, size_t len, rc_unit_t *units, int max,
                 const char *srcname);

/* Scan a directory (e.g. "/etc/rc") for "*.rc" files and parse them
 * all into units[].  Returns the unit count, RC_SCAN_NODIR when the
 * directory does not exist, or 0 when it exists but produced no valid
 * units (call rc_last_error() to tell "empty" from "all files broken"). */
int rc_scan_dir(const char *dirpath, rc_unit_t *units, int max);

#define RC_SCAN_NODIR (-2)

/* First parse error/warning (shared static buffer, "srcname:line: msg"). */
const char *rc_last_error(void);

#endif /* USER_RC_RCPARSE_H */
