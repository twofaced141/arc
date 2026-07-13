#ifndef _LANGINFO_H
#define _LANGINFO_H

#define CODESET 1
#define D_T_FMT 2
#define D_FMT 3
#define T_FMT 4
#define T_FMT_AMPM 5
#define AM_STR 6
#define PM_STR 7
#define DAY_1 8
#define DAY_2 9
#define DAY_3 10
#define DAY_4 11
#define DAY_5 12
#define DAY_6 13
#define DAY_7 14
#define ABDAY_1 15
#define ABDAY_2 16
#define ABDAY_3 17
#define ABDAY_4 18
#define ABDAY_5 19
#define ABDAY_6 20
#define ABDAY_7 21
#define MON_1 22
#define MON_2 23
#define MON_3 24
#define MON_4 25
#define MON_5 26
#define MON_6 27
#define MON_7 28
#define MON_8 29
#define MON_9 30
#define MON_10 31
#define MON_11 32
#define MON_12 33
#define ABMON_1 34
#define ABMON_2 35
#define ABMON_3 36
#define ABMON_4 37
#define ABMON_5 38
#define ABMON_6 39
#define ABMON_7 40
#define ABMON_8 41
#define ABMON_9 42
#define ABMON_10 43
#define ABMON_11 44
#define ABMON_12 45
#define RADIXCHAR 46
#define THOUSEP 47
#define YESSTR 48
#define NOSTR 49
#define CRNCYSTR 50

char *nl_langinfo(int item);

#endif
