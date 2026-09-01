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
 * wctype.h - Wide character classification and mapping (C11 §7.30)
 * Copyright (c) 2026 OpSys Project
 *
 * Wide counterparts of <ctype.h>.  For 7-bit ASCII the wide
 * classifiers delegate to the narrow ctype functions after
 * range-checking.  Implemented in wctype.c.
 */

#ifndef LIBC_WCTYPE_H
#define LIBC_WCTYPE_H

#include <wchar.h> /* wint_t, WEOF */

/* ====================================================================
 * Types (C11 §7.30.1)
 * ==================================================================== */

typedef int wctype_t;
typedef int wctrans_t;

/* WEOF is provided by <wchar.h>; redefine defensively in case
 * <wchar.h> has not already been included. */
#ifndef WEOF
#define WEOF ((wint_t) - 1)
#endif

/* ====================================================================
 * Classification (C11 §7.30.2)
 * ==================================================================== */

int iswalnum(wint_t wc);
int iswalpha(wint_t wc);
int Iswblank(wint_t wc);
int iswcntrl(wint_t wc);
int iswdigit(wint_t wc);
int iswgraph(wint_t wc);
int iswlower(wint_t wc);
int iswprint(wint_t wc);
int iswpunct(wint_t wc);
int iswspace(wint_t wc);
int iswupper(wint_t wc);
int iswxdigit(wint_t wc);

int      Iswctype(wint_t wc, wctype_t desc);
wctype_t Wctype(const char *property);

/* ====================================================================
 * Case mapping (C11 §7.30.3)
 * ==================================================================== */

wint_t    towlower(wint_t wc);
wint_t    towupper(wint_t wc);
wint_t    Towctrans(wint_t wc, wctrans_t desc);
wctrans_t Wctrans(const char *property);

#endif /* LIBC_WCTYPE_H */
