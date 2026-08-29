/*
 * gui.h - Pixel graphics library (v0.7.1, GUI round)
 * Copyright (c) 2026 OpSys Project
 *
 * Minimal pixel-drawing layer for the GUI services.  A gui_canvas_t is
 * just a pixel buffer descriptor — either the system framebuffer
 * (mapped via fb_get_info + fb_map, gated on ATOM_SERVICE_MANAGE like
 * the term service) or an off-screen window buffer owned by the
 * compositor.  All drawing is format-aware (32bpp ARGB word or 24bpp
 * BGR bytes, mirroring the kernel's fb_pixel layout) and clipped to
 * the canvas bounds.
 *
 * Colors are u32 values in 0x00RRGGBB layout (byte 0 = Blue, byte 2 =
 * Red — the framebuffer's native byte order); the 32bpp path stores
 * them verbatim, the 24bpp path splits them into B/G/R bytes.
 */

#ifndef USER_LIB_LIBGUI_GUI_H
#define USER_LIB_LIBGUI_GUI_H

#include <stdint.h>

typedef uint8_t  u8;
typedef uint32_t u32;
typedef int32_t  i32;
typedef uint64_t u64;

typedef struct {
    u32 w;     /* width in pixels   */
    u32 h;     /* height in pixels  */
    u32 pitch; /* bytes per row     */
    u32 bpp;   /* 32 or 24          */
    u8 *buf;   /* pixel buffer base */
} gui_canvas_t;

/* ---- Framebuffer (requires ATOM_SERVICE_MANAGE) ---- */

/* Query the fb descriptor and map it into this process.  Fills *c with
 * the geometry and buf = mapped base.  Returns 0 or a negative error. */
int gui_fb_open(gui_canvas_t *c);

/* Unmap the framebuffer mapping created by gui_fb_open. */
void gui_fb_close(gui_canvas_t *c);

/* ---- Drawing (any canvas, clipped to bounds) ---- */

void gui_pixel(gui_canvas_t *c, int x, int y, u32 color);

/* Fill a rectangle (inclusive of x..x+w-1, y..y+h-1). */
void gui_fill(gui_canvas_t *c, int x, int y, int w, int h, u32 color);

/* Horizontal / vertical lines. */
void gui_hline(gui_canvas_t *c, int x, int y, int len, u32 color);
void gui_vline(gui_canvas_t *c, int x, int y, int len, u32 color);

/* Outlined rectangle (1px border). */
void gui_rect(gui_canvas_t *c, int x, int y, int w, int h, u32 color);

/* Render a UTF-8 string (8x16 VGA font + 16x16 CJK font).  bg may be
 * 0 to skip background pixels (transparent text for title bars). */
void gui_text(gui_canvas_t *c, int x, int y, const char *s, u32 fg, u32 bg);

/* Pixel width of a UTF-8 string (8px per ASCII char, 16px per CJK). */
int gui_text_width(const char *s);

/* Copy a w*h region from src at (sx, sy) to dst at (dx, dy).  Source
 * and destination may overlap only when they are the same buffer;
 * otherwise they must be disjoint (the compositor blits window buffers
 * onto the fb).  Same-bpp copies only (window buffers are 32bpp). */
void gui_blit(gui_canvas_t *dst, int dx, int dy,
              const gui_canvas_t *src, int sx, int sy, int w, int h);

#endif /* USER_LIB_LIBGUI_GUI_H */
