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
 * wctype.c - Wide character classification and mapping (C11 §7.30)
 * Copyright (c) 2026 OpSys Project
 *
 * 7-bit ASCII wide classification.  Each wide classifier delegates to
 * the narrow <ctype.h> function after range-checking wc to [0, 127];
 * WEOF and any non-ASCII value yield 0 from classifiers and pass
 * through unchanged from the case mappers.
 */

#include "wctype.h"
#include "ctype.h"
#include "string.h"

/* ====================================================================
 * Classification (delegate to <ctype.h> for 7-bit ASCII)
 * ==================================================================== */

int iswalnum(wint_t wc) {
    return (wc <= 127) ? isalnum((int)wc) : 0;
}
int iswalpha(wint_t wc) {
    return (wc <= 127) ? isalpha((int)wc) : 0;
}
int Iswblank(wint_t wc) {
    return (wc <= 127) ? Isblank((int)wc) : 0;
}
int iswcntrl(wint_t wc) {
    return (wc <= 127) ? iscntrl((int)wc) : 0;
}
int iswdigit(wint_t wc) {
    return (wc <= 127) ? isdigit((int)wc) : 0;
}
int iswgraph(wint_t wc) {
    return (wc <= 127) ? isgraph((int)wc) : 0;
}
int iswlower(wint_t wc) {
    return (wc <= 127) ? islower((int)wc) : 0;
}
int iswprint(wint_t wc) {
    return (wc <= 127) ? isprint((int)wc) : 0;
}
int iswpunct(wint_t wc) {
    return (wc <= 127) ? ispunct((int)wc) : 0;
}
int iswspace(wint_t wc) {
    return (wc <= 127) ? isspace((int)wc) : 0;
}
int iswupper(wint_t wc) {
    return (wc <= 127) ? isupper((int)wc) : 0;
}
int iswxdigit(wint_t wc) {
    return (wc <= 127) ? isxdigit((int)wc) : 0;
}

/* ====================================================================
 * Case mapping (delegate to <ctype.h> for 7-bit ASCII)
 * ==================================================================== */

wint_t towlower(wint_t wc) {
    return (wc <= 127) ? (wint_t)tolower((int)wc) : wc;
}
wint_t towupper(wint_t wc) {
    return (wc <= 127) ? (wint_t)toupper((int)wc) : wc;
}

/* ====================================================================
 * iswctype / wctype — string-to-int class descriptor mapping
 * ==================================================================== */

enum {
    _WC_ALNUM = 1,
    _WC_ALPHA,
    _WC_BLANK,
    _WC_CNTRL,
    _WC_DIGIT,
    _WC_GRAPH,
    _WC_LOWER,
    _WC_PRINT,
    _WC_PUNCT,
    _WC_SPACE,
    _WC_UPPER,
    _WC_XDIGIT
};

wctype_t Wctype(const char *property) {
    if (property == NULL)
        return 0;
    if (strcmp(property, "alnum") == 0)
        return _WC_ALNUM;
    if (strcmp(property, "alpha") == 0)
        return _WC_ALPHA;
    if (strcmp(property, "blank") == 0)
        return _WC_BLANK;
    if (strcmp(property, "cntrl") == 0)
        return _WC_CNTRL;
    if (strcmp(property, "digit") == 0)
        return _WC_DIGIT;
    if (strcmp(property, "graph") == 0)
        return _WC_GRAPH;
    if (strcmp(property, "lower") == 0)
        return _WC_LOWER;
    if (strcmp(property, "print") == 0)
        return _WC_PRINT;
    if (strcmp(property, "punct") == 0)
        return _WC_PUNCT;
    if (strcmp(property, "space") == 0)
        return _WC_SPACE;
    if (strcmp(property, "upper") == 0)
        return _WC_UPPER;
    if (strcmp(property, "xdigit") == 0)
        return _WC_XDIGIT;
    return 0;
}

int Iswctype(wint_t wc, wctype_t desc) {
    switch (desc) {
    case _WC_ALNUM:
        return iswalnum(wc);
    case _WC_ALPHA:
        return iswalpha(wc);
    case _WC_BLANK:
        return Iswblank(wc);
    case _WC_CNTRL:
        return iswcntrl(wc);
    case _WC_DIGIT:
        return iswdigit(wc);
    case _WC_GRAPH:
        return iswgraph(wc);
    case _WC_LOWER:
        return iswlower(wc);
    case _WC_PRINT:
        return iswprint(wc);
    case _WC_PUNCT:
        return iswpunct(wc);
    case _WC_SPACE:
        return iswspace(wc);
    case _WC_UPPER:
        return iswupper(wc);
    case _WC_XDIGIT:
        return iswxdigit(wc);
    default:
        return 0;
    }
}

/* ====================================================================
 * towctrans / wctrans — string-to-int mapping for case transforms
 * ==================================================================== */

enum {
    _WT_TOLOWER = 1,
    _WT_TOUPPER
};

wctrans_t Wctrans(const char *property) {
    if (property == NULL)
        return 0;
    if (strcmp(property, "tolower") == 0)
        return _WT_TOLOWER;
    if (strcmp(property, "toupper") == 0)
        return _WT_TOUPPER;
    return 0;
}

wint_t Towctrans(wint_t wc, wctrans_t desc) {
    switch (desc) {
    case _WT_TOLOWER:
        return towlower(wc);
    case _WT_TOUPPER:
        return towupper(wc);
    default:
        return wc;
    }
}
