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
 * inttypes.h - Format conversion of integer types (C11 §7.8)
 * Copyright (c) 2026 OpSys Project
 *
 * Provides format specifier macros for [u]intN_t types (fprintf/scanf
 * family) and the strtoimax/strtoumax/imaxdiv/imaxabs functions.
 * Depends on <stdint.h> (GCC freestanding).
 */

#ifndef LIBC_INTTYPES_H
#define LIBC_INTTYPES_H

#include <stdint.h>

/* ====================================================================
 * fprintf format macros for [u]intN_t
 * ==================================================================== */

/* int8_t / uint8_t */
#define PRId8 "d"
#define PRIi8 "i"
#define PRIo8 "o"
#define PRIu8 "u"
#define PRIx8 "x"
#define PRIX8 "X"
#define SCNd8 "hhd"
#define SCNi8 "hhi"
#define SCNo8 "hho"
#define SCNu8 "hhu"
#define SCNx8 "hhx"

/* int16_t / uint16_t */
#define PRId16 "d"
#define PRIi16 "i"
#define PRIo16 "o"
#define PRIu16 "u"
#define PRIx16 "x"
#define PRIX16 "X"
#define SCNd16 "hd"
#define SCNi16 "hi"
#define SCNo16 "ho"
#define SCNu16 "hu"
#define SCNx16 "hx"

/* int32_t / uint32_t */
#define PRId32 "d"
#define PRIi32 "i"
#define PRIo32 "o"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX32 "X"
#define SCNd32 "ld"
#define SCNi32 "li"
#define SCNo32 "lo"
#define SCNu32 "lu"
#define SCNx32 "lx"

/* int64_t / uint64_t */
#define PRId64 "lld"
#define PRIi64 "lli"
#define PRIo64 "llo"
#define PRIu64 "llu"
#define PRIx64 "llx"
#define PRIX64 "llX"
#define SCNd64 "lld"
#define SCNi64 "lli"
#define SCNo64 "llo"
#define SCNu64 "llu"
#define SCNx64 "llx"

/* intmax_t / uintmax_t */
#define PRIdMAX "lld"
#define PRIiMAX "lli"
#define PRIoMAX "llo"
#define PRIuMAX "llu"
#define PRIxMAX "llx"
#define PRIXMAX "llX"
#define SCNdMAX "lld"
#define SCNiMAX "lli"
#define SCNoMAX "llo"
#define SCNuMAX "llu"
#define SCNxMAX "llx"

/* intptr_t / uintptr_t (matches ptrdiff_t / size_t width on x86-64) */
#define PRIdPTR "ld"
#define PRIiPTR "li"
#define PRIoPTR "lo"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
#define PRIXPTR "lX"
#define SCNdPTR "ld"
#define SCNiPTR "li"
#define SCNoPTR "lo"
#define SCNuPTR "lu"
#define SCNxPTR "lx"

/* ====================================================================
 * Greatest-width integer types
 * ==================================================================== */

typedef int64_t  intmax_t;
typedef uint64_t uintmax_t;

typedef struct {
    intmax_t quot;
    intmax_t rem;
} imaxdiv_t;

/* ====================================================================
 * Functions
 * ==================================================================== */

intmax_t  Imaxabs(intmax_t j);
imaxdiv_t Imaxdiv(intmax_t numer, intmax_t denom);
intmax_t  strtoimax(const char *s, char **endptr, int base);
uintmax_t strtoumax(const char *s, char **endptr, int base);

#endif /* LIBC_INTTYPES_H */
