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


#include "aml.h"
#include "debug.h"
#include "vmm.h"
#include "string.h"
#include "acpi.h"
#include "memory.h"


#define AML_MAX_NODES   2048      /* max namespace nodes + temp results */
#define AML_MAX_SCOPE   64        /* max scope nesting during parse */

/* ---- Hardening limits: a hostile or sloppy DSDT must not cause OOB
 * reads, unbounded kmalloc, boot hangs, or namespace pollution. ---- */
#define AML_MAX_STRING_LEN    512          /* stored Name string bytes */
#define AML_MAX_BUFFER_LEN    (64u * 1024) /* stored Buffer payload */
#define AML_MAX_PACKAGE_ELEMS 128          /* Package NumElements */
#define AML_MAX_METHOD_BODY   (256u * 1024)
#define AML_MAX_DEPTH         32           /* Scope/Device/... nesting */
#define AML_MAX_PARSE_STEPS   200000       /* terms visited per aml_init */
#define AML_MAX_EXEC_STEPS    512          /* terms executed per method */
#define AML_MAX_EXEC_DEPTH    16           /* nested If/While in exec */
#define AML_MAX_PATH_LEN      256          /* aml_find path chars */
#define AML_MAX_PATH_SEGS     32           /* aml_find name segments */


static aml_node_t node_pool[AML_MAX_NODES];
static int         node_pool_used;

/* Budget for parse_termlist/parse_term progress (reset in aml_init).
 * A truncated DSDT could otherwise spin the resync logic over garbage. */
static int parse_steps;

static aml_node_t *aml_alloc_node(void) {
    if (node_pool_used >= AML_MAX_NODES) return NULL;
    aml_node_t *n = &node_pool[node_pool_used++];
    memset(n, 0, sizeof(aml_node_t));
    return n;
}


/* True if n bytes at off are fully inside [0, end).  All AML reads go
 * through this; off/end are int offsets into a bounded copy, so no
 * pointer arithmetic on untrusted values ever escapes the buffer. */
static int aml_can_read(int off, int end, int n) {
    if (off < 0 || end < 0 || n < 0) return 0;
    if (off > end) return 0;
    return (end - off) >= n;
}

/* Clamp a value into [lo, hi]. */
static int aml_clamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}


/* Copy a 4-byte NameSeg after validating length and characters.
 * Returns 0 on success, -1 when truncated or not a valid NameSeg
 * (LeadNameChar [_A-Z], NameChar [_A-Z0-9]). */
static int read_name_seg_checked(const uint8_t *data, int off, int end,
                                 char *out) {
    if (!aml_can_read(off, end, 4)) return -1;
    uint8_t c0 = data[off], c1 = data[off+1], c2 = data[off+2], c3 = data[off+3];
    if (!(c0 == '_' || (c0 >= 'A' && c0 <= 'Z'))) return -1;
    uint8_t rest[3] = { c1, c2, c3 };
    for (int i = 0; i < 3; i++) {
        uint8_t c = rest[i];
        if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return -1;
    }
    out[0] = (char)c0; out[1] = (char)c1; out[2] = (char)c2; out[3] = (char)c3;
    return 0;
}


/* Checked AML PkgLength decode (ACPI 6.5 s20.2.4, cf. ACPICA
 * psargs.c:acpi_ps_get_next_package_length).
 * Returns 0 on success with *len_out = decoded length (which includes
 * the PkgLength encoding bytes themselves) and *hdr_out = encoding
 * size (1 + extra).  Returns -1 when the encoding is truncated. */
static int read_pkg_length_checked(const uint8_t *data, int *offset, int end,
                                   int *len_out, int *hdr_out) {
    int off = *offset;
    if (!aml_can_read(off, end, 1)) return -1;
    uint8_t byte0 = data[off++];
    int extra = (byte0 >> 6) & 3;
    if (!aml_can_read(off, end, extra)) return -1;
    /* Single-byte form: bits 5:0 are the length (0-63).
     * Multi-byte form: only the low nibble of the lead byte is length
     * bits 3:0; follow bytes contribute bits 11:4, 19:12, 27:20
     * (i.e. shift 4 + i*8).  Masking with 0x3F / shifting by 6 here
     * inflates every multi-byte container and desyncs the parser
     * ("truncated Scope/Device/Method/Package, clamped" cascade). */
    unsigned len;
    if (extra == 0) {
        len = byte0 & 0x3F;
    } else {
        len = byte0 & 0x0F;
        for (int i = 0; i < extra; i++)
            len |= (unsigned)data[off++] << (4 + i * 8);
    }
    /* PkgLength is at most a 30-bit value; anything beyond our buffer
     * is truncated input, but the length itself is still well-defined. */
    if (len > 0x40000000u) return -1;
    *offset  = off;
    *len_out = (int)len;
    *hdr_out = 1 + extra;
    return 0;
}

/* Validate a PkgLength container end.
 * pkg_off = offset of the PkgLength first byte, len/hdr from the
 * checked decode above, outer_end = enclosing container end.
 * On success stores the container end and returns 0.
 * Returns 1 with end clamped to outer_end when the firmware claims
 * more bytes than the enclosing container holds (truncated table).
 * Returns -1 when len does not even cover its own encoding
 * (corrupt PkgLength). */
static int aml_pkg_end(int pkg_off, int len, int hdr, int outer_end,
                       int *end_out) {
    if (len < hdr || pkg_off < 0 || outer_end < 0) return -1;
    /* len includes its own hdr bytes: end = pkg_off + len.  All values
     * are small (bounded buffer), 64-bit math just for certainty. */
    uint64_t end64 = (uint64_t)(int64_t)pkg_off + (uint64_t)(uint32_t)len;
    if (end64 > (uint64_t)(int64_t)outer_end) {
        *end_out = outer_end;
        return 1;
    }
    *end_out = (int)end64;
    return 0;
}


/* Read a constant integer at *offset with bounds checks.
 * Handles: ZeroOp, OneOp, OnesOp, BytePrefix, WordPrefix, DWordPrefix,
 * QWordPrefix.  Returns 0 on success, -1 when truncated or when the
 * opcode is not a constant integer (caller decides how to recover). */
static int read_integer_checked(const uint8_t *data, int *offset, int end,
                                uint64_t *val_out) {
    int off = *offset;
    if (!aml_can_read(off, end, 1)) return -1;
    uint8_t op = data[off++];

    switch (op) {
    case AML_ZERO_OP:       *val_out = 0; *offset = off; return 0;
    case AML_ONE_OP:        *val_out = 1; *offset = off; return 0;
    case AML_ONES_OP:       *val_out = ~0ULL; *offset = off; return 0;
    case AML_BYTE_PREFIX:
        if (!aml_can_read(off, end, 1)) return -1;
        *val_out = data[off]; *offset = off + 1; return 0;
    case AML_WORD_PREFIX: {
        if (!aml_can_read(off, end, 2)) return -1;
        *val_out = (uint64_t)data[off] | ((uint64_t)data[off+1] << 8);
        *offset = off + 2; return 0;
    }
    case AML_DWORD_PREFIX: {
        if (!aml_can_read(off, end, 4)) return -1;
        *val_out = (uint64_t)data[off] | ((uint64_t)data[off+1] << 8)
                 | ((uint64_t)data[off+2] << 16) | ((uint64_t)data[off+3] << 24);
        *offset = off + 4; return 0;
    }
    case AML_QWORD_PREFIX: {
        if (!aml_can_read(off, end, 8)) return -1;
        uint64_t v = 0;
        for (int i = 0; i < 8; i++)
            v |= (uint64_t)data[off + i] << (i * 8);
        *val_out = v; *offset = off + 8; return 0;
    }
    default:
        return -1;
    }
}


/* Parse an AML NameString at *offset with bounds checks.
 * Handles: RootPrefix, ParentPrefix, NullName, NameSeg,
 * DualNamePrefix, MultiNamePrefix.  Stores the LAST NameSeg (the name
 * being defined).  A bare root ("\" + NullName) yields all-zero out
 * (caller treats it as "current scope").  Returns 0 on success,
 * -1 on truncation or malformed names; *offset is then undefined
 * (caller must resync, typically to start+1). */
static int parse_name_string_checked(const uint8_t *data, int *offset,
                                     int end, char *out) {
    int off = *offset;
    /* Skip leading root/parent prefixes (each 1 byte, bounded by end). */
    while (aml_can_read(off, end, 1) &&
           (data[off] == AML_ROOT_PREFIX || data[off] == AML_PARENT_PREFIX)) {
        off++;
    }
    if (!aml_can_read(off, end, 1)) return -1;

    if (data[off] == AML_DUAL_NAME_PREFIX) {
        off++;
        /* First seg must still be valid; only the last is kept. */
        char first[4];
        if (read_name_seg_checked(data, off, end, first) < 0) return -1;
        off += 4;
        if (read_name_seg_checked(data, off, end, out) < 0) return -1;
        off += 4;
    } else if (data[off] == AML_MULTI_NAME_PREFIX) {
        off++;
        if (!aml_can_read(off, end, 1)) return -1;
        int count = data[off++];
        if (count <= 0) return -1;
        if (!aml_can_read(off, end, count * 4)) return -1;
        for (int i = 0; i < count; i++) {
            if (i == count - 1) {
                if (read_name_seg_checked(data, off, end, out) < 0) return -1;
            } else {
                char skip[4];
                if (read_name_seg_checked(data, off, end, skip) < 0) return -1;
            }
            off += 4;
        }
    } else if (data[off] == 0x00) {
        /* NullName — no name segment follows.  If RootPrefix was seen,
         * the name IS the root scope.  Signal this by zeroing out. */
        off++;
        out[0] = out[1] = out[2] = out[3] = 0;
    } else {
        /* Simple NameSeg */
        if (read_name_seg_checked(data, off, end, out) < 0) return -1;
        off += 4;
    }
    *offset = off;
    return 0;
}


