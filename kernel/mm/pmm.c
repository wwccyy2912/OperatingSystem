/*
 * pmm.c - Physical Memory Manager
 * Copyright (c) 2026 OpSys Project
 *
 * Bitmap-based physical page frame allocator.
 * Uses a reserved region in physical memory for the bitmap rather than
 * a large static array, keeping BSS small. The bitmap is placed in an
 * available gap found via the multiboot2 memory map.
 */

#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/serial.h>
#include <multiboot2.h>

/* ---- Constants ---- */

/* Standard multiboot2 kernel load address */
#define KERNEL_PHYS_START 0x100000ULL

/* ---- State ---- */

static u64 *s_bitmap;       /* Pointer to bitmap (higher-half virtual) */
static u64  s_total_pages;  /* Total physical pages tracked */
static u64  s_free_pages;   /* Currently free pages */
static u64  s_bitmap_phys;  /* Physical address of bitmap */
static u64  s_bitmap_pages; /* Pages occupied by bitmap */
static u64  s_mem_end;      /* Highest physical address + 1 */

/* ---- Bitmap helpers ---- */

static inline void bitmap_set(u64 idx) {
    s_bitmap[idx / 64] |= (1ULL << (idx % 64));
}

static inline void bitmap_clear(u64 idx) {
    s_bitmap[idx / 64] &= ~(1ULL << (idx % 64));
}

static inline bool bitmap_test(u64 idx) {
    return (s_bitmap[idx / 64] >> (idx % 64)) & 1ULL;
}

/*
 * Find the first contiguous run of `count` free (zero) bits in the bitmap.
 * Returns the starting page index, or 0 if not found.
 * Page index 0 is never returned (physical page 0 is never allocated).
 */
static u64 bitmap_find_free(u64 count) {
    if (count == 0 || s_free_pages < count) {
        return 0;
    }

    u64 total     = s_total_pages;
    u64 run       = 0;
    u64 run_start = 0;

    /* Start from page 1 to skip the null page */
    for (u64 i = 1; i < total; i++) {
        if (!bitmap_test(i)) {
            if (run == 0) {
                run_start = i;
            }
            run++;
            if (run == count) {
                return run_start;
            }
        } else {
            run = 0;
        }
    }
    return 0;
}

/* ---- Multiboot2 tag walking ---- */

mboot2_tag_t *mboot2_find_tag(u64 mboot_addr, u32 tag_type) {
    u32 total_size = *(volatile u32 *)mboot_addr;
    u8 *info       = (u8 *)mboot_addr;
    u32 offset     = 8; /* Skip total_size and reserved fields */

    while (offset < total_size) {
        mboot2_tag_t *tag = (mboot2_tag_t *)(info + offset);
        if (tag->type == 0) {
            break; /* End tag */
        }
        if (tag->type == tag_type) {
            return tag;
        }
        /* Tags are padded to 8-byte alignment */
        offset += (tag->size + 7) & ~7U;
    }
    return NULL;
}

mboot2_mmap_entry_t *mboot2_get_mmap(u64 mboot_addr, u32 *count_out) {
    mboot2_tag_t *tag = mboot2_find_tag(mboot_addr, 6);
    if (!tag) {
        *count_out = 0;
        return NULL;
    }

    /* Skip tag header (8 bytes) to reach mmap_tag_t fields */
    mboot2_mmap_tag_t *mmap_tag = (mboot2_mmap_tag_t *)((u8 *)tag + sizeof(mboot2_tag_t));

    /* Data after entry_size + entry_version = entries */
    u32 header_overhead = sizeof(mboot2_tag_t) + sizeof(mboot2_mmap_tag_t);
    u32 data_size       = tag->size - header_overhead;
    *count_out          = data_size / mmap_tag->entry_size;

    return (mboot2_mmap_entry_t *)((u8 *)tag + header_overhead);
}

/* ---- Physical memory manager ---- */

