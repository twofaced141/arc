#include "aml.h"
#include "debug.h"
#include "vmm.h"
#include "string.h"
#include "acpi.h"
#include "memory.h"


#define AML_MAX_NODES   512       /* max namespace nodes + temp results */
#define AML_MAX_SCOPE   64        /* max scope nesting during parse */


static aml_node_t node_pool[AML_MAX_NODES];
static int         node_pool_used;

static aml_node_t *aml_alloc_node(void) {
    if (node_pool_used >= AML_MAX_NODES) return NULL;
    aml_node_t *n = &node_pool[node_pool_used++];
    memset(n, 0, sizeof(aml_node_t));
    return n;
}


/* Copy a 4-byte NameSeg, return pointer past it. */
static const uint8_t *read_name_seg(const uint8_t *p, char *out) {
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
    return p + 4;
}


/* AML PkgLength encoding:
 *   bits[7:6] of first byte = number of extra bytes (0-3)
 *   bits[5:0] of first byte = lower bits of the length
 *   Extra bytes are little-endian continuation bytes.
 * Returns the decoded length and advances *offset past the encoding. */
static int read_pkg_length(const uint8_t *data, int *offset) {
    int off = *offset;
    uint8_t byte0 = data[off++];
    int extra = (byte0 >> 6) & 3;
    int len = byte0 & 0x3F;
    for (int i = 0; i < extra; i++)
        len |= (int)data[off++] << (6 + i * 8);
    *offset = off;
    return len;
}


/* Read a constant integer from data at *offset.
 * Handles: ZeroOp, OneOp, OnesOp, BytePrefix, WordPrefix, DWordPrefix, QWordPrefix.
 * Returns the value and advances offset. */
static uint64_t read_integer(const uint8_t *data, int *offset) {
    int off = *offset;
    uint8_t op = data[off++];

    switch (op) {
    case AML_ZERO_OP:       *offset = off; return 0;
    case AML_ONE_OP:        *offset = off; return 1;
    case AML_ONES_OP:       *offset = off; return ~0ULL;
    case AML_BYTE_PREFIX:   *offset = off + 1; return data[off];
    case AML_WORD_PREFIX: {
        uint16_t v = (uint16_t)data[off] | ((uint16_t)data[off+1] << 8);
        *offset = off + 2; return v;
    }
    case AML_DWORD_PREFIX: {
        uint32_t v = (uint32_t)data[off] | ((uint32_t)data[off+1] << 8)
                    | ((uint32_t)data[off+2] << 16) | ((uint32_t)data[off+3] << 24);
        *offset = off + 4; return v;
    }
    case AML_QWORD_PREFIX: {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++)
            v |= (uint64_t)data[off + i] << (i * 8);
        *offset = off + 8; return v;
    }
    default:
        /* silent: unexpected integer op */
        *offset = off;
        return 0;
    }
}


/* Parse an AML NameString at *offset.
 * Handles: RootPrefix, ParentPrefix, NullName, NameSeg, DualNamePrefix, MultiNamePrefix.
 * For simplicity we only store the LAST NameSeg for the name being defined.
 * Returns the final NameSeg in out[4] and advances offset past the NameString.
 * When the name is just the root scope (RootPrefix + NullName), out is filled
 * with all zeros and the caller treats it as "current scope". */
