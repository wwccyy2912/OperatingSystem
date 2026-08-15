/*
 * string.h - Standard string and memory operations
 * Copyright (c) 2026 OpSys Project
 *
 * POSIX string and memory functions.  All implemented in
 * user/lib/libc/string.c  (no kernel involvement).
 */

#ifndef LIBC_STRING_H
#define LIBC_STRING_H

#include <stddef.h>

/* ====================================================================
 * String length / comparison
 * ==================================================================== */

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);

/* ====================================================================
 * String copying / concatenation
 * ==================================================================== */

char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);

/* ====================================================================
 * String searching
 * ==================================================================== */

char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
char  *strpbrk(const char *s, const char *accept);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);

/* ====================================================================
 * String utilities
 * ==================================================================== */

char *strdup(const char *s);                /* calls malloc() */
char *strerror(int errnum);                 /* returns static string */
char *strtok(char *str, const char *delim); /* uses internal state */
char *strtok_r(char *str, const char *delim, char **saveptr);

/* ====================================================================
 * Memory operations
 * ==================================================================== */

void *memset(void *dest, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);

#endif /* LIBC_STRING_H */
