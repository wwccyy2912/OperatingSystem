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
 * malloc.c - Heap memory allocator
 * Copyright (c) 2026 OpSys Project
 *
 * Simple first-fit free-list allocator backed by the map_memory syscall.
 *
 * Design:
 *   - All allocations come from a contiguous virtual region starting at
 *     the process's randomized heap base (ASLR — fetched from the kernel
 *     via GetHeapBase() on first grow; defaults to 0x70000000),
 *     growing upward as needed.
 *   - The free list tracks blocks not currently in use.
 *   - Allocation: first-fit search; splits blocks when the remainder
 *     is large enough (>= MALLOC_MIN_SIZE).
 *   - Free: returns block to free list; coalesces with adjacent free
 *     blocks (by address following the next pointer).
 *  - When the free list is exhausted, grows the heap via map_memory:
 *    at least CHUNK_SIZE (64 KB), doubling until it covers the request
 *    so single allocations larger than one chunk (e.g. file data blocks
 *    past 64 KiB) can be satisfied and grow in place via realloc.
 *
 * Thread-safety: the free list is guarded by a user-space spinlock
 * (user/lib/libos/spinlock.h) — uncontended malloc/free are zero-syscall
 * (P0 fast-path, see docs/kernel_roadmap.md).  malloc/free/calloc/realloc
 * may be called from any thread.
 
 *
 * ------------------------------------------------------------------
 * Structure (malloc):
 *   ASLR heap base (GetHeapBase) -> contiguous mapped region ->
 *   size-linked block chain (size|FREE header) -> global first-fit
 *   free list + per-size-class bins -> Malloc/Free/Calloc/Realloc ->
 *   MallocStats/MallocCheck/MallocUsableSize plus aligned_alloc/
 *   posix_memalign (over-aligned blocks carry a back-pointer).
 * How it works:
 *   Blocks are carved out of 64 KB chunks mapped contiguously upward
 *   from the randomized heap base; adjacent free blocks coalesce along
 *   the address chain.  Live counters are maintained on every mutation
 *   and MallocCheck() re-derives them from a full walk of the heap.
 * Purpose:
 *   Userspace heap so services need no kernel allocator calls, plus a
 *   self-describing heap shape for the shell's diagnostics commands.
 * Caveats:
 *   - The heap spinlock is NOT recursive: MallocStats()/MallocCheck()
 *     take it, and printf() may allocate, so the check collects its
 *     findings under the lock and prints them after releasing it.
 *   - Over-aligned blocks (aligned_alloc/posix_memalign) waste up to
 *     one alignment of payload and their usable size is reduced by the
 *     alignment slack; free()/realloc() understand the back-pointer
 *     they carry (free() on a foreign pointer is still undefined).
 *   - Diagnostics walk the whole heap; call them from tests/shell
 *     commands, not from a hot loop.
 * ------------------------------------------------------------------
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <malloc.h>
#include <libos/syscalls.h>
#include <libos/spinlock.h>
#include <libc/string.h> /* memcpy, memset */
#include <stdio.h>       /* printf: MallocCheck() problem log (after unlock) */

/* ====================================================================
 * Constants
 * ==================================================================== */

/* Heap virtual address range.
 *
 * ASLR (design item ⑭): the kernel randomizes the heap base per process
 * (process_t.heap_base) and the region size is fixed.  These values are
 * the fallback defaults; the real base is fetched from the kernel via
 * GetHeapBase() on the first heap grow (HeapGrow() below) so the
 * user heap matches the kernel's randomized layout.
 * HEAP_USER_SIZE must match kernel/include/kernel/vmm.h (HEAP_USER_SIZE). */
#define HEAP_BASE_DEFAULT 0x70000000ULL
#define HEAP_USER_SIZE    0x10000000ULL /* 256 MB region */

/* Grow the heap in 64 KB chunks */
#define CHUNK_SIZE (64ULL * 1024)

/* Minimum block size (including header) — avoids fragmentation thrash */
#define MALLOC_MIN_SIZE 64

/* Alignment for all user payloads */
#define MALLOC_ALIGN 16

/* ====================================================================
 * Block header
 * ==================================================================== */

struct block;

typedef struct block {
    size_t        size; /* Total block size incl. header; bit 0 = FREE flag */
    struct block *next; /* Next block in free list (valid only when free) */
} block_t;

#define BLOCK_HDR_SZ sizeof(block_t)

/* Size-rounding: align payload up to MALLOC_ALIGN */
#define ROUND_UP(n) (((n) + MALLOC_ALIGN - 1) & ~(size_t)(MALLOC_ALIGN - 1))

/* Size-field helpers */
#define BLOCK_SIZE(b) ((b)->size & ~(size_t)1)
#define IS_FREE(b)    (((b)->size & 1) != 0)
#define MARK_FREE(b)  ((b)->size |= 1)
#define MARK_USED(b)  ((b)->size &= ~(size_t)1)

/* ====================================================================
 * State
 * ==================================================================== */

/* Head of the singly-linked free list (large blocks / overflow) */
static block_t *s_free_list = NULL;