static void parse_name_string(const uint8_t *data, int *offset, char *out) {
    int off = *offset;
    /* Skip leading root/parent prefixes. */
    while (data[off] == AML_ROOT_PREFIX || data[off] == AML_PARENT_PREFIX) {
        off++;
    }

    if (data[off] == AML_DUAL_NAME_PREFIX) {
        off++;  /* skip 0x2E */
        off = (int)(read_name_seg(data + off, out) - data);
    } else if (data[off] == AML_MULTI_NAME_PREFIX) {
        off++;  /* skip 0x2F */
        int count = data[off++];
        /* Advance through all segments but only keep the last one */
        for (int i = 0; i < count; i++) {
            if (i == count - 1)
                off = (int)(read_name_seg(data + off, out) - data);
            else
                off += 4;
        }
    } else if (data[off] == 0x00) {
        /* NullName — no name segment follows.  If RootPrefix was seen,
         * the name IS the root scope.  Signal this by zeroing out. */
        off++;
        out[0] = out[1] = out[2] = out[3] = 0;
    } else {
        /* Simple NameSeg */
        off = (int)(read_name_seg(data + off, out) - data);
    }
    *offset = off;
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


static int parse_termlist(const uint8_t *data, int offset, int end, aml_node_t *scope);
static int parse_value(const uint8_t *data, int offset, int end, aml_node_t *node);


/* Try to determine the size of a term starting at offset.
 * Returns the number of bytes the term occupies, or -1 if unknown. */
static int term_size(const uint8_t *data, int offset, int end) {
    if (offset >= end) return 0;
    int start = offset;
    uint8_t op = data[offset++];

    switch (op) {
    /* Single-byte constant terms */
    case AML_ZERO_OP:
    case AML_ONE_OP:
    case AML_ONES_OP:
        return 1;

    /* Integer prefix + data */
    case AML_BYTE_PREFIX:   return 2;
    case AML_WORD_PREFIX:   return 3;
    case AML_DWORD_PREFIX:  return 5;
    case AML_QWORD_PREFIX:  return 9;

    /* String prefix */
    case AML_STRING_PREFIX: {
        /* Null-terminated string */
        while (offset < end && data[offset]) offset++;
        return offset + 1 - start;  /* include null terminator */
    }

    /* Simple name reference (4-byte NameSeg) */
    default:
        /* If the opcode is an ASCII letter or underscore, it's a NameSeg (method call) */
        if ((op >= 'A' && op <= 'Z') || op == '_')
            return 5;  /* 1 for the first byte already consumed, +4 more */
        /* Other single-byte opcodes */
        /* Method-body opcodes: Return, Break, Noop, Continue, etc. */
        if (op == AML_RETURN_OP ||
            op == AML_BREAK_OP ||
            op == AML_NOOP_OP)
            return 1;  /* opcode only, return value handled separately */

        /* IfOp, ElseOp, WhileOp have PkgLength after opcode */
        if (op == AML_IF_OP || op == AML_ELSE_OP || op == AML_WHILE_OP) {
            /* Re-read PkgLength from after opcode */
            /* But we need to skip the rest including predicate and body */
            int tmp = start + 1;
            int len = read_pkg_length(data, &tmp);
            return len;
        }

        /* Namespace opcodes with PkgLength */
        if (op == AML_SCOPE_OP || op == AML_BUFFER_OP ||
            op == AML_PACKAGE_OP || op == AML_VAR_PACKAGE_OP ||
            op == AML_METHOD_OP) {
            int tmp = start + 1;
            int len = read_pkg_length(data, &tmp);
            return len;
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
            if (offset >= end) return -1;
            uint8_t ext = data[offset];
            /* Namespace-building extended opcodes with a TermList use PkgLength.
             * OpRegionOp (0x80) has no PkgLength — fixed/self-terminating args.
             * All others (method-body opcodes) do not use PkgLength. */
            if ((ext >= 0x81 && ext <= 0x88) || ext == 0x0C || ext == 0x0D) {
                int tmp = offset + 1;
                int len = read_pkg_length(data, &tmp);
                return (tmp - offset - 1) + len;  /* ext byte + pkg_len bytes + pkg_len content */
            }
            /* Other extended opcodes: unknown size */
            return -1;
        }

        return -1;
    }
}


static int parse_value(const uint8_t *data, int offset, int end, aml_node_t *node) {
    if (offset < 0 || offset >= end) return offset;
    uint8_t op = data[offset];

    switch (op) {
    case AML_ZERO_OP:
    case AML_ONE_OP:
    case AML_ONES_OP:
    case AML_BYTE_PREFIX:
    case AML_WORD_PREFIX:
    case AML_DWORD_PREFIX:
    case AML_QWORD_PREFIX:
        node->type = AML_NODE_INTEGER;
        node->integer = read_integer(data, &offset);
        break;

    case AML_STRING_PREFIX: {
        offset++;
        int len = 0;
        while (offset + len < end && data[offset + len]) len++;
        node->type = AML_NODE_STRING;
        node->string = (char *)kmalloc(len + 1);
        if (node->string) {
            memcpy(node->string, data + offset, len);
            node->string[len] = '\0';
        }
        offset += len + 1;
        break;
    }

    case AML_BUFFER_OP: {
        int start = offset;
        offset++;  /* skip opcode */
        int pkg_len = read_pkg_length(data, &offset);
        int buf_end = start + pkg_len;
        if (buf_end > end) buf_end = end;  /* clamp to available data */
        int buf_len = (int)read_integer(data, &offset);
        node->type = AML_NODE_BUFFER;
        node->buffer.len = buf_len;
        node->buffer.data = NULL;
        if (buf_len > 0) {
            node->buffer.data = (uint8_t *)kmalloc(buf_len);
            if (node->buffer.data && offset + buf_len <= buf_end)
                memcpy(node->buffer.data, data + offset, buf_len);
        }
        offset = buf_end;
        break;
    }

    case AML_PACKAGE_OP:
    case AML_VAR_PACKAGE_OP: {
        int start = offset;
        offset++;  /* skip opcode */
        int pkg_len = read_pkg_length(data, &offset);
        int pkg_end = start + pkg_len;
        if (pkg_end > end) pkg_end = end;  /* clamp to available data */
        int num_elems = (int)read_integer(data, &offset);
        if (num_elems > 128 || num_elems < 0) {
            offset = pkg_end;
            break;
        }
        node->type = AML_NODE_PACKAGE;
        node->package.count = (uint32_t)num_elems;
        node->package.elements = NULL;
        if (num_elems > 0) {
            node->package.elements = (aml_node_t **)kmalloc(
                (uint32_t)num_elems * sizeof(aml_node_t *));
            if (node->package.elements) {
                memset(node->package.elements, 0,
                       (uint32_t)num_elems * sizeof(aml_node_t *));
                for (int i = 0; i < num_elems && offset < pkg_end; i++) {
                    aml_node_t *elem = aml_alloc_node();
                    if (!elem) break;
                    offset = parse_value(data, offset, pkg_end, elem);
                    node->package.elements[i] = elem;
                }
            }
        }
        offset = pkg_end;
        break;
    }

    default:
        break;
    }

    return offset;
}


static int parse_term(const uint8_t *data, int offset, int end, aml_node_t *scope) {
    (void)end;
    int start = offset;
    if (offset < 0) return offset;

    uint8_t op = data[offset++];

    switch (op) {
    /* ---------- Namespace building opcodes ---------- */
    case AML_SCOPE_OP: {
        int pkg_len = read_pkg_length(data, &offset);
        int end = start + 1 + pkg_len;  /* PkgLength counts from its own first byte */
        char seg[4];
        parse_name_string(data, &offset, seg);
        /* If the scope name is the root (NullName sentinel: all zeros),
         * do not create a new node — use the current scope as-is. */
        int is_root = (seg[0] == 0 && seg[1] == 0 && seg[2] == 0 && seg[3] == 0);
        if (is_root) {
            offset = parse_termlist(data, offset, end, scope);
        } else {
            aml_node_t *child = new_node(seg, AML_NODE_SCOPE, scope);
            if (child)
                offset = parse_termlist(data, offset, end, child);
            else
                offset = end;
        }
        break;
    }

    case AML_NAME_OP: {
        char seg[4];
        parse_name_string(data, &offset, seg);
        aml_node_t *node = new_node(seg, AML_NODE_NONE, scope);
        if (node)
            offset = parse_value(data, offset, end, node);
        break;
    }

    case AML_METHOD_OP: {
        int pkg_len = read_pkg_length(data, &offset);
        int end = start + 1 + pkg_len;  /* PkgLength counts from its own first byte */
        char seg[4];
        parse_name_string(data, &offset, seg);
        /* ByteData: bits 2-0 = arg_count, bit 3 = serialized flag */
        uint8_t info = data[offset++];
        uint8_t arg_count = info & 0x07;
        aml_node_t *node = new_node(seg, AML_NODE_METHOD, scope);
        if (node) {
            int body_len = end - offset;
            if (body_len > 0) {
                node->method.body = (uint8_t *)kmalloc((uint32_t)body_len);
                if (node->method.body) {
                    memcpy(node->method.body, data + offset, (uint32_t)body_len);
                    node->method.body_len = (uint32_t)body_len;
                }
            }
            node->method.nargs = arg_count;
        }
        offset = end;
        break;
    }

    case AML_ALIAS_OP: {
        /* Alias(SourceObject, AliasObject) — skip both names */
        char seg[4];
        parse_name_string(data, &offset, seg);  /* source name */
        parse_name_string(data, &offset, seg);  /* alias name */
        /* Create an alias node */
        new_node(seg, AML_NODE_NONE, scope);
        break;
    }

    /* ---------- Extended opcodes (0x5B + second byte) ---------- */
    case AML_EXT_OP: {
        uint8_t ext = data[offset++];
        int ext_start = offset;

        switch (ext) {
        /* ---------- Namespace-building: 0x82-0x88 (with PkgLength) ---------- */
        case AML_EXT_DEVICE: {
            int pkg_len = read_pkg_length(data, &offset);
            char seg[4];
            parse_name_string(data, &offset, seg);
            aml_node_t *child = new_node(seg, AML_NODE_DEVICE, scope);
            if (child)
                offset = parse_termlist(data, offset, ext_start + pkg_len, child);
            else
                offset = ext_start + pkg_len;
            break;
        }

        case AML_EXT_PROCESSOR: {
            int pkg_len = read_pkg_length(data, &offset);
            char seg[4];
            parse_name_string(data, &offset, seg);
            aml_node_t *child = new_node(seg, AML_NODE_PROCESSOR, scope);
            if (child) {
                offset += 6;  /* proc_id(1), pblk_addr(4), pblk_len(1) */
                offset = parse_termlist(data, offset, ext_start + pkg_len, child);
            } else {
                offset = ext_start + pkg_len;
            }
            break;
        }

        case AML_EXT_THERMAL: {
            int pkg_len = read_pkg_length(data, &offset);
            char seg[4];
            parse_name_string(data, &offset, seg);
            aml_node_t *child = new_node(seg, AML_NODE_THERMAL, scope);
            if (child)
                offset = parse_termlist(data, offset, ext_start + pkg_len, child);
            else
                offset = ext_start + pkg_len;
            break;
        }

        case AML_EXT_POWERRES: {
            int pkg_len = read_pkg_length(data, &offset);
            char seg[4];
            parse_name_string(data, &offset, seg);
            aml_node_t *child = new_node(seg, AML_NODE_POWERRES, scope);
            if (child) {
                offset += 3;  /* system_level(1), resource_order(2) */
                offset = parse_termlist(data, offset, ext_start + pkg_len, child);
            } else {
                offset = ext_start + pkg_len;
            }
            break;
        }

        case AML_EXT_OPREGION: {
            /* DefOpRegion := OpRegionOp NameString ByteData TermArg TermArg
             * NO PkgLength — fixed/self-terminating arguments only. */
            char seg[4];
            parse_name_string(data, &offset, seg);
            aml_node_t *node = new_node(seg, AML_NODE_OPREGION, scope);
            if (node) {
                offset++;  /* RegionSpace (1 byte: SystemMemory=0, SystemIO=1, ...) */
                (void)read_integer(data, &offset);  /* RegionOffset */
                (void)read_integer(data, &offset);  /* RegionLength */
            }
            break;
        }

        case AML_EXT_FIELD:
        case AML_EXT_INDEXFIELD:
        case AML_EXT_BANKFIELD: {
            int pkg_len = read_pkg_length(data, &offset);
            char seg[4];
            parse_name_string(data, &offset, seg);
            /* FieldOp has FieldFlags(1) + FieldList after NameString */
            /* We skip entirely using PkgLength */
            new_node(seg, AML_NODE_FIELD, scope);
            offset = ext_start + pkg_len;
            break;
        }

        case AML_EXT_DATAREGION: {
            int pkg_len = read_pkg_length(data, &offset);
            char seg[4];
            parse_name_string(data, &offset, seg);
            new_node(seg, AML_NODE_OPREGION, scope);
            offset = ext_start + pkg_len;
            break;
        }

        /* ---------- Namespace-building (ACPI 5.0+) with PkgLength ---------- */
        case AML_EXT_GPIOPIN:
        case AML_EXT_GENERICSERIAL: {
            int pkg_len = read_pkg_length(data, &offset);
            char seg[4];
            parse_name_string(data, &offset, seg);
            new_node(seg, AML_NODE_NONE, scope);
            offset = ext_start + pkg_len;
            break;
        }

        /* ---------- Namespace/method no-PkgLength opcodes ---------- */
        case AML_EXT_MUTEX:
        case AML_EXT_EVENT: {
            /* Mutex/Event(NameString, ByteData) — no PkgLength */
            char seg[4];
            parse_name_string(data, &offset, seg);
            offset++;  /* skip sync_level / event_flags byte */
            new_node(seg, AML_NODE_NONE, scope);
            break;
        }

        /* ---------- Method-body opcodes (skip silently) ---------- */
        case AML_EXT_COND_REF_OF:
        case AML_EXT_CREATE_FIELD:
        default: {
            offset = start + 2;  /* skip 0x5B + ext byte */
            break;
        }
        }
        break;
    }

    /* ---------- Simple name references (4-byte NameSeg = method call) ---------- */
    default:
        if ((op >= 'A' && op <= 'Z') || op == '_') {
            /* This is a method call or name reference as a term.
             * Read the remaining 3 bytes of the NameSeg and skip. */
            offset = start + 5;
        } else {
            /* Unknown opcode in namespace context — skip if possible */
            int sz = term_size(data, start, end);
            if (sz > 0) {
                offset = start + sz;
            } else {
                log_printf(LOG_LEVEL_WARN, "aml: cannot determine size of term at %d, op=0x%x\n",
                             start, op);
                offset = start + 1;  /* skip at least the opcode */
            }
        }
        break;
    }

    return offset;
}


static int parse_termlist(const uint8_t *data, int offset, int end, aml_node_t *scope) {
    while (offset < end) {
        int sz = term_size(data, offset, end);
        if (sz < 0) {
            /* Unknown size — try parsing it */
            int next = parse_term(data, offset, end, scope);
            if (next <= offset) break;
            offset = next;
        } else if (sz == 0) {
            break;
        } else {
            /* Skip unknown term by size — too risky for structured parse */
            int next = parse_term(data, offset, end, scope);
            if (next <= offset) {
                /* parse_term didn't advance — force skip by term_size */
                offset += sz;
            } else {
                offset = next;
            }
        }
    }
    return offset;
}


/* Execution context for evaluating AML methods.
 * Simple implementation: just enough for Return(Package{...}) patterns. */
typedef struct {
    const uint8_t *data;     /* method body data */
    int            offset;   /* current offset in method body */
    int            end;      /* end offset */
    aml_node_t    *result;   /* evaluation result (if any) */
} aml_exec_t;

/* Forward */
static int exec_term(aml_exec_t *ctx);

/* Evaluate a DataRefObject in execution context (same as parse_value but from exec) */
static int exec_data_ref(aml_exec_t *ctx, aml_node_t *node) {
    return parse_value(ctx->data, ctx->offset, ctx->end, node);
}

/* Execute a single term in method body. Returns 1 if executed, 0 at end, <0 on error. */
static int exec_term(aml_exec_t *ctx) {
    if (ctx->offset >= ctx->end) return 0;

    int start = ctx->offset;
    uint8_t op = ctx->data[ctx->offset++];

    switch (op) {
    case AML_RETURN_OP: {
        /* Parse and evaluate the return value */
        aml_node_t *res = aml_alloc_node();
        if (!res) return -1;
        int next = exec_data_ref(ctx, res);
        if (next < 0) return -1;
        ctx->offset = next;
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
        ctx->offset = next;
        /* Skip target (operand) */
        if (ctx->offset < ctx->end) {
            uint8_t top = ctx->data[ctx->offset];
            if (top == 0x5B) {
                /* Extended opcode target - skip it */
                ctx->offset += 2;  /* 0x5B and the ext op */
            } else if ((top >= 'A' && top <= 'Z') || top == '_') {
                ctx->offset += 4;  /* NameSeg target */
            } else {
                /* Unknown target format - skip 1 byte */
                ctx->offset++;
            }
        }
        return 1;
    }

    case AML_IF_OP: {
        int pkg_len = read_pkg_length(ctx->data, &ctx->offset);
        int if_end = start + pkg_len;
        /* Parse predicate (method call or reference that returns integer) */
        /* For simplicity, check if predicate is Zero/One/constant */
        uint8_t pred_op = ctx->data[ctx->offset];
        uint64_t pred_val = 0;

        switch (pred_op) {
        case AML_ZERO_OP: pred_val = 0; ctx->offset++; break;
        case AML_ONE_OP:  pred_val = 1; ctx->offset++; break;
        default:
            /* Try to evaluate the predicate */
            {
                aml_node_t pred_node;
                memset(&pred_node, 0, sizeof(pred_node));
                int next = exec_data_ref(ctx, &pred_node);
                if (next >= 0) {
                    ctx->offset = next;
                    if (pred_node.type == AML_NODE_INTEGER)
                        pred_val = pred_node.integer;
                    else
                        pred_val = 1;  /* non-zero = true */
                }
            }
            break;
        }

        if (pred_val) {
            /* Execute the If body */
            ctx->offset = parse_termlist(ctx->data, ctx->offset, if_end, NULL);
        } else {
            /* Skip the If body */
            ctx->offset = if_end;
        }
        /* Check for optional Else */
        if (ctx->offset + 1 <= ctx->end && ctx->data[ctx->offset] == AML_ELSE_OP) {
            int else_start = ctx->offset;
            ctx->offset++;  /* skip ElseOp */
            int else_len = read_pkg_length(ctx->data, &ctx->offset);
            int else_end = else_start + else_len;
            if (!pred_val) {
                ctx->offset = parse_termlist(ctx->data, ctx->offset, else_end, NULL);
            } else {
                ctx->offset = else_end;
            }
        }
        return 1;
    }

    case AML_WHILE_OP: {
        int pkg_len = read_pkg_length(ctx->data, &ctx->offset);
        int while_end = start + pkg_len;
        /* For simplicity, evaluate predicate once — if true, execute body once */
        /* This is a minimal implementation; real WhileOp repeats */
        uint8_t pred_op = ctx->data[ctx->offset];
        uint64_t pred_val = 0;

        switch (pred_op) {
        case AML_ZERO_OP: pred_val = 0; ctx->offset++; break;
        case AML_ONE_OP:  pred_val = 1; ctx->offset++; break;
        case AML_ONES_OP: pred_val = 1; ctx->offset++; break;
        default:
            {
                aml_node_t pred_node;
                memset(&pred_node, 0, sizeof(pred_node));
                int next = exec_data_ref(ctx, &pred_node);
                if (next >= 0) {
                    ctx->offset = next;
                    if (pred_node.type == AML_NODE_INTEGER)
                        pred_val = pred_node.integer;
                    else
                        pred_val = 1;
                }
            }
            break;
        }

        /* Execute body once if true (simple loop; don't loop infinitely) */
        if (pred_val) {
            int body_offset = ctx->offset;
            ctx->offset = parse_termlist(ctx->data, ctx->offset, while_end, NULL);
            (void)body_offset; /* would be used for loop back edge */
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

    /* Integer prefix terms (bypass) */
    case AML_BYTE_PREFIX:   ctx->offset++; return 1;
    case AML_WORD_PREFIX:   ctx->offset += 2; return 1;
    case AML_DWORD_PREFIX:  ctx->offset += 4; return 1;
    case AML_QWORD_PREFIX:  ctx->offset += 8; return 1;

    /* NameSeg reference (method call) — skip 3 more bytes */
    default:
        if ((op >= 'A' && op <= 'Z') || op == '_') {
            ctx->offset += 3;  /* complete the 4-byte NameSeg */
            return 1;
        }
        break;
    }

    return 1;
}

/* Execute a method by running through its body until ReturnOp or end. */
static aml_node_t *aml_exec_method_body(const uint8_t *body, uint32_t body_len) {
    aml_exec_t ctx;
    ctx.data   = body;
    ctx.offset = 0;
    ctx.end    = (int)body_len;
    ctx.result = NULL;

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

static int read_integer_raw(const uint8_t *data, int *offset, uint64_t *val);

int aml_raw_scan_s5(uint64_t *out_vals, int max_vals) {
    if (!aml_data_copy || aml_data_len < 12)
        return -1;

    const uint8_t *data = aml_data_copy;
    int end = (int)aml_data_len;

    /* Scan for NameOp + NameSeg("_S5_") = 08 5F 53 35 5F */
    for (int i = 0; i < end - 12; i++) {
        if (data[i] != AML_NAME_OP)                continue;
        if (data[i+1] != '_' || data[i+2] != 'S')  continue;
        if (data[i+3] != '5' || data[i+4] != '_')  continue;

        /* Found Name(_S5_, ...) — try to parse value as PackageOp */
        int off = i + 5;  /* past opcode + name */
        uint8_t op = data[off];
        if (op != AML_PACKAGE_OP && op != AML_VAR_PACKAGE_OP) {
            log_printf(LOG_LEVEL_WARN, "aml: _S5 value not a PackageOp (0x%02X)\n", op);
            return -1;
        }

        off++;  /* skip PackageOp */
        int pkg_end = off + read_pkg_length(data, &off);
        if (pkg_end > end) pkg_end = end;

        int num_elems = data[off++];  /* NumElements is a raw byte in QEMU AML */

        for (int j = 0; j < num_elems && off < pkg_end; j++) {
            uint64_t val = 0;
            if (read_integer_raw(data, &off, &val) == 0) {
                if (j < max_vals) out_vals[j] = val;
            } else {
                log_printf(LOG_LEVEL_WARN, "aml: _S5 cannot parse element %d\n", j);
                break;
            }
        }

        log_printf(LOG_LEVEL_DEBUG, "aml: _S5 found via raw scan, %d elements\n", num_elems);
        return num_elems;
    }

    return -1;
}

/* Simplified integer reader for raw AML scan */
static int read_integer_raw(const uint8_t *data, int *offset, uint64_t *val) {
    int off = *offset;
    uint8_t op = data[off++];

    switch (op) {
    case AML_ZERO_OP:       *val = 0; *offset = off; return 0;
    case AML_ONE_OP:        *val = 1; *offset = off; return 0;
    case AML_ONES_OP:       *val = ~0ULL; *offset = off; return 0;
    case AML_BYTE_PREFIX:   *val = data[off]; *offset = off + 1; return 0;
    case AML_WORD_PREFIX:   *val = data[off] | ((uint32_t)data[off+1] << 8); *offset = off + 2; return 0;
    case AML_DWORD_PREFIX:  *val = (uint32_t)data[off] | ((uint32_t)data[off+1] << 8) |
                                   ((uint32_t)data[off+2] << 16) | ((uint32_t)data[off+3] << 24);
                            *offset = off + 4; return 0;
    case AML_QWORD_PREFIX:  *val = (uint64_t)data[off] | ((uint64_t)data[off+1] << 8) |
                                   ((uint64_t)data[off+2] << 16) | ((uint64_t)data[off+3] << 24) |
                                   ((uint64_t)data[off+4] << 32) | ((uint64_t)data[off+5] << 40) |
                                   ((uint64_t)data[off+6] << 48) | ((uint64_t)data[off+7] << 56);
                            *offset = off + 8; return 0;
    default:
        *val = 0;
        *offset = off;
        return -1;
    }
}


int aml_init(void) {
    node_pool_used = 0;
    aml_namespace_root = NULL;

    uint64_t dsdt_phys = acpi_info.x_dsdt ? acpi_info.x_dsdt : (uint64_t)acpi_info.dsdt_addr;
    uint32_t dsdt_len  = acpi_info.dsdt_length;

    if (!dsdt_phys || dsdt_len <= sizeof(sdt_header_t)) {
        log_print(LOG_LEVEL_ERROR, "aml: no DSDT available\n");
        return -1;
    }

    log_printf(LOG_LEVEL_DEBUG, "aml: DSDT at 0x%lx length=%u\n", (unsigned long)dsdt_phys, dsdt_len);

    /* Copy the DSDT AML bytecode into heap memory.
     * The AML data starts after the SDT header. */
    uint32_t aml_len = dsdt_len - (uint32_t)sizeof(sdt_header_t);
    const uint8_t *src = NULL;
    uint8_t *aml_data = NULL;

    /* Access physical memory */
    {
        /* Use identity map if below 64MB, else temp map */
        uint64_t aml_data_phys = dsdt_phys + sizeof(sdt_header_t);
        if (aml_data_phys < 0x4000000ULL) {
            src = (const uint8_t *)(uintptr_t)(KERNEL_BASE + aml_data_phys);
        } else {
            src = (const uint8_t *)vmm_temp_map(aml_data_phys);
        }
    }

    if (!src) {
        log_print(LOG_LEVEL_ERROR, "aml: cannot access DSDT data\n");
        return -1;
    }

    /* Allocate and copy */
    aml_data = (uint8_t *)kmalloc(aml_len);
    if (!aml_data) {
        log_print(LOG_LEVEL_ERROR, "aml: out of memory for DSDT copy\n");
        if ((uintptr_t)src < KERNEL_BASE || (uintptr_t)src >= KERNEL_BASE + 0x4000000ULL)
            vmm_temp_unmap();
        return -1;
    }
    memcpy(aml_data, src, aml_len);
    if ((uintptr_t)src < KERNEL_BASE || (uintptr_t)src >= KERNEL_BASE + 0x4000000ULL)
        vmm_temp_unmap();



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
    offset = parse_termlist(aml_data, offset, (int)aml_len, aml_namespace_root);

    log_printf(LOG_LEVEL_DEBUG, "aml: parsed %d/%u bytes, %d nodes\n", offset, aml_len, node_pool_used);

    /* DSDT copy is kept (we may need it for method execution) */
    aml_data_copy = aml_data;
    aml_data_len  = aml_len;
    return 0;
}


aml_node_t *aml_find(const char *path) {
    if (!aml_namespace_root || !path || !*path) return NULL;

    /* Start from root if path starts with '\', or root */
    aml_node_t *node = aml_namespace_root;

    /* Skip leading backslash */
    const char *p = path;
    if (*p == '\\') p++;

    /* We need to allocate a temporary buffer for name segments.
     * The input path format is "." or "/" separated 4-char names. */
    char name_buf[5];
    while (*p) {
        /* Extract a name segment (up to 4 chars or until . / or end) */
        int len = 0;
        while (*p && *p != '.' && *p != '/' && len < 4)
            name_buf[len++] = *(p++);
        while (len < 4) name_buf[len++] = '_';  /* pad with '_' */

        /* Find the child */
        if (!node->child) return NULL;  /* no children — not found */

        node = find_child(node, name_buf);
        if (!node) return NULL;

        /* Skip separator */
        if (*p == '.' || *p == '/') p++;
    }

    return node;
}


uint64_t aml_eval_integer(const char *path) {
    aml_node_t *node = aml_find(path);
    if (!node) return 0;

    switch (node->type) {
    case AML_NODE_INTEGER:
        return node->integer;

    case AML_NODE_METHOD:
        /* Execute the method and get the integer result */
        {
            aml_node_t *result = aml_exec_method_body(node->method.body,
                                                       node->method.body_len);
            if (result && result->type == AML_NODE_INTEGER) {
                uint64_t val = result->integer;
                return val;
            }
        }
        return 0;

    default:
        return 0;
    }
}


int aml_get_package_int(const char *path, int index, uint64_t *val) {
    if (!val) return -1;

    aml_node_t *node = aml_find(path);
    if (!node) return -1;

    aml_node_t *pkg = NULL;

    if (node->type == AML_NODE_PACKAGE) {
        pkg = node;
    } else if (node->type == AML_NODE_METHOD) {
        aml_node_t *result = aml_exec_method_body(node->method.body,
                                                   node->method.body_len);
        if (result && result->type == AML_NODE_PACKAGE)
            pkg = result;
    }

    if (!pkg || (uint32_t)index >= pkg->package.count)
        return -1;

    aml_node_t *elem = pkg->package.elements[index];
    if (!elem || elem->type != AML_NODE_INTEGER)
        return -1;

    *val = elem->integer;
    return 0;
}


static void dump_node(aml_node_t *node, int depth) {
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
