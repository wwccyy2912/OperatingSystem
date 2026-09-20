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
 * malloc.h - Heap memory allocator
 * Copyright (c) 2026 OpSys Project
 *
 * Simple free-list allocator backed by map_memory syscall.
 * Thread-safety: the free list is guarded by a user-space spinlock
 * (zero-syscall fast path, contention yields to the scheduler), so all
 * entry points may be called from any thread.
 */

#ifndef MALLOC_H
#define MALLOC_H

#include <stddef.h>

void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

/* ---- Heap diagnostics (v0.9) ----
 * The heap is a first-fit allocator over an mmap'ed region whose base
 * is randomized by the kernel (ASLR).  MallocStats() reports the live
 * shape of the heap without allocating; MallocCheck() walks it and
 * verifies the invariants (block chain, sizes, free-list ordering,
 * guard pages untouched).  Both are safe to call at any time from any
 * thread — they take the same lock as malloc. */

typedef struct {
    size_t heap_base;    /* first byte of the heap region      */
    size_t heap_end;     /* one past the last mapped byte      */
    size_t heap_bytes;   /* heap_end - heap_base               */
    size_t used_bytes;   /* bytes inside live blocks (payload) */
    size_t free_bytes;   /* bytes inside free blocks (payload) */
    size_t blocks_live;  /* number of live allocations         */
    size_t blocks_free;  /* number of free blocks on the list  */
    size_t largest_free; /* largest single free payload        */
    size_t overhead;     /* per-block headers + alignment loss */
    size_t grow_calls;   /* times the heap had to grow         */
    size_t grow_bytes;   /* bytes added by heap growth         */
    size_t fail_count;   /* allocations that returned NULL     */
    size_t peak_used;    /* high-water mark of used_bytes      */
} malloc_stats_t;

/* Fill *out with the heap's current shape.  No-op for a NULL pointer. */
void MallocStats(malloc_stats_t *out);

/* Verify heap invariants.  Returns 0 when the heap is consistent, or a
 * positive count of detected problems (details go to the debug log);
 * -1 when the heap has not been initialised yet. */
int MallocCheck(void);

/* Usable payload size of an allocation (>= the requested size). */
size_t MallocUsableSize(void *ptr);

/* ---- Aligned allocation (v0.9) ----
 * aligned_alloc() (C11) and posix_memalign() (POSIX) are implemented
 * by this heap; their declarations live in <stdlib.h>, where the
 * standard requires them.  A request whose alignment exceeds the
 * heap's natural MALLOC_ALIGN (16) is served from an ordinary block
 * that carries a back-pointer below the returned address, so free(),
 * realloc() and MallocUsableSize() accept the pointer exactly like a
 * plain malloc() result.  posix_memalign() reports EINVAL for an
 * alignment that is not a power of two and a multiple of
 * sizeof(void *), and ENOMEM through its return value (never errno);
 * aligned_alloc() sets errno on failure. */

#endif /* MALLOC_H */