/* ---- Size-class bins (v0.5: dynamic-memory optimization) ----
 *
 * Small allocations (<= BIN_MAX) are served from segregated per-size
 * free lists (tcache style): malloc takes a block in O(1), free puts
 * the block back in O(1).  A bin holds at most BIN_CAP blocks; when it
 * is full, further frees overflow to the global first-fit list, which
 * keeps coalescing working.  When a bin is empty, malloc falls back to
 * the global first-fit path (split + heap_grow as before), so the
 * bins are purely a fast path and never change allocation semantics.
 *
 * Block layout, header flags and the realloc in-place logic are
 * unchanged: bins only reorganize WHERE free blocks are parked.
 */

/* Bin sizes: 16, 32, 64, ..., 2048 (2^4 .. 2^11). */
#define BIN_MIN_SHIFT 4
#define BIN_COUNT     8
#define BIN_MAX       (1u << (BIN_MIN_SHIFT + BIN_COUNT - 1)) /* 2048 */

/* Max blocks parked per bin before overflowing to the global list. */
#define BIN_CAP 6

static block_t *s_bins[BIN_COUNT];

/* Bin index for a payload size, or -1 when the request is too large. */
static int BinIndex(size_t size) {
    if (size > BIN_MAX)
        return -1;
    size_t asize = BLOCK_HDR_SZ + ROUND_UP(size);
    if (asize < MALLOC_MIN_SIZE)
        asize = MALLOC_MIN_SIZE;
    /* Find the smallest bin whose capacity covers asize.  The upper
     * bound is BIN_COUNT - 1 (not BIN_COUNT): asize includes the 16-byte
     * header, so a payload of BIN_MAX (2048) yields asize = 2064, which
     * would otherwise compute shift = 12 and return index 8 — an
     * out-of-bounds s_bins[] access.  Clamping parks such blocks in the
     * largest bin, which is safe (it can serve any smaller request). */
    unsigned shift = BIN_MIN_SHIFT;
    while ((1u << shift) < asize && shift < BIN_MIN_SHIFT + BIN_COUNT - 1)
        shift++;
    return (int)(shift - BIN_MIN_SHIFT);
}

/* Next virtual address to request from kernel when heap grows.
 * Defaults to HEAP_BASE_DEFAULT until the kernel's per-process
 * randomized base is fetched (see heap_grow). */
static uint64_t s_next_virt = HEAP_BASE_DEFAULT;

/* First mapped heap address (set on the first heap_grow).  Heap chunks
 * are mapped contiguously, so [s_heap_base, s_next_virt) is exactly the
 * mapped region — block_is_free uses it to avoid dereferencing
 * by-address candidates past the heap end. */
static uintptr_t s_heap_base = 0;

/* Heap region end = base + HEAP_USER_SIZE (updated with the base). */
static uint64_t s_heap_max = HEAP_BASE_DEFAULT + HEAP_USER_SIZE;

/* Whether the kernel-provided heap base has been fetched yet. */
static bool s_heap_layout_loaded = false;

/* User-space spinlock guarding the free list (P0: zero-syscall
 * fast-path).  Contention yields to the scheduler; the kernel mutex is
 * no longer used for the heap. */
static user_spinlock_t s_heap_lock = 0;

/* ====================================================================
 * Live heap counters
 *
 * Maintained by every allocator mutation (all under s_heap_lock) and
 * re-derived by MallocCheck() from a walk of the block chain, so a
 * drift between the two is reported as a heap problem.  Payload always
 * means the whole block payload (block size minus the header), which is
 * >= the size the caller asked for — the requested size is not stored
 * anywhere (the header layout is frozen at two words).
 * ==================================================================== */

static struct {
    size_t live_blocks; /* blocks handed out to callers            */
    size_t live_bytes;  /* payload bytes inside those blocks       */
    size_t free_blocks; /* blocks parked on the free list or bins  */
    size_t free_bytes;  /* payload bytes inside those blocks       */
    size_t grow_calls;  /* successful map_memory heap growths      */
    size_t grow_bytes;  /* bytes mapped by those growths           */
    size_t fail_count;  /* allocations that had to return NULL     */
    size_t peak_used;   /* high-water mark of live_bytes           */
} s_stat;

/* Payload of a block of total size blocksz. */
static size_t PayloadOf(size_t blocksz) {
    return blocksz > BLOCK_HDR_SZ ? blocksz - BLOCK_HDR_SZ : 0;
}

static void StatAddLive(size_t blocksz) {
    s_stat.live_blocks++;
    s_stat.live_bytes += PayloadOf(blocksz);
    if (s_stat.live_bytes > s_stat.peak_used)
        s_stat.peak_used = s_stat.live_bytes;
}

static void StatDropLive(size_t blocksz) {
    size_t payload = PayloadOf(blocksz);
    if (s_stat.live_blocks > 0)
        s_stat.live_blocks--;
    s_stat.live_bytes = s_stat.live_bytes >= payload ? s_stat.live_bytes - payload : 0;
}

static void StatAddFree(size_t blocksz) {
    s_stat.free_blocks++;
    s_stat.free_bytes += PayloadOf(blocksz);
}

static void StatDropFree(size_t blocksz) {
    size_t payload = PayloadOf(blocksz);
    if (s_stat.free_blocks > 0)
        s_stat.free_blocks--;
    s_stat.free_bytes = s_stat.free_bytes >= payload ? s_stat.free_bytes - payload : 0;
}

/* Count an allocation failure on a path that does not go through
 * malloc_locked() (e.g. a calloc size overflow). */
