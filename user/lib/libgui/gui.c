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
 * gui.c - Pixel graphics library implementation (see gui.h)
 * Copyright (c) 2026 OpSys Project
 *
 * ------------------------------------------------------------------
 * Structure (canvas, drawing, text, blit):
 *   GuiFbOpen -> fb_map() -> gui_canvas_t {w,h,pitch,bpp,buf}
 *   Primitives (Pixel/Fill/Hline/Vline/Rect) -> GuiXrgb -> pitch-addressed writes
 *   GuiText -> UTF-8 decode -> GuiTextCodepoint (8x16 ASCII | 16x16 CJK)
 *   GuiBlit -> same-bpp region copy (src -> dst)
 * How it works:
 *   GuiFbOpen maps the framebuffer below 4 GiB and records its geometry
 *   in the canvas; draw calls then write pixels directly into that
 *   mapping.  GuiText rasterizes each decoded codepoint from bitmap
 *   fonts (gui_font ASCII, font_cjk_lookup double-width CJK, '?' fallback).
 * Purpose:
 *   User-space pixel GUI: primitives, mixed ASCII/CJK text, region blits
 *   for window compositing.
 * Caveats:
 *   Window buffers share the 32bpp xRGB layout; 24bpp canvases cannot be
 *   blitted verbatim.  No unmap syscall exists, so GuiFbClose only
 *   invalidates the descriptor and the mapping outlives the canvas.
 * ------------------------------------------------------------------
 */

#include "gui.h"

#include <libos/syscalls.h>
#include <stddef.h>

/* ---- Framebuffer ---- */

int GuiFbOpen(gui_canvas_t *c) {
    if (!c)
        return -2; /* ERR_INVAL */

    fb_user_info_t info;
    int            ret = FbGetInfo(&info);
    if (ret < 0)
        return ret;

    u64 fb_size;
    if (info.vga_text) {
        /* VGA text mode: the term service owns the text buffer; a pixel
         * GUI cannot render into it.  Refuse so the compositor fails
         * loudly instead of drawing into nothing. */
        return -2; /* ERR_INVAL */
    }
    fb_size = (u64)info.pitch * info.height;

    /* Pick a page-aligned user address below the 4 GiB mark (the same
     * convention term uses) and map the framebuffer. */
    u64 virt = 0x60000000ULL;
    void *mapped = fb_map((void *)virt, fb_size);
    if (mapped == NULL || (uintptr_t)mapped == 0)
        return -1; /* ERR_NOMEM / gated */

    c->w     = info.width;
    c->h     = info.height;
    c->pitch = info.pitch;
    c->bpp   = info.bpp;
    c->buf   = (u8 *)mapped;
    return 0;
}

void GuiFbClose(gui_canvas_t *c) {
    /* No unmap syscall is exposed today; the mapping lives for the
     * process lifetime.  Zero the descriptor so it cannot be reused. */
    if (c)
        c->buf = NULL;
}

/* ---- Drawing ---- */

/* Convert a 0x00RRGGBB color to the framebuffer's 32bpp xRGB word:
 * the QEMU VGA 32bpp layout is [unused, R, G, B] in memory (little
 * endian byte1 = R), so a color's red byte must land at bit 8.  Window
 * buffers use the SAME xRGB words, so blits copy verbatim. */
static u32 GuiXrgb(u32 color) {
    u8 r  = (u8)(color >> 16);
    u8 g  = (u8)(color >> 8);
    u8 b  = (u8)(color);
    return ((u32)b << 24) | ((u32)g << 16) | ((u32)r << 8);
}

void GuiPixel(gui_canvas_t *c, int x, int y, u32 color) {
    if (!c || !c->buf)
        return;
    if (x < 0 || y < 0 || (u32)x >= c->w || (u32)y >= c->h)
        return;

    if (c->bpp == 32) {
        *(volatile u32 *)(c->buf + (u64)y * c->pitch + (u64)x * 4) = GuiXrgb(color);
    } else if (c->bpp == 24) {
        volatile u8 *p = c->buf + (u64)y * c->pitch + (u64)x * 3;
        p[0]           = (u8)(color);
        p[1]           = (u8)(color >> 8);
        p[2]           = (u8)(color >> 16);
    }
}

