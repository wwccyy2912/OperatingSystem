
/*
 * panic.c - Unified kernel panic path
 * Copyright (c) 2026 OpSys Project
 *
 * Single choke point for all fatal kernel faults.  Callers pass a
 * printf-style reason plus any context they already gathered (register
 * dumps, thread info, etc. are printed by the caller before panicking —
 * detection stays distributed, only the final stop is centralized).
 *
 * Output: the panic reason is written to BOTH
 *   - serial (full detail, ungated), and
 *   - the framebuffer (red background, white 8x16 text) so a fatal
 *     kernel fault is visible on the physical screen even though the
 *     user-space term service has halted.  Framebuffer rendering is a
 *     self-contained minimal fallback (8x16 font + solid-fill only) —
 *     the full drawing stack lives in user space per P0.
 *
 * no_stack_protector: the canary check lives at the end of an
 * instrumented frame; panic() never returns, so its own canary is never
 * verified, and running it with a corrupted stack must not re-enter
 * __stack_chk_fail.
 */

#include <kernel/panic.h>
#include <kernel/serial.h>
#include <kernel/framebuffer.h>
#include <kernel/panic_font.h>
#include <stdarg.h>

/* Panic screen colors (32bpp ARGB) */
#define PANIC_BG        0x00C00000   /* dark red background */
#define PANIC_FG        0x00FFFFFF   /* white foreground */

/* Cell geometry matches the user-space term: 8x16 glyph in 9x20 cell */
#define CELL_W          9
#define CELL_H          20
#define GLYPH_W         8
#define GLYPH_H         16

/* Max panic message length (covers all current call sites) */
#define PANIC_MSG_MAX   256

/*
 * Minimal vsnprintf used to format the panic reason into a buffer.
 * Supports the same specifiers as serial_printf (%d %u %x %X %s %c %%
 * plus width and zero-padding) so the on-screen text matches the
 * serial text exactly.
 */
static int panic_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
        int written = 0;
        char numbuf[24];

        while (*fmt) {
                if (written >= (int)size - 1)
                        break;

                if (*fmt != '%') {
                        buf[written++] = *fmt++;
                        continue;
                }

                fmt++;  /* skip '%' */

                /* Zero-padding flag */
                int zero_pad = 0;
                if (*fmt == '0') {
                        zero_pad = 1;
                        fmt++;
                }

                /* Width */
                int width = 0;
                while (*fmt >= '0' && *fmt <= '9')
                        width = width * 10 + (*fmt++ - '0');

                switch (*fmt) {
                case 'd': {
                        i64 val = va_arg(ap, i64);
                        int neg = 0;
                        u64 uval;
                        if (val < 0) {
                                neg = 1;
                                uval = (u64)(-(i64)val);
                        } else {
                                uval = (u64)val;
                        }
                        /* Convert to decimal in numbuf (reverse) */
                        int ni = 0;
                        if (uval == 0) {
                                numbuf[ni++] = '0';
                        } else {
                                while (uval > 0 && ni < 23) {
                                        numbuf[ni++] = "0123456789"[uval % 10];
                                        uval /= 10;
                                }
                        }
                        if (neg && ni < 23)
                                numbuf[ni++] = '-';
                        int total = ni;
                        int pad = width > total ? width - total : 0;
                        /* With zero-padding and sign, the sign goes first */
                        int n = ni;
                        if (zero_pad && neg) {
                                if (written < (int)size - 1)
                                        buf[written++] = '-';
                                n--;
                                pad++;
                        }
                        for (int p = 0; p < pad && written < (int)size - 1; p++)
                                buf[written++] = zero_pad ? '0' : ' ';
                        while (n > 0 && written < (int)size - 1)
                                buf[written++] = numbuf[--n];
                        break;
                }
                case 'u':
                case 'x':
                case 'X': {
                        u64 val = va_arg(ap, u64);
                        int base = (*fmt == 'u') ? 10 : 16;
                        const char *digits = (*fmt == 'X') ? "0123456789ABCDEF"
                                               : "0123456789abcdef";
                        int ni = 0;
                        if (val == 0) {
                                numbuf[ni++] = '0';
                        } else {
                                while (val > 0 && ni < 23) {
                                        numbuf[ni++] = digits[val % base];
                                        val /= base;
                                }
                        }
                        int pad = width > ni ? width - ni : 0;
                        for (int p = 0; p < pad && written < (int)size - 1; p++)
                                buf[written++] = zero_pad ? '0' : ' ';
                        while (ni > 0 && written < (int)size - 1)
                                buf[written++] = numbuf[--ni];
                        break;
                }
                case 's': {
                        const char *s = va_arg(ap, const char *);
                        if (!s)
                                s = "(null)";
                        while (*s && written < (int)size - 1)
                                buf[written++] = *s++;
                        break;
                }
                case 'c': {
                        buf[written++] = (char)va_arg(ap, int);
                        break;
                }
                case '%': {
                        buf[written++] = '%';
                        break;
                }
                default:
                        /* Unknown specifier: copy literally */
                        buf[written++] = '%';
                        if (*fmt && written < (int)size - 1)
                                buf[written++] = *fmt;
                        break;
                }
                if (*fmt)
                        fmt++;
        }

        buf[written] = '\0';
        return written;
}