static void StatFail(void) {
    SpinLock(&s_heap_lock);
    s_stat.fail_count++;
    SpinUnlock(&s_heap_lock);
}

/* A live block changed size in place (realloc growth): the block count
 * is unchanged, only its payload is. */
static void StatResizeLive(size_t old_payload, size_t new_payload) {
    s_stat.live_bytes = s_stat.live_bytes >= old_payload ? s_stat.live_bytes - old_payload : 0;
    s_stat.live_bytes += new_payload;
    if (s_stat.live_bytes > s_stat.peak_used)
        s_stat.peak_used = s_stat.live_bytes;
}

/* ====================================================================
 * Over-aligned blocks (aligned_alloc / posix_memalign)
 *
 * The two-word block header is frozen (size|FREE, next), so an
 * over-aligned payload cannot carry a header of its own.  Instead the
 * words immediately below the returned pointer are borrowed from the
 * underlying block's payload:
 *
 *     [ptr - 16] = MALLOC_ALIGN_TAG  (a word no block header can hold)
 *     [ptr -  8] = payload of the underlying block (the malloc result)
 *
 * free()/realloc()/MallocUsableSize() detect the tag and recover the
 * real block through the back-pointer.  Ordinary blocks never carry a
 * tag: a block size is always >= MALLOC_MIN_SIZE and a multiple of
 * MALLOC_ALIGN, so the value 1 can never be a valid size field. */
#define MALLOC_ALIGN_TAG ((size_t)1)

/* True when ptr came back from aligned_alloc()/posix_memalign() with
 * an alignment greater than MALLOC_ALIGN. */
static int PayloadIsAligned(void *ptr) {
    const block_t *b = (const block_t *)((const char *)ptr - BLOCK_HDR_SZ);
    return b->size == MALLOC_ALIGN_TAG;
}

/* Block owning a payload pointer, resolving the over-aligned case. */
static block_t *BlockOfPayload(void *ptr) {
    block_t *b = (block_t *)((char *)ptr - BLOCK_HDR_SZ);
    if (b->size == MALLOC_ALIGN_TAG)
        b = (block_t *)((char *)((void **)ptr)[-1] - BLOCK_HDR_SZ);
    return b;
}

/* Usable payload bytes starting at payload inside block b. */
static size_t UsableSizeOf(const block_t *b, const void *payload) {
    size_t    total = PayloadOf(BLOCK_SIZE(b));
    uintptr_t start = (uintptr_t)b + BLOCK_HDR_SZ;
    uintptr_t p     = (uintptr_t)payload;
    return (p >= start && p - start < total) ? total - (p - start) : 0;
}

/* ====================================================================
 * Internal helpers
 * ==================================================================== */

/* Add a contiguous chunk of memory to the free list (insert at head). */
static void HeapAddChunk(void *addr, size_t size) {
    block_t *block = (block_t *)addr;
    block->size    = size | 1; /* mark free */
    block->next    = s_free_list;
    s_free_list    = block;
}

/* Request more heap from the kernel via map_memory.  Grows by at least
 * `need` bytes: one CHUNK_SIZE minimum, doubling until it covers the
 * request.  This lets a single allocation larger than one chunk (e.g. a
 * file data block growing past 64 KiB) be satisfied, and gives the block
 * headroom to grow in place afterwards (see realloc). */
static int HeapGrow(size_t need) {
    /* Fetch the kernel's per-process randomized heap base once (ASLR,
     * design item ⑭).  Until the first grow the compile-time default is
     * unused (no heap blocks exist yet), so lazy fetching is safe. */
    if (!s_heap_layout_loaded) {
        uint64_t hb = (uint64_t)GetHeapBase();
        if (hb != 0) {
            s_next_virt = hb;
            s_heap_max  = hb + HEAP_USER_SIZE;
        }
        s_heap_layout_loaded = true;
    }

    if (s_next_virt >= s_heap_max)
        return -1;

    /* Chunk size: at least CHUNK_SIZE, doubling until it covers `need`. */
    uint64_t chunk = CHUNK_SIZE;
    while (chunk < (uint64_t)need && chunk < HEAP_USER_SIZE / 2)
        chunk *= 2;
    if (chunk < (uint64_t)need)
        chunk = HEAP_USER_SIZE;

    uint64_t room = s_heap_max - s_next_virt;
    if (chunk > room)
        chunk = room;
    if (chunk < (uint64_t)need)
        return -1; /* heap region too small for this request */

    /* Create a memory capability and map it */
    int cap = CapCreate(CAP_TYPE_MEM, RIGHT_WRITE);
    if (cap < 0)
        return -1;

    /* map_memory returns the virtual address on success, 0 on failure.
     * The second argument (offset) is the desired virtual address. */
    void *addr = map_memory(cap, s_next_virt, chunk, PROT_READ | PROT_WRITE);
    if (!addr) {
        CapRevoke(cap);
        return -1;
    }

    /* Capability is no longer needed — the mapping persists in the page table */
    CapRevoke(cap);

    if (s_heap_base == 0)
        s_heap_base = (uintptr_t)addr; /* first chunk: the heap floor */
    HeapAddChunk(addr, chunk);
    s_next_virt += chunk;

    s_stat.grow_calls++;
    s_stat.grow_bytes += chunk;
    StatAddFree(chunk); /* the new chunk starts life as one free block */
    return 0;
}

