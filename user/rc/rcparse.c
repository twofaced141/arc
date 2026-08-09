/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026, fierce
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS AS IS AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */


/* rcparse.c — /etc/rc unit parser for the arc init.
 *
 * Freestanding, no libc: string helpers are local.  The pure parser
 * (rc_parse_buf) is compiled into host tests too; the directory scan
 * (rc_scan_dir) needs kernel syscalls and is compiled out for HOST_TEST.
 */

#include "rcparse.h"


static size_t rc_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

static int rc_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int rc_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0')
            return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
    }
    return 0;
}

static void rc_strncpy(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (src[i] && i + 1 < max) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int rc_atoi(const char *s) {
    int v = 0;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static int rc_ends_with(const char *s, const char *suffix) {
    size_t sl = rc_strlen(s), ul = rc_strlen(suffix);
    if (ul > sl)
        return 0;
    return rc_strncmp(s + sl - ul, suffix, ul) == 0;
}

static void rc_memzero(void *d, size_t n) {
    unsigned char *p = (unsigned char *)d;
    while (n--)
        *p++ = 0;
}


static char rc_err[RC_ERR_MAX];

const char *rc_last_error(void) {
    return rc_err;
}

static void rc_set_err(const char *src, int line, const char *msg) {
    size_t n = 0;
    if (src) {
        while (src[n] && n < RC_ERR_MAX - 1)
            n++;
        if (n) {
            size_t i = 0;
            for (; src[i] && i + 1 < RC_ERR_MAX - 32; i++)
                rc_err[i] = src[i];
            n = i;
            rc_err[n++] = ':';
        } else {
            n = 0;
        }
    }
    char num[12];
    int ni = 0;
    if (line < 0) line = 0;
    do { num[ni++] = '0' + (line % 10); line /= 10; } while (line);
    while (ni > 0 && n + 2 < RC_ERR_MAX) rc_err[n++] = num[--ni];
    rc_err[n++] = ':';
    rc_err[n++] = ' ';
    for (size_t i = 0; msg[i] && n + 1 < RC_ERR_MAX; i++)
        rc_err[n++] = msg[i];
    rc_err[n] = '\0';
}


#define RC_TOK_MAX  64
#define RC_CHILD_MAX 16

typedef struct sexp {
    char   name[RC_TOK_MAX];
    int    nchild;
    int    line;
    struct sexp *children[RC_CHILD_MAX];
} sexp_t;

#define RC_POOL_MAX 1024
static sexp_t rc_pool[RC_POOL_MAX];
static int rc_npool;

static sexp_t *rc_node(void) {
    if (rc_npool >= RC_POOL_MAX)
        return NULL;
    sexp_t *n = &rc_pool[rc_npool++];
    for (int i = 0; i < RC_CHILD_MAX; i++)
        n->children[i] = NULL;
    n->nchild = 0;
    n->name[0] = '\0';
    n->line = 0;
    return n;
}

typedef struct lexer {
    const char *p;
    const char *end;
    int         line;
} lexer_t;

static void lexer_skip(lexer_t *lx) {
    for (;;) {
        while (lx->p < lx->end && (*lx->p == ' ' || *lx->p == '\t' ||
                                   *lx->p == '\n' || *lx->p == '\r')) {
            if (*lx->p == '\n') lx->line++;
            lx->p++;
        }
        if (lx->p < lx->end && *lx->p == ';') {
            while (lx->p < lx->end && *lx->p != '\n')
                lx->p++;
            continue;
        }
        break;
    }
}

/* Returns: 1 '(' 2 ')' 0 atom -1 EOF */
static int lexer_next(lexer_t *lx, char *tok, size_t tokmax) {
    lexer_skip(lx);
    if (lx->p >= lx->end)
        return -1;
    char c = *lx->p;
    if (c == '(') { lx->p++; return 1; }
    if (c == ')') { lx->p++; return 2; }
    size_t i = 0;
    while (lx->p < lx->end && *lx->p != '(' && *lx->p != ')' &&
           *lx->p != ' ' && *lx->p != '\t' && *lx->p != '\n' &&
           *lx->p != '\r' && *lx->p != ';' && i + 1 < tokmax)
        tok[i++] = *lx->p++;
    tok[i] = '\0';
    return 0;
}

/* Parse one list: '(' child ... ')'.  'lx' positioned right after the
 * opening paren.  Returns the list node (children are atoms or lists). */
static sexp_t *rc_parse_list(lexer_t *lx) {
    sexp_t *list = rc_node();
    if (!list)
        return NULL;
    list->line = lx->line;
    for (;;) {
        char tok[RC_TOK_MAX];
        int t = lexer_next(lx, tok, sizeof(tok));
        if (t == -1)
            return NULL;           /* unbalanced: caller records error */
        if (t == 2)
            return list;           /* closing paren */
        if (list->nchild >= RC_CHILD_MAX)
            continue;              /* drop excess children silently */
        if (t == 1) {
            sexp_t *child = rc_parse_list(lx);
            if (!child)
                return NULL;
            list->children[list->nchild++] = child;
        } else {
            sexp_t *atom = rc_node();
            if (!atom)
                return NULL;
            rc_strncpy(atom->name, tok, sizeof(atom->name));
            atom->line = lx->line;
            list->children[list->nchild++] = atom;
        }
    }
}

/* Atom at index i inside a field list, or NULL. */
static const char *field_val(sexp_t *field, int i) {
    if (i < field->nchild && field->children[i]->nchild == 0)
        return field->children[i]->name;
    return NULL;
}

static const char *field_key(sexp_t *field) {
    return field_val(field, 0);
}


static int unit_has_dep(rc_unit_t *u, const char *name) {
    for (int i = 0; i < u->ndeps; i++)
        if (rc_strcmp(u->deps[i], name) == 0)
            return 1;
    return 0;
}

static int unit_parse(rc_unit_t *u, sexp_t *node, const char *src) {
    /* node->children[0] == atom "unit", [1] == name, rest are fields */
    if (node->nchild < 2 || field_val(node, 0) == NULL ||
        rc_strcmp(field_val(node, 0), "unit") != 0)
        return 0;
    const char *name = field_val(node, 1);
    if (!name || !name[0])
        return 0;
    rc_memzero(u, sizeof(*u));
    rc_strncpy(u->name, name, sizeof(u->name));
    u->line = node->line;
    u->stdio = RC_IO_CONSOLE;   /* default: console stdio */

    for (int f = 2; f < node->nchild; f++) {
        sexp_t *fd = node->children[f];
        const char *key = field_key(fd);
        if (!key)
            continue;

        if (rc_strcmp(key, "type") == 0) {
            const char *v = field_val(fd, 1);
            if (!v) continue;
            if (rc_strcmp(v, "mount") == 0)          u->type = RC_T_MOUNT;
            else if (rc_strcmp(v, "oneshot") == 0)   u->type = RC_T_ONESHOT;
            else if (rc_strcmp(v, "service") == 0)   u->type = RC_T_SERVICE;
        } else if (rc_strcmp(key, "exec") == 0) {
            const char *v = field_val(fd, 1);
            if (!v) continue;
            rc_strncpy(u->exec, v, sizeof(u->exec));
            for (int a = 2; a < fd->nchild && u->argc < RC_ARG_MAX; a++) {
                const char *av = field_val(fd, a);
                if (av)
                    rc_strncpy(u->argv[u->argc++], av, RC_ARG_LEN);
            }
        } else if (rc_strcmp(key, "stdio") == 0) {
            const char *v = field_val(fd, 1);
            if (v && rc_strcmp(v, "silent") == 0)
                u->stdio = RC_IO_SILENT;
            else
                u->stdio = RC_IO_CONSOLE;
        } else if (rc_strcmp(key, "respawn") == 0) {
            const char *v = field_val(fd, 1);
            if (v && rc_strcmp(v, "never") == 0)         u->respawn = RC_R_NEVER;
            else if (v && rc_strcmp(v, "always") == 0)   u->respawn = RC_R_ALWAYS;
            else                                          u->respawn = RC_R_ON_FAILURE;
        } else if (rc_strcmp(key, "deps") == 0) {
            for (int d = 1; d < fd->nchild && u->ndeps < RC_DEPS_MAX; d++) {
                const char *v = field_val(fd, d);
                if (v && !unit_has_dep(u, v))
                    rc_strncpy(u->deps[u->ndeps++], v, RC_UNIT_NAME_MAX);
            }
        } else if (rc_strcmp(key, "after") == 0) {
            for (int d = 1; d < fd->nchild && u->nafter < RC_DEPS_MAX; d++) {
                const char *v = field_val(fd, d);
                if (v)
                    rc_strncpy(u->after[u->nafter++], v, RC_UNIT_NAME_MAX);
            }
        } else if (rc_strcmp(key, "ready") == 0) {
            const char *kind = field_val(fd, 1);
            const char *arg  = field_val(fd, 2);
            if (kind && arg && rc_strcmp(kind, "port") == 0) {
                u->ready_kind = RC_READY_PORT;
                rc_strncpy(u->ready_port, arg, sizeof(u->ready_port));
            } else if (kind && arg && rc_strcmp(kind, "sleep") == 0) {
                u->ready_kind = RC_READY_SLEEP;
                u->ready_secs = rc_atoi(arg);
            }
        } else if (rc_strcmp(key, "env") == 0) {
            for (int e = 1; e < fd->nchild && u->nenv < RC_ENV_MAX; e++) {
                const char *v = field_val(fd, e);
                if (v)
                    rc_strncpy(u->env[u->nenv++], v, RC_ENV_LEN);
            }
        } else if (rc_strcmp(key, "uid") == 0) {
            const char *v = field_val(fd, 1);
            if (v) u->uid = rc_atoi(v);
        } else if (rc_strcmp(key, "gid") == 0) {
            const char *v = field_val(fd, 1);
            if (v) u->gid = rc_atoi(v);
        } else if (rc_strcmp(key, "device") == 0) {
            const char *v = field_val(fd, 1);
            if (v) rc_strncpy(u->device, v, sizeof(u->device));
        } else if (rc_strcmp(key, "fstype") == 0) {
            const char *v = field_val(fd, 1);
            if (v) rc_strncpy(u->fstype, v, sizeof(u->fstype));
        } else if (rc_strcmp(key, "mountpoint") == 0) {
            const char *v = field_val(fd, 1);
            if (v) rc_strncpy(u->mountpoint, v, sizeof(u->mountpoint));
        } else if (rc_strcmp(key, "flags") == 0) {
            const char *v = field_val(fd, 1);
            if (v) rc_strncpy(u->mflags, v, sizeof(u->mflags));
        }
        /* unknown fields are ignored (tolerant forward-compat) */
    }

    /* Validate */
    if (!u->type) {
        rc_set_err(src, u->line, "unit has no valid (type ...)");
        return 0;
    }
    if (u->type == RC_T_SERVICE) {
        if (!u->exec[0]) {
            rc_set_err(src, u->line, "service unit needs (exec ...)");
            return 0;
        }
        if (u->respawn == 0)
            u->respawn = RC_R_ON_FAILURE;   /* default */
    } else if (u->type == RC_T_ONESHOT) {
        if (!u->exec[0]) {
            rc_set_err(src, u->line, "oneshot unit needs (exec ...)");
            return 0;
        }
    } else if (u->type == RC_T_MOUNT) {
        if (!u->device[0] || !u->fstype[0] || !u->mountpoint[0]) {
            rc_set_err(src, u->line,
                       "mount unit needs (device ...) (fstype ...) (mountpoint ...)");
            return 0;
        }
    }
    return 1;
}


int rc_parse_buf(const char *buf, size_t len, rc_unit_t *units, int max,
                 const char *srcname) {
    rc_err[0] = '\0';
    rc_npool = 0;

    lexer_t lx;
    lx.p = buf;
    lx.end = buf + len;
    lx.line = 1;

    int count = 0;
    for (;;) {
        char tok[RC_TOK_MAX];
        int t = lexer_next(&lx, tok, sizeof(tok));
        if (t == -1)
            break;
        if (t == 1) {
            sexp_t *node = rc_parse_list(&lx);
            if (!node) {
                rc_set_err(srcname, lx.line, "unbalanced parentheses");
                break;
            }
            if (count < max) {
                if (unit_parse(&units[count], node, srcname))
                    count++;
            }
        } else if (t == 2) {
            /* Stray ')' at top level: warn and skip, keep going. */
            rc_set_err(srcname, lx.line, "stray ')' at top level");
        }
        /* stray atoms at top level are skipped */
    }
    return count;
}


#ifndef HOST_TEST

#include "../syscall.h"

/* linux_dirent64-compatible (kernel bsd/include/bsd/dirent.h) */
struct rc_dirent {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
};

#define RC_DT_REG 8

static long rc_bsd_open(const char *path) {
    return syscall3(BSD_SYS(SYS_OPEN), (long)path, 0 /* O_RDONLY */, 0);
}

static long rc_bsd_close(long fd) {
    return syscall1(BSD_SYS(SYS_CLOSE), fd);
}

static long rc_bsd_read(long fd, void *buf, unsigned long cnt) {
    return syscall3(BSD_SYS(SYS_READ), fd, (long)buf, cnt);
}

int rc_scan_dir(const char *dirpath, rc_unit_t *units, int max) {
    long fd = rc_bsd_open(dirpath);
    if (fd < 0)
        return RC_SCAN_NODIR;

    int total = 0;
    for (;;) {
        char dbuf[512];
        long n = syscall3(BSD_SYS(SYS_GETDENTS), fd, (long)dbuf, (long)sizeof(dbuf));
        if (n <= 0)
            break;

        long off = 0;
        while (off < n) {
            struct rc_dirent *de = (struct rc_dirent *)(void *)(dbuf + off);
            if (de->d_reclen < sizeof(struct rc_dirent) - 256 ||
                off + de->d_reclen > n)
                break;
            off += de->d_reclen;

            if (de->d_type != RC_DT_REG || !rc_ends_with(de->d_name, ".rc"))
                continue;

            char path[RC_PATH_MAX];
            size_t dl = rc_strlen(dirpath);
            if (dl + 1 + rc_strlen(de->d_name) + 1 > sizeof(path))
                continue;
            size_t i = 0;
            for (; i < dl; i++) path[i] = dirpath[i];
            path[i++] = '/';
            for (size_t j = 0; de->d_name[j] && i + 1 < sizeof(path); j++)
                path[i++] = de->d_name[j];
            path[i] = '\0';

            long ffd = rc_bsd_open(path);
            if (ffd < 0)
                continue;
            char buf[4096];
            size_t used = 0;
            while (used < sizeof(buf)) {
                long r = rc_bsd_read(ffd, buf + used, sizeof(buf) - used);
                if (r <= 0)
                    break;
                used += (size_t)r;
            }
            rc_bsd_close(ffd);

            int got = rc_parse_buf(buf, used, units + total, max - total, path);
            total += got;
            if (total >= max)
                break;
        }
        if (total >= max)
            break;
    }
    rc_bsd_close(fd);
    return total;
}

#endif /* !HOST_TEST */