void GuiFill(gui_canvas_t *c, int x, int y, int w, int h, u32 color) {
    if (!c || !c->buf || w <= 0 || h <= 0)
        return;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x >= (int)c->w || y >= (int)c->h)
        return;
    if (x + w > (int)c->w)
        w = (int)c->w - x;
    if (y + h > (int)c->h)
        h = (int)c->h - y;
    if (w <= 0 || h <= 0)
        return;

    if (c->bpp == 32) {
        u32 px = GuiXrgb(color);
        for (int row = 0; row < h; row++) {
            volatile u32 *line = (volatile u32 *)(c->buf + (u64)(y + row) * c->pitch);
            for (int col = 0; col < w; col++)
                line[x + col] = px;
        }
    } else if (c->bpp == 24) {
        u8 b = (u8)(color);
        u8 g = (u8)(color >> 8);
        u8 r = (u8)(color >> 16);
        for (int row = 0; row < h; row++) {
            volatile u8 *line = c->buf + (u64)(y + row) * c->pitch;
            for (int col = 0; col < w; col++) {
                u32 off   = (u32)(x + col) * 3;
                line[off] = b;
                line[off + 1] = g;
                line[off + 2] = r;
            }
        }
    }
}

void GuiHline(gui_canvas_t *c, int x, int y, int len, u32 color) {
    if (len < 0) {
        x += len;
        len = -len;
    }
    GuiFill(c, x, y, len, 1, color);
}

void GuiVline(gui_canvas_t *c, int x, int y, int len, u32 color) {
    if (len < 0) {
        y += len;
        len = -len;
    }
    GuiFill(c, x, y, 1, len, color);
}

void GuiRect(gui_canvas_t *c, int x, int y, int w, int h, u32 color) {
    if (w <= 0 || h <= 0)
        return;
    GuiHline(c, x, y, w, color);
    GuiHline(c, x, y + h - 1, w, color);
    GuiVline(c, x, y, h, color);
    GuiVline(c, x + w - 1, y, h, color);
}

/* ---- Text (8x16 VGA font + 16x16 CJK font, UTF-8) ---- */

#include "font.h"
#include "../../lib/font_cjk.h"

/* Render one code point at *px (advanced by the glyph width). */
static void GuiTextCodepoint(gui_canvas_t *c, int *pxp, int y, u32 cp,
                               u32 fg, u32 bg) {
    int px = *pxp;
    if (cp > 0x7F) {
        const u8 *glyph = font_cjk_lookup(cp);
        if (glyph) {
            /* Double-width: 16x16 glyph, background both halves. */
            for (int row = 0; row < 16; row++) {
                u8 hi = glyph[row * 2];
                u8 lo = glyph[row * 2 + 1];
                for (int col = 0; col < 8; col++) {
                    if (hi & (0x80 >> col))
                        GuiPixel(c, px + col, y + row, fg);
                    else if (bg != 0)
                        GuiPixel(c, px + col, y + row, bg);
                }
                for (int col = 0; col < 8; col++) {
                    if (lo & (0x80 >> col))
                        GuiPixel(c, px + 8 + col, y + row, fg);
                    else if (bg != 0)
                        GuiPixel(c, px + 8 + col, y + row, bg);
                }
            }
            *pxp = px + 16;
            return;
        }
        cp = '?'; /* fall back to a visible placeholder */
    }
    if (cp < 0x20 || cp > 0x7E) {
        *pxp = px + 8;
        return;
    }
    const u8 *glyph = gui_font[cp - 0x20];
    for (int row = 0; row < 16; row++) {
        u8 bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                GuiPixel(c, px + col, y + row, fg);
            } else if (bg != 0) {
                GuiPixel(c, px + col, y + row, bg);
            }
        }
    }
    *pxp = px + 8;
}

