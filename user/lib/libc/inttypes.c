/*
 * inttypes.c - Format conversion of integer types (C11 §7.8)
 * Copyright (c) 2026 OpSys Project
 *
 * Implements imaxabs, imaxdiv, strtoimax, strtoumax.
 * Reuses the parse_int core from stdlib.c via strtoll/strtoull.
 */

#include "inttypes.h"
#include "stdlib.h" /* strtoll, strtoull */

intmax_t imaxabs(intmax_t j) {
    return j < 0 ? -j : j;
}

imaxdiv_t imaxdiv(intmax_t numer, intmax_t denom) {
    imaxdiv_t r;
    r.quot = numer / denom;
    r.rem  = numer % denom;
    return r;
}

intmax_t strtoimax(const char *s, char **endptr, int base) {
    return (intmax_t)strtoll(s, endptr, base);
}

uintmax_t strtoumax(const char *s, char **endptr, int base) {
    return (uintmax_t)strtoull(s, endptr, base);
}
