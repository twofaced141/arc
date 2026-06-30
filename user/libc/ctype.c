#include <ctype.h>

int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isxdigit(int c) { return isdigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }
int isprint(int c) { return c >= 0x20 && c <= 0x7E; }
int iscntrl(int c) { return (c >= 0 && c <= 0x1F) || c == 0x7F; }
int ispunct(int c) { return isprint(c) && !isalnum(c) && !isspace(c); }
int isgraph(int c) { return c > 0x20 && c <= 0x7E; }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }
