/*
 * ctype.c - Character classification and case conversion
 * Copyright (c) 2026 OpSys Project
 *
 * 7-bit ASCII classification.  Functions accept EOF (-1)
 * and return 0 for false, non-zero for true.
 */

#include "ctype.h"

/* Internal: check if c is a valid 7-bit ASCII char */
#define VALID(c)  ((c) >= 0 && (c) <= 127)

int isalpha(int c)  { return VALID(c) && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')); }
int isdigit(int c)  { return VALID(c) && (c >= '0' && c <= '9'); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return VALID(c) && ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')); }
int isspace(int c)  { return VALID(c) && (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'); }
int isblank(int c)  { return VALID(c) && (c == ' ' || c == '\t'); }
int isupper(int c)  { return VALID(c) && (c >= 'A' && c <= 'Z'); }
int islower(int c)  { return VALID(c) && (c >= 'a' && c <= 'z'); }
int isprint(int c)  { return VALID(c) && c >= ' ' && c <= '~'; }
int isgraph(int c)  { return VALID(c) && c > ' ' && c <= '~'; }
int ispunct(int c)  { return VALID(c) && isgraph(c) && !isalnum(c); }
int iscntrl(int c)  { return VALID(c) && ((c >= 0 && c <= 0x1F) || c == 0x7F); }

int toupper(int c)  { return (islower(c)) ? c - 32 : c; }
int tolower(int c)  { return (isupper(c)) ? c + 32 : c; }
