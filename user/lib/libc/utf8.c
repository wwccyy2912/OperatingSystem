/*
 * utf8.c - UTF-8 helpers (see utf8.h)
 * Copyright (c) 2026 OpSys Project
 
 *
 * ------------------------------------------------------------------
 * Structure (utf8):
 *   Utf8SeqLen/Utf8Decode (1..4-byte decode), Utf8CharWidth (wcwidth
 *   approximation: 0/1/2 columns), Utf8Prev/Next/Advance/StrWidth.
 * How it works:
 *   Lead-byte length table + continuation checks; width uses CJK
 *   range tests; Prev walks back over continuation bytes.
 * Purpose:
 *   Character-boundary-safe byte-string editing everywhere (shell
 *   line editor, cat, VFS names, term rendering).
 * Caveats:
 *   No normalization (NFC/NFD); malformed sequences degrade to
 *   single-byte passthrough rather than failing hard.
 * ------------------------------------------------------------------
 */

#include "utf8.h"

int Utf8SeqLen(const char *s) {
    if (!s || !s[0])
        return 0;
    unsigned char b = (unsigned char)s[0];
    if (b < 0x80)
        return 1;
    if (b >= 0xC2 && b <= 0xDF)
        return 2;
    if (b >= 0xE0 && b <= 0xEF)
        return 3;
    if (b >= 0xF0 && b <= 0xF4)
        return 4;
    return 0; /* stray continuation or invalid lead */
}

int Utf8Decode(const char *s, uint32_t *cp) {
    if (!s)
        return 0;
    unsigned char b = (unsigned char)s[0];
    uint32_t c;
    int n = Utf8SeqLen(s);
    if (n == 0)
        return 0;
    switch (n) {
    case 1:
        c = b;
        break;
    case 2:
        if (((unsigned char)s[1] & 0xC0) != 0x80)
            return 0;
        c = ((uint32_t)(b & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
        break;
    case 3:
        if (((unsigned char)s[1] & 0xC0) != 0x80 ||
            ((unsigned char)s[2] & 0xC0) != 0x80)
            return 0;
        c = ((uint32_t)(b & 0x0F) << 12) |
            ((uint32_t)((unsigned char)s[1] & 0x3F) << 6) |
            ((unsigned char)s[2] & 0x3F);
        break;
    default: /* 4 */
        if (((unsigned char)s[1] & 0xC0) != 0x80 ||
            ((unsigned char)s[2] & 0xC0) != 0x80 ||
            ((unsigned char)s[3] & 0xC0) != 0x80)
            return 0;
        c = ((uint32_t)(b & 0x07) << 18) |
            ((uint32_t)((unsigned char)s[1] & 0x3F) << 12) |
            ((uint32_t)((unsigned char)s[2] & 0x3F) << 6) |
            ((unsigned char)s[3] & 0x3F);
        break;
    }
    if (cp)
        *cp = c;
    return n;
}

int Utf8CharWidth(uint32_t cp) {
    /* Control / format characters: no column. */
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
        return 0;
    /* Combining diacritics: zero width. */
    if ((cp >= 0x0300 && cp <= 0x036F) ||
        (cp >= 0x1AB0 && cp <= 0x1AFF) ||
        (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0xFE00 && cp <= 0xFE0F))
        return 0;
    /* CJK wide ranges. */
    if (cp >= 0x1100 && cp <= 0x115F) /* Hangul Jamo */
        return 2;
    if (cp >= 0x2E80 && cp <= 0x303E) /* CJK Radicals..CJK Symbols */
        return 2;
    if (cp >= 0x3041 && cp <= 0x33FF) /* Hiragana..CJK Compatibility */
        return 2;
    if (cp >= 0x3400 && cp <= 0x4DBF) /* CJK Ext A */
        return 2;
    if (cp >= 0x4E00 && cp <= 0x9FFF) /* CJK Unified */
        return 2;
    if (cp >= 0xA000 && cp <= 0xA4CF) /* Yi */
        return 2;
    if (cp >= 0xAC00 && cp <= 0xD7A3) /* Hangul Syllables */
        return 2;
    if (cp >= 0xF900 && cp <= 0xFAFF) /* CJK Compat Ideographs */
        return 2;
    if (cp >= 0xFE30 && cp <= 0xFE4F) /* CJK Compatibility Forms */
        return 2;
    if (cp >= 0xFF00 && cp <= 0xFF60) /* Fullwidth Forms */
        return 2;
    if (cp >= 0xFFE0 && cp <= 0xFFE6)
        return 2;
    if (cp >= 0x20000 && cp <= 0x2FFFD) /* CJK Ext B.. */
        return 2;
    if (cp >= 0x30000 && cp <= 0x3FFFD)
        return 2;
    return 1;
}

int Utf8Prev(const char *s, int pos) {
    if (pos <= 0)
        return 0;
    /* Walk back over continuation bytes to the lead. */
    int i = pos;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80)
        i--;
    return i > 0 ? i - 1 : 0;
}

int Utf8Next(const char *s, int pos, int len) {
    if (pos >= len)
        return len;
    int n = Utf8SeqLen(s + pos);
    if (n <= 0)
        n = 1;
    int p = pos + n;
    return p > len ? len : p;
}

int Utf8Advance(const char *s, int pos, int len, int delta) {
    if (delta < 0) {
        while (delta++ < 0 && pos > 0)
            pos = Utf8Prev(s, pos);
    } else {
        while (delta-- > 0 && pos < len)
            pos = Utf8Next(s, pos, len);
    }
    return pos;
}

int Utf8StrWidth(const char *s, int len) {
    int w = 0;
    int pos = 0;
    while (pos < len) {
        uint32_t cp;
        int n = Utf8Decode(s + pos, &cp);
        if (n <= 0) {
            w += 1; /* stray byte: count as one narrow column */
            pos++;
            continue;
        }
        w += Utf8CharWidth(cp);
        pos += n;
    }
    return w;
}
