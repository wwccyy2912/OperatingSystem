/*
 * ctype.h - Character classification and case conversion
 * Copyright (c) 2026 OpSys Project
 *
 * Standard <ctype.h> subset for 7-bit ASCII.
 * All functions take int (EOF-safe) and return int.
 */

#ifndef LIBC_CTYPE_H
#define LIBC_CTYPE_H

/* --- Character classification --- */
int isalpha(int c);
int isdigit(int c);
int isalnum(int c);
int isxdigit(int c);
int isspace(int c);
int isblank(int c);
int isupper(int c);
int islower(int c);
int isprint(int c);
int isgraph(int c);
int ispunct(int c);
int iscntrl(int c);

/* --- Case conversion --- */
int toupper(int c);
int tolower(int c);

#endif /* LIBC_CTYPE_H */
