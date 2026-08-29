/*
 * main.c - Window compositor service (v0.7.1, GUI round)
 * Copyright (c) 2026 OpSys Project
 *
 * Owns the display framebuffer (mapped via libgui, gated on
 * ATOM_SERVICE_MANAGE) and composes an off-screen window buffer per
 * window onto it.  Clients draw into their window over IPC (FILL/TEXT);
 * the compositor re-blits on every mutation and keeps the z-order so
 * the focused window sits on top with a highlighted title bar.
 *
 * Input: an input thread polls the keyboard (KBD_OP_READ) and the PS/2
 * mouse (KBD_OP_MOUSE_READ, both served by the keyboard service),
 * maintains the pointer position, hit-tests clicks to focus windows,
 * and pushes events into a shared ring drained by GUI_OP_POLL.
 *
 * Lifecycle: the service runs from boot but stays idle (no fb writes,
 * no focus) until a client calls GUI_OP_ACTIVATE; GUI_OP_DEACTIVATE
 * restores the term text screen via TERM_OP_REDRAW and releases the
 * keyboard focus.
 *
 * Two threads (mirror of term's perm.ui split): the server thread
 * handles the "gui" port; the input thread polls devices.  Both
 * serialize the window table / events / pointer on s_lock.
 */

#include "gui.h"

#include <libc/stdio.h>
#include <libc/string.h>
#include <libc/utf8.h>    /* UTF-8 decoding for the title bar */
#include <libgui/gui.h>
#include <libgui/font.h> /* gui_font: 8x16 glyphs for the title text */
#include <libos/syscalls.h>
#include <malloc.h> /* malloc / free for window buffers */
#include "../../lib/font_cjk.h" /* 16x16 CJK glyphs for UTF-8 titles */

/* Keyboard service protocol (mirror of keyboard.c — not shared) */
#define KBD_OP_READ      1
#define KBD_OP_MOUSE_READ 5

#define GUI_BG_COLOR 0x00202040 /* desktop background */
#define GUI_BORDER_COLOR  0x00A0A0A0
#define GUI_TITLE_FG      0x00FFFFFF
#define GUI_TITLE_BG      0x00204080 /* focused */
#define GUI_TITLE_BG_IDLE 0x00404040 /* unfocused */
#define GUI_FRAME_FG      0x00C0C0C0
#define GUI_PTR_COLOR     0x000000FF /* xRGB word: red byte at bit 8 (see gui_xrgb) */

/* Taskbar: a 22px strip at the bottom of the screen holding one button
 * per MINIMIZED window (click to restore).  Maximised windows leave
 * this strip visible so a minimized window can always be recovered. */
#define GUI_TASKBAR_H     22
#define GUI_TASKBAR_BG    0x00202028
#define GUI_TASKBAR_BTN   0x003C3C4C
#define GUI_TASKBAR_MAX_W 200 /* widest taskbar button */

/* IPC buffers (single server thread — same pattern as term/perm). */
static u8 s_req[GUI_IPC_MAX];
static u8 s_resp[GUI_IPC_MAX];

typedef struct {
    int          in_use;
    u64          owner; /* creating subject (unforgeable) */
    int          id;
    char         title[GUI_MAX_TITLE];
    i32          x, y; /* fb position of the border */
    i32          w, h; /* content size (excludes border+title bar) */
    gui_canvas_t buf;  /* off-screen content buffer (32bpp) */
    int          maxed; /* maximised (restore rect in rx/ry/rw/rh) */
    int          hidden; /* minimized: drawn as a taskbar button only */
    i32          rx, ry, rw, rh;
} gui_win_t;

static gui_win_t      s_wins[GUI_MAX_WINDOWS];
static gui_canvas_t   s_fb;
static int            s_active;   /* compositor on */
static int            s_focus_id; /* focused window id (0 = none) */
static int            s_next_id = 1;

static i32 s_ptr_x;
static i32 s_ptr_y;
static u8  s_ptr_buttons;

/* Title-bar dragging state (input thread).  While a window is being
 * dragged, pointer motion moves the window instead of the hit test;
 * releasing the button ends the drag. */
static int s_drag_id;    /* window being dragged (0 = none) */
static int s_press_grab; /* window that received the last button press
                          * (implicit pointer grab; 0 = none) */
/* Double-click detection (title-bar double-click = maximise). */
static int s_last_press_ticks;
static int s_last_press_x;
static int s_last_press_y;
static int s_last_press_win;
static int s_drag_offx;  /* pointer offset within the window border */
static int s_drag_offy;

static gui_event_t s_events[GUI_MAX_EVENTS];
static u32         s_ev_head;
static u32         s_ev_count;

static int s_lock = -1; /* mutex: window table + events + pointer */

static int s_kbd_port  = -1;
static int s_term_port = -1;

/* ------------------------------------------------------------------ */
/* Events                                                             */
/* ------------------------------------------------------------------ */

static gui_win_t *win_find(int id); /* fwd: used by ev_push */
static void gui_shutdown(void);     /* fwd: used by do_destroy / input thread */

/* Resolve the owner subject of a window id (0 = none). */
static u64 win_owner(int id) {
    gui_win_t *w = win_find(id);
    return w ? w->owner : 0;
}

static void ev_push(u32 type, u32 code, i32 x, i32 y, i32 win) {
    /* Event isolation: the event is routed to the owner of the target
     * window (KEY -> focused window; mouse -> window under the pointer).
     * No window = broadcast (owner 0): every polling client sees it. */
    u64 owner = 0;
    if (win != 0)
        owner = win_owner(win);
    if (s_ev_count >= GUI_MAX_EVENTS)
        return; /* ring full: drop the oldest-free slot semantics */
    u32 idx          = (s_ev_head + s_ev_count) % GUI_MAX_EVENTS;
    s_events[idx].type = type;
    s_events[idx].code = code;
    s_events[idx].x    = x;
    s_events[idx].y    = y;
    s_events[idx].win  = win;
    s_events[idx].owner = owner;
    s_ev_count++;
}

/* ------------------------------------------------------------------ */
/* Dirty-rectangle compositing                                         */
/*                                                                     */
/* Instead of repainting the whole 1024x768 framebuffer on every       */
/* event, every mutation records the rect it changed; gui_composite()  */
/* repaints ONLY that rect (background + the clipped parts of every    */
/* window intersecting it + the pointer).  Mouse movement therefore    */
/* redraws just a small square, which both cuts framebuffer writes and */
/* shrinks the VNC dirty region — the visible refresh-rate win.        */
/* ------------------------------------------------------------------ */

typedef struct {
    int x, y, w, h;
} gui_rect_t;

static gui_rect_t s_dirty;
static int        s_dirty_valid;

/* Lock-free variant: the caller already holds s_lock (used by the
 * pointer-input thread while dragging — the kernel mutex is NOT
 * recursive, so calling gui_dirty_add() there would release the lock
 * it is still holding). */
static void gui_dirty_add_nolock(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x >= (int)s_fb.w || y >= (int)s_fb.h || w <= 0 || h <= 0)
        return;
    if (x + w > (int)s_fb.w)
        w = (int)s_fb.w - x;
    if (y + h > (int)s_fb.h)
        h = (int)s_fb.h - y;
    if (!s_dirty_valid) {
        s_dirty.x = x;
        s_dirty.y = y;
        s_dirty.w = w;
        s_dirty.h = h;
        s_dirty_valid = 1;
        return;
    }
    /* Union into the pending rect. */
    {
        int x1 = s_dirty.x + s_dirty.w;
        int y1 = s_dirty.y + s_dirty.h;
        if (x < s_dirty.x)
            s_dirty.x = x;
        if (y < s_dirty.y)
            s_dirty.y = y;
        if (x + w > x1)
            x1 = x + w;
        if (y + h > y1)
            y1 = y + h;
        s_dirty.w = x1 - s_dirty.x;
        s_dirty.h = y1 - s_dirty.y;
    }
}