void pmm_init(u64 mboot_addr, u64 kernel_end) {
    serial_puts("PMM: Initializing...\n");

    /* Step 1: Parse multiboot2 memory map */
    u32                  mmap_count = 0;
    mboot2_mmap_entry_t *mmap       = mboot2_get_mmap(mboot_addr, &mmap_count);
    if (!mmap) {
        serial_puts("PMM: ERROR - No memory map found\n");
        return;
    }

    /* Step 2: Find total usable RAM.
     * Only consider AVAILABLE regions (type 1). Reserved regions (like
     * mmap[6] at 0xFD00000000) are beyond the identity-mapped 4GB range
     * and including them would make the bitmap impossibly large. */
    s_mem_end = 0;
    for (u32 i = 0; i < mmap_count; i++) {
        u64 region_end = mmap[i].base_addr + mmap[i].length;
        serial_printf_level(SERIAL_LOG_DEBUG,
                            "  mmap[%u]: base=0x%x len=0x%x type=%u\n",
                            i,
                            (u32)mmap[i].base_addr,
                            (u32)mmap[i].length,
                            mmap[i].type);
        if (mmap[i].type == MULTIBOOT2_MEMORY_AVAILABLE && region_end > s_mem_end) {
            s_mem_end = region_end;
        }
    }
    /* Clamp s_mem_end to the range the boot page tables actually map.
     * boot.asm maps the first 4GB with 2MB huge pages and the 4GB-128GB
     * range with 1GB huge pages (PDP[4..127]) in the shared identity /
     * higher-half PDP, so phys_to_virt() is valid up to the highest
     * present PDP entry.  Reading the live page tables keeps this limit
     * in sync with boot.asm without duplicating a constant: allocating
     * a frame beyond the mapped range would #PF on access. */
    u64 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    u64 *boot_pml4    = (u64 *)(cr3 + KERNEL_VIRT_BASE);
    u64 *boot_pdp     = (u64 *)((boot_pml4[256] & ~0xFFFULL) + KERNEL_VIRT_BASE);
    u64  mapped_limit = 0;
    for (int i = 511; i >= 0; i--) {
        if (boot_pdp[i] & PTE_PRESENT) {
            mapped_limit = (u64)(i + 1) << 30;
            break;
        }
    }
    if (mapped_limit > 0 && s_mem_end > mapped_limit)
        s_mem_end = mapped_limit;

    s_total_pages = s_mem_end / PAGE_SIZE;

    serial_printf("PMM: Memory: 0x%x bytes, 0x%x pages\n", s_mem_end, s_total_pages);

    /* Step 3: Calculate bitmap size in bytes and pages */
    u64 bitmap_words        = (s_total_pages + 63) / 64;
    u64 bitmap_bytes        = bitmap_words * sizeof(u64);
    u64 bitmap_pages_needed = (bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    serial_printf("PMM: bitmap: words=%u bytes=%u pages=%u\n",
                  bitmap_words,
                  bitmap_bytes,
                  bitmap_pages_needed);

    /* Step 4: Find a gap in the memory map for the bitmap.
     * The bitmap must be placed in an available region, after the kernel
     * AND after the multiboot2 info structure (GRUB places it just above
     * the kernel image; overwriting it would destroy the memory map that
     * Step 6 is about to walk).  It is accessed via s_bitmap at
     * bitmap_phys + KERNEL_VIRT_BASE (higher-half), which the boot page
     * tables map for the whole physical range, so any gap works. */
    u32 mb_info_size = *(volatile u32 *)mboot_addr;
    u64 mb_begin     = mboot_addr & ~(u64)(PAGE_SIZE - 1);
    u64 mb_end       = (mboot_addr + mb_info_size + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);

    u64 bitmap_phys = 0;
    for (u32 i = 0; i < mmap_count; i++) {
        if (mmap[i].type != MULTIBOOT2_MEMORY_AVAILABLE) {
            continue;
        }
        u64 region_start = mmap[i].base_addr;
        u64 region_end   = mmap[i].base_addr + mmap[i].length;

        /* Align start up to page boundary, skip past kernel.
         * Re-align after bumping past kernel_end (may not be page-aligned). */
        u64 aligned = (region_start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (aligned < kernel_end) {
            aligned = (kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        }

        /* Skip past the multiboot2 info structure if the bitmap would
         * overlap it.  This matters once the bitmap is large enough to
         * reach mboot_addr (it grew from 2 pages at 256M to 56 pages
         * at 6G in testing). */
        if (aligned < mb_end && aligned + bitmap_bytes > mb_begin) {
            aligned = mb_end;
        }

        if (aligned + bitmap_bytes <= region_end) {
            bitmap_phys = aligned;
            break;
        }
    }

    if (!bitmap_phys) {
        serial_puts("PMM: ERROR - No room for bitmap\n");
        return;
    }

    s_bitmap       = (u64 *)(bitmap_phys + KERNEL_VIRT_BASE);
    s_bitmap_phys  = bitmap_phys;
    s_bitmap_pages = bitmap_pages_needed;

    /* Step 5: Mark ALL pages as used (set every bit to 1) */
    for (u64 i = 0; i < bitmap_words; i++) {
        s_bitmap[i] = ~0ULL;
    }

    /* Step 6: Walk memory map again, mark available regions as free (0).
     * The mmap may contain AVAILABLE regions above s_mem_end (e.g. QEMU
     * reports RAM past the 4GB boundary) — clamp end_page so the bitmap
     * is never written past s_total_pages bits. */
    for (u32 i = 0; i < mmap_count; i++) {
        if (mmap[i].type != MULTIBOOT2_MEMORY_AVAILABLE) {
            continue;
        }
        u64 start_page = (mmap[i].base_addr + PAGE_SIZE - 1) / PAGE_SIZE;
        u64 end_page   = (mmap[i].base_addr + mmap[i].length) / PAGE_SIZE;
        if (end_page > s_total_pages)
            end_page = s_total_pages;
        for (u64 p = start_page; p < end_page; p++) {
            bitmap_clear(p);
        }
    }

    /* Step 7: Mark bitmap region itself as used */
    u64 bm_start_page = bitmap_phys / PAGE_SIZE;
    u64 bm_end_page   = (bitmap_phys + bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    for (u64 p = bm_start_page; p < bm_end_page; p++) {
        bitmap_set(p);
    }

    /* Step 8: Mark kernel code/data region as used */
    u64 k_start_page = KERNEL_PHYS_START / PAGE_SIZE;
    u64 k_end_page   = (kernel_end + PAGE_SIZE - 1) / PAGE_SIZE;
    for (u64 p = k_start_page; p < k_end_page; p++) {
        bitmap_set(p);
    }

    /* Step 9: Mark multiboot2 info structure as used.
     * The total_size field is the first u32 at mboot_addr. */
    u32 mb_total_size = *(volatile u32 *)mboot_addr;
    u64 mb_start_page = mboot_addr / PAGE_SIZE;
    u64 mb_end_page   = (mboot_addr + mb_total_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (u64 p = mb_start_page; p < mb_end_page; p++) {
        bitmap_set(p);
    }

    /* Step 9b: Mark the framebuffer physical range as used.
     *
     * Some firmware/hypervisors (notably VirtualBox) report the VBE
     * linear framebuffer as part of an AVAILABLE memory region in the
     * multiboot2 memory map, unlike QEMU which marks it RESERVED.  When
     * cap tables (and other allocations) were statically embedded in
     * BSS this was harmless — the kernel image sat low in physical
     * memory, far below the framebuffer.  Now that PMM hands out pages
     * on demand, pmm_alloc_pages could return a page overlapping the
     * framebuffer, corrupting the display.
     *
     * Parse the framebuffer tag (type 8) here and reserve its physical
     * range so no allocation can touch it.  fb_init() runs later, but
     * the tag is already in the multiboot2 info structure. */
    {
        mboot2_tag_t *fb_tag = mboot2_find_tag(mboot_addr, 8);
        if (fb_tag) {
            u8 *fb_data   = (u8 *)fb_tag;
            u64 fb_phys   = *(u64 *)(fb_data + 8);
            u32 fb_pitch  = *(u32 *)(fb_data + 16);
            u32 fb_width  = *(u32 *)(fb_data + 20);
            u32 fb_height = *(u32 *)(fb_data + 24);
            if (fb_phys != 0 && fb_pitch != 0 && fb_width != 0 && fb_height != 0) {
                u64 fb_size       = (u64)fb_pitch * fb_height;
                u64 fb_start_page = fb_phys / PAGE_SIZE;
                u64 fb_end_page   = (fb_phys + fb_size + PAGE_SIZE - 1) / PAGE_SIZE;
                /* Only reserve pages that fall within PMM's tracked
                 * range.  If the framebuffer is entirely above
                 * s_total_pages (common when RAM < fb address), PMM
                 * never allocates there anyway — skip to avoid
                 * underflow in the page-count printout. */
                if (fb_start_page < s_total_pages) {
                    if (fb_end_page > s_total_pages)
                        fb_end_page = s_total_pages;
                    for (u64 p = fb_start_page; p < fb_end_page; p++) {
                        bitmap_set(p);
                    }
                    serial_printf("PMM: reserved framebuffer "
                                  "phys=0x%x size=0x%x (%u pages)\n",
                                  (u32)fb_phys,
                                  (u32)fb_size,
                                  (u32)(fb_end_page - fb_start_page));
                } else {
                    serial_printf("PMM: framebuffer phys=0x%x above "
                                  "tracked range (no reservation "
                                  "needed)\n",
                                  (u32)fb_phys);
                }
            }
        }
    }

    /* Step 10: Count free pages */
    s_free_pages = 0;
    for (u64 i = 0; i < s_total_pages; i++) {
        if (!bitmap_test(i)) {
            s_free_pages++;
        }
    }

    u64 free_mb = (s_free_pages * PAGE_SIZE) / (1024 * 1024);
    serial_printf("PMM: Free: 0x%x pages (%u MB)\n", s_free_pages, free_mb);

    /* Self-check: if the machine has memory above 4GB, verify the 1GB
     * huge-page mapping (added by boot.asm) actually works by probing
     * the first page above the 4GB boundary via phys_to_virt().  If the
     * mapping were missing, this access would #PF immediately. */
    if (s_mem_end > 0x100000000ULL) {
        volatile u64 *probe = (volatile u64 *)(0x100000000ULL + KERNEL_VIRT_BASE);
        *probe              = 0xCAFEBABECAFEBABEULL;
        if (*probe == 0xCAFEBABECAFEBABEULL)
            serial_puts("PMM: >4GB phys mapping verified\n");
        else
            serial_puts("PMM: WARNING - >4GB phys mapping broken!\n");
    }

    serial_puts("PMM: Initialized\n");
}

u64 pmm_alloc_page(void) {
    u64 idx = bitmap_find_free(1);
    if (idx == 0) {
        return 0;
    }
    bitmap_set(idx);
    s_free_pages--;
    return idx * PAGE_SIZE;
}

void pmm_free_page(u64 phys) {
    if (phys == 0 || phys >= s_mem_end) {
        return;
    }
    u64 idx = phys / PAGE_SIZE;
    if (idx >= s_total_pages || !bitmap_test(idx)) {
        return; /* Out of range or already free */
    }
    bitmap_clear(idx);
    s_free_pages++;
}

u64 pmm_alloc_pages(u64 count) {
    if (count == 0) {
        return 0;
    }
    u64 idx = bitmap_find_free(count);
    if (idx == 0) {
        return 0;
    }
    for (u64 i = 0; i < count; i++) {
        bitmap_set(idx + i);
    }
    s_free_pages -= count;
    return idx * PAGE_SIZE;
}

void pmm_free_pages(u64 phys, u64 count) {
    if (phys == 0 || count == 0) {
        return;
    }
    u64 start_idx = phys / PAGE_SIZE;
    for (u64 i = 0; i < count; i++) {
        u64 idx = start_idx + i;
        if (idx < s_total_pages && bitmap_test(idx)) {
            bitmap_clear(idx);
            s_free_pages++;
        }
    }
}

u64 pmm_get_total_memory(void) {
    return s_total_pages * PAGE_SIZE;
}

u64 pmm_get_free_memory(void) {
    return s_free_pages * PAGE_SIZE;
}