/* Compare two 4-byte name segments. */
static int seg_eq(const char *a, const char *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* Find a child node by its 4-byte name segment. */
static aml_node_t *find_child(aml_node_t *parent, const char *seg) {
    for (aml_node_t *c = parent->child; c; c = c->next) {
        if (seg_eq(c->seg, seg))
            return c;
    }
    return NULL;
}

/* Add a child node to a parent. */
static void add_child(aml_node_t *parent, aml_node_t *child) {
    child->parent = parent;
    child->next = parent->child;
    parent->child = child;
}

/* Create a new namespace node with the given name segment and type, add to scope. */
static aml_node_t *new_node(const char seg[4], aml_node_type_t type, aml_node_t *scope) {
    aml_node_t *n = aml_alloc_node();
    if (!n) return NULL;
    n->seg[0] = seg[0]; n->seg[1] = seg[1];
    n->seg[2] = seg[2]; n->seg[3] = seg[3];
    n->type = type;
    if (scope)
        add_child(scope, n);
    return n;
}


static int parse_termlist(const uint8_t *data, int offset, int end,
                           aml_node_t *scope, int depth);
static int parse_value(const uint8_t *data, int offset, int end, aml_node_t *node);


/* Try to determine the size of a term starting at offset.
 * Fallback advance for the parse_termlist resync path only; the real
 * parse is done by parse_term.  Fully bounds-checked: never reads past
 * end and never returns a size reaching past end.
 * Returns the number of bytes the term occupies, or -1 if unknown. */
static int term_size(const uint8_t *data, int offset, int end) {
    if (offset >= end) return 0;
    if (!aml_can_read(offset, end, 1)) return 0;
    int start = offset;
    uint8_t op = data[offset++];

    switch (op) {
    /* Single-byte constant terms */
    case AML_ZERO_OP:
    case AML_ONE_OP:
    case AML_ONES_OP:
        return 1;

    /* Integer prefix + data (truncate-safe: skip opcode only) */
    case AML_BYTE_PREFIX:   return aml_can_read(offset, end, 1) ? 2 : 1;
    case AML_WORD_PREFIX:   return aml_can_read(offset, end, 2) ? 3 : 1;
    case AML_DWORD_PREFIX:  return aml_can_read(offset, end, 4) ? 5 : 1;
    case AML_QWORD_PREFIX:  return aml_can_read(offset, end, 8) ? 9 : 1;

    /* String prefix */
    case AML_STRING_PREFIX: {
        /* Null-terminated string, bounded by end */
        while (offset < end && data[offset]) offset++;
        if (offset >= end) return end - start;  /* unterminated: to end */
        return offset + 1 - start;  /* include null terminator */
    }

    /* Simple name reference (4-byte NameSeg total) */
    default:
        /* A NameSeg starts with LeadNameChar ('_'/'A'-'Z') */
        if (op == '_' || (op >= 'A' && op <= 'Z')) {
            char seg[4];
            if (read_name_seg_checked(data, start, end, seg) < 0)
                return end - start;
            return 4;
        }
        /* Other single-byte opcodes */
        /* Method-body opcodes: Return, Break, Noop, Continue, etc. */
        if (op == AML_RETURN_OP ||
            op == AML_BREAK_OP ||
            op == AML_NOOP_OP)
            return 1;  /* opcode only, return value handled separately */

        /* IfOp, ElseOp, WhileOp, ScopeOp, BufferOp, PackageOp,
         * VarPackageOp, MethodOp have PkgLength after opcode.
         * Total = opcode(1) + decoded length (which covers itself). */
        if (op == AML_IF_OP || op == AML_ELSE_OP || op == AML_WHILE_OP ||
            op == AML_SCOPE_OP || op == AML_BUFFER_OP ||
            op == AML_PACKAGE_OP || op == AML_VAR_PACKAGE_OP ||
            op == AML_METHOD_OP) {
            int tmp = start + 1;
            int len, hdr;
            if (read_pkg_length_checked(data, &tmp, end, &len, &hdr) < 0)
                return 1;
            int cend, rc = aml_pkg_end(start + 1, len, hdr, end, &cend);
            if (rc < 0) return 1;
            return cend - start;
        }

        /* Two-byte opcodes */
        if (op == AML_STORE_OP ||
            op == AML_ADD_OP || op == AML_SUBTRACT_OP ||
            op == AML_MULTIPLY_OP || op == AML_DIVIDE_OP ||
            op == AML_AND_OP || op == AML_NAND_OP ||
            op == AML_OR_OP || op == AML_XOR_OP ||
            op == AML_NOT_OP ||
            op == AML_CONCAT_OP || op == AML_MOD_OP ||
            op == AML_EQUAL_OP || op == AML_LEQUAL_OP ||
            op == AML_LGREATER_OP || op == AML_LLESS_OP ||
            op == AML_TO_INTEGER_OP) {
            /* These have fixed operands - but operands can be variable size.
             * We can't easily determine the size without parsing. */
            return -1;  /* unknown */
        }

        /* Extended opcodes */
        if (op == AML_EXT_OP) {
            if (!aml_can_read(offset, end, 1)) return 1;
            uint8_t ext = data[offset];
            /* Namespace-building extended opcodes with a TermList use PkgLength.
             * OpRegionOp (0x80) has no PkgLength — fixed/self-terminating args.
             * All others (method-body opcodes) do not use PkgLength. */
            if ((ext >= 0x81 && ext <= 0x88) || ext == 0x0C || ext == 0x0D) {
                int tmp = offset + 1;
                int len, hdr;
                if (read_pkg_length_checked(data, &tmp, end, &len, &hdr) < 0)
                    return 2;
                int cend, rc = aml_pkg_end(offset + 1, len, hdr, end, &cend);
                if (rc < 0) return 2;
                return cend - start;
            }
            /* Other extended opcodes: unknown size */
            return -1;
        }

        return -1;
    }
}


/* Parse a DataObject value at offset into node.
 * Fully bounds-checked; on corrupt/truncated input the node is left as
 * AML_NODE_NONE and the return value jumps to the enclosing container
 * end (never backwards, never past end). */
static int parse_value(const uint8_t *data, int offset, int end, aml_node_t *node) {
    if (!node) return aml_clamp(offset, 0, end >= 0 ? end : 0);
    if (end < 0) end = 0;
    if (!aml_can_read(offset, end, 1)) return end;
    uint8_t op = data[offset];

    switch (op) {
    case AML_ZERO_OP:
    case AML_ONE_OP:
    case AML_ONES_OP:
    case AML_BYTE_PREFIX:
    case AML_WORD_PREFIX:
    case AML_DWORD_PREFIX:
    case AML_QWORD_PREFIX: {
        uint64_t v;
        int off = offset;
        if (read_integer_checked(data, &off, end, &v) < 0) {
            log_print(LOG_LEVEL_WARN, "aml: truncated integer constant\n");
            node->type = AML_NODE_NONE;
            return end;
        }
        node->type = AML_NODE_INTEGER;
        node->integer = v;
        return off;
    }

    case AML_STRING_PREFIX: {
        offset++;
        int full = 0;
        while (offset + full < end && data[offset + full]) full++;
        if (offset + full >= end) {
            log_print(LOG_LEVEL_WARN, "aml: unterminated string\n");
            node->type = AML_NODE_NONE;
            return end;
        }
        int store = full > AML_MAX_STRING_LEN ? AML_MAX_STRING_LEN : full;
        node->type = AML_NODE_STRING;
        node->string = NULL;
        if (store >= 0) {
            node->string = (char *)kmalloc((uint32_t)store + 1);
            if (node->string) {
                if (store > 0) memcpy(node->string, data + offset, (uint32_t)store);
                node->string[store] = '\0';
            } else {
                node->type = AML_NODE_NONE;
            }
        }
        if (full > AML_MAX_STRING_LEN)
            log_print(LOG_LEVEL_WARN, "aml: string truncated to cap\n");
        return offset + full + 1;
    }

    case AML_BUFFER_OP: {
        int start = offset;
        offset++;  /* skip opcode */
        int pkg_len, hdr;
        if (read_pkg_length_checked(data, &offset, end, &pkg_len, &hdr) < 0) {
            log_print(LOG_LEVEL_WARN, "aml: truncated Buffer PkgLength\n");
            node->type = AML_NODE_NONE;
            return end;
        }
        int buf_end, rc = aml_pkg_end(start + 1, pkg_len, hdr, end, &buf_end);
        if (rc < 0) {
            log_print(LOG_LEVEL_WARN, "aml: corrupt Buffer PkgLength\n");
            node->type = AML_NODE_NONE;
            return aml_clamp(start + 1, 0, end);
        }
        if (rc > 0)
            log_print(LOG_LEVEL_WARN, "aml: truncated Buffer, clamped\n");
        if (buf_end < offset) {
            log_print(LOG_LEVEL_WARN, "aml: empty Buffer body\n");
            node->type = AML_NODE_NONE;
            return buf_end;
        }
        uint64_t buf_len;
        int off = offset;
        if (read_integer_checked(data, &off, buf_end, &buf_len) < 0) {
            log_print(LOG_LEVEL_WARN, "aml: truncated Buffer size\n");
            node->type = AML_NODE_NONE;
            return buf_end;
        }
        if (buf_len > AML_MAX_BUFFER_LEN) {
            log_printf(LOG_LEVEL_WARN, "aml: Buffer too large (%u), dropped\n",
                       (unsigned)(buf_len > 0xFFFFFFFFu ? 0xFFFFFFFFu : buf_len));
            node->type = AML_NODE_NONE;
            return buf_end;
        }
        node->type = AML_NODE_BUFFER;
        node->buffer.len = (uint32_t)buf_len;
        node->buffer.data = NULL;
        if (buf_len > 0) {
            node->buffer.data = (uint8_t *)kmalloc((uint32_t)buf_len);
            if (!node->buffer.data) {
                node->type = AML_NODE_NONE;
                node->buffer.len = 0;
                return buf_end;
            }
            if ((uint64_t)(buf_end - off) >= buf_len) {
                memcpy(node->buffer.data, data + off, (uint32_t)buf_len);
            } else {
                /* Truncated payload: zero-fill the tail so no heap
                 * garbage is ever exposed to readers. */
                int avail = buf_end > off ? buf_end - off : 0;
                if (avail > 0) memcpy(node->buffer.data, data + off, (uint32_t)avail);
                memset(node->buffer.data + avail, 0, (uint32_t)buf_len - (uint32_t)avail);
                log_print(LOG_LEVEL_WARN, "aml: truncated Buffer payload\n");
            }
        }
        return buf_end;
    }

    case AML_PACKAGE_OP:
    case AML_VAR_PACKAGE_OP: {
        int start = offset;
        offset++;  /* skip opcode */
        int pkg_len, hdr;
        if (read_pkg_length_checked(data, &offset, end, &pkg_len, &hdr) < 0) {
            log_print(LOG_LEVEL_WARN, "aml: truncated Package PkgLength\n");
            node->type = AML_NODE_NONE;
            return end;
        }
        int pkg_end, rc = aml_pkg_end(start + 1, pkg_len, hdr, end, &pkg_end);
        if (rc < 0) {
            log_print(LOG_LEVEL_WARN, "aml: corrupt Package PkgLength\n");
            node->type = AML_NODE_NONE;
            return aml_clamp(start + 1, 0, end);
        }
        if (rc > 0)
            log_print(LOG_LEVEL_WARN, "aml: truncated Package, clamped\n");
        /* Per the ACPI spec NumElements is a single ByteData, not a
         * full integer object. */
        if (!aml_can_read(offset, pkg_end, 1)) {
            log_print(LOG_LEVEL_WARN, "aml: truncated Package count\n");
            node->type = AML_NODE_NONE;
            return pkg_end;
        }
        int num_elems = data[offset++];
        if (num_elems > AML_MAX_PACKAGE_ELEMS) {
            log_printf(LOG_LEVEL_WARN, "aml: Package too large (%d), dropped\n",
                       num_elems);
            node->type = AML_NODE_NONE;
            return pkg_end;
        }
        node->type = AML_NODE_PACKAGE;
        node->package.count = (uint32_t)num_elems;
        node->package.elements = NULL;
        if (num_elems > 0) {
            node->package.elements = (aml_node_t **)kmalloc(
                (uint32_t)num_elems * sizeof(aml_node_t *));
            if (!node->package.elements) {
                node->type = AML_NODE_NONE;
                node->package.count = 0;
                return pkg_end;
            }
            memset(node->package.elements, 0,
                   (uint32_t)num_elems * sizeof(aml_node_t *));
            for (int i = 0; i < num_elems && offset < pkg_end; i++) {
                aml_node_t *elem = aml_alloc_node();
                if (!elem) {
                    log_print(LOG_LEVEL_WARN, "aml: node pool exhausted in Package\n");
                    break;
                }
                int next = parse_value(data, offset, pkg_end, elem);
                if (next > offset) {
                    offset = next;
                    node->package.elements[i] = elem;
                    continue;
                }
                /* No progress: the element is a DataRefObject we do not
                 * model (NameString reference such as the LNKE/LNKF link
                 * names in _PRT sub-packages, a method invocation, ...).
                 * This is legal AML, not corruption: keep the slot as a
                 * NONE placeholder (indices stay aligned for readers
                 * like aml_get_package_int) and skip the object. */
                elem->type = AML_NODE_NONE;
                int adv = -1;
                uint8_t eop = data[offset];
                if (eop == AML_ROOT_PREFIX || eop == AML_PARENT_PREFIX ||
                    eop == AML_DUAL_NAME_PREFIX ||
                    eop == AML_MULTI_NAME_PREFIX || eop == 0x00 ||
                    eop == '_' || (eop >= 'A' && eop <= 'Z')) {
                    char dummy[4];
                    int off = offset;
                    if (parse_name_string_checked(data, &off, pkg_end,
                                                  dummy) == 0)
                        adv = off;
                } else {
                    adv = term_size(data, offset, pkg_end);
                }
                if (adv > offset) {
                    offset = aml_clamp(adv, 0, pkg_end);
                    node->package.elements[i] = elem;
                    continue;
                }
                /* Last resort: consume one byte so the loop cannot stall.
                 * Bounded by pkg_end; the outer stream resyncs at pkg_end
                 * regardless. */
                offset = aml_clamp(offset + 1, 0, pkg_end);
                node->package.elements[i] = elem;
            }
        }
        return pkg_end;
    }

    default:
        break;
    }

    return offset;
}


/* Parse one namespace term.  Always returns an offset strictly inside
 * [start+1, end] (progress guaranteed, never OOB), so the caller's
 * resync loop cannot stall or escape the buffer on garbage input. */
static int parse_term(const uint8_t *data, int offset, int end,
                      aml_node_t *scope, int depth) {
    int start = offset;
    if (!aml_can_read(offset, end, 1)) return end;

    uint8_t op = data[offset++];

    switch (op) {
    /* ---------- Namespace building opcodes ---------- */
    case AML_SCOPE_OP: {
        int pkg_off = offset;
        int pkg_len, hdr;
        if (read_pkg_length_checked(data, &offset, end, &pkg_len, &hdr) < 0) {
            log_print(LOG_LEVEL_WARN, "aml: truncated Scope PkgLength\n");
            return aml_clamp(start + 1, 0, end);
        }
        int cend, rc = aml_pkg_end(pkg_off, pkg_len, hdr, end, &cend);
        if (rc < 0) {
            log_print(LOG_LEVEL_WARN, "aml: corrupt Scope PkgLength\n");
            return aml_clamp(start + 1, 0, end);
        }
        if (rc > 0)
            log_print(LOG_LEVEL_WARN, "aml: truncated Scope, clamped\n");
        char seg[4];
        if (parse_name_string_checked(data, &offset, cend, seg) < 0) {
            log_print(LOG_LEVEL_WARN, "aml: bad Scope name\n");
            return cend;
        }
        /* If the scope name is the root (NullName sentinel: all zeros),
         * do not create a new node — use the current scope as-is. */
        int is_root = (seg[0] == 0 && seg[1] == 0 && seg[2] == 0 && seg[3] == 0);
        if (offset > cend) return cend;
        if (depth >= AML_MAX_DEPTH) {
            log_print(LOG_LEVEL_WARN, "aml: scope nesting too deep, skipped\n");
            return cend;
        }
        if (is_root) {
            offset = parse_termlist(data, offset, cend, scope, depth + 1);
        } else {
            aml_node_t *child = new_node(seg, AML_NODE_SCOPE, scope);
            if (child)
                offset = parse_termlist(data, offset, cend, child, depth + 1);
            else
                offset = cend;
        }
        break;
    }

    case AML_NAME_OP: {
        char seg[4];
        if (parse_name_string_checked(data, &offset, end, seg) < 0) {
            log_print(LOG_LEVEL_WARN, "aml: bad Name name\n");
            return aml_clamp(start + 1, 0, end);
        }
        aml_node_t *node = new_node(seg, AML_NODE_NONE, scope);
        if (node) {
            offset = parse_value(data, offset, end, node);
        } else {
            /* Pool exhausted: still skip the value with a scratch node
             * so the stream stays in sync. */
            aml_node_t tmp;
            memset(&tmp, 0, sizeof(tmp));
            offset = parse_value(data, offset, end, &tmp);
            /* Free any heap the scratch value grabbed. */
            if (tmp.type == AML_NODE_STRING && tmp.string) kfree(tmp.string);
            else if (tmp.type == AML_NODE_BUFFER && tmp.buffer.data) kfree(tmp.buffer.data);
            else if (tmp.type == AML_NODE_PACKAGE && tmp.package.elements)
                kfree(tmp.package.elements);
        }
        break;
    }

    case AML_METHOD_OP: {
        int pkg_off = offset;
        int pkg_len, hdr;
        if (read_pkg_length_checked(data, &offset, end, &pkg_len, &hdr) < 0) {
            log_print(LOG_LEVEL_WARN, "aml: truncated Method PkgLength\n");
            return aml_clamp(start + 1, 0, end);
        }
        int cend, rc = aml_pkg_end(pkg_off, pkg_len, hdr, end, &cend);
        if (rc < 0) {
            log_print(LOG_LEVEL_WARN, "aml: corrupt Method PkgLength\n");
            return aml_clamp(start + 1, 0, end);
        }
        if (rc > 0)
            log_print(LOG_LEVEL_WARN, "aml: truncated Method, clamped\n");
        char seg[4];
        if (parse_name_string_checked(data, &offset, cend, seg) < 0) {
            log_print(LOG_LEVEL_WARN, "aml: bad Method name\n");
            return cend;
        }
        /* ByteData: bits 2-0 = arg_count, bit 3 = serialized flag */
        if (!aml_can_read(offset, cend, 1)) {
            log_print(LOG_LEVEL_WARN, "aml: truncated Method flags\n");
            return cend;
        }
        uint8_t info = data[offset++];
        uint8_t arg_count = info & 0x07;
        if (arg_count > 7) arg_count = 7;  /* 3-bit field, defensive */
        aml_node_t *node = new_node(seg, AML_NODE_METHOD, scope);
        if (node) {
            int body_len = cend - offset;
            if (body_len < 0) body_len = 0;
            node->method.body = NULL;
            node->method.body_len = 0;
            node->method.nargs = arg_count;
            if (body_len > 0) {
                if (body_len > (int)AML_MAX_METHOD_BODY) {
                    log_printf(LOG_LEVEL_WARN,
                               "aml: Method body too large (%d), dropped\n",
                               body_len);
                } else {
                    node->method.body = (uint8_t *)kmalloc((uint32_t)body_len);
                    if (node->method.body) {
                        memcpy(node->method.body, data + offset, (uint32_t)body_len);
                        node->method.body_len = (uint32_t)body_len;
                    } else {
                        log_print(LOG_LEVEL_WARN, "aml: out of memory for Method body\n");
                    }
                }
            }
        }
        offset = cend;
        break;
    }

    case AML_ALIAS_OP: {
        /* Alias(SourceObject, AliasObject) — skip both names */
        char seg[4];
        int off = offset;
        if (parse_name_string_checked(data, &off, end, seg) < 0 ||
            parse_name_string_checked(data, &off, end, seg) < 0) {
            log_print(LOG_LEVEL_WARN, "aml: bad Alias names\n");
            return aml_clamp(start + 1, 0, end);
        }
        offset = off;
        /* Create an alias node */
        new_node(seg, AML_NODE_NONE, scope);
        break;
    }

    /* ---------- Extended opcodes (0x5B + second byte) ---------- */
    case AML_EXT_OP: {
        if (!aml_can_read(offset, end, 1)) {
            log_print(LOG_LEVEL_WARN, "aml: truncated extended opcode\n");
            return end;
        }
        uint8_t ext = data[offset++];
        int ext_start = offset;

        switch (ext) {
        /* ---------- Namespace-building: 0x82-0x88 (with PkgLength) ---------- */
        case AML_EXT_DEVICE: {
            int pkg_len, hdr;
            if (read_pkg_length_checked(data, &offset, end, &pkg_len, &hdr) < 0) {
                log_print(LOG_LEVEL_WARN, "aml: truncated Device PkgLength\n");
                return aml_clamp(start + 1, 0, end);
            }
            int cend, rc = aml_pkg_end(ext_start, pkg_len, hdr, end, &cend);
            if (rc < 0) {
                log_print(LOG_LEVEL_WARN, "aml: corrupt Device PkgLength\n");
                return aml_clamp(start + 1, 0, end);
            }
            if (rc > 0)
                log_print(LOG_LEVEL_WARN, "aml: truncated Device, clamped\n");
            char seg[4];
            if (parse_name_string_checked(data, &offset, cend, seg) < 0) {
                log_print(LOG_LEVEL_WARN, "aml: bad Device name\n");
                return cend;
            }
            if (offset > cend) return cend;
            if (depth >= AML_MAX_DEPTH) {
                log_print(LOG_LEVEL_WARN, "aml: nesting too deep, Device skipped\n");
                return cend;
            }
            aml_node_t *child = new_node(seg, AML_NODE_DEVICE, scope);
            if (child)
                offset = parse_termlist(data, offset, cend, child, depth + 1);
            else
                offset = cend;
            break;
        }

        case AML_EXT_PROCESSOR: {
            int pkg_len, hdr;
            if (read_pkg_length_checked(data, &offset, end, &pkg_len, &hdr) < 0) {
                log_print(LOG_LEVEL_WARN, "aml: truncated Processor PkgLength\n");
                return aml_clamp(start + 1, 0, end);
            }
            int cend, rc = aml_pkg_end(ext_start, pkg_len, hdr, end, &cend);
            if (rc < 0) {
                log_print(LOG_LEVEL_WARN, "aml: corrupt Processor PkgLength\n");
                return aml_clamp(start + 1, 0, end);
            }
            if (rc > 0)
                log_print(LOG_LEVEL_WARN, "aml: truncated Processor, clamped\n");
            char seg[4];
            if (parse_name_string_checked(data, &offset, cend, seg) < 0) {
                log_print(LOG_LEVEL_WARN, "aml: bad Processor name\n");
                return cend;
            }
            /* proc_id(1), pblk_addr(4), pblk_len(1) */
            if (!aml_can_read(offset, cend, 6)) {
                log_print(LOG_LEVEL_WARN, "aml: truncated Processor header\n");
                return cend;
            }
            if (depth >= AML_MAX_DEPTH) {
                log_print(LOG_LEVEL_WARN, "aml: nesting too deep, Processor skipped\n");
                return cend;
            }
            aml_node_t *child = new_node(seg, AML_NODE_PROCESSOR, scope);
            if (child) {
                offset += 6;
                offset = parse_termlist(data, offset, cend, child, depth + 1);
            } else {
                offset = cend;
            }
            break;
        }

        case AML_EXT_THERMAL: {
            int pkg_len, hdr;
            if (read_pkg_length_checked(data, &offset, end, &pkg_len, &hdr) < 0) {
                log_print(LOG_LEVEL_WARN, "aml: truncated Thermal PkgLength\n");
                return aml_clamp(start + 1, 0, end);
            }
            int cend, rc = aml_pkg_end(ext_start, pkg_len, hdr, end, &cend);
            if (rc < 0) {
                log_print(LOG_LEVEL_WARN, "aml: corrupt Thermal PkgLength\n");
                return aml_clamp(start + 1, 0, end);
            }
            if (rc > 0)
                log_print(LOG_LEVEL_WARN, "aml: truncated Thermal, clamped\n");
            char seg[4];
            if (parse_name_string_checked(data, &offset, cend, seg) < 0) {
                log_print(LOG_LEVEL_WARN, "aml: bad Thermal name\n");
                return cend;
            }
            if (offset > cend) return cend;
            if (depth >= AML_MAX_DEPTH) {
                log_print(LOG_LEVEL_WARN, "aml: nesting too deep, Thermal skipped\n");
                return cend;
            }
            aml_node_t *child = new_node(seg, AML_NODE_THERMAL, scope);
            if (child)
                offset = parse_termlist(data, offset, cend, child, depth + 1);
            else
                offset = cend;
            break;
        }

        case AML_EXT_POWERRES: {
            int pkg_len, hdr;
            if (read_pkg_length_checked(data, &offset, end, &pkg_len, &hdr) < 0) {
                log_print(LOG_LEVEL_WARN, "aml: truncated PowerRes PkgLength\n");
                return aml_clamp(start + 1, 0, end);
            }
            int cend, rc = aml_pkg_end(ext_start, pkg_len, hdr, end, &cend);
            if (rc < 0) {
                log_print(LOG_LEVEL_WARN, "aml: corrupt PowerRes PkgLength\n");
                return aml_clamp(start + 1, 0, end);
            }
            if (rc > 0)
                log_print(LOG_LEVEL_WARN, "aml: truncated PowerRes, clamped\n");
            char seg[4];
            if (parse_name_string_checked(data, &offset, cend, seg) < 0) {
                log_print(LOG_LEVEL_WARN, "aml: bad PowerRes name\n");
                return cend;
            }
            /* system_level(1), resource_order(2) */
            if (!aml_can_read(offset, cend, 3)) {
                log_print(LOG_LEVEL_WARN, "aml: truncated PowerRes header\n");
                return cend;
            }
            if (depth >= AML_MAX_DEPTH) {
                log_print(LOG_LEVEL_WARN, "aml: nesting too deep, PowerRes skipped\n");
                return cend;
            }
            aml_node_t *child = new_node(seg, AML_NODE_POWERRES, scope);
            if (child) {
                offset += 3;
                offset = parse_termlist(data, offset, cend, child, depth + 1);
            } else {
                offset = cend;
            }
            break;
        }

        case AML_EXT_OPREGION: {
            /* DefOpRegion := OpRegionOp NameString ByteData TermArg TermArg
             * NO PkgLength — fixed/self-terminating arguments only. */
            char seg[4];
            int off = offset;
            if (parse_name_string_checked(data, &off, end, seg) < 0) {
                log_print(LOG_LEVEL_WARN, "aml: bad OpRegion name\n");
                return aml_clamp(start + 1, 0, end);
            }
            if (!aml_can_read(off, end, 1)) {
                log_print(LOG_LEVEL_WARN, "aml: truncated OpRegion space\n");
                return end;
            }
            off++;  /* RegionSpace (1 byte: SystemMemory=0, SystemIO=1, ...) */
            uint64_t dummy;
            if (read_integer_checked(data, &off, end, &dummy) < 0 ||
                read_integer_checked(data, &off, end, &dummy) < 0) {
                log_print(LOG_LEVEL_WARN, "aml: truncated OpRegion args\n");
                return end;
            }
            offset = off;
            new_node(seg, AML_NODE_OPREGION, scope);
            break;
        }

        case AML_EXT_FIELD:
        case AML_EXT_INDEXFIELD:
        case AML_EXT_BANKFIELD: {
            int pkg_len, hdr;
            if (read_pkg_length_checked(data, &offset, end, &pkg_len, &hdr) < 0) {
                log_print(LOG_LEVEL_WARN, "aml: truncated Field PkgLength\n");
                return aml_clamp(start + 1, 0, end);
            }
            int cend, rc = aml_pkg_end(ext_start, pkg_len, hdr, end, &cend);
            if (rc < 0) {
                log_print(LOG_LEVEL_WARN, "aml: corrupt Field PkgLength\n");
                return aml_clamp(start + 1, 0, end);
            }
            if (rc > 0)
                log_print(LOG_LEVEL_WARN, "aml: truncated Field, clamped\n");
            char seg[4];
            if (parse_name_string_checked(data, &offset, cend, seg) < 0) {
                log_print(LOG_LEVEL_WARN, "aml: bad Field name\n");
                return cend;
            }
            /* FieldOp has FieldFlags(1) + FieldList after NameString.
             * We skip the body entirely using PkgLength: fields are
             * not modelled, and their bit-level list needs no parse. */
            new_node(seg, AML_NODE_FIELD, scope);
            offset = cend;
            break;
        }

        case AML_EXT_DATAREGION: {
            int pkg_len, hdr;
            if (read_pkg_length_checked(data, &offset, end, &pkg_len, &hdr) < 0) {
                log_print(LOG_LEVEL_WARN, "aml: truncated DataRegion PkgLength\n");
                return aml_clamp(start + 1, 0, end);
            }
            int cend, rc = aml_pkg_end(ext_start, pkg_len, hdr, end, &cend);
            if (rc < 0) {
                log_print(LOG_LEVEL_WARN, "aml: corrupt DataRegion PkgLength\n");
                return aml_clamp(start + 1, 0, end);
            }
            if (rc > 0)
                log_print(LOG_LEVEL_WARN, "aml: truncated DataRegion, clamped\n");
            char seg[4];
            if (parse_name_string_checked(data, &offset, cend, seg) < 0) {
                log_print(LOG_LEVEL_WARN, "aml: bad DataRegion name\n");
                return cend;
            }
            new_node(seg, AML_NODE_OPREGION, scope);
            offset = cend;
            break;
        }

        /* ---------- Namespace-building (ACPI 5.0+) with PkgLength ---------- */
        case AML_EXT_GPIOPIN:
        case AML_EXT_GENERICSERIAL: {
            int pkg_len, hdr;
            if (read_pkg_length_checked(data, &offset, end, &pkg_len, &hdr) < 0) {
                log_print(LOG_LEVEL_WARN, "aml: truncated ext PkgLength\n");
                return aml_clamp(start + 1, 0, end);
            }
            int cend, rc = aml_pkg_end(ext_start, pkg_len, hdr, end, &cend);
            if (rc < 0) {
                log_print(LOG_LEVEL_WARN, "aml: corrupt ext PkgLength\n");
                return aml_clamp(start + 1, 0, end);
            }
            if (rc > 0)
                log_print(LOG_LEVEL_WARN, "aml: truncated ext body, clamped\n");
            char seg[4];
            if (parse_name_string_checked(data, &offset, cend, seg) < 0) {
                log_print(LOG_LEVEL_WARN, "aml: bad ext name\n");
                return cend;
            }
            new_node(seg, AML_NODE_NONE, scope);
            offset = cend;
            break;
        }

        /* ---------- Namespace/method no-PkgLength opcodes ---------- */
        case AML_EXT_MUTEX:
        case AML_EXT_EVENT: {
            /* Mutex/Event(NameString, ByteData) — no PkgLength */
            char seg[4];
            int off = offset;
            if (parse_name_string_checked(data, &off, end, seg) < 0 ||
                !aml_can_read(off, end, 1)) {
                log_print(LOG_LEVEL_WARN, "aml: truncated Mutex/Event\n");
                return aml_clamp(start + 1, 0, end);
            }
            off++;  /* skip sync_level / event_flags byte */
            offset = off;
            new_node(seg, AML_NODE_NONE, scope);
            break;
        }

        /* ---------- Method-body opcodes (skip silently) ---------- */
        case AML_EXT_COND_REF_OF:
        case AML_EXT_CREATE_FIELD:
        default: {
            offset = aml_clamp(start + 2, 0, end);  /* skip 0x5B + ext byte */
            break;
        }
        }
        break;
    }

    /* ---------- Simple name references (4-byte NameSeg = method call) ---------- */
    default:
        if (op == '_' || (op >= 'A' && op <= 'Z')) {
            /* A bare NameSeg term: validate and skip exactly 4 bytes.
             * (Was start+5 — an off-by-one that ate the next opcode.) */
            char seg[4];
            if (read_name_seg_checked(data, start, end, seg) < 0) {
                log_printf(LOG_LEVEL_WARN,
                           "aml: bad NameSeg at %d, op=0x%x\n", start, op);
                offset = aml_clamp(start + 1, 0, end);
            } else {
                offset = aml_clamp(start + 4, 0, end);
            }
        } else {
            /* Unknown opcode in namespace context — skip if possible */
            int sz = term_size(data, start, end);
            if (sz > 0) {
                offset = aml_clamp(start + sz, 0, end);
            } else {
                log_printf(LOG_LEVEL_WARN, "aml: cannot determine size of term at %d, op=0x%x\n",
                             start, op);
                offset = aml_clamp(start + 1, 0, end);  /* skip at least the opcode */
            }
        }
        break;
    }

    /* Final safety net: guaranteed forward progress inside the buffer. */
    if (offset <= start) offset = aml_clamp(start + 1, 0, end);
    if (offset > end)    offset = end;
    return offset;
}


static int parse_termlist(const uint8_t *data, int offset, int end,
                           aml_node_t *scope, int depth) {
    offset = aml_clamp(offset, 0, end < 0 ? 0 : end);
    if (end < 0) end = offset;
    while (offset < end) {
        if (++parse_steps > AML_MAX_PARSE_STEPS) {
            log_print(LOG_LEVEL_ERROR, "aml: parse budget exhausted, table truncated\n");
            break;
        }
        int sz = term_size(data, offset, end);
        int next;
        if (sz < 0) {
            /* Unknown size — try parsing it */
            next = parse_term(data, offset, end, scope, depth);
        } else if (sz == 0) {
            break;
        } else {
            next = parse_term(data, offset, end, scope, depth);
            if (next <= offset) {
                /* parse_term didn't advance — force skip by term_size */
                next = aml_clamp(offset + sz, 0, end);
            }
        }
        if (next <= offset) {
            /* Still stuck (should be unreachable): force progress. */
            log_print(LOG_LEVEL_WARN, "aml: term made no progress, resync\n");
            next = aml_clamp(offset + 1, 0, end);
        }
        offset = next;
    }
    return offset;
}


/* Execution context for evaluating AML methods.
 * Minimal evaluator: just enough for Return(Package{...}) patterns.
 * Hardening: step budget + nesting depth cap (no unbounded loops or
 * stack growth on hostile method bodies), fully bounds-checked reads,
 * and — unlike the old code — executing an If/Else/While body runs the
 * terms through exec_term instead of parse_termlist(NULL), so method
 * bodies can no longer allocate namespace nodes as a side effect. */
typedef struct {
    const uint8_t *data;     /* method body data */
    int            offset;   /* current offset in method body */
    int            end;      /* end offset */
    aml_node_t    *result;   /* evaluation result (if any) */
    int            steps;    /* terms executed (budget) */
    int            depth;    /* nested If/While depth */
} aml_exec_t;

/* Forward */
static int exec_term(aml_exec_t *ctx);
static void exec_range(aml_exec_t *ctx, int range_end);

/* Evaluate a DataRefObject in execution context.  Returns the next
 * offset, or -1 when the value is truncated/corrupt.  Heap objects
 * grabbed by a failed evaluation are released by the caller. */
static int exec_data_ref(aml_exec_t *ctx, aml_node_t *node) {
    if (!ctx || !node) return -1;
    if (!aml_can_read(ctx->offset, ctx->end, 1)) return -1;
    aml_node_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    /* parse_value never returns a value outside [offset, end]; treat
     * "no progress on non-value" as -1 so callers can fall back. */
    int next = parse_value(ctx->data, ctx->offset, ctx->end, &tmp);
    if (next < 0 || (next == ctx->offset && tmp.type == AML_NODE_NONE)) {
        if (tmp.type == AML_NODE_STRING && tmp.string) kfree(tmp.string);
        else if (tmp.type == AML_NODE_BUFFER && tmp.buffer.data) kfree(tmp.buffer.data);
        else if (tmp.type == AML_NODE_PACKAGE && tmp.package.elements)
            kfree(tmp.package.elements);
        return -1;
    }
    /* Move the parsed value into the caller's node without leaking. */
    *node = tmp;
    return next;
}

/* Free a transient exec value (strings/buffers/packages grabbed by
 * exec_data_ref into stack nodes).  Pool nodes themselves are never
 * freed (bump allocator). */
static void exec_value_free(aml_node_t *v) {
    if (!v) return;
    if (v->type == AML_NODE_STRING && v->string) {
        kfree(v->string);
        v->string = NULL;
    } else if (v->type == AML_NODE_BUFFER && v->buffer.data) {
        kfree(v->buffer.data);
        v->buffer.data = NULL;
    } else if (v->type == AML_NODE_PACKAGE && v->package.elements) {
        kfree(v->package.elements);
        v->package.elements = NULL;
    }
    v->type = AML_NODE_NONE;
}

/* Evaluate a predicate object: constant integers directly, anything
 * else via exec_data_ref.  Unparseable predicates are FALSE (skip the
 * body) — the safe direction for unknown firmware. */
static uint64_t exec_predicate(aml_exec_t *ctx) {
    if (!aml_can_read(ctx->offset, ctx->end, 1)) return 0;
    uint8_t pop = ctx->data[ctx->offset];
    if (pop == AML_ZERO_OP) { ctx->offset++; return 0; }
    if (pop == AML_ONE_OP || pop == AML_ONES_OP) { ctx->offset++; return 1; }
    aml_node_t pred;
    memset(&pred, 0, sizeof(pred));
    int next = exec_data_ref(ctx, &pred);
    if (next < 0) return 0;
    ctx->offset = next;
    uint64_t v = 0;
    if (pred.type == AML_NODE_INTEGER)
        v = pred.integer;
    else
        v = 1;  /* non-integer truthy object */
    exec_value_free(&pred);
    return v;
}

/* Execute terms in [offset, range_end).  Stops at the first Return
 * (ctx->result set), on error, or when the step budget is spent. */
static void exec_range(aml_exec_t *ctx, int range_end) {
    if (!ctx) return;
    range_end = aml_clamp(range_end, ctx->offset < 0 ? 0 : ctx->offset, ctx->end);
    while (ctx->offset < range_end && !ctx->result) {
        int r = exec_term(ctx);
        if (r <= 0) {
            ctx->offset = range_end;
            break;
        }
        if (ctx->offset > range_end)
            ctx->offset = range_end;
    }
}

/* Execute a single term in method body. Returns 1 if executed, 0 at end, <0 on error. */
static int exec_term(aml_exec_t *ctx) {
    if (!ctx || !ctx->data) return -1;
    if (ctx->offset < 0 || ctx->offset >= ctx->end) return 0;
    if (++ctx->steps > AML_MAX_EXEC_STEPS) {
        log_print(LOG_LEVEL_WARN, "aml: method exec budget exhausted\n");
        return -1;
    }

    int start = ctx->offset;
    uint8_t op = ctx->data[ctx->offset++];

    switch (op) {
    case AML_RETURN_OP: {
        /* Parse and evaluate the return value */
        aml_node_t *res = aml_alloc_node();
        if (!res) return -1;
        int next = exec_data_ref(ctx, res);
        if (next < 0) return -1;
        ctx->offset = aml_clamp(next, start + 1, ctx->end);
        ctx->result = res;
        return 1;
    }

    case AML_STORE_OP: {
        /* Store(Source, Target) — evaluate source, store to target.
         * For simplicity, just evaluate and discard result. */
        aml_node_t src;
        memset(&src, 0, sizeof(src));
        int next = exec_data_ref(ctx, &src);
        if (next < 0) return -1;
        ctx->offset = aml_clamp(next, start + 1, ctx->end);
        exec_value_free(&src);
        /* Skip target operand: an extended-op target, a NameString,
         * or a single fallback byte — all bounds-checked. */
        if (ctx->offset < ctx->end) {
            uint8_t top = ctx->data[ctx->offset];
            if (top == AML_EXT_OP) {
                /* Extended opcode target - skip prefix + ext byte */
                ctx->offset = aml_clamp(ctx->offset + 2, 0, ctx->end);
            } else if (top == AML_ROOT_PREFIX || top == AML_PARENT_PREFIX ||
                       top == AML_DUAL_NAME_PREFIX ||
                       top == AML_MULTI_NAME_PREFIX ||
                       top == '_' || (top >= 'A' && top <= 'Z') ||
                       top == 0x00) {
                char dummy[4];
                int off = ctx->offset;
                if (parse_name_string_checked(ctx->data, &off, ctx->end,
                                              dummy) == 0)
                    ctx->offset = off;
                else
                    ctx->offset = aml_clamp(ctx->offset + 1, 0, ctx->end);
            } else {
                /* Unknown target format - skip 1 byte */
                ctx->offset++;
            }
        }
        return 1;
    }

    case AML_IF_OP: {
        int pkg_off = ctx->offset;
        int pkg_len, hdr;
        if (read_pkg_length_checked(ctx->data, &ctx->offset, ctx->end,
                                    &pkg_len, &hdr) < 0) {
            log_print(LOG_LEVEL_WARN, "aml: truncated If PkgLength\n");
            return -1;
        }
        int if_end;
        if (aml_pkg_end(pkg_off, pkg_len, hdr, ctx->end, &if_end) < 0) {
            log_print(LOG_LEVEL_WARN, "aml: corrupt If PkgLength\n");
            return -1;
        }
        if (ctx->depth >= AML_MAX_EXEC_DEPTH) {
            log_print(LOG_LEVEL_WARN, "aml: If nesting too deep, skipped\n");
            ctx->offset = if_end;
            return 1;
        }
        uint64_t pred_val = exec_predicate(ctx);
        if (ctx->offset > if_end) {
            log_print(LOG_LEVEL_WARN, "aml: If predicate overruns body\n");
            ctx->offset = if_end;
            return 1;
        }
        ctx->depth++;
        if (pred_val) {
            /* Execute the If body */
            exec_range(ctx, if_end);
        } else {
            /* Skip the If body */
            ctx->offset = if_end;
        }
        ctx->depth--;
        if (ctx->result) return 1;
        /* Check for optional Else */
        if (aml_can_read(ctx->offset, ctx->end, 1) &&
            ctx->data[ctx->offset] == AML_ELSE_OP) {
            int else_start = ctx->offset;
            ctx->offset++;  /* skip ElseOp */
            int else_len, else_hdr;
            if (read_pkg_length_checked(ctx->data, &ctx->offset, ctx->end,
                                        &else_len, &else_hdr) < 0) {
                log_print(LOG_LEVEL_WARN, "aml: truncated Else PkgLength\n");
                return -1;
            }
            int else_end;
            if (aml_pkg_end(else_start + 1, else_len, else_hdr,
                            ctx->end, &else_end) < 0) {
                log_print(LOG_LEVEL_WARN, "aml: corrupt Else PkgLength\n");
                return -1;
            }
            if (ctx->depth >= AML_MAX_EXEC_DEPTH) {
                log_print(LOG_LEVEL_WARN, "aml: Else nesting too deep, skipped\n");
                ctx->offset = else_end;
                return 1;
            }
            ctx->depth++;
            if (!pred_val) {
                exec_range(ctx, else_end);
            } else {
                ctx->offset = else_end;
            }
            ctx->depth--;
        }
        return 1;
    }

    case AML_WHILE_OP: {
        int pkg_off = ctx->offset;
        int pkg_len, hdr;
        if (read_pkg_length_checked(ctx->data, &ctx->offset, ctx->end,
                                    &pkg_len, &hdr) < 0) {
            log_print(LOG_LEVEL_WARN, "aml: truncated While PkgLength\n");
            return -1;
        }
        int while_end;
        if (aml_pkg_end(pkg_off, pkg_len, hdr, ctx->end, &while_end) < 0) {
            log_print(LOG_LEVEL_WARN, "aml: corrupt While PkgLength\n");
            return -1;
        }
        /* Evaluate predicate once and run the body at most once: a real
         * WhileOp repeats, but an unbounded loop on firmware-controlled
         * input is a boot hang.  One pass is enough to find Return()
         * values in _S5-style methods. */
        uint64_t pred_val = exec_predicate(ctx);
        if (ctx->offset > while_end) {
            log_print(LOG_LEVEL_WARN, "aml: While predicate overruns body\n");
            ctx->offset = while_end;
            return 1;
        }
        if (pred_val) {
            if (ctx->depth >= AML_MAX_EXEC_DEPTH) {
                log_print(LOG_LEVEL_WARN, "aml: While nesting too deep, skipped\n");
                ctx->offset = while_end;
                return 1;
            }
            ctx->depth++;
            exec_range(ctx, while_end);
            ctx->depth--;
            if (!ctx->result)
                ctx->offset = while_end;
        } else {
            ctx->offset = while_end;
        }
        return 1;
    }

    case AML_NOOP_OP:
        return 1;

    case AML_BREAK_OP:
        /* Treat as end of execution */
        ctx->offset = ctx->end;
        return 1;

    /* Integer constants as terms (no-op) */
    case AML_ZERO_OP:
    case AML_ONE_OP:
    case AML_ONES_OP:
        return 1;

    /* Integer prefix terms (bypass, truncated-safe) */
    case AML_BYTE_PREFIX:
        if (!aml_can_read(ctx->offset, ctx->end, 1)) { ctx->offset = ctx->end; return 1; }
        ctx->offset++; return 1;
    case AML_WORD_PREFIX:
        if (!aml_can_read(ctx->offset, ctx->end, 2)) { ctx->offset = ctx->end; return 1; }
        ctx->offset += 2; return 1;
    case AML_DWORD_PREFIX:
        if (!aml_can_read(ctx->offset, ctx->end, 4)) { ctx->offset = ctx->end; return 1; }
        ctx->offset += 4; return 1;
    case AML_QWORD_PREFIX:
        if (!aml_can_read(ctx->offset, ctx->end, 8)) { ctx->offset = ctx->end; return 1; }
        ctx->offset += 8; return 1;

    /* NameSeg reference (method call) — skip the full 4-byte seg */
    default:
        if (op == '_' || (op >= 'A' && op <= 'Z')) {
            char seg[4];
            if (read_name_seg_checked(ctx->data, start, ctx->end, seg) < 0)
                ctx->offset = aml_clamp(start + 1, 0, ctx->end);
            else
                ctx->offset = aml_clamp(start + 4, 0, ctx->end);
            return 1;
        }
        if (op == AML_EXT_OP) {
            /* Extended opcode inside a method body: skip prefix + ext
             * byte (operands, if any, are handled as following terms). */
            if (!aml_can_read(ctx->offset, ctx->end, 1))
                return 0;
            ctx->offset++;
            return 1;
        }
        break;
    }

    return 1;
}

/* Execute a method by running through its body until ReturnOp or end. */
static aml_node_t *aml_exec_method_body(const uint8_t *body, uint32_t body_len) {
    if (!body || body_len == 0 || body_len > AML_MAX_METHOD_BODY)
        return NULL;
    aml_exec_t ctx;
    ctx.data   = body;
    ctx.offset = 0;
    ctx.end    = (int)body_len;
    ctx.result = NULL;
    ctx.steps  = 0;
    ctx.depth  = 0;

    /* Run through terms until we hit Return or end */
    while (ctx.offset < ctx.end && !ctx.result) {
        int ret = exec_term(&ctx);
        if (ret < 0) break;
        if (ret == 0) break;
    }

    return ctx.result;
}


aml_node_t *aml_namespace_root = NULL;

/* Raw AML data copy (kept after parse for raw-scan fallbacks) */
static const uint8_t *aml_data_copy = NULL;
static uint32_t        aml_data_len  = 0;

/* Some ACPI tables embed _S5 inside large PackageOp bodies or
 * behind a broken PkgLength chain that the namespace builder cannot
 * recover.  This fallback does a direct byte scan for the _S5 name
 * and parses the following AML value without the full namespace. */

#define AML_SEG_S5  0x5F53355F   /* little-endian bytes 5F 53 35 5F */

static int read_integer_raw(const uint8_t *data, int *offset, int end,
                              uint64_t *val);

/* Raw AML scan for _S5: finds Name(_S5_, Package(){...}) and extracts
 * the leading constant integers.  Fully bounds-checked; returns the
 * number of integers extracted (>=2 on success) or -1.  Only constant
 * elements are accepted — anything else means "not parseable", never
 * garbage values for the shutdown path. */
int aml_raw_scan_s5(uint64_t *out_vals, int max_vals) {
    if (!out_vals || max_vals <= 0)
        return -1;
    if (!aml_data_copy || aml_data_len < 12)
        return -1;

    const uint8_t *data = aml_data_copy;
    /* aml_data_len is capped at ACPI_MAX_DSDT_LEN by aml_init, safely
     * inside int range. */
    int end = (aml_data_len > ACPI_MAX_DSDT_LEN) ?
              (int)ACPI_MAX_DSDT_LEN : (int)aml_data_len;

    /* Scan for NameOp + NameSeg("_S5_") = 08 5F 53 35 5F */
    for (int i = 0; i + 8 <= end; i++) {
        if (data[i] != AML_NAME_OP)                continue;
        if (!aml_can_read(i + 1, end, 4))          break;
        if (data[i+1] != '_' || data[i+2] != 'S')  continue;
        if (data[i+3] != '5' || data[i+4] != '_')  continue;

        /* Found Name(_S5_, ...) — value must be a PackageOp.  A Method
         * _S5_ is handled by the namespace path; keep scanning. */
        int off = i + 5;  /* past opcode + name */
        if (!aml_can_read(off, end, 1)) continue;
        uint8_t op = data[off];
        if (op != AML_PACKAGE_OP && op != AML_VAR_PACKAGE_OP)
            continue;

        off++;  /* skip PackageOp */
        int pkg_len, hdr;
        if (read_pkg_length_checked(data, &off, end, &pkg_len, &hdr) < 0)
            continue;
        int pkg_end;
        if (aml_pkg_end(off - hdr, pkg_len, hdr, end, &pkg_end) < 0)
            continue;
        if (!aml_can_read(off, pkg_end, 1)) continue;

        /* Per the ACPI spec NumElements is a single ByteData. */
        int num_elems = data[off++];
        if (num_elems < 2 || num_elems > AML_MAX_PACKAGE_ELEMS)
            continue;

        int parsed = 0;
        uint64_t tmp[AML_MAX_PACKAGE_ELEMS];
        for (int j = 0; j < num_elems && off < pkg_end; j++) {
            uint64_t val = 0;
            int noff = off;
            if (read_integer_raw(data, &noff, pkg_end, &val) == 0) {
                off = noff;
                if (parsed < AML_MAX_PACKAGE_ELEMS)
                    tmp[parsed] = val;
                parsed++;
            } else {
                break;
            }
        }

        if (parsed >= 2) {
            int n = parsed < max_vals ? parsed : max_vals;
            for (int j = 0; j < n; j++) out_vals[j] = tmp[j];
            log_printf(LOG_LEVEL_DEBUG, "aml: _S5 found via raw scan, %d elements\n",
                       parsed);
            return parsed;
        }
        /* Partial/truncated package — keep scanning for another _S5_. */
    }

    return -1;
}

/* Constant-integer reader for the raw AML scan.  Bounds-checked
 * against end (usually the Package end, not the whole table). */
static int read_integer_raw(const uint8_t *data, int *offset, int end,
                            uint64_t *val) {
    int off = *offset;
    if (!aml_can_read(off, end, 1)) return -1;
    uint8_t op = data[off++];

    switch (op) {
    case AML_ZERO_OP:       *val = 0; *offset = off; return 0;
    case AML_ONE_OP:        *val = 1; *offset = off; return 0;
    case AML_ONES_OP:       *val = ~0ULL; *offset = off; return 0;
    case AML_BYTE_PREFIX:
        if (!aml_can_read(off, end, 1)) return -1;
        *val = data[off]; *offset = off + 1; return 0;
    case AML_WORD_PREFIX:
        if (!aml_can_read(off, end, 2)) return -1;
        *val = data[off] | ((uint32_t)data[off+1] << 8); *offset = off + 2; return 0;
    case AML_DWORD_PREFIX:
        if (!aml_can_read(off, end, 4)) return -1;
        *val = (uint32_t)data[off] | ((uint32_t)data[off+1] << 8) |
               ((uint32_t)data[off+2] << 16) | ((uint32_t)data[off+3] << 24);
        *offset = off + 4; return 0;
    case AML_QWORD_PREFIX:
        if (!aml_can_read(off, end, 8)) return -1;
        *val = (uint64_t)data[off] | ((uint64_t)data[off+1] << 8) |
               ((uint64_t)data[off+2] << 16) | ((uint64_t)data[off+3] << 24) |
               ((uint64_t)data[off+4] << 32) | ((uint64_t)data[off+5] << 40) |
               ((uint64_t)data[off+6] << 48) | ((uint64_t)data[off+7] << 56);
        *offset = off + 8; return 0;
    default:
        return -1;
    }
}


/* Copy len bytes from physical memory to a heap buffer, one page at
 * a time.  The old code mapped only the first page and memcpy'd the
 * whole table through it — an OOB read (or #PF) for any DSDT above the
 * identity region.  Returns 0 on success. */
static int aml_copy_phys(uint64_t phys, uint8_t *dest, uint32_t len) {
    if (!dest) return -1;
    while (len > 0) {
        /* Wrap-safe: firmware phys near 2^64 must not wrap to low mem. */
        if (phys + 0x1000ULL < phys)
            return -1;
        uint64_t page_off = phys & 0xFFFULL;
        uint32_t chunk = 0x1000U - (uint32_t)page_off;
        if (chunk > len)
            chunk = len;
        if (phys + chunk < phys)
            return -1;
        if (phys + chunk <= 0x4000000ULL) {
            /* Fully inside the identity window: direct access. */
            memcpy(dest, (const void *)(uintptr_t)(KERNEL_BASE + phys), chunk);
        } else {
            /* Single-slot temp map: exactly one map/unmap per chunk,
             * never nested. */
            void *base = vmm_temp_map(phys & ~0xFFFULL);
            if (!base)
                return -1;
            memcpy(dest, (const uint8_t *)base + page_off, chunk);
            vmm_temp_unmap();
        }
        phys += chunk;
        dest += chunk;
        len  -= chunk;
    }
    return 0;
}


int aml_init(void) {
    node_pool_used = 0;
    parse_steps = 0;
    aml_namespace_root = NULL;

    /* Drop any previous copy (aml_init is single-shot in practice,
     * but a second call must not leak or leave stale globals). */
    if (aml_data_copy) {
        kfree((void *)aml_data_copy);
        aml_data_copy = NULL;
        aml_data_len  = 0;
    }

    uint64_t dsdt_phys = acpi_info.x_dsdt ? acpi_info.x_dsdt : (uint64_t)acpi_info.dsdt_addr;
    uint32_t dsdt_len  = acpi_info.dsdt_length;

    if (!dsdt_phys || dsdt_len <= sizeof(sdt_header_t) ||
        dsdt_len > ACPI_MAX_DSDT_LEN) {
        log_print(LOG_LEVEL_ERROR, "aml: no (sane) DSDT available\n");
        return -1;
    }
    if (dsdt_phys + dsdt_len < dsdt_phys) {
        log_print(LOG_LEVEL_ERROR, "aml: DSDT address range wraps\n");
        return -1;
    }

    log_printf(LOG_LEVEL_DEBUG, "aml: DSDT at 0x%lx length=%u\n", (unsigned long)dsdt_phys, dsdt_len);

    /* Copy the DSDT AML bytecode into heap memory.
     * The AML data starts after the SDT header. */
    uint32_t aml_len = dsdt_len - (uint32_t)sizeof(sdt_header_t);
    if (aml_len == 0 || aml_len > ACPI_MAX_DSDT_LEN) {
        log_print(LOG_LEVEL_ERROR, "aml: insane DSDT AML length\n");
        return -1;
    }
    uint64_t aml_data_phys = dsdt_phys + sizeof(sdt_header_t);
    if (aml_data_phys < dsdt_phys) {
        log_print(LOG_LEVEL_ERROR, "aml: DSDT data address wraps\n");
        return -1;
    }

    /* Re-validate the header straight from physical memory: acpi_info
     * is filled by probe_dsdt, but a second pair of eyes on signature
     * and length costs nothing and catches stale state. */
    {
        sdt_header_t hdr;
        if (aml_copy_phys(dsdt_phys, (uint8_t *)&hdr, sizeof(hdr)) < 0) {
            log_print(LOG_LEVEL_ERROR, "aml: cannot read DSDT header\n");
            return -1;
        }
        if (memcmp(hdr.signature, DSDT_SIGNATURE, 4) != 0) {
            log_print(LOG_LEVEL_ERROR, "aml: DSDT signature mismatch\n");
            return -1;
        }
        if (hdr.length != dsdt_len) {
            log_print(LOG_LEVEL_ERROR, "aml: DSDT length mismatch\n");
            return -1;
        }
    }

    /* Allocate and copy page-by-page (see aml_copy_phys). */
    uint8_t *aml_data = (uint8_t *)kmalloc(aml_len);
    if (!aml_data) {
        log_print(LOG_LEVEL_ERROR, "aml: out of memory for DSDT copy\n");
        return -1;
    }
    if (aml_copy_phys(aml_data_phys, aml_data, aml_len) < 0) {
        log_print(LOG_LEVEL_ERROR, "aml: cannot access DSDT data\n");
        kfree(aml_data);
        return -1;
    }

    /* Create root namespace node */
    aml_namespace_root = aml_alloc_node();
    if (!aml_namespace_root) {
        kfree(aml_data);
        return -1;
    }
    aml_namespace_root->seg[0] = '\\';
    aml_namespace_root->seg[1] = 0;
    aml_namespace_root->seg[2] = 0;
    aml_namespace_root->seg[3] = 0;
    aml_namespace_root->type = AML_NODE_SCOPE;

    /* Parse the AML */
    int offset = 0;
    offset = parse_termlist(aml_data, offset, (int)aml_len, aml_namespace_root, 0);

    log_printf(LOG_LEVEL_DEBUG, "aml: parsed %d/%u bytes, %d nodes\n", offset, aml_len, node_pool_used);
    if (parse_steps >= AML_MAX_PARSE_STEPS)
        log_print(LOG_LEVEL_WARN, "aml: DSDT truncated by parse budget\n");
    else if (offset < (int)aml_len)
        log_printf(LOG_LEVEL_WARN, "aml: DSDT trailing garbage (%u bytes skipped)\n",
                   aml_len - (uint32_t)offset);

    /* DSDT copy is kept (we may need it for method execution) */
    aml_data_copy = aml_data;
    aml_data_len  = aml_len;
    return 0;
}


aml_node_t *aml_find(const char *path) {
    if (!aml_namespace_root || !path || !*path) return NULL;

    /* Bound the scan: paths are kernel constants, but a corrupted
     * pointer must not spin us. */
    int plen = 0;
    while (plen <= AML_MAX_PATH_LEN && path[plen]) plen++;
    if (plen == 0 || plen > AML_MAX_PATH_LEN) return NULL;

    /* Start from root if path starts with '\', or root */
    aml_node_t *node = aml_namespace_root;

    /* Skip leading backslash */
    const char *p = path;
    if (*p == '\\') p++;

    /* The input path format is "." or "/" separated 4-char names. */
    char name_buf[5];
    int nsegs = 0;
    while (*p) {
        if (++nsegs > AML_MAX_PATH_SEGS) return NULL;
        /* Extract a name segment: exactly 1-4 chars up to separator.
         * Longer runs are malformed (NameSeg is 4 chars) — reject
         * instead of mis-splitting them into phantom segments. */
        int len = 0;
        while (*p && *p != '.' && *p != '/') {
            if (len >= 4) return NULL;
            name_buf[len++] = *(p++);
        }
        if (len == 0) return NULL;  /* empty segment ("..", trailing ".", ...) */
        while (len < 4) name_buf[len++] = '_';  /* pad with '_' */
        name_buf[4] = '\0';

        /* Find the child */
        if (!node || !node->child) return NULL;  /* no children — not found */

        node = find_child(node, name_buf);
        if (!node) return NULL;

        /* Skip separator */
        if (*p == '.' || *p == '/') p++;
    }

    return node;
}


uint64_t aml_eval_integer(const char *path) {
    if (!path) return 0;
    aml_node_t *node = aml_find(path);
    if (!node) return 0;

    switch (node->type) {
    case AML_NODE_INTEGER:
        return node->integer;

    case AML_NODE_METHOD: {
        /* Execute the method and get the integer result */
        if (!node->method.body || node->method.body_len == 0 ||
            node->method.body_len > AML_MAX_METHOD_BODY)
            return 0;
        aml_node_t *result = aml_exec_method_body(node->method.body,
                                                  node->method.body_len);
        if (result && result->type == AML_NODE_INTEGER) {
            uint64_t val = result->integer;
            return val;
        }
        return 0;
    }

    default:
        return 0;
    }
}


int aml_get_package_int(const char *path, int index, uint64_t *val) {
    if (!val || !path || index < 0) return -1;

    aml_node_t *node = aml_find(path);
    if (!node) return -1;

    aml_node_t *pkg = NULL;

    if (node->type == AML_NODE_PACKAGE) {
        pkg = node;
    } else if (node->type == AML_NODE_METHOD) {
        if (!node->method.body || node->method.body_len == 0 ||
            node->method.body_len > AML_MAX_METHOD_BODY)
            return -1;
        aml_node_t *result = aml_exec_method_body(node->method.body,
                                                  node->method.body_len);
        if (result && result->type == AML_NODE_PACKAGE)
            pkg = result;
    }

    if (!pkg) return -1;
    if (pkg->package.count == 0 ||
        pkg->package.count > AML_MAX_PACKAGE_ELEMS)
        return -1;
    if (!pkg->package.elements) return -1;
    if ((uint32_t)index >= pkg->package.count)
        return -1;

    aml_node_t *elem = pkg->package.elements[index];
    if (!elem || elem->type != AML_NODE_INTEGER)
        return -1;

    *val = elem->integer;
    return 0;
}


static void dump_node(aml_node_t *node, int depth) {
    /* Debug-only dump, but a hostile 500-deep Scope chain must not
     * smash the kernel stack. */
    if (!node || depth > AML_MAX_DEPTH) return;
    for (int i = 0; i < depth; i++) log_print(LOG_LEVEL_DEBUG, "  ");

    /* Print name */
    debug_putchar(node->seg[0]);
    debug_putchar(node->seg[1]);
    debug_putchar(node->seg[2]);
    debug_putchar(node->seg[3]);

    /* Print type info */
    switch (node->type) {
    case AML_NODE_INTEGER:
        log_printf(LOG_LEVEL_DEBUG, " = 0x%lx", (unsigned long)node->integer);
        break;
    case AML_NODE_STRING:
        if (node->string)
            log_printf(LOG_LEVEL_DEBUG, " = \"%s\"", node->string);
        break;
    case AML_NODE_BUFFER:
        log_printf(LOG_LEVEL_DEBUG, " = [%u bytes]", node->buffer.len);
        break;
    case AML_NODE_PACKAGE:
        log_printf(LOG_LEVEL_DEBUG, " = Package(%u)", node->package.count);
        break;
    case AML_NODE_METHOD:
        log_printf(LOG_LEVEL_DEBUG, " = Method(%u args, %u bytes)",
                     node->method.nargs, node->method.body_len);
        break;
    case AML_NODE_SCOPE:    break;  /* just scope */
    case AML_NODE_DEVICE:   log_printf(LOG_LEVEL_DEBUG, " [Device]"); break;
    case AML_NODE_PROCESSOR: log_printf(LOG_LEVEL_DEBUG, " [Processor]"); break;
    case AML_NODE_POWERRES: log_printf(LOG_LEVEL_DEBUG, " [PowerRes]"); break;
    case AML_NODE_THERMAL:  log_printf(LOG_LEVEL_DEBUG, " [Thermal]"); break;
    case AML_NODE_OPREGION: log_printf(LOG_LEVEL_DEBUG, " [OpRegion]"); break;
    case AML_NODE_FIELD:    log_printf(LOG_LEVEL_DEBUG, " [Field]"); break;
    default: break;
    }
    log_print(LOG_LEVEL_DEBUG, "\n");

    /* Recurse into children */
    for (aml_node_t *c = node->child; c; c = c->next)
        dump_node(c, depth + 1);
}

void aml_dump(void) {
    if (!aml_namespace_root) {
        log_print(LOG_LEVEL_DEBUG, "aml: namespace empty\n");
        return;
    }
    log_print(LOG_LEVEL_DEBUG, "aml: namespace tree:\n");
    for (aml_node_t *c = aml_namespace_root->child; c; c = c->next)
        dump_node(c, 1);
}