static void gui_dirty_add(int x, int y, int w, int h) {
    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    gui_dirty_add_nolock(x, y, w, h);
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);
}

static int rect_intersect(gui_rect_t a, gui_rect_t b, gui_rect_t *out) {
    int x1 = (a.x + a.w < b.x + b.w) ? a.x + a.w : b.x + b.w;
    int y1 = (a.y + a.h < b.y + b.h) ? a.y + a.h : b.y + b.h;
    int x0 = (a.x > b.x) ? a.x : b.x;
    int y0 = (a.y > b.y) ? a.y : b.y;
    if (x1 <= x0 || y1 <= y0)
        return 0;
    out->x = x0;
    out->y = y0;
    out->w = x1 - x0;
    out->h = y1 - y0;
    return 1;
}

/* Title-bar TEXT only (the caller fills the bar, clipped).  Each glyph
 * is drawn ROW BY ROW and only where it intersects the dirty rect:
 * drawing a whole 16px glyph would paint the lower window's title
 * rows that an overlapping higher window's clipped blit does not
 * cover (the "lower title shows through" artifact).  d may be NULL
 * for a full repaint. */
static void draw_titlebar(gui_win_t *w, const gui_rect_t *d) {
    int tx = w->x + GUI_BORDER + 3;
    int ty = w->y + GUI_BORDER;
    /* Title text (skip background: the bar is already filled).
     * Leave room on the right for the title-bar buttons. */
    int xr = w->x + w->w + GUI_BORDER - 1; /* right edge inside border */
    int max_px = xr - 42 - tx;             /* 3 buttons * 14px */
    char t[GUI_MAX_TITLE + 2];
    int  n  = 0;
    int  px = 0;
    /* Truncate by DISPLAY width (ASCII 8px, CJK 16px) so a UTF-8 title
     * is never cut in the middle of a character. */
    const char *p = w->title;
    while (*p && n < (int)sizeof(t) - 3) {
        u32 cp;
        int len = utf8_decode(p, &cp);
        if (len <= 0) {
            cp  = (u32)(u8)*p;
            len = 1;
        }
        int gw = (cp > 0x7F && font_cjk_lookup(cp)) ? 16 : 8;
        if (px + gw > max_px)
            break;
        memcpy(t + n, p, (size_t)len);
        n += len;
        px += gw;
        p += len;
    }
    t[n++] = ' ';
    t[n++] = '*'; /* focus marker on the focused window */
    t[n]   = '\0';
    int y0 = ty, y1 = ty + 16;
    if (d) {
        if (y1 <= d->y || y0 >= d->y + d->h)
            return; /* bar fully outside the dirty rect vertically */
        if (y0 < d->y)
            y0 = d->y;
        if (y1 > d->y + d->h)
            y1 = d->y + d->h;
    }
    int        gx = tx;
    const char *q = t;
    while (*q) {
        u32 cp;
        int len = utf8_decode(q, &cp);
        if (len <= 0) {
            cp  = (u32)(u8)q[0];
            len = 1;
        }
        int gw = 8;
        const u8 *glyph = NULL;
        if (cp > 0x7F) {
            glyph = font_cjk_lookup(cp);
            gw    = glyph ? 16 : 8;
            if (!glyph)
                cp = '?'; /* missing glyph: visible placeholder */
        }
        if (cp >= 0x20 && cp <= 0x7E)
            glyph = gui_font[cp - 0x20];
        if (!glyph) {
            gx += gw;
            q += len;
            continue; /* control char / unrenderable: skip */
        }
        /* X clip: only paint glyph columns inside the dirty rect — a
         * partially-covered glyph painted in full would leave its tail
         * on the overlapping window above. */
        int cx0 = 0, cx1 = gw;
        if (d) {
            if (gx + gw <= d->x || gx >= d->x + d->w) {
                gx += gw;
                q += len;
                continue; /* glyph fully outside the dirty rect */
            }
            if (gx < d->x)
                cx0 = d->x - gx;
            if (gx + gw > d->x + d->w)
                cx1 = d->x + d->w - gx;
        }
        /* Row-by-row paint (CJK: 16x16 glyph = 2 bytes per row). */
        for (int row = y0; row < y1; row++) {
            int r = row - ty;
            for (int col = cx0; col < cx1; col++) {
                int bit;
                if (gw == 16) {
                    u8 hi = glyph[r * 2];
                    u8 lo = glyph[r * 2 + 1];
                    bit = (col < 8) ? (hi & (0x80 >> col))
                                    : (lo & (0x80 >> (col - 8)));
                } else {
                    bit = glyph[r] & (0x80 >> col);
                }
                if (bit)
                    gui_pixel(&s_fb, gx + col, row, GUI_TITLE_FG);
            }
        }
        gx += gw;
        q += len;
    }

    /* Title-bar buttons: [min] [max] [close] at the right edge.
     * 12x10 px, 2px apart, only the focused window shows them. */
    if (w->id == s_focus_id) {
        int by = w->y + GUI_BORDER + 3;
        int bx = xr - 7; /* close (rightmost) */
        /* glyphs: close 'x', max '□', min '–' */
        for (int b = 0; b < 3; b++) {
            int cx = bx - b * 14;
            if (d && (cx + 6 <= d->x || cx - 6 >= d->x + d->w))
                continue;
            /* button background */
            for (int yy = 0; yy < 10; yy++)
                for (int xx = -6; xx <= 6; xx++)
                    gui_pixel(&s_fb, cx + xx, by + yy,
                              (b == 0) ? 0x00C04040 : 0x00404040);
            /* glyph */
            if (b == 0) { /* close: X */
                for (int k = -3; k <= 3; k++) {
                    gui_pixel(&s_fb, cx + k, by + 2 + k, 0x00FFFFFF);
                    gui_pixel(&s_fb, cx + k, by + 7 - k, 0x00FFFFFF);
                }
            } else if (b == 1) { /* max: square outline */
                for (int xx = -4; xx <= 4; xx++) {
                    gui_pixel(&s_fb, cx + xx, by + 1, 0x00FFFFFF);
                    gui_pixel(&s_fb, cx + xx, by + 8, 0x00FFFFFF);
                }
                for (int yy = 1; yy <= 8; yy++) {
                    gui_pixel(&s_fb, cx - 4, by + yy, 0x00FFFFFF);
                    gui_pixel(&s_fb, cx + 4, by + yy, 0x00FFFFFF);
                }
            } else { /* min: horizontal bar */
                for (int xx = -4; xx <= 4; xx++) {
                    gui_pixel(&s_fb, cx + xx, by + 4, 0x00FFFFFF);
                    gui_pixel(&s_fb, cx + xx, by + 5, 0x00FFFFFF);
                }
            }
        }
    }
}

