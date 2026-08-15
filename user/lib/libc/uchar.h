/*
 * uchar.h - Unicode utilities (C11 §7.28)
 * Copyright (c) 2026 OpSys Project
 *
 * 16/32-bit character types and the multibyte <-> char16_t/char32_t
 * conversion helpers.  mbstate_t is shared with <wchar.h>.
 */

#ifndef LIBC_UCHAR_H
#define LIBC_UCHAR_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>      /* mbstate_t */

/* ====================================================================
 * Limits
 * ==================================================================== */

/* Maximum number of bytes in a multibyte character, any locale. */
#ifndef MB_LEN_MAX
#define MB_LEN_MAX  4
#endif

/* ====================================================================
 * Character types (C11 §7.28.1)
 * ==================================================================== */

typedef uint_least16_t  char16_t;
typedef uint_least32_t  char32_t;

/* ====================================================================
 * Conversion functions (C11 §7.28.2)
 * ==================================================================== */

size_t  mbrtoc16(char16_t *pc16, const char *s, size_t n, mbstate_t *ps);
size_t  c16rtomb(char *s, char16_t c16, mbstate_t *ps);

size_t  mbrtoc32(char32_t *pc32, const char *s, size_t n, mbstate_t *ps);
size_t  c32rtomb(char *s, char32_t c32, mbstate_t *ps);

#endif /* LIBC_UCHAR_H */