/* Forward declarations: bin/block helpers used by coalesce_after and
 * the _locked allocators below. */
static int  BinCount(int idx);
static int  BlockIsFree(block_t *blk);
static int  BlockUnlink(block_t *blk);

/* Coalesce adjacent free blocks.
 * After marking a block free, check if the block that immediately follows
 * it in address space is also free — if so, merge them. */
static void CoalesceAfter(block_t *block) {
    size_t   block_sz = BLOCK_SIZE(block);
    block_t *next     = (block_t *)((char *)block + block_sz);

    /* The following block may be parked in the global list OR in a
     * size-class bin.  block_is_free/BlockUnlink(defined below)
     * cover both. */
    if (BlockIsFree(next)) {
        size_t next_sz = BLOCK_SIZE(next);

        (void)BlockUnlink(next);
        block->size = (block_sz + next_sz) | 1;

        /* Two free blocks become one.  The bytes do not leave the free
         * pool: the merged payload is the sum of both payloads plus the
         * absorbed block's header.  Only the block count drops. */
        if (s_stat.free_blocks > 0)
            s_stat.free_blocks--;
        s_stat.free_bytes += BLOCK_HDR_SZ;
    }
}

/* ====================================================================
 * Internal _locked helpers (caller holds s_heap_lock)
 *
 * realloc needs to call malloc+memcpy+free atomically (otherwise a
 * concurrent free(ptr) between the two causes a use-after-free).  Since
 * the heap spinlock is non-recursive, the public malloc/free cannot be
 * called while holding the lock — these _locked variants do the same
 * work assuming the lock is already held.
 * ==================================================================== */

static void *malloc_locked(size_t size) {
    if (size == 0)
        return NULL;

    /* A request close to SIZE_MAX would wrap in the size rounding below
     * and hand back a block far smaller than asked for, so reject it up
     * front (malloc()/realloc() still see NULL + ENOMEM). */
    if (size > (size_t)-1 - MALLOC_MIN_SIZE) {
        s_stat.fail_count++;
        return NULL;
    }

    size_t asize = BLOCK_HDR_SZ + ROUND_UP(size);
    if (asize < MALLOC_MIN_SIZE)
        asize = MALLOC_MIN_SIZE;

    /* Fast path: take a block from the matching size-class bin.  A bin
     * holds blocks of one power-of-two class, but the class is wider
     * than the request (bin k covers asize in (2^k-1, 2^k]), so the
     * head of the bin can be too SMALL for this request — take the
     * first block that actually fits (bins hold at most BIN_CAP blocks,
     * so this stays a short scan).  Returning a too-small block would
     * break the usable-size contract of malloc.h and let the caller
     * overwrite the following block's header. */
    if (asize <= BIN_MAX) {
        int b = BinIndex(size);
        if (b >= 0) {
            block_t **pp = &s_bins[b];
            while (*pp && BLOCK_SIZE(*pp) < asize)
                pp = &(*pp)->next;
            if (*pp) {
                block_t *blk = *pp;
                *pp          = blk->next;
                MARK_USED(blk);
                StatDropFree(BLOCK_SIZE(blk));
                StatAddLive(BLOCK_SIZE(blk));
                return (char *)blk + BLOCK_HDR_SZ;
            }
        }
    }

    for (;;) {
        block_t **pp = &s_free_list;
        while (*pp) {
            block_t *b   = *pp;
            size_t   bsz = BLOCK_SIZE(b);

            if (bsz >= asize) {
                size_t remainder = bsz - asize;

                if (remainder >= MALLOC_MIN_SIZE) {
                    block_t *newb = (block_t *)((char *)b + asize);
                    newb->size    = remainder | 1; /* free */
                    newb->next    = b->next;

                    b->size = asize;
                    b->next = newb;

                    StatDropFree(bsz);   /* the whole block left the list */
                    StatAddLive(asize);  /* ... and comes back used, trimmed */
                    StatAddFree(remainder);
                } else {
                    /* Sub-minimum remainder: b keeps its full size and the
                     * caller gets it whole, so the chain has no hole and
                     * there is no new free block to account for. */
                    StatDropFree(bsz);
                    StatAddLive(bsz);
                }

                *pp = b->next;
                MARK_USED(b);
                return (char *)b + BLOCK_HDR_SZ;
            }

            pp = &b->next;
        }

        if (HeapGrow(asize) < 0) {
            s_stat.fail_count++;
            return NULL;
        }
    }
}

/* Count the blocks currently parked in a bin. */
static int BinCount(int idx) {
    int n = 0;
    for (block_t *b = s_bins[idx]; b; b = b->next)
        n++;
    return n;
}

/* Is `blk` a free block?  Checks the global list AND every bin (a free
 * small block may be parked in its size-class bin).  O(1) fast reject:
 * a block with the FREE bit clear is used and can never be on a free
 * list — this is the common case in realloc's in-place growth, where
 * the following block is almost always used (previously O(n) per call
 * -> O(n^2) while growing a file block 4 KiB at a time).
 *
 * SAFETY: `blk` is a by-ADDRESS candidate (block + BLOCK_SIZE) that may
 * point past the mapped heap end (the block after the last one in a
 * chunk).  Dereferencing it would #PF, so the pointer is first checked
 * against the mapped range [s_heap_base, s_next_virt) — the old
 * pointer-scan-only version never dereferenced it, and treating an
 * out-of-range address as "not free" is exactly what the list scan
 * would conclude anyway. */
