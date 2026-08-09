#ifndef AML_H
#define AML_H

#include <stdint.h>
#include <stddef.h>

#define AML_ZERO_OP           0x00
#define AML_ONE_OP            0x01
#define AML_ALIAS_OP          0x06
#define AML_NAME_OP           0x08
#define AML_BYTE_PREFIX       0x0A
#define AML_WORD_PREFIX       0x0B
#define AML_DWORD_PREFIX      0x0C
#define AML_STRING_PREFIX     0x0D
#define AML_QWORD_PREFIX      0x0E
#define AML_SCOPE_OP          0x10
#define AML_BUFFER_OP         0x11
#define AML_PACKAGE_OP        0x12
#define AML_VAR_PACKAGE_OP    0x13
#define AML_METHOD_OP         0x14
#define AML_DUAL_NAME_PREFIX  0x2E
#define AML_MULTI_NAME_PREFIX 0x2F
#define AML_EXT_OP            0x5B
#define AML_ROOT_PREFIX       0x5C
#define AML_PARENT_PREFIX     0x5E
#define AML_STORE_OP          0x70
#define AML_ADD_OP            0x72
#define AML_SUBTRACT_OP       0x73
#define AML_MULTIPLY_OP       0x74
#define AML_DIVIDE_OP         0x75
#define AML_AND_OP            0x78
#define AML_NAND_OP           0x79
#define AML_OR_OP             0x7A
#define AML_XOR_OP            0x7C
#define AML_NOT_OP            0x7D
#define AML_CONCAT_OP         0x81
#define AML_MOD_OP            0x83
#define AML_EQUAL_OP          0x8A
#define AML_LEQUAL_OP         0x8B
#define AML_LGREATER_OP       0x8C
#define AML_LLESS_OP          0x8D
#define AML_TO_INTEGER_OP     0x92
#define AML_IF_OP             0xA0
#define AML_ELSE_OP           0xA1
#define AML_WHILE_OP          0xA2
#define AML_RETURN_OP         0xA4
#define AML_BREAK_OP          0xA5
#define AML_NOOP_OP           0xA6
#define AML_ONES_OP           0xFF

/* Extended opcodes (after 0x5B) — namespace-building 0x80-0x88 use PkgLength */
#define AML_EXT_MUTEX         0x01
#define AML_EXT_EVENT         0x02
#define AML_EXT_COND_REF_OF   0x12
#define AML_EXT_CREATE_FIELD  0x13
#define AML_EXT_OPREGION      0x80
#define AML_EXT_FIELD         0x81
#define AML_EXT_DEVICE        0x82
#define AML_EXT_PROCESSOR     0x83
#define AML_EXT_POWERRES      0x84
#define AML_EXT_THERMAL       0x85
#define AML_EXT_INDEXFIELD    0x86
#define AML_EXT_BANKFIELD     0x87
#define AML_EXT_DATAREGION    0x88
/* ACPI 5.0+ extended opcodes (also use PkgLength) */
#define AML_EXT_GPIOPIN       0x0C
#define AML_EXT_GENERICSERIAL 0x0D

/* Standalone (not extended) opcodes */
#define AML_NOTIFY_OP         0x86
#define AML_TO_HEX_OP         0x98
#define AML_COPY_OBJECT_OP    0x9D

/* NameSeg is always 4 characters (padded with '_') */
#define AML_NAMESEG_LEN  4

typedef enum {
    AML_NODE_NONE,
    AML_NODE_INTEGER,
    AML_NODE_STRING,
    AML_NODE_BUFFER,
    AML_NODE_PACKAGE,
    AML_NODE_METHOD,
    AML_NODE_SCOPE,
    AML_NODE_DEVICE,
    AML_NODE_PROCESSOR,
    AML_NODE_POWERRES,
    AML_NODE_THERMAL,
    AML_NODE_OPREGION,
    AML_NODE_FIELD,
    AML_NODE_MUTEX,
} aml_node_type_t;

typedef struct aml_node {
    char             seg[AML_NAMESEG_LEN];   /* 4-char name segment */
    struct aml_node *parent;
    struct aml_node *child;                  /* first child */
    struct aml_node *next;                   /* next sibling */
    aml_node_type_t  type;
    union {
        uint64_t integer;
        char    *string;
        struct { uint8_t *data; uint32_t len; } buffer;
        struct { struct aml_node **elements; uint32_t count; } package;
        struct { uint8_t *body; uint32_t body_len; uint8_t nargs; } method;
    };
} aml_node_t;


/* Parse DSDT/SSDT AML into a namespace tree.
 * Called once during ACPI init. */
int aml_init(void);

/* Find a namespace node by path (e.g. "\\_S5_" or "_SB_.PCI0").
 * Returns NULL if not found. */
aml_node_t *aml_find(const char *path);

/* Evaluate a named object that should yield an integer.
 * Handles both NameOp integers and MethodOp returning integer. */
uint64_t aml_eval_integer(const char *path);

/* Extract an integer element from a named Package.
 * Returns 0 on success, -1 if not found/not a package/wrong index. */
int aml_get_package_int(const char *path, int index, uint64_t *val);

/* Debug dump the namespace tree. */
void aml_dump(void);

/* Raw AML scan for _S5 — bypasses namespace tree lookup.
 * Scans DSDT AML bytecode for Name(_S5, Package(...)) and extracts
 * the integer elements.  Returns number of elements found, or -1.
 * max_vals caps output array size. */
int aml_raw_scan_s5(uint64_t *out_vals, int max_vals);

/* The namespace root (external for iteration). */
extern aml_node_t *aml_namespace_root;

#endif /* AML_H */
