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


/* host_fs_test.c — functional test of the full read-write ext2 driver.
 *
 * Builds a real mke2fs image in RAM, mounts it through the ext2 driver
 * (compiled for the host), exercises create/write/read/mkdir/unlink/
 * symlink/hardlink/rename/truncate/sparse, unmounts, and then validates
 * the image with the real e2fsck and debugfs from e2fsprogs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "ext2.h"
#include "bsd/block.h"
#include "bsd/stat.h"
#include "bsd/errno.h"
#include "test_platform.h"

#define IMG_PATH  "/tmp/arc_ext2_test.img"

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
} while(0)

static uint8_t *g_image;
static size_t   g_size = 4 * 1024 * 1024;
static block_dev_t g_dev;

static int img_read(block_dev_t *d, uint64_t lba, void *buf, size_t count) {
    uint64_t off = lba * d->block_size;
    size_t len = count * d->block_size;
    if (off + len > g_size) return -1;
    memcpy(buf, g_image + off, len);
    return (int)count;
}

static int img_write(block_dev_t *d, uint64_t lba, const void *buf, size_t count) {
    uint64_t off = lba * d->block_size;
    size_t len = count * d->block_size;
    if (off + len > g_size) return -1;
    memcpy(g_image + off, buf, len);
    return (int)count;
}

static void build_image(void) {
    FILE *f;
    g_image = (uint8_t *)calloc(1, g_size);
    if (!g_image) { printf("OOM\n"); exit(1); }

    f = fopen(IMG_PATH, "wb");
    if (!f) { printf("cannot open %s\n", IMG_PATH); exit(1); }
    fwrite(g_image, 1, g_size, f);
    fclose(f);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "mke2fs -q -t ext2 -I 128 -b 1024 -L ARCROOT -F %s 2>/dev/null", IMG_PATH);
    if (system(cmd) != 0) { printf("mke2fs failed\n"); exit(1); }

    f = fopen(IMG_PATH, "rb");
    fread(g_image, 1, g_size, f);
    fclose(f);

    memset(&g_dev, 0, sizeof(g_dev));
    strncpy(g_dev.name, "arcdisk", sizeof(g_dev.name) - 1);
    g_dev.block_size = 512;
    g_dev.num_blocks = g_size / 512;
    g_dev.read = img_read;
    g_dev.write = img_write;
}

static void dump_image(void) {
    FILE *f = fopen(IMG_PATH, "wb");
    fwrite(g_image, 1, g_size, f);
    fclose(f);
}

/* Simple read-back helper */
static int vn_read(vnode_t *vp, void *buf, size_t count, int offset) {
    return vp->ops->read(vp, buf, count, offset);
}

static int vn_write(vnode_t *vp, const void *buf, size_t count, int offset) {
    return vp->ops->write(vp, buf, count, offset);
}

