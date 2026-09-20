/*
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details: <https://www.gnu.org/licenses/>.
 *
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

/**
 * @brief POSIX strerror_r, GNU flavour.
 *
 * Writes the strerror() message for @p errnum into @p buf (always
 * NUL-terminated, truncated to @p buflen - 1 bytes) and returns a
 * pointer to it.  Unlike the XSI flavour this returns char *, not int
 * — the GNU signature used by glibc-compatible code.  When @p buf is
 * NULL or @p buflen is 0 the pointer to the
 * static table entry is returned instead (never invalidated).
 *
 * @param errnum  Error number (any value; unknown ones yield a
 *                generic message).
 * @param buf     Caller buffer, or NULL.
 * @param buflen  Size of @p buf in bytes.
 * @return Pointer to the (possibly caller-owned) message text.
 */
char *strerror_r(int errnum, char *buf, size_t buflen);

/* ====================================================================
 * Bounded / BSD / GNU string extensions
 * ==================================================================== */

/**
 * @brief Length of @p s, at most @p maxlen.
 * @return The number of bytes before the NUL, capped at @p maxlen.
 */
size_t strnlen(const char *s, size_t maxlen);

/**
 * @brief Duplicate at most @p n bytes of @p s (malloc'ed).
 * @return New string, always NUL-terminated, or NULL when s is NULL or
 *         the allocation fails.
 */
char *strndup(const char *s, size_t n);

/**
 * @brief Split @p *stringp at the first byte from @p delim (BSD).
 *
 * The delimiter byte is overwritten with NUL and @p *stringp is
 * advanced past it; @p *stringp is set to NULL at the end of the
 * string, so subsequent calls return NULL.
 *
 * @return Pointer to the token (possibly empty), or NULL.
 */
char *strsep(char **stringp, const char *delim);

/**
 * @brief Size-bounded string copy (BSD).
 *
 * Copies at most @p size - 1 bytes and always NUL-terminates (unless
 * @p size is 0).
 *
 * @return strlen(src); a result >= @p size means truncation happened.
 */
size_t strlcpy(char *dest, const char *src, size_t size);

/**
 * @brief Size-bounded string concatenation (BSD).
 * @return The length the result would have had (strlen(dest) +
 *         strlen(src)); a result >= @p size means truncation happened.
 */
size_t strlcat(char *dest, const char *src, size_t size);

/* ====================================================================
 * Locale-aware collation (C11 §7.24.4.3 / §7.24.4.5)
 * ==================================================================== */

/* Only the "C" locale exists in v0.9, so these are byte-order
 * operations: strcoll() == strcmp(), strxfrm() == memcpy-with-length. */

int    strcoll(const char *a, const char *b);
size_t strxfrm(char *dest, const char *src, size_t n);

/* ====================================================================
 * Memory operations
 * ==================================================================== */

void *memset(void *dest, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);

/**
 * @brief Find the first occurrence of a byte needle inside a buffer (GNU).
 * @param haystack     Buffer to search.
 * @param haystacklen  Bytes to search.
 * @param needle       Byte pattern to look for.
 * @param needlelen    Length of @p needle in bytes.
 * @return Pointer to the match, or NULL when there is none; an empty
 *         needle matches at the start of @p haystack.
 */
void *memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen);

#endif /* LIBC_STRING_H */
