/*
 * framebuffer.c - Linear framebuffer driver
 * Copyright (c) 2026 OpSys Project
 *
 * Parses the Multiboot2 framebuffer tag and maps the framebuffer into
 * kernel virtual space (via phys_to_virt).
 *
 * P0 (docs/kernel_roadmap.md): all drawing primitives (fb_pixel/fb_fill/
 * fb_puts/fb_printf and the embedded 8x16 font) were REMOVED — the
 * framebuffer is owned by the user-space term service, which maps it via
 * SYS_FB_MAP and draws with its own font (user/services/term/font.h).
 * The kernel keeps only init + query + screen-clear on boot.
 */

#include <kernel/framebuffer.h>
#include <kernel/serial.h>
#include <kernel/types.h>
#include <kernel/vmm.h>
#include <multiboot2.h>

/* ====================================================================
 * Framebuffer state
 * ==================================================================== */

static fb_info_t s_fb;           /* Framebuffer info */
static bool      s_initialized;  /* fb_init() called successfully */
static bool      s_vga_text;     /* True if VGA text mode (type 1) */

/* Convert physical address to kernel virtual address. */
static inline void *fb_phys_to_virt(u64 phys) {
        return (void *)(phys + KERNEL_VIRT_BASE);
}

/* Clear the whole screen to dark blue (boot-time background).
 * VGA text mode: fill cells with space + dark-blue attribute.
 * Linear mode: fill with 0x00082860 (BGR order for 24bpp). */
static void fb_clear_screen(void)
{
        if (s_vga_text) {
                volatile u16 *buf = (volatile u16 *)s_fb.addr;
                u32 cells = (s_fb.width / 9) * (s_fb.height / 20);
                for (u32 i = 0; i < cells; i++)
                        buf[i] = (u16)(0x10 << 8) | ' ';  /* attr=dark blue bg, black fg */
        } else if (s_fb.bpp == 32) {
                volatile u32 *fb = (volatile u32 *)s_fb.addr;
                for (u32 i = 0; i < s_fb.width * s_fb.height; i++)
                        fb[i] = 0x00082860;
        } else if (s_fb.bpp == 24) {
                /* Framebuffer expects BGR byte order (byte 0 = Blue, byte 2 = Red) */
                for (u32 row = 0; row < s_fb.height; row++) {
                        volatile u8 *line = (volatile u8 *)(s_fb.addr + (u64)row * s_fb.pitch);
                        for (u32 col = 0; col < s_fb.width; col++) {
                                u8 *p = (u8 *)line + col * 3;
                                p[0] = 0x60;  /* B */
                                p[1] = 0x28;  /* G */
                                p[2] = 0x08;  /* R */
                        }
                }
        }
}

/* ====================================================================
 * Initialization
 * ==================================================================== */

int fb_init(u64 mboot_addr)
{
        /* Find the framebuffer tag (type 8) */
        mboot2_tag_t *tag = mboot2_find_tag(mboot_addr, 8);
        if (!tag) {
                serial_puts("  FB: no framebuffer tag found\n");
                return -1;
        }

        /* Parse the framebuffer tag.
     * Layout: type(4) + size(4) + addr(8) + pitch(4) + width(4)
     *         + height(4) + bpp(1) + fb_type(1) + reserved(2)
     * Total header = 32 bytes before colour info */
        u8 *data = (u8 *)tag;
        u64 fb_phys  = *(u64 *)(data + 8);   /* offset 8: physical address */
        u32 pitch    = *(u32 *)(data + 16);  /* offset 16: bytes per scanline */
        u32 width    = *(u32 *)(data + 20);  /* offset 20: visible width */
        u32 height   = *(u32 *)(data + 24);  /* offset 24: visible height */
        u8  bpp      = *(u8  *)(data + 28);  /* offset 28: bits per pixel */
        u8  fb_type  = *(u8  *)(data + 29);  /* offset 29: 0=text, 1=VGA text, 2=linear */

        /* Debug: dump tag header for validation */
        serial_printf_level(SERIAL_LOG_DEBUG, "  FB tag dump: type=%u sz=%u addr=0x%x pit=%u w=%u h=%u "
                                                "bpp=%u fbtype=%u\n",
                                                *(u32*)(data+0), *(u32*)(data+4), (u32)fb_phys, pitch,
                                                width, height, bpp, fb_type);

        if (fb_phys == 0 || width == 0 || height == 0) {
                serial_puts("  FB: invalid framebuffer info\n");
                return -1;
        }

        /* Determine mode type from framebuffer parameters:
     * - If address is 0xB8000 (VGA text buffer) and dimensions are text-mode
     *   (80x25 or less): treat as VGA text mode, even if GRUB reports
     *   fb_type=2 (linear). GRUB sometimes lies about fb_type for 0xB8000.
     * - Otherwise: treat as linear framebuffer. */
        if (fb_phys == 0xB8000 && width <= 80 && height <= 25) {
                s_vga_text = true;
        } else if (fb_type == 2 || width * height > 80*25 || bpp >= 24 || fb_phys != 0xB8000) {
                s_vga_text = false;
        } else {
                s_vga_text = true;
        }

        /* Map framebuffer into kernel virtual space.
     * For VGA text mode: scale width/height to logical pixels (9×20 per cell)
     * so the user-facing descriptor (SYS_FB_GET_INFO) reports the same
     * coordinate space as linear mode. */
        s_fb.addr = (u64)fb_phys_to_virt(fb_phys);
        if (s_vga_text) {
                s_fb.width  = width * 9;    /* 80 cols → 720 logical pixels */
                s_fb.height = height * 20;  /* 25 rows → 500 logical pixels */
        } else {
                s_fb.width  = width;
                s_fb.height = height;
        }
        s_fb.pitch  = pitch;
        s_fb.bpp    = bpp;
        s_initialized = true;

        serial_printf("  FB: %ux%u %ubpp @ phys 0x%x (pitch=%u)",
                                    width, height, bpp, (u32)fb_phys, pitch);
        if (s_vga_text)
                serial_puts(" (VGA text mode)\n");
        else
                serial_puts(" (linear framebuffer)\n");

        fb_clear_screen();
        serial_puts("  FB: initialized\n");
        return 0;
}

const fb_info_t *fb_get_info(void)
{
        return s_initialized ? &s_fb : NULL;
}

u64 fb_get_phys(void)
{
        if (!s_initialized)
                return 0;
        /* s_fb.addr is the kernel virtual mapping (phys + KERNEL_VIRT_BASE);
     * the VGA text buffer and the linear framebuffer are both mapped
     * this way by fb_phys_to_virt(), so the same subtraction works. */
        return s_fb.addr - KERNEL_VIRT_BASE;
}

int fb_is_vga_text(void)
{
        return s_initialized && s_vga_text;
}

int fb_get_user_info(fb_user_info_t *out)
{
        if (!s_initialized || !out)
                return -1;
        out->phys_addr = fb_get_phys();
        out->width     = s_fb.width;
        out->height    = s_fb.height;
        out->pitch     = s_fb.pitch;
        out->bpp       = s_fb.bpp;
        out->vga_text  = s_vga_text ? 1 : 0;
        return 0;
}
