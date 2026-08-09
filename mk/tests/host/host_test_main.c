#include <stdio.h>

void host_string_tests(void);
void host_bitmap_tests(void);
void host_elf_tests(void);
void host_rcparse_tests(void);
#ifdef HOST_TEST_EXT2
void host_ext2_tests(void);
void host_fs_tests(void);
#endif
#ifdef HOST_TEST_UFS
void host_ufs_tests(void);
#endif

int main(void) {
    printf("=== ARC Host Tests ===\n\n");
    host_string_tests();
    host_bitmap_tests();
    host_elf_tests();
    host_rcparse_tests();
#ifdef HOST_TEST_EXT2
    host_ext2_tests();
    host_fs_tests();
#endif
#ifdef HOST_TEST_UFS
    host_ufs_tests();
#endif
    printf("\n=== All host tests done ===\n");
    return 0;
}
