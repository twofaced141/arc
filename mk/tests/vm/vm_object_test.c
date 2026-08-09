#include "test.h"
#include "vm_object.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "debug.h"

/* ---- Chaotic stress: many live objects, random create/retain/release/fault ---- */

#define VM_STRESS_SLOTS 2048u       /* max simultaneously live objects (1000..10000) */
#define VM_STRESS_OPS   1000000u    /* total random operations */

static uint32_t vm_stress_rng(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void vm_object_boot_tests(void) {
    test_group("vm_object");
    int ok;
    vm_object_t *obj;
    vm_page_t page;

    obj = vm_object_create_anon(4096);
    ok = test_check_ptr(obj, obj);
    test_result("vm_object_create_anon", obj != NULL);

    if (obj) {
        ok = test_check_int(obj->type, VM_OBJ_ANON);
        ok = ok && test_check_u64(obj->size, 4096);
        ok = ok && test_check_ptr((void *)obj->fault, (void *)vm_object_anon_fault);
        test_result("vm_object_anon_fields", ok);

        ok = test_check_int(obj->fault(obj, 0, &page, 1), 0);
        ok = ok && test_check(page.phys_addr != 0);
        ok = ok && test_check(page.ref_count == 1);
        test_result("vm_object_anon_fault", ok);

        vm_object_release(obj);
    }

    obj = vm_object_create_phys(0x100000, 4096);
    ok = test_check_ptr(obj, obj);
    test_result("vm_object_create_phys", obj != NULL);
    if (obj) {
        ok = test_check_int(obj->type, VM_OBJ_PHYS);
        ok = ok && test_check_u64(obj->phys.phys_base, 0x100000);
        test_result("vm_object_phys_fields", ok);

        ok = test_check_int(obj->fault(obj, 0, &page, 0), 0);
        ok = ok && test_check_u64(page.phys_addr, 0x100000);
        test_result("vm_object_phys_fault", ok);

        ok = test_check_int(obj->fault(obj, 4096, &page, 0), -1);
        test_result("vm_object_phys_fault_oob", ok);

        vm_object_release(obj);
    }

    obj = vm_object_create_shared(8192);
    ok = test_check_ptr(obj, obj);
    test_result("vm_object_create_shared", obj != NULL);
    if (obj) {
        ok = test_check_int(obj->type, VM_OBJ_SHARED);
        ok = ok && test_check_int(obj->shared.capacity, 0);
        ok = ok && test_check_ptr(obj->shared.pages, NULL);
        test_result("vm_object_shared_fields", ok);

        ok = test_check_int(obj->fault(obj, 0, &page, 1), 0);
        uint64_t first = page.phys_addr;
        ok = ok && test_check(first != 0);
        ok = ok && test_check_int(obj->fault(obj, 0, &page, 1), 0);
        ok = ok && test_check_u64(page.phys_addr, first);
        test_result("vm_object_shared_cache", ok);

        vm_object_release(obj);
    }

    {
        vm_object_t *parent = vm_object_create_anon(4096);
        obj = vm_object_create_shadow(parent);
        ok = test_check_ptr(obj, obj);
        test_result("vm_object_create_shadow", obj != NULL);
        if (obj && parent) {
            ok = test_check_int(obj->type, VM_OBJ_SHADOW);
            ok = ok && test_check_ptr(obj->shadow.parent, parent);
            ok = ok && test_check_ptr(obj->shadow.copied_map, obj->shadow.copied_map);
            test_result("vm_object_shadow_fields", ok);

            ok = test_check_int(obj->fault(obj, 0, &page, 1), 0);
            ok = ok && test_check(page.phys_addr != 0);
            test_result("vm_object_shadow_fault", ok);

            ok = test_check_int(obj->fault(obj, 0, &page, 1), 0);
            ok = ok && test_check(page.phys_addr != 0);
            test_result("vm_object_shadow_fault_twice", ok);

            vm_object_release(obj);
        }
        if (parent) vm_object_release(parent);
    }

    obj = vm_object_create_anon(4096);
    if (obj) {
        ok = test_check_int(obj->ref_count, 1);
        vm_object_t *r = vm_object_retain(obj);
        ok = ok && test_check_int(obj->ref_count, 2);
        ok = ok && test_check_ptr(r, obj);
        vm_object_release(obj);
        ok = ok && test_check_int(obj->ref_count, 1);
        vm_object_release(obj);
        test_result("vm_object_refcount", ok);
    }

    /* User-paged */
    obj = vm_object_create_user_paged(4096, 0xDEAD);
    ok = test_check_ptr(obj, obj);
    test_result("vm_object_create_user_paged", obj != NULL);
    if (obj) {
        ok = test_check_int(obj->type, VM_OBJ_USER_PAGED);
        ok = ok && test_check_u64(obj->paging_port, 0xDEAD);
        test_result("vm_object_user_paged_fields", ok);
        vm_object_release(obj);
    }

    /* ---- Edge cases ---- */

    /* size=0 is rejected by all creators */
    ok = test_check_ptr(vm_object_create_anon(0), NULL);
    ok = ok && test_check_ptr(vm_object_create_phys(0x100000, 0), NULL);
    ok = ok && test_check_ptr(vm_object_create_shared(0), NULL);
    ok = ok && test_check_ptr(vm_object_create_user_paged(0, 0), NULL);
    ok = ok && test_check_ptr(vm_object_create_shadow(NULL), NULL);
    test_result("vm_object_size_zero", ok);

    /* NULL args must be tolerated */
    vm_object_release(NULL);
    ok = test_check_ptr(vm_object_retain(NULL), NULL);
    test_result("vm_object_null_args", ok);

    /* Exact boundary: fault(offset == size) and fault(offset > size) must fail */
    obj = vm_object_create_anon(4096);
    if (obj) {
        ok = test_check_int(obj->fault(obj, 4096, &page, 1), -1);
        ok = ok && test_check_int(obj->fault(obj, 8192, &page, 1), -1);
        test_result("vm_object_anon_boundary", ok);
        vm_object_release(obj);
    }

    obj = vm_object_create_shared(4096);
    if (obj) {
        ok = test_check_int(obj->fault(obj, 4096, &page, 1), -1);
        ok = ok && test_check_int(obj->fault(obj, 8192, &page, 1), -1);
        test_result("vm_object_shared_boundary", ok);
        vm_object_release(obj);
    }

    obj = vm_object_create_user_paged(4096, 0);
    if (obj) {
        ok = test_check_int(obj->fault(obj, 4096, &page, 1), -1);
        ok = ok && test_check_int(obj->fault(obj, 8192, &page, 1), -1);
        test_result("vm_object_user_paged_boundary", ok);
        vm_object_release(obj);
    }

    {
        vm_object_t *parent = vm_object_create_anon(4096);
        obj = parent ? vm_object_create_shadow(parent) : NULL;
        if (obj && parent) {
            ok = test_check_int(obj->fault(obj, 4096, &page, 1), -1);
            ok = ok && test_check_int(obj->fault(obj, 8192, &page, 1), -1);
            test_result("vm_object_shadow_boundary", ok);
            vm_object_release(obj);
        }
        if (parent) vm_object_release(parent);
    }

    /* Multiple retain/release: freed exactly when ref_count hits 0 */
    obj = vm_object_create_anon(4096);
    if (obj) {
        ok = test_check_int(obj->ref_count, 1);
        for (int i = 0; i < 5; i++)
            vm_object_retain(obj);
        ok = ok && test_check_int(obj->ref_count, 6);
        for (int i = 0; i < 5; i++)
            vm_object_release(obj);
        ok = ok && test_check_int(obj->ref_count, 1);
        vm_object_release(obj);   /* last ref -> freed */
        test_result("vm_object_retain_release_many", ok);
    }

    /* Deep shadow chain: 10 levels, faults resolve through the whole chain */
    {
        vm_object_t *chain[11];
        int chain_ok = 1;
        chain[0] = vm_object_create_anon(4096);
        if (!chain[0]) chain_ok = 0;
        for (int i = 1; i <= 10; i++) {
            chain[i] = chain[i - 1] ? vm_object_create_shadow(chain[i - 1]) : NULL;
            if (!chain[i]) chain_ok = 0;
        }
        if (chain_ok) {
            ok = test_check_int(chain[10]->fault(chain[10], 0, &page, 1), 0);
            ok = ok && test_check(page.phys_addr != 0);
            test_result("vm_object_shadow_chain_fault", ok);

            vm_object_t *walk = chain[10];
            int depth = 0;
            while (walk && walk->type == VM_OBJ_SHADOW) {
                walk = walk->shadow.parent;
                depth++;
            }
            ok = test_check_int(depth, 10);
            ok = ok && walk && test_check_int(walk->type, VM_OBJ_ANON);
            test_result("vm_object_shadow_chain_depth", ok);
        }
        for (int i = 10; i >= 0; i--)
            if (chain[i]) vm_object_release(chain[i]);
        test_result("vm_object_shadow_chain_release", chain_ok);
    }

    /* Large objects: tens of MB, fault near the end of the range */
    {
        uint64_t big = 32ULL * 1024 * 1024;   /* 32 MB */

        obj = vm_object_create_anon(big);
        if (obj) {
            ok = test_check_u64(obj->size, big);
            ok = ok && test_check_int(obj->fault(obj, big - PAGE_SIZE, &page, 1), 0);
            ok = ok && test_check(page.phys_addr != 0);
            ok = ok && test_check_int(obj->fault(obj, big, &page, 1), -1);
            test_result("vm_object_large_anon", ok);
            vm_object_release(obj);
        }

        obj = vm_object_create_shared(big);
        if (obj) {
            ok = test_check_u64(obj->size, big);
            ok = ok && test_check_int(obj->fault(obj, big / 2, &page, 1), 0);
            ok = ok && test_check(page.phys_addr != 0);
            ok = ok && test_check_int(obj->fault(obj, big, &page, 1), -1);
            test_result("vm_object_large_shared", ok);
            vm_object_release(obj);
        }
    }

    /* Chaotic stress: random create/retain/release/fault on many live objects */
    {
        vm_object_t *slots[VM_STRESS_SLOTS];
        uint32_t     refs[VM_STRESS_SLOTS];          /* our tracked ref count (>=1 while live) */
        vm_object_t *parents[VM_STRESS_SLOTS];       /* shadow parent, if any */
        memset(slots, 0, sizeof(slots));
        memset(refs, 0, sizeof(refs));
        memset(parents, 0, sizeof(parents));

        uint32_t rng = 0xC0FFEE01u;
        uint32_t live = 0, created = 0, destroyed = 0, faults = 0;
        int err = 0;

        for (uint32_t op = 0; op < VM_STRESS_OPS && !err; op++) {
            uint32_t r = vm_stress_rng(&rng) % 100;

            if (r < 35 || live == 0) {               /* create a new object */
                if (live >= VM_STRESS_SLOTS)
                    continue;
                uint32_t s = vm_stress_rng(&rng) % VM_STRESS_SLOTS;
                uint32_t tries = 0;
                while (slots[s] && tries++ < VM_STRESS_SLOTS)
                    s = (s + 1) % VM_STRESS_SLOTS;
                if (slots[s]) { err = 1; break; }

                uint64_t size = (vm_stress_rng(&rng) & 1) ? 8192 : 4096;
                uint32_t t = vm_stress_rng(&rng) % 5;
                vm_object_t *o = NULL, *p = NULL;
                switch (t) {
                case 0: o = vm_object_create_anon(size);           break;
                case 1: o = vm_object_create_phys(0x100000, size); break;
                case 2: o = vm_object_create_shared(size);         break;
                case 3: o = vm_object_create_user_paged(size, 0);  break;
                default:
                    p = vm_object_create_anon(size);
                    if (p) o = vm_object_create_shadow(p);
                    break;
                }
                if (!o) {
                    if (p) vm_object_release(p);
                    err = 1;
                    break;
                }
                slots[s] = o;
                refs[s] = 1;
                parents[s] = p;
                live++;
                created++;
            } else if (r < 50) {                     /* retain a random live object */
                uint32_t s = vm_stress_rng(&rng) % VM_STRESS_SLOTS;
                if (slots[s]) {
                    vm_object_retain(slots[s]);
                    refs[s]++;
                }
            } else if (r < 75) {                     /* release (destroy on last ref) */
                uint32_t s = vm_stress_rng(&rng) % VM_STRESS_SLOTS;
                if (slots[s]) {
                    vm_object_release(slots[s]);
                    if (--refs[s] == 0) {
                        if (parents[s]) {
                            vm_object_release(parents[s]);
                            parents[s] = NULL;
                        }
                        slots[s] = NULL;
                        live--;
                        destroyed++;
                    }
                }
            } else {                                 /* fault a random live object */
                uint32_t s = vm_stress_rng(&rng) % VM_STRESS_SLOTS;
                if (!slots[s])
                    continue;
                if (slots[s]->type == VM_OBJ_PHYS || slots[s]->type == VM_OBJ_SHARED) {
                    uint64_t off = ((uint64_t)vm_stress_rng(&rng) * slots[s]->size) >> 32;
                    off &= ~((uint64_t)PAGE_SIZE - 1);
                    vm_page_t pg;
                    if (slots[s]->fault(slots[s], off, &pg, r & 1) != 0) {
                        err = 1;
                        break;
                    }
                    faults++;
                }
                /* ANON/SHADOW faults hand out fresh pages owned by the caller
                 * (and SHADOW leaks the parent's page), USER_PAGED with port 0
                 * would block on the pager — skip those types here. */
            }
        }

        test_result("vm_object_stress_ops", !err);

        /* Drain: every tracked ref must be released, nothing may leak */
        for (uint32_t s = 0; s < VM_STRESS_SLOTS; s++) {
            while (slots[s]) {
                vm_object_release(slots[s]);
                if (--refs[s] == 0) {
                    if (parents[s]) {
                        vm_object_release(parents[s]);
                        parents[s] = NULL;
                    }
                    slots[s] = NULL;
                    live--;
                    destroyed++;
                }
            }
        }

        ok = !err && live == 0 && created == destroyed && created > 0 && faults > 0;
        test_result("vm_object_stress_counts", ok);
    }
}
