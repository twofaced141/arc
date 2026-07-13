#ifndef _LIMITS_H
#define _LIMITS_H

#define CHAR_BIT    8
#define SCHAR_MIN   (-128)
#define SCHAR_MAX   127
#define UCHAR_MAX   255
#define CHAR_MIN    (-128)
#define CHAR_MAX    127
#define SHRT_MIN    (-32768)
#define SHRT_MAX    32767
#define USHRT_MAX   65535
#define INT_MIN     (-2147483647 - 1)
#define INT_MAX     2147483647
#define UINT_MAX    4294967295U
#define LONG_MIN    (-2147483647L - 1L)
#define LONG_MAX    2147483647L
#define ULONG_MAX   4294967295UL
#define LLONG_MIN   (-9223372036854775807LL - 1LL)
#define LLONG_MAX   9223372036854775807LL
#define ULLONG_MAX  18446744073709551615ULL

#define ARG_MAX     65536
#define PATH_MAX    4096
#define NAME_MAX    255
#define FILENAME_MAX 4096
#define TMP_MAX     238328
#define HOST_NAME_MAX 64
#define LOGIN_NAME_MAX 256
#define TTY_NAME_MAX 32
#define PIPE_BUF    4096
#define OPEN_MAX    256
#define LINK_MAX    127
#define MAX_CANON   255
#define MAX_INPUT   255
#define NGROUPS_MAX 65536
#define PAGE_SIZE   4096
#define SSIZE_MAX   2147483647
#ifndef SIZE_MAX
#define SIZE_MAX    4294967295U
#endif

#define BUFSIZ      1024
#define L_tmpnam    20
#define TMPDIR_MAX  4096

#endif
