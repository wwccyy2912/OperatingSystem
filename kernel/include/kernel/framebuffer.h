/*
 * framebuffer.h - Linear framebuffer driver
 * Copyright (c) 2026 OpSys Project
 *
 * Parses the Multiboot2 framebuffer tag and maps the framebuffer into
 * kernel space.
 *
 * P0 (docs/kernel_roadmap.md): the drawing primitives (fb_pixel/fb_fill/
 * fb_puts/fb_printf) were REMOVED — the framebuffer is owned by the
 * user-space term service.  The kernel exposes only init + query.
 */

#ifndef KERNEL_FRAMEBUFFER_H
#define KERNEL_FRAMEBUFFER_H

#include <kernel/types.h>

/* Framebuffer info (filled by fb_init) */
typedef struct {
    u64  addr;          /* Virtual address of mapped framebuffer */
    u32  width;         /* Width in pixels */
    u32  height;        /* Height in pixels */
    u32  pitch;         /* Bytes per scanline */
    u8   bpp;           /* Bits per pixel */
} fb_info_t;

/*
 * User-facing framebuffer descriptor (SYS_FB_GET_INFO).
 *
 * Unlike fb_info_t, addr is the PHYSICAL framebuffer address: a user
 * process cannot dereference the kernel's virtual mapping, so it needs
 * the physical address to call SYS_FB_MAP and map the framebuffer into
 * its own address space.  The layout must stay in sync with the mirror
 * struct in user/lib/libos/syscalls.h.
 */
typedef struct {
    u64  phys_addr;     /* Physical address of the framebuffer */
    u32  width;         /* Width in pixels (logical px in VGA text mode) */
    u32  height;        /* Height in pixels (logical px in VGA text mode) */
    u32  pitch;         /* Bytes per scanline (linear mode only) */
    u8   bpp;           /* Bits per pixel (linear mode only) */
    u8   vga_text;      /* 1 = VGA text mode (0xB8000), 0 = linear RGB */
} fb_user_info_t;

/* Common 32-bit RGB colors */
#define FB_BLACK        0x00000000
#define FB_WHITE        0x00FFFFFF
#define FB_RED          0x00FF0000
#define FB_GREEN        0x0000FF00
#define FB_BLUE         0x000000FF
#define FB_CYAN         0x0000FFFF
#define FB_MAGENTA      0x00FF00FF
#define FB_YELLOW       0x00FFFF00
#define FB_ORANGE       0x00FFA500
#define FB_GRAY         0x00808080
#define FB_DARK_BLUE    0x00000080
#define FB_LIGHT_GRAY   0x00C0C0C0
#define FB_DARK_GRAY    0x00404040

/**
 * Initialize the framebuffer from Multiboot2 info.
 * Parses the framebuffer tag (type 8), maps physical address into
 * kernel virtual space via phys_to_virt, and stores fb_info.
 * @param mboot_addr  Physical address of Multiboot2 info structure.
 * @return 0 on success, -1 if no framebuffer found.
 */
int fb_init(u64 mboot_addr);

/**
 * Get framebuffer info.
 * @return Pointer to static fb_info_t, or NULL if not initialized.
 */
const fb_info_t *fb_get_info(void);

/**
 * Get the PHYSICAL address of the framebuffer.
 * @return Physical address, or 0 if not initialized.
 */
u64 fb_get_phys(void);

/**
 * Check whether the framebuffer is in VGA text mode (0xB8000).
 * @return 1 if VGA text mode, 0 if linear RGB mode.
 */
int fb_is_vga_text(void);

/**
 * Fill a user-facing framebuffer descriptor (SYS_FB_GET_INFO payload).
 * @param out  Output descriptor (kernel memory).
 * @return 0 on success, -1 if the framebuffer is not initialized.
 */
int fb_get_user_info(fb_user_info_t *out);

#endif /* KERNEL_FRAMEBUFFER_H */