static int BlockIsFree(block_t *blk) {
    uintptr_t p = (uintptr_t)blk;
    if (p < s_heap_base || p >= s_next_virt)
        return 0; /* past the heap: never a list node */
    if (!IS_FREE(blk))
        return 0;
    for (block_t *f = s_free_list; f; f = f->next)
        if (f == blk)
            return 1;
    for (int i = 0; i < BIN_COUNT; i++)
        for (block_t *f = s_bins[i]; f; f = f->next)
            if (f == blk)
                return 1;
    return 0;
}

/* Unlink `blk` from wherever it is parked (global list or a bin).
 * Returns 1 when found and removed, 0 when it was not free. */
static int BlockUnlink(block_t *blk) {
    block_t **pp = &s_free_list;
    while (*pp) {
        if (*pp == blk) {
            *pp = blk->next;
            blk->size &= ~(size_t)1; /* keep invariant: set bit <=> on a list */
            return 1;
        }
        pp = &(*pp)->next;
    }
    for (int i = 0; i < BIN_COUNT; i++) {
        block_t **bp = &s_bins[i];
        while (*bp) {
            if (*bp == blk) {
                *bp = blk->next;
                blk->size &= ~(size_t)1;
                return 1;
            }
            bp = &(*bp)->next;
        }
    }
    return 0;
}

static void FreeLocked(void *ptr) {
    if (!ptr)
        return;

    /* Resolve an over-aligned payload back to the block it came from
     * (aligned_alloc/posix_memalign), so free() stays the single exit
     * point for every allocation. */
    block_t *b = BlockOfPayload(ptr);

    StatDropLive(BLOCK_SIZE(b));
    MARK_FREE(b);
    StatAddFree(BLOCK_SIZE(b));

    /* Fast path: park small blocks in their size-class bin (bounded;
     * full bins overflow to the global list so coalescing still runs). */
    size_t bsz = BLOCK_SIZE(b);
    if (bsz <= BIN_MAX) {
        int idx = BinIndex(bsz - BLOCK_HDR_SZ);
        if (idx >= 0 && BinCount(idx) < BIN_CAP) {
            b->next = s_bins[idx];
            s_bins[idx] = b;
            return;
        }
    }

    b->next     = s_free_list;
    s_free_list = b;

    CoalesceAfter(b);
}

/* ====================================================================
 * Public API
 * ==================================================================== */

void *malloc(size_t size) {
    SpinLock(&s_heap_lock);
    void *result = malloc_locked(size);
    SpinUnlock(&s_heap_lock);

    if (!result && size != 0)
        errno = ENOMEM;
    return result;
}

void free(void *ptr) {
    if (!ptr)
        return;

    SpinLock(&s_heap_lock);
    FreeLocked(ptr);
    SpinUnlock(&s_heap_lock);
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    if (nmemb != 0 && total / nmemb != size) {
        /* Overflow */
        StatFail();
        errno = ENOMEM;
        return NULL;
    }
    void *ptr = malloc(total);
    if (ptr)
        memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr)
        return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    /* Over-aligned allocation (aligned_alloc/posix_memalign): the word
     * in front of the pointer is the back-pointer slot, not a block
     * header, so the block can never grow in place — allocate, copy and
     * free through the public entry points. */
    if (PayloadIsAligned(ptr)) {
        block_t *blk  = BlockOfPayload(ptr);
        size_t   have = UsableSizeOf(blk, ptr);
        void    *newp = malloc(size);
        if (!newp)
            return NULL; /* malloc() already set errno */
        memcpy(newp, ptr, have < size ? have : size);
        free(ptr);
        return newp;
    }

    block_t *b           = (block_t *)((char *)ptr - BLOCK_HDR_SZ);
    size_t   old_payload = BLOCK_SIZE(b) - BLOCK_HDR_SZ;

    /* If the new size fits in the existing block, return ptr */
    if (size <= old_payload)
        return ptr;

    /* ---- In-place growth: absorb the free block(s) that immediately
     * follow this one in address space.  Without this, growing a file
     * data block 4 KiB at a time degenerates into malloc+copy+free of
     * the whole file on every write (O(n^2) copy). */
    {
        size_t asize = BLOCK_HDR_SZ + ROUND_UP(size);
        if (asize < MALLOC_MIN_SIZE)
            asize = MALLOC_MIN_SIZE;

        SpinLock(&s_heap_lock);

        /* Pass 1: compute how much contiguous free space follows. */
        size_t   have = BLOCK_SIZE(b);
        block_t *last = b;
        while (have < asize) {
            block_t *nxt = (block_t *)((char *)last + BLOCK_SIZE(last));
            if (!BlockIsFree(nxt))
                break; /* next block is used or absent */
            have += BLOCK_SIZE(nxt);
            last = nxt;
        }

        if (have >= asize) {
            /* Pass 2: unlink every absorbed block from wherever it is
             * parked (global list or a size-class bin). */
            block_t *cur = (block_t *)((char *)b + BLOCK_SIZE(b));
            block_t *end = (block_t *)((char *)last + BLOCK_SIZE(last));
            while (cur != end) {
                size_t csz = BLOCK_SIZE(cur);

                (void)BlockUnlink(cur);
                StatDropFree(csz);
                cur = (block_t *)((char *)cur + csz);
            }

            /* Grow the used block and re-free any leftover tail.  A
             * remainder below MALLOC_MIN_SIZE stays part of the live
             * block: a sub-minimum block would leave a hole in the
             * size-linked chain (the next block is found by adding the
             * current size, so any unrepresented bytes desynchronise
             * the whole walk). */
            size_t rem = have - asize;
            if (rem >= MALLOC_MIN_SIZE) {
                block_t *nb = (block_t *)((char *)b + asize);

                b->size     = asize; /* used (no FREE flag) */
                nb->size    = rem | 1; /* free */
                nb->next    = s_free_list;
                s_free_list = nb;

                StatResizeLive(old_payload, PayloadOf(asize));
                StatAddFree(rem);
            } else {
                b->size = have; /* used, spans every absorbed block */

                StatResizeLive(old_payload, PayloadOf(have));
            }

            SpinUnlock(&s_heap_lock);
            return ptr;
        }

        /* Fallback: allocate a new block, copy, and free the old one —
         * ALL under the same lock.  Releasing the lock between malloc
         * and free would let another thread free(ptr), causing a
         * double-free / use-after-free.  Use the _locked helpers so
         * the spinlock is not re-acquired recursively. */
        void *newp = malloc_locked(size);
        if (newp)
            memcpy(newp, ptr, old_payload);
        FreeLocked(ptr);

        SpinUnlock(&s_heap_lock);

        if (!newp)
            errno = ENOMEM;
        return newp;
    }
}