void GuiText(gui_canvas_t *c, int x, int y, const char *s, u32 fg, u32 bg) {
    if (!c || !c->buf || !s)
        return;

    /* Incremental UTF-8 decoder. */
    u32 cp = 0;
    int  left = 0;

    int px = x;
    for (const char *p = s; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (left > 0) {
            if ((ch & 0xC0) == 0x80) {
                cp = (cp << 6) | (ch & 0x3F);
                if (--left == 0)
                    GuiTextCodepoint(c, &px, y, cp, fg, bg);
            } else {
                left = 0; /* malformed: drop */
            }
            continue;
        }
        if (ch >= 0xC2 && ch <= 0xDF) {
            cp = ch & 0x1F;
            left = 1;
        } else if (ch >= 0xE0 && ch <= 0xEF) {
            cp = ch & 0x0F;
            left = 2;
        } else if (ch >= 0xF0 && ch <= 0xF4) {
            cp = ch & 0x07;
            left = 3;
        } else if (ch < 0x80) {
            GuiTextCodepoint(c, &px, y, ch, fg, bg);
        } else {
            GuiTextCodepoint(c, &px, y, '?', fg, bg);
        }
    }
}

/* Pixel width of a UTF-8 string (ASCII 8px, CJK 16px). */
int GuiTextWidth(const char *s) {
    if (!s)
        return 0;
    int  w = 0;
    u32  cp = 0;
    int  left = 0;
    for (const char *p = s; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (left > 0) {
            if ((ch & 0xC0) == 0x80) {
                cp = (cp << 6) | (ch & 0x3F);
                if (--left == 0)
                    /* Width matches the renderer: 16px only when the
                     * CJK glyph actually exists (else 8px '?'). */
                    w += (cp > 0x7F && font_cjk_lookup(cp)) ? 16 : 8;
            } else {
                left = 0;
            }
            continue;
        }
        if (ch >= 0xC2 && ch <= 0xDF) {
            cp = ch & 0x1F;
            left = 1;
        } else if (ch >= 0xE0 && ch <= 0xEF) {
            cp = ch & 0x0F;
            left = 2;
        } else if (ch >= 0xF0 && ch <= 0xF4) {
            cp = ch & 0x07;
            left = 3;
        } else if (ch >= 0x20 && ch < 0x7F) {
            w += 8;
        } else if (ch >= 0x80) {
            w += 8; /* stray byte placeholder */
        }
    }
    return w;
}

/* ---- Blit (same-bpp region copy) ---- */

void GuiBlit(gui_canvas_t *dst, int dx, int dy,
              const gui_canvas_t *src, int sx, int sy, int w, int h) {
    if (!dst || !dst->buf || !src || !src->buf)
        return;
    if (dst->bpp != src->bpp || w <= 0 || h <= 0)
        return;
    if (sx < 0) {
        w += sx;
        dx -= sx;
        sx = 0;
    }
    if (sy < 0) {
        h += sy;
        dy -= sy;
        sy = 0;
    }
    if (dx < 0) {
        w += dx;
        sx -= dx;
        dx = 0;
    }
    if (dy < 0) {
        h += dy;
        sy -= dy;
        dy = 0;
    }
    if (w <= 0 || h <= 0)
        return;
    if (sx >= (int)src->w || sy >= (int)src->h || dx >= (int)dst->w || dy >= (int)dst->h)
        return;
    if (sx + w > (int)src->w)
        w = (int)src->w - sx;
    if (sy + h > (int)src->h)
        h = (int)src->h - sy;
    if (dx + w > (int)dst->w)
        w = (int)dst->w - dx;
    if (dy + h > (int)dst->h)
        h = (int)dst->h - dy;
    if (w <= 0 || h <= 0)
        return;

    if (src->bpp == 32) {
        for (int row = 0; row < h; row++) {
            const volatile u32 *sline =
                (const volatile u32 *)(src->buf + (u64)(sy + row) * src->pitch);
            volatile u32 *dline = (volatile u32 *)(dst->buf + (u64)(dy + row) * dst->pitch);
            for (int col = 0; col < w; col++)
                dline[dx + col] = sline[sx + col];
        }
    } else if (src->bpp == 24) {
        for (int row = 0; row < h; row++) {
            const volatile u8 *sline = src->buf + (u64)(sy + row) * src->pitch;
            volatile u8 *dline       = dst->buf + (u64)(dy + row) * dst->pitch;
            for (int col = 0; col < w; col++) {
                u32 so = (u32)(sx + col) * 3;
                u32 do_ = (u32)(dx + col) * 3;
                dline[do_]     = sline[so];
                dline[do_ + 1] = sline[so + 1];
                dline[do_ + 2] = sline[so + 2];
            }
        }
    }
}
