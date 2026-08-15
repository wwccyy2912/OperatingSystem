/*
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
int iswblank(wint_t wc);
int iswcntrl(wint_t wc);
int iswdigit(wint_t wc);
int iswgraph(wint_t wc);
int iswlower(wint_t wc);
int iswprint(wint_t wc);
int iswpunct(wint_t wc);
int iswspace(wint_t wc);
int iswupper(wint_t wc);
int iswxdigit(wint_t wc);

int      iswctype(wint_t wc, wctype_t desc);
wctype_t wctype(const char *property);

/* ====================================================================
 * Case mapping (C11 §7.30.3)
 * ==================================================================== */

wint_t    towlower(wint_t wc);
wint_t    towupper(wint_t wc);
wint_t    towctrans(wint_t wc, wctrans_t desc);
wctrans_t wctrans(const char *property);

#endif /* LIBC_WCTYPE_H */