void host_fs_tests(void) {
    printf("[ext2 functional]\n");
    build_image();

    mount_t mp;
    memset(&mp, 0, sizeof(mp));
    setvbuf(stdout, NULL, _IONBF, 0);
    int r = ext2_mount(&g_dev, &mp);
    TEST("mount mke2fs image", r == 0);
    if (r != 0) {
        printf("[ext2 functional] %d/%d passed\n", total - failures, total);
        return;
    }
    vnode_t *root = mp.root;

    /* ---- create + write + read ---- */
    vnode_t *f1 = NULL;
    r = root->ops->create(root, "hello.txt", 0644, &f1);
    TEST("create hello.txt", r == 0 && f1 != NULL);
    if (f1) {
        static const char msg[] = "Hello, ext2 world!\n";
        int w = vn_write(f1, msg, sizeof(msg) - 1, 0);
        TEST("write hello.txt", w == (int)(sizeof(msg) - 1));

        char buf[64];
        memset(buf, 0, sizeof(buf));
        int rd = vn_read(f1, buf, sizeof(buf) - 1, 0);
        TEST("read back hello.txt", rd == (int)(sizeof(msg) - 1) &&
             memcmp(buf, msg, sizeof(msg) - 1) == 0);

        struct stat st;
        TEST("fstat hello.txt", f1->ops->stat(f1, &st) == 0 &&
             st.st_size == (int)(sizeof(msg) - 1) && st.st_nlink == 1 &&
             (st.st_mode & S_IFMT) == S_IFREG);
        f1->ops->close(f1);
    }

    /* ---- mkdir + nested file + multi-block file ---- */
    r = root->ops->mkdir(root, "etc", 0755);
    TEST("mkdir etc", r == 0);
    r = root->ops->mkdir(root, "etc", 0755);
    TEST("mkdir etc again -> EEXIST", r == -EEXIST);

    vnode_t *etc = root->ops->lookup(root, "etc");
    TEST("lookup etc", etc != NULL && etc->type == VDIR);
    if (etc) {
        static const char *const words[] = {
            "alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta",
            "iota", "kappa", "lambda", "mu", "nu", "xi", "omicron", "pi",
            "rho", "sigma", "tau", "upsilon", "phi", "chi", "psi", "omega"
        };
        char big[8192];
        int off = 0;
        for (int i = 0; i < (int)(sizeof(words) / sizeof(words[0])); i++)
            off += snprintf(big + off, sizeof(big) - off - 1, "%s:%d\n", words[i], i * 1000);

        vnode_t *pf = NULL;
        r = etc->ops->create(etc, "passwd", 0644, &pf);
        TEST("create etc/passwd", r == 0 && pf != NULL);
        if (pf) {
            int w = vn_write(pf, big, (size_t)off, 0);
            TEST("write multi-block passwd", w == off);

            char *rb = (char *)malloc((size_t)off + 1);
            memset(rb, 0, (size_t)off + 1);
            int rd = vn_read(pf, rb, (size_t)off, 0);
            TEST("read back passwd", rd == off && memcmp(rb, big, (size_t)off) == 0);
            free(rb);
            pf->ops->close(pf);
        }
        etc->ops->close(etc);
    }

    /* ---- big file: double-indirect ---- */
    {
        vnode_t *bf = NULL;
        int cr = root->ops->create(root, "big.bin", 0644, &bf);
        TEST("create big.bin", cr == 0 && bf != NULL);
        if (bf) {
            size_t sz = 300 * 1024;
            uint8_t *data = (uint8_t *)malloc(sz);
            uint8_t *rb = (uint8_t *)malloc(sz);
            for (size_t i = 0; i < sz; i++) data[i] = (uint8_t)(i * 31 + 7);
            int w = vn_write(bf, data, sz, 0);
            TEST("write 300KB big.bin", w == (int)sz);
            memset(rb, 0, sz);
            int rd = vn_read(bf, rb, sz, 0);
            TEST("read back 300KB big.bin", rd == (int)sz && memcmp(data, rb, sz) == 0);

            /* truncate down to 3KB and re-read */
            struct stat st;
            bf->ops->open(bf, O_TRUNC);
            bf->ops->stat(bf, &st);
            TEST("O_TRUNC big.bin -> size 0", st.st_size == 0);

            w = vn_write(bf, data, 3000, 0);
            TEST("rewrite 3KB after truncate", w == 3000);
            memset(rb, 0, 3000);
            rd = vn_read(bf, rb, 3000, 0);
            TEST("read back after truncate", rd == 3000 && memcmp(data, rb, 3000) == 0);
            free(data);
            free(rb);
            bf->ops->close(bf);
        }
    }

    /* ---- sparse file ---- */
    {
        vnode_t *sf = NULL;
        int cr = root->ops->create(root, "sparse.dat", 0644, &sf);
        TEST("create sparse.dat", cr == 0 && sf != NULL);
        if (sf) {
            uint8_t buf[256];
            memset(buf, 0xAB, sizeof(buf));
            int w = vn_write(sf, buf, sizeof(buf), 70000);
            TEST("sparse write at offset 70000", w == (int)sizeof(buf));

            uint8_t rd[256];
            memset(rd, 0, sizeof(rd));
            int got = vn_read(sf, rd, sizeof(rd), 0);
            TEST("sparse hole reads zero", got == (int)sizeof(rd));
            int allzero = 1;
            for (size_t i = 0; i < sizeof(rd); i++)
                if (rd[i] != 0) { allzero = 0; break; }
            TEST("sparse hole content zero", allzero);

            struct stat st;
            sf->ops->stat(sf, &st);
            TEST("sparse file size", st.st_size == 70000 + (int)sizeof(buf));
            sf->ops->close(sf);
        }
    }

    /* ---- symlink ---- */
    {
        r = root->ops->symlink(root, "link1", "hello.txt");
        TEST("symlink link1 -> hello.txt", r == 0);
        vnode_t *sl = root->ops->lookup(root, "link1");
        TEST("lookup link1 (VLNK)", sl != NULL && sl->type == VLNK);
        if (sl) {
            char target[64];
            int n = sl->ops->readlink(sl, target, sizeof(target));
            target[n] = '\0';
            TEST("readlink link1", n == 9 && strcmp(target, "hello.txt") == 0);
            struct stat st;
            sl->ops->stat(sl, &st);
            TEST("stat link1 is symlink", (st.st_mode & S_IFMT) == S_IFLNK);
            sl->ops->close(sl);
        }
    }

    /* ---- hard link ---- */
    {
        vnode_t *tgt = root->ops->lookup(root, "hello.txt");
        r = root->ops->link(root, "hello_hard", tgt);
        TEST("hard link hello_hard", r == 0);
        tgt->ops->close(tgt);

        vnode_t *hl = root->ops->lookup(root, "hello_hard");
        TEST("lookup hello_hard", hl != NULL);
        if (hl) {
            char buf[64];
            memset(buf, 0, sizeof(buf));
            int rd = vn_read(hl, buf, sizeof(buf) - 1, 0);
            TEST("read via hard link", rd > 0 && memcmp(buf, "Hello, ext2 world!\n", 19) == 0);
            struct stat st;
            hl->ops->stat(hl, &st);
            TEST("hard link nlink == 2", st.st_nlink == 2);
            hl->ops->close(hl);
        }

        r = root->ops->unlink(root, "hello.txt");
        TEST("unlink hello.txt (nlink 2->1)", r == 0);

        vnode_t *hl2 = root->ops->lookup(root, "hello_hard");
        TEST("hello_hard survives unlink", hl2 != NULL);
        if (hl2) {
            struct stat st;
            hl2->ops->stat(hl2, &st);
            TEST("hello_hard nlink == 1", st.st_nlink == 1);
            hl2->ops->close(hl2);
        }
    }

    /* ---- rename ---- */
    {
        vnode_t *etc = root->ops->lookup(root, "etc");
        TEST("rename src dir lookup", etc != NULL);
        if (!etc) {
            printf("  SKIP: rename block (lookup failed)\n");
        } else {
        vnode_t *etc2 = etc;
        r = etc->ops->rename(etc, "passwd", etc, "shadow");
        TEST("rename etc/passwd -> etc/shadow", r == 0);
        vnode_t *sh = etc->ops->lookup(etc, "shadow");
        TEST("lookup etc/shadow", sh != NULL);
        if (sh) {
            char *buf = (char *)malloc(8193);
            memset(buf, 0, 8193);
            int rd = vn_read(sh, buf, 8192, 0);
            TEST("shadow content after rename", rd > 100);
            free(buf);
            sh->ops->close(sh);
        }
        vnode_t *gone = etc->ops->lookup(etc, "passwd");
        TEST("old name gone", gone == NULL);
        if (etc2) etc2->ops->close(etc2);
        }
    }

    /* ---- many files in one dir (multi-block directory) ---- */
    {
        vnode_t *sub = NULL;
        r = root->ops->mkdir(root, "sub", 0755);
        TEST("mkdir sub", r == 0);
        sub = root->ops->lookup(root, "sub");
        TEST("lookup sub", sub != NULL);
        if (sub) {
            int ok = 1;
            for (int i = 0; i < 60; i++) {
                char name[32];
                snprintf(name, sizeof(name), "file%03d", i);
                vnode_t *nf = NULL;
                if (sub->ops->create(sub, name, 0644, &nf) != 0 || !nf) { ok = 0; break; }
                uint8_t b[4] = {(uint8_t)i, (uint8_t)(i >> 8), (uint8_t)(i >> 16), (uint8_t)(i >> 24)};
                nf->ops->write(nf, b, 4, 0);
                nf->ops->close(nf);
            }
            TEST("create 60 files in sub", ok);

            vnode_t *f17 = sub->ops->lookup(sub, "file017");
            TEST("lookup file017 (multi-block dir)", f17 != NULL);
            if (f17) {
                uint8_t rd[4] = {0, 0, 0, 0};
                f17->ops->read(f17, rd, 4, 0);
                TEST("file017 content", rd[0] == 17);
                f17->ops->close(f17);
            }

            /* unlink every other */
            int okd = 1;
            for (int i = 0; i < 60; i += 2) {
                char name[32];
                snprintf(name, sizeof(name), "file%03d", i);
                if (sub->ops->unlink(sub, name) != 0) { okd = 0; break; }
            }
            TEST("unlink 30 files in sub", okd);

            vnode_t *f58 = sub->ops->lookup(sub, "file058");
            TEST("file058 gone (even, unlinked)", f58 == NULL);
            if (f58) { f58->ops->close(f58); }
            vnode_t *f01 = sub->ops->lookup(sub, "file001");
            TEST("file001 survives", f01 != NULL);
            if (f01) { f01->ops->close(f01); }
            vnode_t *f00 = sub->ops->lookup(sub, "file000");
            TEST("file000 gone", f00 == NULL);

            /* rename across directories */
            r = sub->ops->rename(sub, "file001", root, "moved.dat");
            TEST("rename sub/file001 -> /moved.dat", r == 0);
            vnode_t *mv = root->ops->lookup(root, "moved.dat");
            TEST("lookup moved.dat", mv != NULL);
            if (mv) { mv->ops->close(mv); }

            sub->ops->close(sub);
        }
    }

    /* ---- rmdir semantics ---- */
    {
        vnode_t *sub = root->ops->lookup(root, "sub");
        r = root->ops->rmdir(root, "sub");
        TEST("rmdir non-empty sub -> ENOTEMPTY", r == -ENOTEMPTY);
        if (sub) sub->ops->close(sub);

        vnode_t *sub2 = NULL;
        r = root->ops->mkdir(root, "empty", 0755);
        TEST("mkdir empty", r == 0);
        sub2 = root->ops->lookup(root, "empty");
        if (sub2) sub2->ops->close(sub2);
        r = root->ops->rmdir(root, "empty");
        TEST("rmdir empty ok", r == 0);
        vnode_t *gone = root->ops->lookup(root, "empty");
        TEST("empty dir gone", gone == NULL);
    }

    /* ---- stat of a directory ---- */
    {
        struct stat st;
        vnode_t *etc = root->ops->lookup(root, "etc");
        if (etc) {
            etc->ops->stat(etc, &st);
            TEST("stat etc is dir", (st.st_mode & S_IFMT) == S_IFDIR);
            TEST("etc nlink >= 2", st.st_nlink >= 2);
            etc->ops->close(etc);
        }
    }

    /* ---- unmount, then verify with real e2fsprogs ---- */
    ext2_unmount(&mp);
    dump_image();

    int e2 = system("e2fsck -fn " IMG_PATH " >/tmp/arc_e2fsck.out 2>&1");
    TEST("e2fsck clean (exit 0)", e2 == 0);
    if (e2 != 0) {
        FILE *o = fopen("/tmp/arc_e2fsck.out", "r");
        char line[256];
        while (o && fgets(line, sizeof(line), o))
            printf("      %s", line);
        if (o) fclose(o);
    }

    int db = system("debugfs -R 'cat /etc/shadow' " IMG_PATH
                    " >/tmp/arc_dbg.out 2>&1");
    TEST("debugfs cat /etc/shadow works", db == 0);

    db = system("debugfs -R 'cat /big.bin' " IMG_PATH
                " >/tmp/arc_dbg_big.out 2>&1");
    TEST("debugfs cat /big.bin works", db == 0);

    db = system("debugfs -R 'stat /link1' " IMG_PATH
                " >/tmp/arc_dbg_lnk.out 2>&1");
    TEST("debugfs stat /link1 works", db == 0);

    free(g_image);
    printf("[ext2 functional] %d/%d passed\n", total - failures, total);
}
