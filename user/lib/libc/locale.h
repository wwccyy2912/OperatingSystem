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
 * locale.h - Localization (C11 §7.11)
 * Copyright (c) 2026 OpSys Project
 *
 * Provides the LC_* category macros, the lconv structure returned by
 * localeconv(), and the setlocale() function.  v0.1 ships only the "C"
 * locale — setlocale() accepts "C" or "" (the implementation's native
 * locale, also "C") and returns the category name otherwise.
 */

#ifndef LIBC_LOCALE_H
#define LIBC_LOCALE_H

#include <stddef.h> /* NULL */

/* ====================================================================
 * Category macros (C11 §7.11.1)
 *
 * Distinct integer values; the specific numbers are not specified by
 * the standard.
 * ==================================================================== */

#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5

/* ====================================================================
 * struct lconv (C11 §7.11.2.1)
 *
 * Returned by localeconv().  Members of type char * point to strings
 * ("" or the values shown for the "C" locale); char members hold a
 * non-negative value or CHAR_MAX when the information is unavailable.
 * The field order matches C11 §7.11.2.1 paragraph 2.
 * ==================================================================== */

struct lconv {
    /* Non-monetary numeric formatting */
    char *decimal_point; /* decimal point character ("." in C) */
    char *thousands_sep; /* thousands separator ("" in C)     */
    char *grouping;      /* grouping specification ("" in C)   */

    /* Monetary formatting — international currency */
    char *int_curr_symbol; /* 3-letter ISO symbol + separator    */

    /* Monetary formatting — local currency */
    char *currency_symbol;   /* local currency symbol ("")         */
    char *mon_decimal_point; /* monetary decimal point ("")         */
    char *mon_thousands_sep; /* monetary thousands separator ("")  */
    char *mon_grouping;      /* monetary grouping spec ("")         */
    char *positive_sign;     /* non-negative amount sign ("")       */
    char *negative_sign;     /* negative amount sign ("")          */

    /* Digit counts (CHAR_MAX = unavailable) */
    char int_frac_digits; /* international fractional digits     */
    char frac_digits;     /* local fractional digits            */

    /* Symbol position — non-negative amounts */
    char p_cs_precedes;  /* 1 = symbol first, 0 = symbol last  */
    char p_sep_by_space; /* 0/1/2: none/space/other separator  */
    char p_sign_posn;    /* 0..4: sign placement               */

    /* Symbol position — negative amounts */
    char n_cs_precedes;
    char n_sep_by_space;
    char n_sign_posn;
};

/* ====================================================================
 * Functions (C11 §7.11.1.1, §7.11.2.1)
 * ==================================================================== */

/* Set or query the current locale for the given category.  Passing
 * NULL for `locale` queries the current setting without changing it.
 * Returns a pointer to a string identifying the new locale, or NULL
 * on failure. */
char *setlocale(int category, const char *locale);

/* Return a pointer to a struct lconv describing the current locale's
 * numeric and monetary formatting.  The pointed-to object is static
 * and may be overwritten by subsequent calls to localeconv() or
 * setlocale(). */
struct lconv *localeconv(void);

#endif /* LIBC_LOCALE_H */
