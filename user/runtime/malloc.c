/*
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
 *   HeapStart/HeapEnd (fixed region from the process image) ->
 *   free list (first-fit, 16-byte aligned) -> Malloc/Free/Calloc/
 *   Realloc.
 * How it works:
 *   Split/coalesce free blocks; a static list plus a bump region for
 *   the initial burst; no system calls after the heap is sized.
 * Purpose:
 *   Userspace heap so services need no kernel allocator calls.
 * Caveats:
 *   Single-threaded by default (no lock unless built threaded);
 *   capacity is fixed by the runtime's heap segment.
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
        (void)BlockUnlink(next);
        block->size = (block_sz + BLOCK_SIZE(next)) | 1;
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

    size_t asize = BLOCK_HDR_SZ + ROUND_UP(size);
    if (asize < MALLOC_MIN_SIZE)
        asize = MALLOC_MIN_SIZE;

    /* Fast path: take a block from the matching size-class bin. */
    if (asize <= BIN_MAX) {
        int b = BinIndex(size);
        if (b >= 0 && s_bins[b]) {
            block_t *blk = s_bins[b];
            s_bins[b]    = blk->next;
            MARK_USED(blk);
            return (char *)blk + BLOCK_HDR_SZ;
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
                }

                *pp = b->next;
                MARK_USED(b);
                return (char *)b + BLOCK_HDR_SZ;
            }

            pp = &b->next;
        }

        if (HeapGrow(asize) < 0)
            return NULL;
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

    block_t *b = (block_t *)((char *)ptr - BLOCK_HDR_SZ);
    MARK_FREE(b);

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
                (void)BlockUnlink(cur);
                cur = (block_t *)((char *)cur + BLOCK_SIZE(cur));
            }

            /* Grow the used block and re-free any leftover tail. */
            b->size    = asize; /* used (no FREE flag) */
            size_t rem = have - asize;
            if (rem >= MALLOC_MIN_SIZE) {
                block_t *nb = (block_t *)((char *)b + asize);
                nb->size    = rem | 1; /* free */
                nb->next    = s_free_list;
                s_free_list = nb;
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
