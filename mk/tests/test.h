#ifndef MK_TEST_H
#define MK_TEST_H

#include <stdint.h>

void test_banner(const char *msg);
void test_group(const char *name);
int  test_check(int cond);
int  test_check_u64(uint64_t a, uint64_t b);
int  test_check_int(int a, int b);
int  test_check_ptr(const void *a, const void *b);
void test_result(const char *name, int ok);
void test_run_boot_tests(void);

#endif
