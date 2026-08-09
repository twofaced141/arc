#include "test.h"
#include "assert.h"
#include "debug.h"

void assert_boot_tests(void) {
    test_group("assert");
    int ok;

    ok = test_check_int(VERIFY(1 == 1), 1);
    test_result("assert_verify_true", ok);

#ifndef CONFIG_DEBUG
    ok = test_check_int(VERIFY(0), 0);
    test_result("assert_verify_false", ok);
#endif

    {
        int x = 42;
        ok = test_check_int(VERIFY(x == 42), 1);
        ok = ok && test_check_int(VERIFY(x), 1);
        ok = ok && test_check_int(VERIFY(!0), 1);
        test_result("assert_verify_expr", ok);
    }

#ifndef CONFIG_DEBUG
    ASSERT(1);
    ASSERT(0);
    test_result("assert_assert_release", 1);
#else
    ASSERT(1);
    test_result("assert_assert_debug", 1);
#endif
}