/* ====================================================================
 * Heap diagnostics (malloc.h: MallocStats / MallocCheck / MallocUsableSize)
 * ==================================================================== */

/* Problem lines kept for the debug log: MallocCheck() counts every
 * finding but prints at most the first CHECK_MAX_REPORT of them. */
#define CHECK_MAX_REPORT 16

/* Free blocks indexed during the chain walk, used for the membership
 * test that proves a free-list node really is a block of the chain.
 * Heaps with more free blocks skip the fine-grained tests (the chain
 * walk itself is unlimited).  The scratch arrays are static because
 * the check runs under the heap lock (one checker at a time) and the
 * user stack is only USER_STACK_PAGES (4) pages. */
#define CHECK_MAX_FREE 256

static uintptr_t s_check_free[CHECK_MAX_FREE]; /* free blocks of the chain */
static uintptr_t s_check_list[CHECK_MAX_FREE]; /* nodes on free lists/bins */

typedef struct {
    const char *msg;
    uintptr_t   addr;
} check_problem_t;

typedef struct {
    check_problem_t items[CHECK_MAX_REPORT];
    int             found;  /* every problem seen (may exceed the log) */
    int             logged; /* entries stored in items[]              */
} check_ctx_t;

static void CheckProblem(check_ctx_t *ctx, const char *msg, uintptr_t addr) {
    ctx->found++;
    if (ctx->logged < CHECK_MAX_REPORT) {
        ctx->items[ctx->logged].msg  = msg;
        ctx->items[ctx->logged].addr = addr;
        ctx->logged++;
    }
}

/* Largest payload parked on the free list or in a size-class bin
 * (caller holds s_heap_lock).  Only the allocator's own structures are
 * walked here, and the walk is bounded by the number of blocks the
 * counters know about, so a corrupt (cyclic) list makes this return
 * early instead of hanging the caller — MallocCheck() then reports
 * the cycle. */
static size_t LargestFreePayload(void) {
    size_t best  = 0;
    size_t limit = s_stat.live_blocks + s_stat.free_blocks + 1;
    size_t seen  = 0;

    for (block_t *b = s_free_list; b; b = b->next) {
        size_t payload = PayloadOf(BLOCK_SIZE(b));

        if (++seen > limit)
            return best;
        if (payload > best)
            best = payload;
    }
    for (int i = 0; i < BIN_COUNT; i++) {
        for (block_t *b = s_bins[i]; b; b = b->next) {
            size_t payload = PayloadOf(BLOCK_SIZE(b));

            if (++seen > limit)
                return best;
            if (payload > best)
                best = payload;
        }
    }
    return best;
}

void MallocStats(malloc_stats_t *out) {
    if (!out)
        return;

    memset(out, 0, sizeof(*out));

    SpinLock(&s_heap_lock);

    /* Before the first grow there is no heap at all (the ASLR base is
     * fetched lazily by HeapGrow), so the region reads as empty. */
    if (s_heap_base != 0 && s_next_virt > s_heap_base) {
        out->heap_base  = (size_t)s_heap_base;
        out->heap_end   = (size_t)s_next_virt;
        out->heap_bytes = (size_t)(s_next_virt - s_heap_base);
    }
    out->used_bytes   = s_stat.live_bytes;
    out->free_bytes   = s_stat.free_bytes;
    out->blocks_live  = s_stat.live_blocks;
    out->blocks_free  = s_stat.free_blocks;
    out->largest_free = LargestFreePayload();
    out->grow_calls   = s_stat.grow_calls;
    out->grow_bytes   = s_stat.grow_bytes;
    out->fail_count   = s_stat.fail_count;
    out->peak_used    = s_stat.peak_used;

    /* Everything mapped is either live payload, free payload or a block
     * header; what is left over is header + alignment padding. */
    out->overhead = out->heap_bytes > out->used_bytes + out->free_bytes
                        ? out->heap_bytes - out->used_bytes - out->free_bytes
                        : 0;

    SpinUnlock(&s_heap_lock);
}

