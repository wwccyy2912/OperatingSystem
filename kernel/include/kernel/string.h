/*
 * string.h - Kernel string/memory utilities
 * Copyright (c) 2026 OpSys Project
 *
 * Freestanding kernel cannot use <string.h> from any libc.
 * Provide minimal memset/memcpy/memmove as static inlines.
 */

#ifndef KERNEL_STRING_H
#define KERNEL_STRING_H

#include <kernel/types.h>

static inline void *memset(void *dst, int c, size_t n) {
    u8 *d   = (u8 *)dst;
    u8  val = (u8)c;
    for (size_t i = 0; i < n; i++)
        d[i] = val;
    return dst;
}

static inline void *memcpy(void *dst, const void *src, size_t n) {
    u8       *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    /* Hot path (blob loads, IPC buffers, page tables): align both
     * pointers to 8 bytes with a byte prefix, then copy qwords, then
     * the tail.  The former byte-at-a-time loop made large copies (ELF
     * blobs up to ~100 KB) run at ~1/8 of memory throughput. */
    while (((uptr)d & 7) && n > 0) {
        *d++ = *s++;
        n--;
    }
    if (n >= 8) {
        u64       *dq = (u64 *)d;
        const u64 *sq = (const u64 *)s;
        do {
            *dq++ = *sq++;
            n -= 8;
        } while (n >= 8);
        d = (u8 *)dq;
        s = (const u8 *)sq;
    }
    while (n-- > 0)
        *d++ = *s++;
    return dst;
}

static inline void *memmove(void *dst, const void *src, size_t n) {
    u8       *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
    return dst;
}

static inline int memcmp(const void *a, const void *b, size_t n) {
    const u8 *pa = (const u8 *)a;
    const u8 *pb = (const u8 *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

static inline size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len])
        len++;
    return len;
}

#endif /* KERNEL_STRING_H */