/* ------------------------------------------------------------------
 * Framebuffer panic rendering (self-contained minimal fallback).
 * ------------------------------------------------------------------ */

/*
 * Fill the whole framebuffer with a solid color.
 */
static void panic_fb_fill(const fb_info_t *fb, u32 color)
{
        u8 *base = (u8 *)(u64)fb->addr;
        u32 pitch = fb->pitch;

        if (fb->bpp == 32) {
                for (u32 y = 0; y < fb->height; y++) {
                        volatile u32 *line = (volatile u32 *)(base + (u64)y * pitch);
                        for (u32 x = 0; x < fb->width; x++)
                                line[x] = color;
                }
        } else if (fb->bpp == 24) {
                /* 24bpp is BGR byte order: byte0=Blue, byte1=Green, byte2=Red */
                for (u32 y = 0; y < fb->height; y++) {
                        volatile u8 *line = base + (u64)y * pitch;
                        for (u32 x = 0; x < fb->width; x++) {
                                line[x * 3 + 0] = (u8)(color);
                                line[x * 3 + 1] = (u8)(color >> 8);
                                line[x * 3 + 2] = (u8)(color >> 16);
                        }
                }
        }
        /* Other bpp: leave as-is (unlikely on QEMU) */
}

/*
 * Stamp one 8x16 glyph at cell (cx, cy).  Cells outside the screen are
 * ignored.
 */
static void panic_fb_putc(const fb_info_t *fb, u32 cx, u32 cy, u8 ch)
{
        if (ch < 0x20 || ch > 0x7E)
                return;                     /* no glyph for control chars */

        u32 px = cx * CELL_W;
        u32 py = cy * CELL_H;
        if (px + GLYPH_W > fb->width || py + GLYPH_H > fb->height)
                return;

        u8 *base = (u8 *)(u64)fb->addr;
        u32 pitch = fb->pitch;
        const u8 *glyph = s_panic_font[ch - 0x20];

        for (int row = 0; row < GLYPH_H; row++) {
                u8 bits = glyph[row];
                for (int col = 0; col < GLYPH_W; col++) {
                        if (!(bits & (0x80 >> col)))
                                continue;
                        if (fb->bpp == 32) {
                                volatile u32 *p = (volatile u32 *)(base + (u64)(py + row) * pitch
                                                   + (u64)(px + col) * 4);
                                *p = PANIC_FG;
                        } else if (fb->bpp == 24) {
                                volatile u8 *p = base + (u64)(py + row) * pitch
                                 + (u64)(px + col) * 3;
                                p[0] = (u8)(PANIC_FG);
                                p[1] = (u8)(PANIC_FG >> 8);
                                p[2] = (u8)(PANIC_FG >> 16);
                        }
                }
        }
}

