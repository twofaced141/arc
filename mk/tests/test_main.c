#include "test.h"
#include "debug.h"

void test_banner(const char *msg) {
    debug_print(msg);
    debug_print("\r\n");
}

void test_group(const char *name) {
    debug_print("test: [");
    debug_print(name);
    debug_print("]\r\n");
}

int test_check(int cond) {
    return cond;
}

int test_check_u64(uint64_t a, uint64_t b) {
    return a == b;
}

int test_check_int(int a, int b) {
    return a == b;
}

int test_check_ptr(const void *a, const void *b) {
    return a == b;
}

void test_result(const char *name, int ok) {
    debug_print("test: ");
    debug_print(name);
    debug_print(ok ? " PASS\r\n" : " FAIL\r\n");
}

/* ---- Boot test group declarations ---- */
void vm_object_boot_tests(void);
void vm_map_boot_tests(void);
void ipc_boot_tests(void);
void assert_boot_tests(void);
void pmm_boot_tests(void);
void heap_boot_tests(void);
void heap_exhaust_boot_test(void);
void cspace_boot_tests(void);

void test_run_boot_tests(void) {
    test_banner("mk: tests enabled");
    /* heap_boot_tests() must run before the memory-heavy groups: it
     * allocates big contiguous blocks and asserts first-fit reuses the
     * coalesced region (p1), which only holds on a mostly-empty heap.
     * heap_exhaust_boot_test() runs LAST: it drains the heap to the
     * point of refusal, which is slow and leaves the allocator near its
     * cap until the run ends. */
    pmm_boot_tests();
    heap_boot_tests();
    cspace_boot_tests();
    vm_object_boot_tests();
    vm_map_boot_tests();
    ipc_boot_tests();
    assert_boot_tests();
    heap_exhaust_boot_test();
    test_banner("mk: tests done");
}