static void gui_composite(void) {
    if (!s_active || !s_dirty_valid)
        return;
    if (s_lock >= 0)
        (void)mutex_lock(s_lock);

    gui_rect_t d = s_dirty;
    s_dirty_valid = 0;

    /* Desktop background inside the dirty rect. */
    gui_fill(&s_fb, d.x, d.y, d.w, d.h, GUI_BG_COLOR);

    /* Windows in creation order (later = on top; focus drawn last).
     * Each window repaints only its intersection with the dirty rect
     * (border + title bar are cheap whole-window draws; the content
     * blit is clipped). */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
            gui_win_t *w = &s_wins[i];
            if (!w->in_use || w->hidden)
                continue; /* minimized windows live on the taskbar only */
            if (pass == 0 && w->id == s_focus_id)
                continue; /* focused window goes in the top pass */
            if (pass == 1 && w->id != s_focus_id)
                continue;

            gui_rect_t wrect;
            wrect.x = w->x;
            wrect.y = w->y;
            wrect.w = w->w + 2 * GUI_BORDER;
            wrect.h = w->h + 2 * GUI_BORDER + GUI_TITLE_H;
            gui_rect_t clip;
            if (!rect_intersect(d, wrect, &clip))
                continue;

            /* Border frame: every edge is CLIPPED to the dirty rect.
             * Drawing the whole frame would repaint pixels that an
             * overlapping higher window already owns — its content
             * would show the lower window's border through it (the
             * "two windows visible at once" bug). */
            {
                int ww = w->w + 2 * GUI_BORDER;
                int wh = w->h + 2 * GUI_BORDER + GUI_TITLE_H;
                /* top edge */
                if (d.y <= w->y && w->y < d.y + d.h) {
                    int x0 = (w->x > d.x) ? w->x : d.x;
                    int x1 = (w->x + ww < d.x + d.w) ? w->x + ww : d.x + d.w;
                    if (x1 > x0)
                        gui_hline(&s_fb, x0, w->y, x1 - x0, GUI_BORDER_COLOR);
                }
                /* bottom edge */
                {
                    int by = w->y + wh - 1;
                    if (d.y <= by && by < d.y + d.h) {
                        int x0 = (w->x > d.x) ? w->x : d.x;
                        int x1 = (w->x + ww < d.x + d.w) ? w->x + ww : d.x + d.w;
                        if (x1 > x0)
                            gui_hline(&s_fb, x0, by, x1 - x0, GUI_BORDER_COLOR);
                    }
                }
                /* left edge */
                if (d.x <= w->x && w->x < d.x + d.w) {
                    int y0 = (w->y > d.y) ? w->y : d.y;
                    int y1 = (w->y + wh < d.y + d.h) ? w->y + wh : d.y + d.h;
                    if (y1 > y0)
                        gui_vline(&s_fb, w->x, y0, y1 - y0, GUI_BORDER_COLOR);
                }
                /* right edge */
                {
                    int rx = w->x + ww - 1;
                    if (d.x <= rx && rx < d.x + d.w) {
                        int y0 = (w->y > d.y) ? w->y : d.y;
                        int y1 = (w->y + wh < d.y + d.h) ? w->y + wh : d.y + d.h;
                        if (y1 > y0)
                            gui_vline(&s_fb, rx, y0, y1 - y0, GUI_BORDER_COLOR);
                    }
                }
            }

            /* Title bar: fill clipped to the dirty rect, text only when
             * the dirty rect touches the bar. */
            {
                gui_rect_t trect;
                trect.x = w->x + GUI_BORDER;
                trect.y = w->y + GUI_BORDER;
                trect.w = w->w;
                trect.h = GUI_TITLE_H;
                gui_rect_t tclip;
                if (rect_intersect(d, trect, &tclip)) {
                    gui_fill(&s_fb, tclip.x, tclip.y, tclip.w, tclip.h,
                             (w->id == s_focus_id) ? GUI_TITLE_BG
                                                    : GUI_TITLE_BG_IDLE);
                    /* Text clipped glyph-by-glyph to the dirty rect —
                     * never repaint a lower window's title onto an
                     * overlapping window's pixels. */
                    draw_titlebar(w, &d);
                }
            }

            /* Content blit: only the part of the window that changed. */
            int cx0 = w->x + GUI_BORDER;
            int cy0 = w->y + GUI_BORDER + GUI_TITLE_H;
            gui_rect_t cclip;
            gui_rect_t crect = {cx0, cy0, w->w, w->h};
            if (rect_intersect(d, crect, &cclip)) {
                gui_blit(&s_fb, cclip.x, cclip.y, &w->buf,
                         cclip.x - cx0, cclip.y - cy0, cclip.w, cclip.h);
            }
        }
    }

    /* Taskbar: a strip at the bottom with one button per minimized
     * window (click to restore).  Drawn above every window so a
     * maximised window can never hide it. */
    {
        gui_rect_t trect = {0, (i32)s_fb.h - GUI_TASKBAR_H, (i32)s_fb.w,
                            GUI_TASKBAR_H};
        gui_rect_t tclip;
        if (rect_intersect(d, trect, &tclip)) {
            gui_fill(&s_fb, tclip.x, tclip.y, tclip.w, tclip.h,
                     GUI_TASKBAR_BG);
            int bx = 4;
            for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
                gui_win_t *w = &s_wins[i];
                if (!w->in_use || !w->hidden)
                    continue;
                int tw = gui_text_width(w->title);
                int bw = 12 + tw;
                if (bw > GUI_TASKBAR_MAX_W)
                    bw = GUI_TASKBAR_MAX_W;
                int by = (i32)s_fb.h - GUI_TASKBAR_H + 5;
                gui_fill(&s_fb, bx, by, bw, GUI_TASKBAR_H - 10,
                         GUI_TASKBAR_BTN);
                /* Button label: truncate the title by display width. */
                char lbl[GUI_MAX_TITLE];
                int  llen = 0, lpx = 0;
                const char *p = w->title;
                while (*p && lpx + 8 < bw - 10 && llen < (int)sizeof(lbl) - 1) {
                    u32 cp;
                    int n = utf8_decode(p, &cp);
                    if (n <= 0) {
                        cp = (u32)(u8)*p;
                        n  = 1;
                    }
                    int gw = (cp > 0x7F && font_cjk_lookup(cp)) ? 16 : 8;
                    if (lpx + gw >= bw - 10)
                        break;
                    memcpy(lbl + llen, p, (size_t)n);
                    llen += n;
                    lpx += gw;
                    p += n;
                }
                lbl[llen] = '\0';
                gui_text(&s_fb, bx + 5, by + 3, lbl, 0x00E0E0E0, GUI_TASKBAR_BTN);
                bx += bw + 4;
                if (bx >= (int)s_fb.w)
                    break;
            }
        }
    }

    /* Pointer: a small filled square (drawn last, on top). */
    if (s_ptr_x >= 0 && s_ptr_y >= 0) {
        gui_rect_t prect = {s_ptr_x, s_ptr_y, 5, 5};
        gui_rect_t pclip;
        if (rect_intersect(d, prect, &pclip))
            gui_fill(&s_fb, s_ptr_x, s_ptr_y, 5, 5, GUI_PTR_COLOR);
    }

    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);
}

/* ------------------------------------------------------------------ */
/* Window ops (server thread)                                          */
/* ------------------------------------------------------------------ */

static gui_win_t *win_find(int id) {
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (s_wins[i].in_use && s_wins[i].id == id)
            return &s_wins[i];
    return NULL;
}

/* Reallocate the content buffer to (nw, nh), preserving the top-left
 * overlapping pixels (RESIZE, and title-bar maximise/restore — the
 * off-screen buffer must always match w/h or the composite blit reads
 * out of bounds and the maximised window renders only partially). */
