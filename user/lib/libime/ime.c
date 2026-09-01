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
 * ime.c - Pinyin IME engine (see ime.h)
 * Copyright (c) 2026 OpSys Project
 *
 * ------------------------------------------------------------------
 * Structure (lookup):
 *   ImeLookup(pinyin) -> binary search ime_tab[] -> UTF-8 candidates
 *   ImePrefix(pinyin) -> lower-bound search -> prefix match check
 * How it works:
 *   Both functions rely on ime_tab being sorted by pinyin.  ImeLookup
 *   finds an exact match and splits the candidate string on UTF-8
 *   sequence boundaries into chars_out (up to IME_MAX_CAND).  ImePrefix
 *   locates the first entry >= pinyin and tests whether it starts with it.
 * Purpose:
 *   Pinyin input-method candidate lookup and prefix completion testing
 *   for the IME service.
 * Caveats:
 *   The table must stay pinyin-sorted or the binary searches are wrong.
 *   ImePrefix checks only the first entry >= prefix, not the whole range;
 *   malformed UTF-8 in a candidate stops decoding.
 * ------------------------------------------------------------------
 */

#include "ime.h"
#include "ime_tab.h"
#include "../libc/string.h"
#include "../libc/utf8.h"

int ImeLookup(const char *pinyin, const char **chars_out) {
    if (!pinyin || !chars_out)
        return 0;

    /* Binary search over the pinyin-sorted table. */
    int lo = 0, hi = IME_TAB_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c   = strcmp(pinyin, ime_tab[mid].py);
        if (c == 0) {
            const char *s = ime_tab[mid].chars;
            int         n = 0;
            while (*s && n < IME_MAX_CAND) {
                int len = Utf8SeqLen(s);
                if (len <= 0)
                    break;
                chars_out[n++] = s;
                s += len;
            }
            return n;
        }
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return 0;
}

int ImePrefix(const char *pinyin) {
    if (!pinyin || pinyin[0] == '\0')
        return 0;

    /* Binary search for the first entry >= pinyin, then check whether
     * that entry (or, when equal, the next one) starts with pinyin. */
    int lo = 0, hi = IME_TAB_COUNT - 1;
    int first = IME_TAB_COUNT; /* first index with entry >= pinyin */
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c   = strcmp(pinyin, ime_tab[mid].py);
        if (c <= 0) {
            first = mid;
            hi    = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    if (first >= IME_TAB_COUNT)
        return 0;
    if (strncmp(ime_tab[first].py, pinyin, strlen(pinyin)) == 0)
        return 1;
    return 0;
}
