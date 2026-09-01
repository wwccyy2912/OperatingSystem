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
 * inttypes.c - Format conversion of integer types (C11 §7.8)
 * Copyright (c) 2026 OpSys Project
 *
 * Implements imaxabs, Imaxdiv, strtoimax, strtoumax.
 * Reuses the parse_int core from stdlib.c via strtoll/strtoull.
 */

#include "inttypes.h"
#include "stdlib.h" /* strtoll, strtoull */

intmax_t Imaxabs(intmax_t j) {
    return j < 0 ? -j : j;
}

imaxdiv_t Imaxdiv(intmax_t numer, intmax_t denom) {
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