static int gui_win_realloc(gui_win_t *w, int nw, int nh) {
    size_t bytes = (size_t)nw * nh * 4;
    u8    *nbuf  = (u8 *)malloc(bytes);
    if (!nbuf)
        return -1;
    memset(nbuf, 0, bytes);
    int copyw = (nw < w->w) ? nw : w->w;
    int copyh = (nh < w->h) ? nh : w->h;
    for (int yy = 0; yy < copyh; yy++)
        for (int xx = 0; xx < copyw; xx++) {
            u32 v = 0;
            if (w->buf.buf)
                v = ((u32 *)w->buf.buf)[yy * w->w + xx];
            ((u32 *)nbuf)[yy * nw + xx] = v;
        }
    if (w->buf.buf)
        free((void *)w->buf.buf);
    w->buf.buf   = nbuf;
    w->buf.w     = (u32)nw;
    w->buf.h     = (u32)nh;
    w->buf.pitch = (u32)nw * 4;
    w->buf.bpp   = 32;
    w->w         = nw;
    w->h         = nh;
    return 0;
}

/* Number of live windows (hidden/minimized included). */
static int gui_win_count(void) {
    int n = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (s_wins[i].in_use)
            n++;
    return n;
}

/* Minimized-window taskbar button under (px, py): the window id, or 0.
 * Buttons lay left-to-right in window-slot order (must match the
 * composite's taskbar drawing exactly). */
static int taskbar_button_at(i32 px, i32 py) {
    if (py < (i32)s_fb.h - GUI_TASKBAR_H || py >= (i32)s_fb.h)
        return 0;
    int bx = 4;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        gui_win_t *w = &s_wins[i];
        if (!w->in_use || !w->hidden)
            continue;
        int bw = 12 + gui_text_width(w->title);
        if (bw > GUI_TASKBAR_MAX_W)
            bw = GUI_TASKBAR_MAX_W;
        if (px >= bx && px < bx + bw)
            return w->id;
        bx += bw + 4;
        if (bx >= (i32)s_fb.w)
            break;
    }
    return 0;
}

/* Dirty the taskbar strip (a minimize/restore changed its buttons).
 * The caller must already hold s_lock (both call sites are inside the
 * input thread's press handling). */
static void gui_dirty_taskbar(void) {
    gui_dirty_add_nolock(0, (i32)s_fb.h - GUI_TASKBAR_H, (i32)s_fb.w, GUI_TASKBAR_H);
}

