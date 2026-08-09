#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

void *kernel_memcpy(void *dest, const void *src, size_t n);
void *kernel_memset(void *s, int c, size_t n);
void *kernel_memmove(void *dest, const void *src, size_t n);
int   kernel_memcmp(const void *s1, const void *s2, size_t n);
int   kernel_atoi(const char *s);
size_t kernel_strlen(const char *s);
int   kernel_strcmp(const char *s1, const char *s2);
int   kernel_strncmp(const char *s1, const char *s2, size_t n);
char *kernel_strcpy(char *dest, const char *src);
char *kernel_strncpy(char *dest, const char *src, size_t n);
char *kernel_strcat(char *dest, const char *src);

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

/* The kernel's string.c implements these. We compile mk/lib/string.c
 * directly into this test and #define the functions to kernel_xxx to
 * avoid colliding with libc. */
#define memcpy  kernel_memcpy
#define memset  kernel_memset
#define memmove kernel_memmove
#define memcmp  kernel_memcmp
#define atoi    kernel_atoi
#define strlen  kernel_strlen
#define strcmp  kernel_strcmp
#define strncmp kernel_strncmp
#define strcpy  kernel_strcpy
#define strncpy kernel_strncpy
#define strcat  kernel_strcat
#include "../../lib/string.c"
#undef memcpy
#undef memset
#undef memmove
#undef memcmp
#undef atoi
#undef strlen
#undef strcmp
#undef strncmp
#undef strcpy
#undef strncpy
#undef strcat

void host_string_tests(void) {
    printf("[string]\n");

    {
        uint8_t src[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
        uint8_t dst[16] = {0};
        kernel_memcpy(dst, src, 16);
        TEST("memcpy full", memcmp(dst, src, 16) == 0);
    }

    {
        uint8_t dst[16] = {0};
        uint8_t src[8]  = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};
        kernel_memcpy(dst, src, 8);
        TEST("memcpy partial", memcmp(dst, src, 8) == 0 && dst[8] == 0);
    }

    {
        uint8_t buf[32];
        kernel_memset(buf, 0xFF, 32);
        int ok = 1;
        for (int i = 0; i < 32; i++) if (buf[i] != 0xFF) { ok = 0; break; }
        TEST("memset 0xFF", ok);
    }

    {
        uint8_t buf[16];
        kernel_memset(buf, 0x00, 16);
        kernel_memset(buf + 4, 0x42, 8);
        TEST("memset partial",
             buf[0] == 0x00 && buf[3] == 0x00 &&
             buf[4] == 0x42 && buf[11] == 0x42 &&
             buf[12] == 0x00 && buf[15] == 0x00);
    }

    {
        uint8_t buf[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
        kernel_memmove(buf + 4, buf, 8);
        TEST("memmove forward",
             buf[0]==1 && buf[4]==1 && buf[11]==8 && buf[12]==13);
    }

    {
        uint8_t buf[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
        kernel_memmove(buf, buf + 4, 8);
        TEST("memmove backward",
             buf[0]==5 && buf[4]==9 && buf[8]==9 && buf[15]==16);
    }

    {
        uint8_t a[8] = {1,2,3,4,5,6,7,8};
        uint8_t b[8] = {1,2,3,4,5,6,7,8};
        TEST("memcmp equal", kernel_memcmp(a, b, 8) == 0);
    }

    {
        uint8_t a[8] = {1,2,3,4,5,6,7,8};
        uint8_t b[8] = {1,2,3,4,5,6,7,9};
        TEST("memcmp diff", kernel_memcmp(a, b, 8) != 0);
    }

    {
        uint8_t a[8] = {1,2,3,4,5,6,7,8};
        uint8_t b[8] = {1,2,3,4,5,6,7,8};
        TEST("memcmp zero length", kernel_memcmp(a, b, 0) == 0);
    }

    TEST("strlen empty", kernel_strlen("") == 0);
    TEST("strlen hello", kernel_strlen("hello") == 5);
    TEST("strlen spaces", kernel_strlen("a b c") == 5);

    TEST("strcmp equal", kernel_strcmp("hello", "hello") == 0);
    TEST("strcmp less", kernel_strcmp("abc", "abd") < 0);
    TEST("strcmp greater", kernel_strcmp("abd", "abc") > 0);
    TEST("strcmp empty", kernel_strcmp("", "") == 0);
    TEST("strcmp prefix", kernel_strcmp("abc", "abcd") < 0);

    TEST("strncmp equal", kernel_strncmp("hello", "hello", 5) == 0);
    TEST("strncmp limited", kernel_strncmp("abcde", "abcde", 3) == 0);
    TEST("strncmp diff", kernel_strncmp("abcde", "abXde", 5) != 0);
    TEST("strncmp zero n", kernel_strncmp("abc", "xyz", 0) == 0);

    {
        char buf[32];
        memset(buf, 0xAA, 32);
        kernel_strcpy(buf, "hello");
        TEST("strcpy", kernel_strcmp(buf, "hello") == 0);
    }

    {
        char buf[32];
        memset(buf, 0xAA, 32);
        kernel_strcpy(buf, "");
        TEST("strcpy empty", kernel_strcmp(buf, "") == 0);
    }

    {
        char buf[32];
        memset(buf, 0xBB, 32);
        kernel_strncpy(buf, "hello", 10);
        TEST("strncpy", kernel_strncmp(buf, "hello", 5) == 0 && buf[5] == 0);
    }

    {
        char buf[32];
        memset(buf, 0xBB, 32);
        kernel_strncpy(buf, "hello", 3);
        TEST("strncpy truncated", kernel_strncmp(buf, "hel", 3) == 0);
    }

    {
        char buf[32] = "hello";
        kernel_strcat(buf, " world");
        TEST("strcat", kernel_strcmp(buf, "hello world") == 0);
    }

    {
        char buf[32] = "";
        kernel_strcat(buf, "test");
        TEST("strcat empty", kernel_strcmp(buf, "test") == 0);
    }

    TEST("atoi positive", kernel_atoi("123") == 123);
    TEST("atoi negative", kernel_atoi("-456") == -456);
    TEST("atoi zero", kernel_atoi("0") == 0);
    TEST("atoi spaces", kernel_atoi("  42") == 42);
    TEST("atoi large", kernel_atoi("2147483647") == 2147483647);
    TEST("atoi positive sign", kernel_atoi("+99") == 99);

    printf("[string] %d/%d passed\n", total - failures, total);
}
