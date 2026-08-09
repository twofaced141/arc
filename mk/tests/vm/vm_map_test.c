#include "test.h"
#include "vm_object.h"
#include "vmm.h"
#include "string.h"
#include "debug.h"

struct page_directory {
    int dummy;
};

void vm_map_boot_tests(void) {
    test_group("vm_map");
    int ok;

    vm_map_t map;
    struct page_directory pd;
    vm_object_t *obj = vm_object_create_anon(0x100000);
    if (!obj) {
        test_result("vm_map_init", 0);
        return;
    }

    ok = test_check_int(vm_map_init(&map, &pd), 0);
    ok = ok && test_check_int(map.entry_count, 0);
    ok = ok && test_check_int(map.max_entries, 64);
    ok = ok && test_check_ptr(map.pml4, &pd);
    test_result("vm_map_init", ok);

    ok = test_check_int(vm_map_map(&map, 0x10000000, obj, 0, 0x1000, VM_PROT_ALL, VM_INHERIT_COPY), 0);
    ok = ok && test_check_int(map.entry_count, 1);
    test_result("vm_map_map", ok);

    ok = test_check_int(vm_map_map(&map, 0x20000000, obj, 0, 0x1000, VM_PROT_READ, VM_INHERIT_NONE), 0);
    ok = ok && test_check_int(map.entry_count, 2);
    test_result("vm_map_map_second", ok);

    ok = test_check_int(vm_map_map(&map, 0x10000000, obj, 0, 0x1000, VM_PROT_ALL, VM_INHERIT_COPY), -1);
    test_result("vm_map_map_overlap", ok);

    {
        vm_entry_t *e = vm_map_find_entry(&map, 0x10000000);
        ok = test_check_ptr(e, e);
        ok = ok && test_check_ptr(e ? (void *)e->obj : NULL, (void *)obj);
        ok = ok && test_check_u64(e ? e->start : 0, 0x10000000);
        test_result("vm_map_find_entry", ok);
    }

    {
        vm_entry_t *e = vm_map_find_entry(&map, 0x50000000);
        ok = test_check_ptr(e, NULL);
        test_result("vm_map_find_entry_miss", ok);
    }

    {
        vm_entry_t *e = vm_map_find_entry(&map, 0x18000000);
        ok = test_check_ptr(e, NULL);
        test_result("vm_map_find_entry_gap", ok);
    }

    /* unmap full (case a) */
    {
        int old_count = map.entry_count;
        ok = test_check_int(vm_map_unmap(&map, 0x10000000, 0x1000), 0);
        ok = ok && test_check_int(map.entry_count, old_count - 1);
        test_result("vm_map_unmap_full", ok);
    }

    vm_map_destroy(&map);
    vm_map_init(&map, &pd);
    vm_map_map(&map, 0x10000000, obj, 0, 0x4000, VM_PROT_ALL, VM_INHERIT_COPY);

    /* unmap left partial (case b) */
    {
        ok = test_check_int(vm_map_unmap(&map, 0x10000000, 0x1000), 0);
        vm_entry_t *e = vm_map_find_entry(&map, 0x10000000);
        ok = ok && test_check_ptr(e, NULL);
        e = vm_map_find_entry(&map, 0x10001000);
        ok = ok && test_check_ptr(e, e);
        ok = ok && test_check_u64(e->start, 0x10001000);
        ok = ok && test_check_u64(e->end, 0x10004000);
        test_result("vm_map_unmap_left_partial", ok);
    }

    vm_map_destroy(&map);
    vm_map_init(&map, &pd);
    vm_map_map(&map, 0x10000000, obj, 0, 0x4000, VM_PROT_ALL, VM_INHERIT_COPY);

    /* unmap right partial (case c) */
    {
        ok = test_check_int(vm_map_unmap(&map, 0x10003000, 0x1000), 0);
        vm_entry_t *e = vm_map_find_entry(&map, 0x10003000);
        ok = ok && test_check_ptr(e, NULL);
        e = vm_map_find_entry(&map, 0x10001000);
        ok = ok && test_check_ptr(e, e);
        ok = ok && test_check_u64(e->end, 0x10003000);
        test_result("vm_map_unmap_right_partial", ok);
    }

    vm_map_destroy(&map);
    vm_map_init(&map, &pd);
    vm_map_map(&map, 0x10000000, obj, 0, 0x4000, VM_PROT_ALL, VM_INHERIT_COPY);

    /* unmap split (case d) */
    {
        ok = test_check_int(vm_map_unmap(&map, 0x10001000, 0x2000), 0);
        vm_entry_t *e = vm_map_find_entry(&map, 0x10000000);
        ok = ok && test_check_ptr(e, e);
        ok = ok && test_check_u64(e->end, 0x10001000);
        e = vm_map_find_entry(&map, 0x10003000);
        ok = ok && test_check_ptr(e, e);
        ok = ok && test_check_u64(e->start, 0x10003000);
        ok = ok && test_check_u64(e->end, 0x10004000);
        test_result("vm_map_unmap_split", ok);
    }

    /* protect */
    vm_map_destroy(&map);
    vm_map_init(&map, &pd);
    vm_map_map(&map, 0x10000000, obj, 0, 0x1000, VM_PROT_READ, VM_INHERIT_COPY);
    ok = test_check_int(vm_map_protect(&map, 0x10000000, 0x1000, VM_PROT_ALL), 0);
    {
        vm_entry_t *e = vm_map_find_entry(&map, 0x10000000);
        ok = ok && test_check_u64(e->prot, VM_PROT_ALL);
    }
    test_result("vm_map_protect", ok);

    /* protect partial */
    vm_map_map(&map, 0x20000000, obj, 0, 0x4000, VM_PROT_NONE, VM_INHERIT_COPY);
    ok = test_check_int(vm_map_protect(&map, 0x20001000, 0x2000, VM_PROT_READ|VM_PROT_WRITE), 0);
    {
        vm_entry_t *e = vm_map_find_entry(&map, 0x20000000);
        ok = ok && test_check_u64(e->end, 0x20001000);
        ok = ok && test_check_u64(e->prot, VM_PROT_NONE);
        e = vm_map_find_entry(&map, 0x20001000);
        ok = ok && test_check_ptr(e, e);
        ok = ok && test_check_u64(e->prot, VM_PROT_READ|VM_PROT_WRITE);
        e = vm_map_find_entry(&map, 0x20003000);
        ok = ok && test_check_ptr(e, e);
        ok = ok && test_check_u64(e->prot, VM_PROT_NONE);
    }
    test_result("vm_map_protect_partial", ok);

    vm_map_destroy(&map);
    vm_object_release(obj);
}