static void do_create(int token, int msg_len, u64 caller) {
    gui_resp_t *resp = (gui_resp_t *)s_resp;
    resp->ret        = -2; /* ERR_INVAL */
    if (msg_len < (int)(8 + sizeof(gui_req_create_t))) {
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    gui_req_t  *req  = (gui_req_t *)s_req;
    gui_req_create_t *c = (gui_req_create_t *)req->data;
    if (c->w <= 0 || c->h <= 0 || c->w > 1024 || c->h > 768) {
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }

    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    gui_win_t *slot = NULL;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!s_wins[i].in_use) {
            slot = &s_wins[i];
            break;
        }
    }
    if (!slot) {
        if (s_lock >= 0)
            (void)mutex_unlock(s_lock);
        resp->ret = -1; /* ERR_NOMEM */
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }

    /* Off-screen content buffer (32bpp). */
    size_t bytes = (size_t)c->w * c->h * 4;
    u8    *buf   = (u8 *)malloc(bytes);
    if (!buf) {
        if (s_lock >= 0)
            (void)mutex_unlock(s_lock);
        resp->ret = -1;
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }

    memset(slot, 0, sizeof(*slot));
    slot->in_use = 1;
    slot->owner  = caller;
    slot->id     = s_next_id++;
    strncpy(slot->title, c->title, sizeof(slot->title) - 1);
    slot->title[sizeof(slot->title) - 1] = '\0';
    slot->w = c->w;
    slot->h = c->h;
    /* Auto-placement: walk a cascade grid and pick the first slot that
     * does NOT overlap an existing window — new windows never stack
     * their borders over an existing one by default.  (Clients may
     * still MOVE a window anywhere, including on top, and drag the
     * title bar to rearrange.) */
    {
        int ww = c->w + 2 * GUI_BORDER;
        int wh = c->h + 2 * GUI_BORDER + GUI_TITLE_H;
        int px = 20, py = 20;
        for (int attempt = 0; attempt < 400; attempt++) {
            int free = 1;
            for (int j = 0; j < GUI_MAX_WINDOWS; j++) {
                if (!s_wins[j].in_use)
                    continue;
                int ox = s_wins[j].x, oy = s_wins[j].y;
                int ow = s_wins[j].w + 2 * GUI_BORDER;
                int oh = s_wins[j].h + 2 * GUI_BORDER + GUI_TITLE_H;
                if (px < ox + ow && ox < px + ww && py < oy + oh && oy < py + wh) {
                    free = 0;
                    break;
                }
            }
            if (free)
                break;
            px += 24;
            py += 18;
            if (px + ww + 20 > (int)s_fb.w) {
                px = 20;
                py += 130;
            }
            if (py + wh > (int)s_fb.h)
                py = 20; /* wrapped: accept overlap rather than fail */
        }
        slot->x = px;
        slot->y = py;
    }
    slot->buf.w     = (u32)c->w;
    slot->buf.h     = (u32)c->h;
    slot->buf.pitch = (u32)c->w * 4;
    slot->buf.bpp   = 32;
    slot->buf.buf   = buf;
    gui_fill(&slot->buf, 0, 0, c->w, c->h, 0x00000000);

    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);

    resp->ret = slot->id;
    gui_dirty_add(slot->x, slot->y, slot->w + 2 * GUI_BORDER,
                  slot->h + 2 * GUI_BORDER + GUI_TITLE_H);
    gui_composite();
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_destroy(int token, int msg_len, u64 caller) {
    gui_resp_t *resp = (gui_resp_t *)s_resp;
    resp->ret        = -4; /* ERR_NOENT */
    if (msg_len < (int)(8 + 4)) {
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    gui_req_t *req = (gui_req_t *)s_req;
    int        id  = ((i32 *)req->data)[0];

    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    gui_win_t *w = win_find(id);
    if (!w || (w->owner != 0 && w->owner != caller)) {
        if (s_lock >= 0)
            (void)mutex_unlock(s_lock);
        resp->ret = -4;
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    /* Capture the geometry BEFORE freeing the slot (needed for the
     * dirty rect, which must be recorded outside the lock). */
    int dx = w->x, dy = w->y;
    int dw = w->w + 2 * GUI_BORDER;
    int dh = w->h + 2 * GUI_BORDER + GUI_TITLE_H;
    free(w->buf.buf);
    memset(w, 0, sizeof(*w));
    if (s_focus_id == id)
        s_focus_id = 0;
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);

    resp->ret = 0;
    gui_dirty_add(dx, dy, dw, dh);
    if (gui_win_count() == 0) {
        /* Last window destroyed: hand the screen back to the shell
         * (the term repaints its text screen). */
        gui_shutdown();
    } else {
        gui_composite();
    }
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_move(int token, int msg_len, u64 caller) {
    gui_resp_t *resp = (gui_resp_t *)s_resp;
    resp->ret        = -4;
    if (msg_len < (int)(8 + 12)) {
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    gui_req_t *req = (gui_req_t *)s_req;
    i32       *a   = (i32 *)req->data;

    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    gui_win_t *w = win_find(a[0]);
    if (!w || (w->owner != 0 && w->owner != caller)) {
        if (s_lock >= 0)
            (void)mutex_unlock(s_lock);
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    int ox = w->x, oy = w->y;
    int ow = w->w + 2 * GUI_BORDER;
    int oh = w->h + 2 * GUI_BORDER + GUI_TITLE_H;
    /* Clamp so the window can never be pushed fully off-screen (a
     * lost window cannot be clicked back). */
    int nx = a[1], ny = a[2];
    int maxx = (int)s_fb.w - ow;
    int maxy = (int)s_fb.h - oh;
    if (nx < 0)
        nx = 0;
    if (ny < 0)
        ny = 0;
    if (nx > maxx)
        nx = maxx;
    if (ny > maxy)
        ny = maxy;
    w->x = nx;
    w->y = ny;
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);

    resp->ret = 0;
    gui_dirty_add(ox, oy, ow, oh); /* old spot */
    gui_dirty_add(w->x, w->y, ow, oh); /* new spot */
    gui_composite();
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* RESIZE: {id; w; h} — reallocate the content buffer and clamp the
 * window inside the screen.  Only the owner may resize. */
static void do_resize(int token, int msg_len, u64 caller) {
    gui_resp_t *resp = (gui_resp_t *)s_resp;
    resp->ret        = -4;
    if (msg_len < (int)(8 + 12)) {
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    gui_req_t        *req = (gui_req_t *)s_req;
    gui_req_resize_t *r   = (gui_req_resize_t *)req->data;
    if (r->w < 16 || r->h < 16 || r->w > (i32)s_fb.w - 2 * GUI_BORDER ||
        r->h > (i32)s_fb.h - 2 * GUI_BORDER - GUI_TITLE_H) {
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }

    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    gui_win_t *w = win_find(r->id);
    if (!w || (w->owner != 0 && w->owner != caller)) {
        if (s_lock >= 0)
            (void)mutex_unlock(s_lock);
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    int ox = w->x, oy = w->y;
    int ow = w->w + 2 * GUI_BORDER;
    int oh = w->h + 2 * GUI_BORDER + GUI_TITLE_H;

    /* Allocate the new buffer before touching state (realloc preserves
     * the overlapping top-left of the old content). */
    if (gui_win_realloc(w, (int)r->w, (int)r->h) < 0) {
        if (s_lock >= 0)
            (void)mutex_unlock(s_lock);
        resp->ret = -1;
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }

    /* Clamp the window back inside the screen. */
    int maxx = (int)s_fb.w - (w->w + 2 * GUI_BORDER);
    int maxy = (int)s_fb.h - (w->h + 2 * GUI_BORDER + GUI_TITLE_H);
    if (w->x > maxx)
        w->x = maxx;
    if (w->y > maxy)
        w->y = maxy;
    if (w->x < 0)
        w->x = 0;
    if (w->y < 0)
        w->y = 0;
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);

    resp->ret = 0;
    gui_dirty_add(ox, oy, ow, oh); /* old spot */
    gui_dirty_add(w->x, w->y, w->w + 2 * GUI_BORDER,
                  w->h + 2 * GUI_BORDER + GUI_TITLE_H);
    gui_composite();
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_focus(int token, int msg_len, u64 caller) {
    gui_resp_t *resp = (gui_resp_t *)s_resp;
    resp->ret        = -4;
    if (msg_len < (int)(8 + 4)) {
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    gui_req_t *req = (gui_req_t *)s_req;
    int        id  = ((i32 *)req->data)[0];

    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    gui_win_t *w = win_find(id);
    if (!w || (w->owner != 0 && w->owner != caller)) {
        if (s_lock >= 0)
            (void)mutex_unlock(s_lock);
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    int old_focus = s_focus_id;
    s_focus_id    = id;
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);

    resp->ret = 0;
    /* Title-bar highlight changed on the old and the new focused
     * window — redraw both whole windows. */
    if (old_focus != id) {
        int ox = 0, oy = 0, ow = 0, oh = 0;
        if (s_lock >= 0)
            (void)mutex_lock(s_lock);
        gui_win_t *old = win_find(old_focus);
        if (old) {
            ox = old->x;
            oy = old->y;
            ow = old->w + 2 * GUI_BORDER;
            oh = old->h + 2 * GUI_BORDER + GUI_TITLE_H;
        }
        if (s_lock >= 0)
            (void)mutex_unlock(s_lock);
        if (ow > 0)
            gui_dirty_add(ox, oy, ow, oh);
        gui_dirty_add(w->x, w->y, w->w + 2 * GUI_BORDER,
                      w->h + 2 * GUI_BORDER + GUI_TITLE_H);
    }
    gui_composite();
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_fill(int token, int msg_len, u64 caller) {
    gui_resp_t *resp = (gui_resp_t *)s_resp;
    resp->ret        = -4;
    if (msg_len < (int)(8 + sizeof(gui_req_fill_t))) {
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    gui_req_t      *req = (gui_req_t *)s_req;
    gui_req_fill_t *f   = (gui_req_fill_t *)req->data;

    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    gui_win_t *w = win_find(f->id);
    if (!w || (w->owner != 0 && w->owner != caller)) {
        if (s_lock >= 0)
            (void)mutex_unlock(s_lock);
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    gui_fill(&w->buf, f->x, f->y, f->w, f->h, f->color);
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);

    resp->ret = 0;
    gui_dirty_add(w->x + GUI_BORDER + f->x, w->y + GUI_BORDER + GUI_TITLE_H + f->y,
                  f->w, f->h);
    gui_composite();
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_text(int token, int msg_len, u64 caller) {
    gui_resp_t *resp = (gui_resp_t *)s_resp;
    resp->ret        = -4;
    if (msg_len < (int)(8 + 20)) { /* header fields + at least empty text */
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    gui_req_t      *req = (gui_req_t *)s_req;
    gui_req_text_t *t   = (gui_req_text_t *)req->data;
    /* Text length is implicit: whatever the sender actually put in the
     * message.  Never strlen() past the received bytes — s_req is a
     * static buffer that retains the previous request's tail. */
    int text_len = msg_len - (int)(8 + offsetof(gui_req_text_t, text));
    if (text_len < 0)
        text_len = 0;
    if (text_len > (int)sizeof(t->text) - 1)
        text_len = (int)sizeof(t->text) - 1;
    t->text[text_len] = '\0';

    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    gui_win_t *w = win_find(t->id);
    if (!w || (w->owner != 0 && w->owner != caller)) {
        if (s_lock >= 0)
            (void)mutex_unlock(s_lock);
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    gui_text(&w->buf, t->x, t->y, t->text, t->fg, t->bg);
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);

    resp->ret = 0;
    gui_dirty_add(w->x + GUI_BORDER + t->x, w->y + GUI_BORDER + GUI_TITLE_H + t->y,
                  gui_text_width(t->text), 16);
    gui_composite();
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* Title-bar button hit test: 1=close, 2=max, 3=min, 0=none. */
static int title_button_at(gui_win_t *w, int px, int py) {
    if (!w)
        return 0;
    int by = w->y + GUI_BORDER + 3;
    if (py < by || py >= by + 10)
        return 0;
    int xr = w->x + w->w + GUI_BORDER - 1;
    for (int b = 0; b < 3; b++) {
        int cx = xr - 7 - b * 14;
        if (px >= cx - 6 && px < cx + 6)
            return b + 1; /* 1 close, 2 max, 3 min */
    }
    return 0;
}

static void do_poll(int token, u64 caller) {
    gui_resp_poll_t *resp = (gui_resp_poll_t *)s_resp;
    resp->ret             = 0;

    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    resp->count = 0;
    /* Event isolation: consume events addressed to this client (owner
     * == caller subject) or broadcast (owner == 0); compact the ring,
     * keeping other windows' events for their owners.  The ring holds
     * at most GUI_MAX_EVENTS so the response can never overflow. */
    u32 n   = s_ev_count;
    u32 src = s_ev_head;
    u32 dst = s_ev_head;
    for (u32 i = 0; i < n; i++) {
        gui_event_t e = s_events[src];
        src = (src + 1) % GUI_MAX_EVENTS;
        if (e.owner != 0 && e.owner != caller) {
            s_events[dst] = e; /* not ours: keep */
            dst = (dst + 1) % GUI_MAX_EVENTS;
        } else {
            resp->events[resp->count++] = e; /* ours: deliver */
        }
    }
    s_ev_head  = dst;
    s_ev_count = n - resp->count;
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);

    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_pointer(int token) {
    gui_resp_t *resp = (gui_resp_t *)s_resp;
    resp->ret        = 0;
    i32        *p    = (i32 *)resp->data;
    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    p[0] = s_ptr_x;
    p[1] = s_ptr_y;
    p[2] = (i32)s_ptr_buttons;
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_activate(int token, u64 caller) {
    gui_resp_t *resp = (gui_resp_t *)s_resp;
    (void)caller;

    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    s_active      = 1;
    s_ptr_x       = (i32)s_fb.w / 2;
    s_ptr_y       = (i32)s_fb.h / 2;
    s_ptr_buttons = 0;
    s_focus_id    = 0;
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);

    /* Take the keyboard focus (keys now route to the GUI, not the
     * shell's parked read). */
    if (s_kbd_port >= 0) {
        u32 req[2] = {3, 0}; /* KBD_OP_TAKE_FOCUS */
        u8  rsp[4];
        int rl = (int)sizeof(rsp);
        (void)ipc_call(s_kbd_port, req, 8, rsp, &rl);
    }

    resp->ret = 0;
    gui_dirty_add(0, 0, (int)s_fb.w, (int)s_fb.h);
    gui_composite();
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* Release the compositor: keyboard focus back to the shell, term
 * screen restored, compositor idle.  Must be called WITHOUT s_lock
 * held (it IPC-calls the keyboard and term services while the input
 * thread may be parked in do_poll). */
static void gui_shutdown(void) {
    /* Release the keyboard focus. */
    if (s_kbd_port >= 0) {
        u32 req[2] = {4, 0}; /* KBD_OP_RELEASE_FOCUS */
        u8  rsp[4];
        int rl = (int)sizeof(rsp);
        (void)ipc_call(s_kbd_port, req, 8, rsp, &rl);
    }

    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    s_active      = 0;
    s_dirty_valid = 0; /* drop any pending rect: the screen is handed back */
    s_focus_id    = 0;
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);

    /* Restore the text screen (term repaints from its cell buffer). */
    if (s_term_port >= 0) {
        u32 req[2] = {11, 0}; /* TERM_OP_REDRAW */
        u8  rsp[4];
        int rl = (int)sizeof(rsp);
        (void)ipc_call(s_term_port, req, 8, rsp, &rl);
    }
}

static void do_deactivate(int token) {
    gui_resp_t *resp = (gui_resp_t *)s_resp;
    gui_shutdown();
    resp->ret = 0;
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* ------------------------------------------------------------------ */
/* Server thread                                                       */
/* ------------------------------------------------------------------ */

static void gui_server_loop(int port) {
    for (;;) {
        int msg_len = (int)sizeof(s_req);
        int token   = 0;
        u64 caller  = 0;
        int ret     = ipc_recv_from(port, s_req, &msg_len, &token, &caller);
        if (ret < 0) {
            printf("gui: ipc_recv failed (%d)\n", ret);
            thread_exit(1);
        }
        if (msg_len < 8) {
            gui_resp_t *resp = (gui_resp_t *)s_resp;
            resp->ret        = -2;
            (void)ipc_reply(token, resp, (int)sizeof(*resp));
            continue;
        }
        gui_req_t *req = (gui_req_t *)s_req;
        switch (req->op) {
        case GUI_OP_CREATE:    do_create(token, msg_len, caller); break;
        case GUI_OP_DESTROY:   do_destroy(token, msg_len, caller); break;
        case GUI_OP_MOVE:      do_move(token, msg_len, caller); break;
        case GUI_OP_FOCUS:     do_focus(token, msg_len, caller); break;
        case GUI_OP_FILL:      do_fill(token, msg_len, caller); break;
        case GUI_OP_TEXT:      do_text(token, msg_len, caller); break;
        case GUI_OP_POLL:      do_poll(token, caller); break;
        case GUI_OP_POINTER:   do_pointer(token); break;
        case GUI_OP_ACTIVATE:  do_activate(token, caller); break;
        case GUI_OP_DEACTIVATE: do_deactivate(token); break;
        case GUI_OP_RESIZE:    do_resize(token, msg_len, caller); break;
        default: {
            gui_resp_t *resp = (gui_resp_t *)s_resp;
            resp->ret        = -2;
            (void)ipc_reply(token, resp, (int)sizeof(*resp));
            break;
        }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Input thread (keyboard + mouse polling)                             */
/* ------------------------------------------------------------------ */

/* Topmost window under (px, py), or 0. */
/* Hit-test in the SAME order the compositor paints: the focused
 * window is drawn last (top), everything else in slot order with
 * later slots on top.  A pointer click must route to the window the
 * user actually sees at that pixel. */
static int hit_test(i32 px, i32 py) {
    gui_win_t *f = win_find(s_focus_id);
    if (f && f->in_use && !f->hidden &&
        px >= f->x && px < f->x + f->w + 2 * GUI_BORDER &&
        py >= f->y && py < f->y + f->h + 2 * GUI_BORDER + GUI_TITLE_H)
        return f->id;
    for (int i = GUI_MAX_WINDOWS - 1; i >= 0; i--) {
        gui_win_t *w = &s_wins[i];
        if (!w->in_use || w->hidden || w->id == s_focus_id)
            continue;
        if (px >= w->x && px < w->x + w->w + 2 * GUI_BORDER &&
            py >= w->y && py < w->y + w->h + 2 * GUI_BORDER + GUI_TITLE_H)
            return w->id;
    }
    return 0;
}

static void gui_input_main(void *arg) {
    (void)arg;

    for (;;) {
        if (!s_active) {
            (void)sleep(5);
            continue;
        }

        int changed = 0;

        /* Keyboard: non-blocking READ (keys route here while focused). */
        if (s_kbd_port >= 0) {
            u32 req[2] = {KBD_OP_READ, 8};
            u8  resp[4 + 8];
            int rl = (int)sizeof(resp);
            if (ipc_call(s_kbd_port, req, 8, resp, &rl) == 0 && rl >= 4) {
                i32 n = (i32)((u32 *)resp)[0];
                if (n > 8)
                    n = 8;
                for (i32 i = 0; i < n; i++) {
                    if (s_lock >= 0)
                        (void)mutex_lock(s_lock);
                    ev_push(GUI_EV_KEY, (u8)resp[4 + i], s_ptr_x, s_ptr_y, s_focus_id);
                    if (s_lock >= 0)
                        (void)mutex_unlock(s_lock);
                }
            }
        }

        /* Mouse: accumulated deltas + buttons + wheel. */
        if (s_kbd_port >= 0) {
            u32 req[2] = {KBD_OP_MOUSE_READ, 16};
            u8  resp[4 + 16];
            int rl = (int)sizeof(resp);
            if (ipc_call(s_kbd_port, req, 8, resp, &rl) == 0 && rl >= 4 + 16) {
                i32 dx = ((i32 *)(resp + 4))[0];
                i32 dy = ((i32 *)(resp + 4))[1];
                u8  bt = (u8)((i32 *)(resp + 4))[2];
                i32 wh = ((i32 *)(resp + 4))[3];
                if (wh != 0) {
                    /* Wheel: deliver to the window under the pointer. */
                    ev_push(GUI_EV_WHEEL, (u32)wh, s_ptr_x, s_ptr_y,
                            hit_test(s_ptr_x, s_ptr_y));
                    changed = 1;
                }
                if (dx != 0 || dy != 0) {
                    int ox, oy;
                    if (s_lock >= 0)
                        (void)mutex_lock(s_lock);
                    ox = s_ptr_x;
                    oy = s_ptr_y;
                    s_ptr_x += dx;
                    s_ptr_y -= dy; /* PS/2 Y is up-positive */
                    if (s_ptr_x < 0)
                        s_ptr_x = 0;
                    if (s_ptr_y < 0)
                        s_ptr_y = 0;
                    if (s_ptr_x >= (i32)s_fb.w)
                        s_ptr_x = (i32)s_fb.w - 1;
                    if (s_ptr_y >= (i32)s_fb.h)
                        s_ptr_y = (i32)s_fb.h - 1;
                    ev_push(GUI_EV_MOUSEMOVE, 0, s_ptr_x, s_ptr_y, hit_test(s_ptr_x, s_ptr_y));
                    /* Dragging: move the window under the pointer. */
                    if (s_drag_id != 0) {
                        gui_win_t *dw = win_find(s_drag_id);
                        if (dw) {
                            int nx = s_ptr_x - s_drag_offx;
                            int ny = s_ptr_y - s_drag_offy;
                            if (nx + dw->w + 2 * GUI_BORDER > (i32)s_fb.w)
                                nx = (i32)s_fb.w - dw->w - 2 * GUI_BORDER;
                            if (ny + dw->h + 2 * GUI_BORDER + GUI_TITLE_H >
                                (i32)s_fb.h)
                                ny = (i32)s_fb.h - dw->h - 2 * GUI_BORDER -
                                     GUI_TITLE_H;
                            if (nx < 0)
                                nx = 0;
                            if (ny < 0)
                                ny = 0;
                            if (nx != dw->x || ny != dw->y) {
                                /* Caller (input thread) already holds
                                 * s_lock: use the nolock dirty add. */
                                gui_dirty_add_nolock(dw->x, dw->y,
                                                     dw->w + 2 * GUI_BORDER,
                                                     dw->h + 2 * GUI_BORDER +
                                                         GUI_TITLE_H);
                                dw->x = nx;
                                dw->y = ny;
                                gui_dirty_add_nolock(dw->x, dw->y,
                                                     dw->w + 2 * GUI_BORDER,
                                                     dw->h + 2 * GUI_BORDER +
                                                         GUI_TITLE_H);
                            }
                        }
                    }
                    if (s_lock >= 0)
                        (void)mutex_unlock(s_lock);
                    /* Redraw only the old + new pointer squares. */
                    gui_dirty_add(ox - 1, oy - 1, 7, 7);
                    gui_dirty_add(s_ptr_x - 1, s_ptr_y - 1, 7, 7);
                    changed = 1;
                }
                /* Button transitions: press focuses the hit window and
                 * starts a title-bar drag; release ends it. */
                if (bt != s_ptr_buttons) {
                    int focus_changed = 0;
                    int fx = 0, fy = 0, fw = 0, fh = 0;
                    int shutdown = 0; /* auto-deactivate after the last window closes */
                    if (s_lock >= 0)
                        (void)mutex_lock(s_lock);
                    u8 pressed = bt & ~s_ptr_buttons;
                    u8 released = s_ptr_buttons & ~bt;
                    s_ptr_buttons = bt;
                    if (pressed) {
                        /* Taskbar: a click on a minimized window's button
                         * restores it (takes priority over the hit test —
                         * hidden windows are not hit-testable). */
                        int tb = taskbar_button_at(s_ptr_x, s_ptr_y);
                        if (tb != 0) {
                            gui_win_t *tw = win_find(tb);
                            if (tw) {
                                tw->hidden = 0;
                                gui_dirty_add_nolock(tw->x, tw->y,
                                                     tw->w + 2 * GUI_BORDER,
                                                     tw->h + 2 * GUI_BORDER +
                                                         GUI_TITLE_H);
                                gui_dirty_taskbar();
                                if (s_focus_id != tb) {
                                    gui_win_t *old = win_find(s_focus_id);
                                    if (old) {
                                        fx = old->x;
                                        fy = old->y;
                                        fw = old->w + 2 * GUI_BORDER;
                                        fh = old->h + 2 * GUI_BORDER + GUI_TITLE_H;
                                    }
                                    focus_changed = 1;
                                }
                                s_focus_id = tb;
                                changed    = 1;
                            }
                            s_press_grab = tb;
                            ev_push(GUI_EV_BUTTON, 1, s_ptr_x, s_ptr_y, tb);
                            goto button_done;
                        }
                        int hit = hit_test(s_ptr_x, s_ptr_y);
                        if (hit != 0) {
                            gui_win_t *hw = win_find(hit);
                            int in_title = hw && s_ptr_y >= hw->y &&
                                           s_ptr_y < hw->y + GUI_BORDER + GUI_TITLE_H;
                            /* Double-click on the title bar (not on a
                             * button) toggles maximise.  Only the FOCUSED
                             * window reacts: on an unfocused window the
                             * buttons are invisible, so a press there just
                             * focuses it (the second click of a fast pair
                             * already lands on the now-focused window). */
                            if (in_title && hit == s_focus_id &&
                                title_button_at(hw, s_ptr_x, s_ptr_y) == 0) {
                                int now = get_time();
                                if (hit == s_last_press_win &&
                                    now - s_last_press_ticks < 30 &&
                                    s_ptr_x > s_last_press_x - 8 && s_ptr_x < s_last_press_x + 8 &&
                                    s_ptr_y > s_last_press_y - 8 && s_ptr_y < s_last_press_y + 8) {
                                    int ox = hw->x, oy = hw->y;
                                    int ow = hw->w + 2 * GUI_BORDER;
                                    int oh = hw->h + 2 * GUI_BORDER + GUI_TITLE_H;
                                    int nw, nh;
                                    if (hw->maxed) {
                                        nw = hw->rw;
                                        nh = hw->rh;
                                    } else {
                                        /* Save the restore rect BEFORE the
                                         * realloc changes w/h. */
                                        hw->rx = hw->x;
                                        hw->ry = hw->y;
                                        hw->rw = hw->w;
                                        hw->rh = hw->h;
                                        nw = (i32)s_fb.w - 2 * GUI_BORDER;
                                        nh = (i32)s_fb.h - GUI_TASKBAR_H -
                                             2 * GUI_BORDER - GUI_TITLE_H;
                                    }
                                    /* Reallocate first so the composite
                                     * blit never reads out of bounds. */
                                    if (gui_win_realloc(hw, nw, nh) == 0) {
                                        if (hw->maxed) {
                                            hw->x = hw->rx;
                                            hw->y = hw->ry;
                                            hw->maxed = 0;
                                        } else {
                                            hw->x  = 0;
                                            hw->y  = 0;
                                            hw->maxed = 1;
                                        }
                                        gui_dirty_add_nolock(ox, oy, ow, oh);
                                        gui_dirty_add_nolock(hw->x, hw->y,
                                                             hw->w + 2 * GUI_BORDER,
                                                             hw->h + 2 * GUI_BORDER +
                                                                 GUI_TITLE_H);
                                        changed = 1;
                                    }
                                    s_last_press_win = 0; /* consume */
                                    goto button_done;
                                }
                                s_last_press_ticks = now;
                                s_last_press_x     = s_ptr_x;
                                s_last_press_y     = s_ptr_y;
                                s_last_press_win   = hit;
                            }
                            /* Title-bar buttons act only on the FOCUSED
                             * window (they are drawn there only — on an
                             * unfocused window the press falls through to
                             * focus/drag below). */
                            if (in_title && hit == s_focus_id) {
                                int btn = title_button_at(hw, s_ptr_x, s_ptr_y);
                                if (btn != 0) {
                                    if (btn == 1) { /* close */
                                        int dx = hw->x, dy = hw->y;
                                        int dw = hw->w + 2 * GUI_BORDER;
                                        int dh = hw->h + 2 * GUI_BORDER + GUI_TITLE_H;
                                        free(hw->buf.buf);
                                        memset(hw, 0, sizeof(*hw));
                                        if (s_focus_id == hit)
                                            s_focus_id = 0;
                                        gui_dirty_add_nolock(dx, dy, dw, dh);
                                        if (gui_win_count() == 0)
                                            shutdown = 1; /* last window: back to the shell */
                                        changed = 1;
                                    } else if (btn == 2) { /* maximise */
                                        int ox = hw->x, oy = hw->y;
                                        int ow = hw->w + 2 * GUI_BORDER;
                                        int oh = hw->h + 2 * GUI_BORDER + GUI_TITLE_H;
                                        int nw, nh;
                                        if (hw->maxed) {
                                            nw = hw->rw;
                                            nh = hw->rh;
                                        } else {
                                            /* Save the restore rect BEFORE
                                             * the realloc changes w/h. */
                                            hw->rx = hw->x;
                                            hw->ry = hw->y;
                                            hw->rw = hw->w;
                                            hw->rh = hw->h;
                                            nw = (i32)s_fb.w - 2 * GUI_BORDER;
                                            nh = (i32)s_fb.h - GUI_TASKBAR_H -
                                                 2 * GUI_BORDER - GUI_TITLE_H;
                                        }
                                        if (gui_win_realloc(hw, nw, nh) == 0) {
                                            if (hw->maxed) {
                                                hw->x = hw->rx;
                                                hw->y = hw->ry;
                                                hw->maxed = 0;
                                            } else {
                                                hw->x  = 0;
                                                hw->y  = 0;
                                                hw->maxed = 1;
                                            }
                                            gui_dirty_add_nolock(ox, oy, ow, oh);
                                            gui_dirty_add_nolock(hw->x, hw->y,
                                                                 hw->w + 2 * GUI_BORDER,
                                                                 hw->h + 2 * GUI_BORDER +
                                                                     GUI_TITLE_H);
                                            changed = 1;
                                        }
                                    } else { /* btn 3: minimise */
                                        int dx = hw->x, dy = hw->y;
                                        int dw = hw->w + 2 * GUI_BORDER;
                                        int dh = hw->h + 2 * GUI_BORDER + GUI_TITLE_H;
                                        hw->hidden = 1;
                                        if (s_focus_id == hit)
                                            s_focus_id = 0;
                                        gui_dirty_add_nolock(dx, dy, dw, dh);
                                        gui_dirty_taskbar();
                                        changed = 1;
                                    }
                                    goto button_done;
                                }
                            }
                            /* Dragging starts when the press lands on
                             * the window's border/title-bar strip. */
                            if (in_title) {
                                s_drag_id  = hit;
                                s_drag_offx = s_ptr_x - hw->x;
                                s_drag_offy = s_ptr_y - hw->y;
                            }
                            if (hit != s_focus_id) {
                                gui_win_t *old = win_find(s_focus_id);
                                gui_win_t *neu = win_find(hit);
                                if (old) {
                                    fx = old->x;
                                    fy = old->y;
                                    fw = old->w + 2 * GUI_BORDER;
                                    fh = old->h + 2 * GUI_BORDER + GUI_TITLE_H;
                                }
                                if (neu) {
                                    gui_dirty_add_nolock(neu->x, neu->y,
                                                         neu->w + 2 * GUI_BORDER,
                                                         neu->h + 2 * GUI_BORDER +
                                                             GUI_TITLE_H);
                                }
                                focus_changed = 1;
                                s_focus_id    = hit;
                            } else {
                                s_focus_id = hit;
                            }
                        }
                        s_press_grab = hit; /* implicit grab for release */
                        ev_push(GUI_EV_BUTTON, 1, s_ptr_x, s_ptr_y, hit);
                    }
                button_done:
                    ;
                    if (released) {
                        s_drag_id = 0; /* end the drag */
                        /* Implicit pointer grab: the release is delivered
                         * to the window that received the press, not the
                         * window under the pointer now (a drag that ends
                         * over a neighbour must not click the neighbour). */
                        int rel_win = s_press_grab ? s_press_grab
                                                   : hit_test(s_ptr_x, s_ptr_y);
                        s_press_grab = 0;
                        ev_push(GUI_EV_BUTTON, 0, s_ptr_x, s_ptr_y, rel_win);
                    }
                    if (s_lock >= 0)
                        (void)mutex_unlock(s_lock);
                    if (shutdown) {
                        /* The last window was closed: hand the screen
                         * back to the shell (keyboard focus + term
                         * redraw).  Skip the composite — the term owns
                         * the framebuffer again. */
                        gui_shutdown();
                        changed = 0;
                    } else {
                        if (focus_changed && fw > 0)
                            gui_dirty_add(fx, fy, fw, fh);
                        changed = 1;
                    }
                }
            }
        }

        if (changed)
            gui_composite();

        (void)sleep(1);
    }
}

/* ------------------------------------------------------------------ */
/* Entry                                                              */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("gui: starting window compositor\n");

    /* 1. Map the display framebuffer (needs ATOM_SERVICE_MANAGE via
     *    blob identity — the gui service is in the kernel seed list). */
    if (gui_fb_open(&s_fb) < 0) {
        printf("gui: framebuffer unavailable\n");
        thread_exit(1);
    }
    printf("gui: framebuffer %ux%u %ubpp buf=%p\n", s_fb.w, s_fb.h, s_fb.bpp,
           (void *)s_fb.buf);

    /* 2. Resolve the keyboard and term ports (lazily retried). */
    s_kbd_port = port_get("keyboard");
    if (s_kbd_port < 0)
        printf("gui: 'keyboard' port unresolved yet\n");
    s_term_port = port_get("term");

    /* 3. IPC port. */
    int port = ipc_port_create();
    if (port < 0) {
        printf("gui: ipc_port_create failed (%d)\n", port);
        thread_exit(1);
    }
    int ret = port_register(GUI_PORT_NAME, port);
    if (ret < 0) {
        printf("gui: port_register('%s') failed (%d)\n", GUI_PORT_NAME, ret);
        thread_exit(1);
    }
    printf("gui: port %d registered as '%s'\n", port, GUI_PORT_NAME);

    /* 4. Render lock + input thread. */
    s_lock = mutex_create();
    if (s_lock < 0)
        printf("gui: mutex_create failed (%d)\n", s_lock);
    int itid = thread_create(gui_input_main, NULL, 10);
    if (itid < 0)
        printf("gui: thread_create(input) failed (%d)\n", itid);

    /* 5. Serve clients. */
    printf("gui: serving on port %d (idle until ACTIVATE)\n", port);
    gui_server_loop(port);
    return 0;
}