/*
 * Render a multi-line message on the panic screen.  '\n' advances to the
 * next cell row; long lines wrap at the screen width.
 */
static void panic_fb_puts(const fb_info_t *fb, const char *s)
{
        u32 cols = fb->width / CELL_W;
        u32 rows = fb->height / CELL_H;
        u32 cx = 0, cy = 0;

        while (*s) {
                if (*s == '\n') {
                        cx = 0;
                        cy++;
                } else if (*s == '\r') {
                        cx = 0;
                } else {
                        panic_fb_putc(fb, cx, cy, (u8)*s);
                        if (++cx >= cols) {
                                cx = 0;
                                cy++;
                        }
                }
                if (cy >= rows)
                        break;
                s++;
        }
}

/*
 * Draw the full panic screen: solid dark-red background, then a bold
 * "KERNEL PANIC" title line, a blank row, and the reason message.
 */
static void panic_render_screen(const char *msg)
{
        const fb_info_t *fb = fb_get_info();
        if (!fb)
                return;                     /* no framebuffer yet (early boot) */

        /* screen = "KERNEL PANIC\n\n<msg>" */
        char screen[PANIC_MSG_MAX + 32];
        const char *title = "KERNEL PANIC\n\n";
        char *dst = screen;
        char *end = screen + sizeof(screen) - 1;
        while (*title && dst < end)
                *dst++ = *title++;
        while (*msg && dst < end)
                *dst++ = *msg++;
        *dst = '\0';

        /* VGA text mode (0xB8000): write cells directly, no font needed */
        if (fb_is_vga_text()) {
                u16 *cells = (u16 *)(u64)fb->addr;
                u32 cols = fb->width / CELL_W;
                u32 rows = fb->height / CELL_H;
                u16 attr = 0x4F;            /* white on red (attr nibbles: bg|fg) */
                for (u32 i = 0; i < cols * rows; i++)
                        cells[i] = (u16)(attr << 8) | ' ';
                u32 cx = 0, cy = 0;
                for (const char *p = screen; *p; p++) {
                        if (*p == '\n') {
                                cx = 0;
                                cy++;
                        } else if (*p == '\r') {
                                cx = 0;
                        } else {
                                u8 ch = (*p >= 0x20 && *p <= 0x7E) ? (u8)*p : ' ';
                                cells[cy * cols + cx] = (u16)((attr << 8) | ch);
                                if (++cx >= cols) {
                                        cx = 0;
                                        cy++;
                                }
                        }
                        if (cy >= rows)
                                break;
                }
                return;
        }

        /* Linear RGB mode: fill + render font */
        panic_fb_fill(fb, PANIC_BG);
        panic_fb_puts(fb, screen);
}

__attribute__((noreturn, no_stack_protector))
void panic(const char *fmt, ...)
{
        /* Disable interrupts first: a stray IRQ must not re-enter the
     * kernel while we are stopping. */
        __asm__ volatile("cli");

        /* Format the reason once; feed the same text to serial + screen */
        char msg[PANIC_MSG_MAX];
        va_list ap;
        va_start(ap, fmt);
        panic_vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);

        /* Serial output (ungated) */
        serial_puts("\n========== KERNEL PANIC ==========\n");
        serial_puts(msg);
        serial_puts("\n==================================\n");
        serial_puts("System halted.\n");

        /* Screen output: red screen + white message */
        panic_render_screen(msg);

        /* Park the CPU forever.  No attempt to unwind: after a fatal fault
     * the kernel state is untrustworthy. */
        for (;;)
                __asm__ volatile("hlt");
        __builtin_unreachable();
}
