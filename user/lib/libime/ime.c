/*
 * ime.c - Pinyin IME engine (see ime.h)
 * Copyright (c) 2026 OpSys Project
 */

#include "ime.h"
#include "ime_tab.h"
#include "../libc/string.h"
#include "../libc/utf8.h"

int ime_lookup(const char *pinyin, const char **chars_out) {
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
                int len = utf8_seq_len(s);
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

int ime_prefix(const char *pinyin) {
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
