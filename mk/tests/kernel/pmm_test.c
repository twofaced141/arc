#include "test.h"
#include "debug.h"
#include "pmm.h"
#include "memory.h"

/* Enough entries to hold every free page in a 64MB guest (~16K). */
static void *pmm_all[16384];

void pmm_boot_tests(void) {
    test_group("pmm");
    int ok;
    uint32_t free0 = pmm_get_free_pages();

    /* ---- alloc/free roundtrip: aligned, unique, count restored ---- */
    {
        void *pages[64];
        ok = 1;
        for (int i = 0; i < 64; i++) {
            pages[i] = pmm_alloc_page();
            if (!pages[i] || ((uintptr_t)pages[i] & (PAGE_SIZE - 1))) {
                ok = 0;
                break;
            }
            for (int j = 0; j < i; j++) {
                if (pages[j] == pages[i]) { ok = 0; break; }
            }
        }
        test_result("pmm_alloc_aligned_unique", ok);

        for (int i = 0; i < 64; i++)
            if (pages[i]) pmm_free_page(pages[i]);
        test_result("pmm_free_restores_count", pmm_get_free_pages() == free0);
    }

    /* ---- alloc_pages: contiguous, zero count, too-big count ---- */
    {
        void *blk = pmm_alloc_pages(8);
        ok = blk != NULL &&
             ((uintptr_t)blk & (PAGE_SIZE - 1)) == 0;
        if (blk)
            pmm_free_pages(blk, 8);
        test_result("pmm_alloc_pages_contiguous", ok);
        test_result("pmm_alloc_pages_zero", pmm_alloc_pages(0) == NULL);
        test_result("pmm_alloc_pages_too_big", pmm_alloc_pages(free0 + 1) == NULL);
        test_result("pmm_alloc_pages_count_ok", pmm_get_free_pages() == free0);
    }

    /* ---- double free must be a silent no-op (no count inflation) ---- */
    {
        void *p = pmm_alloc_page();
        if (p) {
            uint32_t f1 = pmm_get_free_pages();
            pmm_free_page(p);
            uint32_t f2 = pmm_get_free_pages();
            pmm_free_page(p);                    /* second free: ignored */
            uint32_t f3 = pmm_get_free_pages();
            test_result("pmm_double_free_safe",
                        f2 == f1 + 1 && f3 == f2);
        } else {
            test_result("pmm_double_free_safe", 0);
        }
    }

    /* ---- partial free_pages ---- */
    {
        void *b = pmm_alloc_pages(4);
        ok = b != NULL;
        if (b) {
            pmm_free_pages(b, 2);
            pmm_free_pages((uint8_t *)b + 2 * PAGE_SIZE, 2);
        }
        test_result("pmm_free_pages_partial", ok && pmm_get_free_pages() == free0);
    }

    /* ---- exhaustion: alloc everything, then recovery ----
     * pmm_all holds up to 16384 pages; on guests with more RAM than
     * that the exhaustion checks run against that bound instead. */
    {
        uint32_t snap = pmm_get_free_pages();
        int n = 0;
        while (n < 16384 && (pmm_all[n] = pmm_alloc_page()) != NULL)
            n++;

        if (snap <= 16384) {
            test_result("pmm_exhaust_returns_null",
                        n == (int)snap && n > 0);
            test_result("pmm_exhaust_count_matches", (uint32_t)n == snap);
            test_result("pmm_exhaust_free_zero", pmm_get_free_pages() == 0);
            test_result("pmm_exhaust_alloc_fails", pmm_alloc_page() == NULL);
            test_result("pmm_exhaust_pages_fails", pmm_alloc_pages(1) == NULL);
        } else {
            test_result("pmm_exhaust_returns_null", n == 16384);
            test_result("pmm_exhaust_count_matches", (uint32_t)n == 16384);
            test_result("pmm_exhaust_free_zero",
                        pmm_get_free_pages() == snap - 16384);
            test_result("pmm_exhaust_alloc_fails", pmm_alloc_page() != NULL);
            test_result("pmm_exhaust_pages_fails", pmm_alloc_pages(1) != NULL);
        }

        for (int i = 0; i < n; i++)
            pmm_free_page(pmm_all[i]);
        test_result("pmm_exhaust_recovery", pmm_get_free_pages() == snap);
    }
}