size_t MallocUsableSize(void *ptr) {
    if (!ptr)
        return 0;

    SpinLock(&s_heap_lock);

    block_t  *b      = BlockOfPayload(ptr);
    uintptr_t p      = (uintptr_t)b;
    size_t    usable = 0;

    /* The owning block must sit inside the mapped heap and be in use;
     * anything else is a foreign pointer, which is as undefined for the
     * caller as it is for free(). */
    if (p >= s_heap_base && p + BLOCK_HDR_SZ <= s_next_virt && !IS_FREE(b))
        usable = UsableSizeOf(b, ptr);

    SpinUnlock(&s_heap_lock);
    return usable;
}

/* Validate one free-list node (bin < 0 for the global list).  Only the
 * node itself is dereferenced here — its membership in the block chain
 * is checked by the caller against the index built in pass 1, so a
 * bogus pointer can never be followed any further. */
static void CheckFreeNode(check_ctx_t *ctx, block_t *f, int bin) {
    uintptr_t p   = (uintptr_t)f;
    size_t    bsz = BLOCK_SIZE(f);

    if (p < s_heap_base || p + BLOCK_HDR_SZ > s_next_virt) {
        CheckProblem(ctx, "free-list node outside the heap", p);
        return;
    }
    if ((p & (MALLOC_ALIGN - 1)) != 0) {
        CheckProblem(ctx, "free-list node is misaligned", p);
        return;
    }
    if (!IS_FREE(f)) {
        CheckProblem(ctx, "free-list node without the FREE bit", p);
        return;
    }
    if (bsz < MALLOC_MIN_SIZE || (bsz & (MALLOC_ALIGN - 1)) != 0 || p + bsz > s_next_virt) {
        CheckProblem(ctx, "free-list node has an impossible size", p);
        return;
    }
    if (bin >= 0 && BinIndex(PayloadOf(bsz)) != bin)
        CheckProblem(ctx, "free block parked in the wrong size-class bin", p);
}

int MallocCheck(void) {
    check_ctx_t ctx;
    ctx.found  = 0;
    ctx.logged = 0;

    SpinLock(&s_heap_lock);

    if (s_heap_base == 0 || s_next_virt <= s_heap_base) {
        SpinUnlock(&s_heap_lock);
        return -1; /* heap never grew: nothing to verify */
    }

    /* ---- Pass 1: walk the size-linked block chain from the heap floor.
     * The walk must cover [heap_base, heap_end) exactly.  Since a block's
     * successor starts at block + size, a valid walk proves that every
     * block — live and free alike — is disjoint from its neighbours: no
     * two live blocks overlap and no two free blocks overlap. ---- */
    block_t *b               = (block_t *)s_heap_base;
    size_t   n_blocks        = 0;
    size_t   n_live          = 0;
    size_t   n_free          = 0;
    size_t   live_bytes      = 0;
    size_t   free_bytes      = 0;
    size_t   free_indexed    = 0;
    int      index_truncated = 0;

    while ((uintptr_t)b < s_next_virt) {
        size_t bsz = BLOCK_SIZE(b);

        if (bsz < MALLOC_MIN_SIZE || (bsz & (MALLOC_ALIGN - 1)) != 0) {
            CheckProblem(&ctx, "block size out of range", (uintptr_t)b);
            break;
        }
        if ((uintptr_t)b + bsz > s_next_virt) {
            CheckProblem(&ctx, "block runs past the heap end", (uintptr_t)b);
            break;
        }

        n_blocks++;
        if (IS_FREE(b)) {
            n_free++;
            free_bytes += PayloadOf(bsz);
            if (free_indexed < CHECK_MAX_FREE)
                s_check_free[free_indexed++] = (uintptr_t)b;
            else
                index_truncated = 1;
        } else {
            n_live++;
            live_bytes += PayloadOf(bsz);
        }

        b = (block_t *)((char *)b + bsz);
    }

    if ((uintptr_t)b != s_next_virt)
        CheckProblem(&ctx, "block chain does not reach the heap end", (uintptr_t)b);

    /* ---- Pass 2: every free-list node must be a free block of the
     * chain, respect its size class and be listed exactly once.  Each
     * walk is bounded by the block count, so a cyclic list terminates
     * with a report instead of hanging the process. ---- */
    size_t limit     = n_blocks + 1;
    size_t list_seen = 0;
    size_t steps     = 0;

    for (block_t *f = s_free_list; f; f = f->next) {
        if (++steps > limit) {
            CheckProblem(&ctx, "global free list is cyclic", (uintptr_t)f);
            break;
        }
        if (list_seen < CHECK_MAX_FREE)
            s_check_list[list_seen] = (uintptr_t)f;
        list_seen++;
        CheckFreeNode(&ctx, f, -1);
    }

    for (int i = 0; i < BIN_COUNT; i++) {
        size_t bin_nodes = 0;

        steps = 0;
        for (block_t *f = s_bins[i]; f; f = f->next) {
            if (++steps > limit) {
                CheckProblem(&ctx, "size-class bin is cyclic", (uintptr_t)f);
                break;
            }
            if (list_seen < CHECK_MAX_FREE)
                s_check_list[list_seen] = (uintptr_t)f;
            list_seen++;
            bin_nodes++;
            CheckFreeNode(&ctx, f, i);
        }
        if (bin_nodes > BIN_CAP)
            CheckProblem(&ctx, "size-class bin holds more than BIN_CAP blocks",
                         (uintptr_t)s_bins[i]);
    }

    /* Duplicates and membership (skipped when the index overflowed). */
    if (!index_truncated && list_seen <= CHECK_MAX_FREE) {
        for (size_t i = 0; i < list_seen; i++) {
            int found = 0;

            for (size_t j = 0; j < free_indexed; j++) {
                if (s_check_free[j] == s_check_list[i]) {
                    found = 1;
                    break;
                }
            }
            if (!found)
                CheckProblem(&ctx, "free-list node is not a free block of the chain",
                             s_check_list[i]);

            for (size_t j = i + 1; j < list_seen; j++) {
                if (s_check_list[j] == s_check_list[i]) {
                    CheckProblem(&ctx, "free block is listed more than once",
                                 s_check_list[i]);
                    break;
                }
            }
        }

        if (list_seen != n_free)
            CheckProblem(&ctx, "free-list node count differs from the free block count",
                         list_seen);
    }

    /* ---- Cross-check the counters kept by the allocator against the
     * numbers just re-derived from the heap. ---- */
    if (n_live != s_stat.live_blocks)
        CheckProblem(&ctx, "live block counter drift", n_live);
    if (n_free != s_stat.free_blocks)
        CheckProblem(&ctx, "free block counter drift", n_free);
    if (live_bytes != s_stat.live_bytes)
        CheckProblem(&ctx, "live byte counter drift", live_bytes);
    if (free_bytes != s_stat.free_bytes)
        CheckProblem(&ctx, "free byte counter drift", free_bytes);

    int found  = ctx.found;
    int logged = ctx.logged;

    SpinUnlock(&s_heap_lock);

    /* printf() may allocate, so the log is printed only after the heap
     * lock has been released (the spinlock is not recursive). */
    for (int i = 0; i < logged; i++)
        printf("malloc: heap check: %s (addr=%p)\n", ctx.items[i].msg,
               (void *)ctx.items[i].addr);
    if (found > logged)
        printf("malloc: heap check: %d problem(s), first %d shown\n", found, logged);

    return found;
}

/* ====================================================================
 * Aligned allocation (C11 7.22.3.1 aligned_alloc, POSIX posix_memalign)
 * ==================================================================== */

/* Over-aligned allocation; the caller holds s_heap_lock.  Alignments up
 * to MALLOC_ALIGN need no work at all: every payload is 16-byte aligned
 * already.  Larger alignments over-allocate by one alignment and hand
 * back an interior pointer, so the two words below it can carry the tag
 * and the back-pointer (see the over-aligned block comment above).
 *
 * The arithmetic relies on two facts: block payloads are always
 * MALLOC_ALIGN (16) aligned (chunks are page aligned and every block
 * size is a multiple of 16), and for a 16-aligned base the distance to
 * the next multiple of a power-of-two alignment a >= 32 that is at least
 * base + 16 is at most a — which is why a payload of size + alignment
 * always covers the aligned interior. */
static void *MallocAlignedLocked(size_t alignment, size_t size) {
    if (alignment <= MALLOC_ALIGN)
        return malloc_locked(size);

    if (size == 0)
        size = 1; /* zero-size request: still hand back a unique pointer */

    if (size > (size_t)-1 - alignment) {
        s_stat.fail_count++;
        return NULL; /* size + alignment would wrap around */
    }

    void *raw = malloc_locked(size + alignment);
    if (!raw)
        return NULL; /* malloc_locked() counted the failure */

    uintptr_t base = (uintptr_t)raw;
    uintptr_t p    = (base + 2 * sizeof(void *) + alignment - 1) & ~(uintptr_t)(alignment - 1);

    ((void **)p)[-1]  = raw;              /* back-pointer (p - 8)  */
    ((size_t *)p)[-2] = MALLOC_ALIGN_TAG; /* tag          (p - 16) */
    return (void *)p;
}

void *aligned_alloc(size_t alignment, size_t size) {
    /* C11: alignment must be a power of two.  A size that is not a
     * multiple of the alignment is accepted (the payload is rounded up
     * anyway), which is the permissive behaviour of the C17 library. */
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        errno = EINVAL;
        return NULL;
    }

    SpinLock(&s_heap_lock);
    void *p = MallocAlignedLocked(alignment, size);
    SpinUnlock(&s_heap_lock);

    if (!p)
        errno = ENOMEM;
    return p;
}

/* The single definition in the tree: libc's <stdlib.h> declares it, but
 * the allocator itself must own it so that an over-aligned block is
 * released by the same free()/realloc() that produced it. */
int posix_memalign(void **memptr, size_t alignment, size_t size) {
    if (!memptr)
        return EINVAL;

    /* POSIX: alignment must be a power of two AND a multiple of
     * sizeof(void *).  Errors travel through the return value only —
     * errno is not touched and *memptr is left alone on failure. */
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
        return EINVAL;

    SpinLock(&s_heap_lock);
    void *p = MallocAlignedLocked(alignment, size);
    SpinUnlock(&s_heap_lock);

    if (!p)
        return ENOMEM;
    *memptr = p;
    return 0;
}
