/*
 * term.c - Userspace framebuffer terminal service
 * Copyright (c) 2026 OpSys Project
 *
 * Ring-3 terminal emulator for the framebuffer.  The kernel provides
 * SYS_FB_GET_INFO / SYS_FB_MAP; this service maps the framebuffer into
 * its own address space and draws text to it.  Clients (the shell)
 * send display text over a registered IPC port — no per-character
 * syscall, no kernel involvement in rendering.  Architecture:
 *
 *   manager process (spawned by init via SYS_PROCESS_CREATE)
 *     └─ SYS_PROCESS_CREATE("term")                  term process
 *          └─ main() = term_service_main()           server thread
 *               ├─ fb_get_info(&info)                framebuffer geometry
 *               ├─ fb_map(fb, size)                  map fb into THIS
 *               │                                   address space
 *               ├─ ipc_port_create() + port_register("term")
 *               └─ loop: ipc_recv(port, req, &len) -> WRITE (render text) /
 *                             CLEAR (blank screen) -> ipc_reply(port, resp)
 *
 * The terminal keeps a full-screen character buffer (s_cells) so the
 * cursor can be erased/redrawn and lines can scroll without needing to
 * read pixels back from the framebuffer.  Geometry follows the kernel's
 * FB_COL/FB_ROW convention: 8x16 font glyphs in a 9x20 px cell grid.
 *
 * Two framebuffer modes are supported, mirroring the kernel driver:
 *   - VGA text mode (0xB8000): write u16 cells (attr<<8 | ch) directly.
 *   - Linear RGB mode: render the 8x16 bitmap font into the pixel
 *     buffer (32bpp ARGB or 24bpp BGR, matching fb_pixel).
 *
 * IPC protocol (flat structs, raw copy, native little-endian):
 *   Request:  { u32 op; u32 len; u8 data[]; }
 *     op 1 = WRITE (data = UTF-8/ASCII text to render, len = byte count)
 *     op 2 = CLEAR (data unused)
 *   Response: { i32 ret; }
 *     ret >= 0 : bytes processed (WRITE) or 0 (CLEAR)
 *     ret <  0 : negative error code (ERR_INVAL / ERR_FAULT)
 *
 * Three threads: the server thread (term_server_loop) serves the "term"
 * port, and two perm.ui threads handle the Powerbox permission panel:
 *   - perm_ui_main         — serves the "perm.ui" port, rendering the
 *                            full-screen TUI panel (snapshot/restore).
 *   - perm_ui_input_main   — owns the keyboard focus while a PENDING
 *                            query is on screen and sends the user's
 *                            y/n verdict back to the perm-manager.
 * The split guarantees the perm-manager's synchronous UI_SHOW push
 * (ipc_call) is always acknowledged immediately — the recv thread is
 * parked in ipc_recv even while the input thread blocks on the
 * keyboard, so the perm-manager can never be stuck waiting for term
 * while term waits for the user.
 * All rendering serializes on s_render_lock so the cursor state
 * machine never races.
 */

#include "../lib/libc/stdio.h"
#include "../lib/libc/string.h"
#include "../lib/libos/syscalls.h"
#include <stdint.h>

/* Fixed-width types.  kernel/types.h is not includable from user space:
 * its error_t enum collides with the OK/ERR_* macros in syscalls.h. */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  i32;

#include "../perm/perm.h" /* perm.ui port: Powerbox prompt rendering */
#include "font.h"         /* 8x16 glyphs, s_font[95][16], 0x20..0x7E */
#include "../../lib/font_cjk.h" /* 16x16 CJK glyphs + font_cjk_lookup() */

/* ====================================================================
 * Constants
 * ==================================================================== */

/* Protocol ops */
#define TERM_OP_WRITE       1 /* render text at cursor */
#define TERM_OP_CLEAR       2 /* clear screen + reset cursor */
#define TERM_OP_STATUS      3 /* render status bar (prefix + msg) */
#define TERM_OP_BOX         4 /* render box border + title */
#define TERM_OP_RENDER_LINE 5 /* render line at (x,y) without cursor change */
#define TERM_OP_SET_CURSOR  6 /* set cursor position */
#define TERM_OP_GET_CURSOR  7 /* query cursor position */
#define TERM_OP_SNAPSHOT    8 /* save a cell region: {x,y,w,h} -> cells[] */
#define TERM_OP_RESTORE     9 /* redraw a saved cell region: {x,y,w,h,cells} */
#define TERM_OP_SCROLLVIEW  10 /* {i32 delta}: page through scrollback */
#define TERM_OP_REDRAW      11 /* repaint the whole screen from s_cells
                                * (the GUI compositor restores the text
                                * screen with this after it releases the
                                * framebuffer) */
#define TERM_OP_GET_SIZE    12 /* -> {cols, rows}: query terminal size */

#define TERM_MAX_DATA 256 /* max payload bytes per request */

/* Snapshot/restore region cap: cells moved per op.  A 113x38 screen is
 * 4294 cells; dialogs/panels are far smaller, so 2048 cells (e.g.
 * 64x32 or a 100x20 box) is generous and keeps the IPC copy bounded. */
#define TERM_MAX_REGION_CELLS 2048

/* Virtual address for the framebuffer mapping.  Must be page-aligned,
 * below USER_PTR_MAX (0x0000800000000000), and must not collide with
 * the user-address-space regions the kernel already lays out:
 *   ELF           @ 0x400000
 *   heap          @ [0x70000000, 0x78000000) + 256 MB region
 *   stack         @ [0x90000000, 0x100000000) (ASLR, 1 MB blocks)
 *   0x400000000 (16 GiB) is far above all of them. */
#define TERM_FB_VA 0x400000000ULL

/* Palette — the terminal DEFAULT fg/bg colours.  Cells carry ANSI
 * 16-colour indices; s_ansi_palette maps them to RGB. */
#define TERM_FG 0x00FFFFFF /* white text     */
#define TERM_BG 0x00000000 /* black bg (ANSI standard) */

/* ANSI 16-colour palette (XTerm-ish).  7 = default fg, 0 = bg. */
static const u32 s_ansi_palette[16] = {
    0x00000000, 0x00AA0000, 0x0000AA00, 0x00AA5500,
    0x000000AA, 0x00AA00AA, 0x0000AAAA, 0x00AAAAAA,
    0x00555555, 0x00FF5555, 0x0055FF55, 0x00FFFF55,
    0x005555FF, 0x00FF55FF, 0x0055FFFF, 0x00FFFFFF,
};

/* Extended palette for TUI enhancements */
#define TERM_STATUS_BG  0x004B6EA6 /* lighter blue for status bar */
#define TERM_ERROR_FG   0x00FF6B6B /* red for errors */
#define TERM_SUCCESS_FG 0x00A8E6A1 /* green for success */
#define TERM_WARN_FG    0x00FFD93D /* yellow for warnings */
#define TERM_INFO_FG    0x006DB3F2 /* cyan for info */

/* Terminal geometry limits (static buffer sizing) */
#define TERM_MAX_COLS 256
#define TERM_MAX_ROWS 128

/* Status bar configuration */
#define TERM_STATUS_ROW     (TERM_MAX_ROWS - 1) /* reserve last row for status */
#define TERM_STATUS_ENABLED 1

/* ---- Cell model: 32-bit per cell ----
 *   bits 0-15  ch    Unicode code point (0x20-0x7E ASCII, CJK, ...)
 *   bits 16-23 fg    ANSI colour index (0-15)
 *   bits 24-31 bg    ANSI colour index (0-15)
 * CELL_WIDE_CONT marks the second (continuation) cell of a double-width
 * CJK glyph: it is skipped by the cursor and drawn as part of the glyph. */
#define CELL_CH(c)      ((c) & 0xFFFF)
#define CELL_FG(c)      (((c) >> 16) & 0xFF)
#define CELL_BG(c)      (((c) >> 24) & 0xFF)
#define CELL_SET(c, ch, fg, bg) \
    ((c) = ((u32)(ch) & 0xFFFF) | (((u32)(fg) & 0xFF) << 16) | (((u32)(bg) & 0xFF) << 24))
#define CELL_WIDE_CONT  0xFFFE /* continuation of a double-width glyph */
#define CELL_DEFAULT_FG 7      /* white */
#define CELL_DEFAULT_BG 0      /* black */

/* ====================================================================
 * Protocol structures (flat, raw copy — see header comment)
 * ==================================================================== */

typedef struct {
    u32 op;
    u32 len;
    u8  data[]; /* payload (WRITE) */
} term_req_t;

typedef struct {
    i32 ret;
} term_resp_t;

#define TERM_REQ_HDR  ((u32)sizeof(term_req_t))
#define TERM_RESP_HDR ((u32)sizeof(term_resp_t))

/* ====================================================================
 * Terminal state
 * ==================================================================== */

/* RESTORE payload = {x,y,w,h} + cells, so the req buffer must hold the
 * region header plus the cells (bounded by TERM_MAX_REGION_CELLS). */
static u8 s_req_buf[TERM_REQ_HDR + 16 + TERM_MAX_REGION_CELLS];
/* GET_CURSOR packs {i32 ret; u32 x; u32 y} (12 bytes); SNAPSHOT returns
 * the region cells after ret (TERM_RESP_HDR + TERM_MAX_REGION_CELLS). */
static u8 s_resp_buf[TERM_RESP_HDR + TERM_MAX_REGION_CELLS];

/* Framebuffer descriptor (from SYS_FB_GET_INFO) */
static u64 s_fb_va;     /* mapped virtual address        */
static u32 s_fb_width;  /* logical px (VGA text: scaled) */
static u32 s_fb_height; /* logical px (VGA text: scaled) */
static u32 s_fb_pitch;  /* bytes per scanline (linear)   */
static u8  s_fb_bpp;    /* bits per pixel (linear)       */
static u8  s_vga_text;  /* 1 = 0xB8000 text buffer       */

/* Character grid (text cells): 32-bit cells {ch, fg, bg}. */
static u32 s_cols;                                /* columns = width  / 9          */
static u32 s_rows;                                /* rows    = height / 20         */
static u32 s_cells[TERM_MAX_ROWS][TERM_MAX_COLS]; /* screen buffer */
static u32 s_cursor_x;                            /* cell coordinates              */
static u32 s_cursor_y;
static int s_render_lock = -1; /* mutex: term loop ⇄ perm.ui     */

/* ---- SGR / input state ---- */
static u8 s_cur_fg = CELL_DEFAULT_FG; /* current ANSI fg index   */
static u8 s_cur_bg = CELL_DEFAULT_BG; /* current ANSI bg index   */
static u8 s_cursor_visible = 1;

/* ---- ANSI escape state machine ---- */
#define ANSI_STATE_TEXT 0
#define ANSI_STATE_ESC  1
#define ANSI_STATE_CSI  2
static u8   s_ansi_state = ANSI_STATE_TEXT;
static u8   s_csi_params[16]; /* up to 16 numeric parameters */
static u8   s_csi_nparams;
static u8   s_csi_priv;       /* '?' private marker */
static char s_csi_inter;      /* intermediate char */
static int  s_csi_param;      /* current parameter being collected */

/* ---- UTF-8 decoder state ---- */
static u32 s_utf8_cp;   /* code point under assembly */
static int  s_utf8_left; /* bytes remaining */

/* ---- Scrollback ring (v0.7 Track 4) ----
 * Rows scrolled off the top are saved here so the user can page back
 * through history (TERM_OP_SCROLLVIEW).  s_sb_view is the look-back
 * depth: 0 = live screen; when > 0 the display shows scrollback rows
 * ending (s_sb_next - s_sb_view).  Any WRITE resets the view to live. */
#define SCROLLBACK_ROWS 200
static u32 s_sb[SCROLLBACK_ROWS][TERM_MAX_COLS];
static u32 s_sb_next;   /* next ring slot to write */
static u32 s_sb_count;  /* rows accumulated so far */
static u32 s_sb_view;   /* look-back depth (0 = live) */
static u8 s_splash_clear;  /* clear the boot splash on the first write */
static volatile u32 s_clear_gen; /* bumped by every term_clear; invalidates perm-UI snapshots */

/* ====================================================================
 * Color helpers (mirror of kernel rgb_to_vga_attr)
 * ==================================================================== */

static u8 term_rgb_to_vga_attr(u32 rgb, int bg) {
    u8 r = (u8)(rgb >> 16);
    u8 g = (u8)(rgb >> 8);
    u8 b = (u8)(rgb);

    static const u8 vga_r[16] = {0, 0, 0, 0, 170, 170, 85, 255, 85, 85, 85, 85, 255, 255, 255, 255};
    static const u8 vga_g[16] = {0, 0, 170, 170, 0, 0, 85, 255, 85, 85, 255, 255, 85, 85, 255, 255};
    static const u8 vga_b[16] = {
        0, 170, 0, 170, 0, 170, 0, 255, 85, 255, 85, 255, 85, 255, 85, 255};

    u8  best      = 7;
    u32 best_dist = 0xFFFFFFFF;
    for (int i = 0; i < 16; i++) {
        i32 dr   = (i32)r - (i32)vga_r[i];
        i32 dg   = (i32)g - (i32)vga_g[i];
        i32 db   = (i32)b - (i32)vga_b[i];
        u32 dist = (u32)(dr * dr + dg * dg + db * db);
        if (dist < best_dist) {
            best_dist = dist;
            best      = (u8)i;
        }
    }
    if (bg)
        return best & 7; /* no bright backgrounds */
    return best;
}

/* ====================================================================
 * Pixel / rectangle drawing (linear RGB mode)
 * ==================================================================== */

static void term_pixel(u32 x, u32 y, u32 c) {
    if (x >= s_fb_width || y >= s_fb_height)
        return;

    if (s_fb_bpp == 32) {
        *(volatile u32 *)(s_fb_va + (u64)y * s_fb_pitch + (u64)x * 4) = c;
    } else if (s_fb_bpp == 24) {
        /* Framebuffer expects BGR byte order (byte 0 = Blue, byte 2 = Red) */
        volatile u8 *p = (volatile u8 *)(s_fb_va + (u64)y * s_fb_pitch + (u64)x * 3);
        p[0]           = (u8)(c);
        p[1]           = (u8)(c >> 8);
        p[2]           = (u8)(c >> 16);
    }
}

static void term_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 c) {
    if (x >= s_fb_width || y >= s_fb_height)
        return;
    if (x + w > s_fb_width)
        w = s_fb_width - x;
    if (y + h > s_fb_height)
        h = s_fb_height - y;

    if (s_fb_bpp == 32) {
        for (u32 row = 0; row < h; row++) {
            volatile u32 *line = (volatile u32 *)(s_fb_va + (u64)(y + row) * s_fb_pitch);
            for (u32 col = 0; col < w; col++)
                line[x + col] = c;
        }
    } else if (s_fb_bpp == 24) {
        u8 b_val = (u8)(c);
        u8 g     = (u8)(c >> 8);
        u8 r     = (u8)(c >> 16);
        for (u32 row = 0; row < h; row++) {
            volatile u8 *line = (volatile u8 *)(s_fb_va + (u64)(y + row) * s_fb_pitch);
            for (u32 col = 0; col < w; col++) {
                u32 off       = (x + col) * 3;
                line[off + 0] = b_val;
                line[off + 1] = g;
                line[off + 2] = r;
            }
        }
    }
}

/* ====================================================================
 * Cell drawing (both modes)
 * ==================================================================== */

/* Convert RGB to the VGA attribute byte: (bg << 4) | fg. */
static u8 term_cell_attr(u32 fg, u32 bg) {
    return (u8)((term_rgb_to_vga_attr(bg, 1) << 4) | term_rgb_to_vga_attr(fg, 0));
}

/* ANSI colour index -> RGB (clamped to the 16-entry palette). */
static u32 term_ansi_rgb(u8 idx) {
    return s_ansi_palette[idx & 15];
}

/*
 * Stamp one glyph into the pixel buffer at cell (cx, cy) using the
 * 8x16 ASCII font (1 cell wide).
 */
static void term_stamp_ascii(u32 cx, u32 cy, u32 ch, u32 fg, u32 bg) {
    u32 px = cx * 9;
    u32 py = cy * 20;
    term_fill_rect(px, py, 9, 20, bg);
    if (ch < 0x20 || ch > 0x7E)
        return; /* no glyph for control chars */

    const u8 *glyph = s_font[ch - 0x20];
    for (int row = 0; row < 16; row++) {
        u8 bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col))
                term_pixel(px + col, py + row, fg);
        }
    }
}

/*
 * Stamp a double-width CJK glyph spanning cells (cx, cx+1).  The
 * glyph bitmap is 16x16; the two cells cover 18px, the glyph is
 * centred at x = cx*9 + (18-16)/2 = cx*9+1.
 */
static void term_stamp_cjk(u32 cx, u32 cy, const u8 *glyph, u32 fg, u32 bg) {
    u32 px0 = cx * 9;
    u32 py  = cy * 20;
    term_fill_rect(px0, py, 18, 20, bg);
    for (int row = 0; row < 16; row++) {
        u8 hi = glyph[row * 2];
        u8 lo = glyph[row * 2 + 1];
        for (int col = 0; col < 8; col++) {
            if (hi & (0x80 >> col))
                term_pixel(px0 + 1 + col, py + row, fg);
        }
        for (int col = 0; col < 8; col++) {
            if (lo & (0x80 >> col))
                term_pixel(px0 + 1 + 8 + col, py + row, fg);
        }
    }
}

/*
 * Draw one text cell (s_cells coordinate) on the framebuffer.
 * Cell grid is 9 px wide x 20 px tall.  A CJK code point draws a
 * double-width glyph; CELL_WIDE_CONT continuation cells are skipped
 * (they were painted by their lead cell).
 */
static void term_draw_cell(u32 cx, u32 cy, u32 cell) {
    if (cx >= s_cols || cy >= s_rows)
        return;
    u32 ch = CELL_CH(cell);
    if (ch == CELL_WIDE_CONT)
        return; /* painted by the lead cell of the pair */

    u32 fg = term_ansi_rgb((u8)CELL_FG(cell));
    u32 bg = term_ansi_rgb((u8)CELL_BG(cell));

    if (s_vga_text) {
        u8 ach = (ch < 0x20 || ch > 0x7E) ? ' ' : (u8)ch;
        u8 attr = term_cell_attr(fg, bg);
        volatile u16 *buf = (volatile u16 *)s_fb_va;
        buf[cy * s_cols + cx] = (u16)((u16)attr << 8) | ach;
        return;
    }

    if (ch > 0x7F) {
        const u8 *glyph = font_cjk_lookup((u32)ch);
        if (glyph) {
            term_stamp_cjk(cx, cy, glyph, fg, bg);
            return;
        }
        ch = '?'; /* no glyph: fall back to a visible placeholder */
    }
    term_stamp_ascii(cx, cy, ch, fg, bg);
}

/* Legacy RGB-colour cell writer (used by STATUS/BOX/RENDER_LINE and
 * the perm panel): quantise the RGB to the ANSI palette and build a
 * cell.  CJK text is accepted (double-width). */
static void term_draw_ch(u32 cx, u32 cy, u32 ch, u32 fg_rgb, u32 bg_rgb) {
    u8 fg = term_rgb_to_vga_attr(fg_rgb, 0);
    u8 bg = term_rgb_to_vga_attr(bg_rgb, 1);
    u32 cell;
    CELL_SET(cell, ch, fg, bg);
    term_draw_cell(cx, cy, cell);
}

/* Store a cell into the screen buffer AND draw it (keeps s_cells in
 * sync with the framebuffer so a later redraw keeps the colours). */
static void term_set_cell(u32 x, u32 y, u32 ch, u32 fg_rgb, u32 bg_rgb) {
    if (x >= s_cols || y >= s_rows)
        return;
    u8 fg = term_rgb_to_vga_attr(fg_rgb, 0);
    u8 bg = term_rgb_to_vga_attr(bg_rgb, 1);
    CELL_SET(s_cells[y][x], ch, fg, bg);
    term_draw_cell(x, y, s_cells[y][x]);
}

/* ====================================================================
 * Cursor rendering
 *
 * The cursor is drawn as an inverted-color cell: the stored cell is
 * re-rendered with foreground/background swapped.  To erase it we
 * re-render the same cell normally (s_cells holds the cell, so no
 * pixel read-back is needed).
 * ==================================================================== */

static void term_draw_cursor(void) {
    if (!s_cursor_visible)
        return;
    u32 cell = s_cells[s_cursor_y][s_cursor_x];
    u32 ch   = CELL_CH(cell);
    if (ch == CELL_WIDE_CONT) /* cursor on a continuation cell: move left */
        cell = s_cells[s_cursor_y][s_cursor_x > 0 ? s_cursor_x - 1 : 0];
    CELL_SET(cell, CELL_CH(cell), (u8)CELL_BG(cell), (u8)CELL_FG(cell)); /* swap */
    term_draw_cell(s_cursor_x, s_cursor_y, cell);
}

static void term_erase_cursor(void) {
    term_draw_cell(s_cursor_x, s_cursor_y, s_cells[s_cursor_y][s_cursor_x]);
}

/* ====================================================================
 * Scrolling
 * ==================================================================== */

/*
 * Scroll the whole screen up by one text row (20 px / one cell row).
 * Both the pixel buffer and the s_cells screen buffer are shifted;
 * the new bottom row is cleared with the background color.
 */
static void term_scroll(void) {
    /* Preserve the row being scrolled off into the scrollback ring. */
    memcpy(s_sb[s_sb_next], s_cells[0], s_cols * sizeof(u32));
    s_sb_next = (s_sb_next + 1) % SCROLLBACK_ROWS;
    if (s_sb_count < SCROLLBACK_ROWS)
        s_sb_count++;

    /* Shift the character buffer up by one row */
    for (u32 r = 1; r < s_rows; r++)
        memcpy(s_cells[r - 1], s_cells[r], s_cols * sizeof(u32));
    for (u32 c = 0; c < s_cols; c++)
        CELL_SET(s_cells[s_rows - 1][c], ' ', CELL_DEFAULT_FG, CELL_DEFAULT_BG);

    /* Shift the framebuffer up by one cell row */
    if (s_vga_text) {
        u32 row_bytes = s_cols * 2;
        memmove((void *)s_fb_va, (const void *)(s_fb_va + row_bytes), row_bytes * (s_rows - 1));
        /* Clear the bottom row with background attribute + space */
        u8            attr = term_cell_attr(term_ansi_rgb(CELL_DEFAULT_FG),
                                            term_ansi_rgb(CELL_DEFAULT_BG));
        volatile u16 *buf  = (volatile u16 *)s_fb_va;
        for (u32 c = 0; c < s_cols; c++)
            buf[(s_rows - 1) * s_cols + c] = (u16)((u16)attr << 8) | ' ';
    } else {
        u32 row_px = 20;
        u32 shift  = row_px * s_fb_pitch;
        u32 bytes  = (s_rows - 1) * row_px * s_fb_pitch;
        memmove((void *)s_fb_va, (const void *)(s_fb_va + shift), bytes);
        term_fill_rect(0, (s_rows - 1) * row_px, s_fb_width, row_px,
                       term_ansi_rgb(CELL_DEFAULT_BG));
    }
}

/* ====================================================================
 * Text output: ANSI escapes + UTF-8 + SGR colour + CJK double-width
 * ==================================================================== */

/* Write one decoded code point at the cursor. */
static void term_put_code_point(u32 cp) {
    /* Control characters (except those handled by the state machine). */
    switch (cp) {
    case '\n':
        s_cursor_x = 0;
        s_cursor_y++;
        goto finish;
    case '\r':
        s_cursor_x = 0;
        goto finish;
    case '\b':
        if (s_cursor_x > 0) {
            s_cursor_x--;
            CELL_SET(s_cells[s_cursor_y][s_cursor_x], ' ',
                     CELL_DEFAULT_FG, CELL_DEFAULT_BG);
            term_draw_cell(s_cursor_x, s_cursor_y, s_cells[s_cursor_y][s_cursor_x]);
        }
        goto finish;
    case '\t':
        /* Clamp at the right edge (XTerm: stay on the last column). */
        s_cursor_x = (s_cursor_x / 8 + 1) * 8;
        if (s_cursor_x >= s_cols)
            s_cursor_x = s_cols - 1;
        goto finish;
    case 0x0C: /* \f form feed = clear screen */
        for (u32 r = 0; r < s_rows; r++)
            for (u32 c = 0; c < s_cols; c++)
                CELL_SET(s_cells[r][c], ' ', CELL_DEFAULT_FG, CELL_DEFAULT_BG);
        term_fill_rect(0, 0, s_fb_width, s_fb_height, term_ansi_rgb(CELL_DEFAULT_BG));
        s_cursor_x = 0;
        s_cursor_y = 0;
        goto finish;
    default:
        break;
    }
    if (cp < 0x20)
        goto finish; /* other control chars: ignore */

    /* Double-width (CJK) or single-width glyph. */
    const u8 *glyph = NULL;
    int       wide  = 0;
    if (cp > 0x7F) {
        glyph = font_cjk_lookup(cp);
        wide  = glyph ? 1 : 0;
    }

    /* Write the lead cell (and continuation cell for wide glyphs),
     * with a minimum 1-space advance; never write past the edge. */
    u32 x = s_cursor_x;
    if (x >= s_cols) {
        x = 0;
        s_cursor_y++;
        if (s_cursor_y >= s_rows) {
            term_scroll();
            s_cursor_y = s_rows - 1;
        }
    }
    CELL_SET(s_cells[s_cursor_y][x], cp, s_cur_fg, s_cur_bg);
    term_draw_cell(x, s_cursor_y, s_cells[s_cursor_y][x]);
    s_cursor_x = x + 1;
    if (wide) {
        if (s_cursor_x < s_cols) {
            CELL_SET(s_cells[s_cursor_y][s_cursor_x], CELL_WIDE_CONT,
                     s_cur_fg, s_cur_bg);
            s_cursor_x++;
        }
    }
finish:
    /* Wrap at the right edge, scroll at the bottom */
    if (s_cursor_x >= s_cols) {
        s_cursor_x = 0;
        s_cursor_y++;
    }
    if (s_cursor_y >= s_rows) {
        term_scroll();
        s_cursor_y = s_rows - 1;
    }
}

/* ANSI SGR (Select Graphic Rendition): parse s_csi_params. */
static void term_sgr(void) {
    if (s_csi_nparams == 0) { /* ESC[m = reset */
        s_cur_fg = CELL_DEFAULT_FG;
        s_cur_bg = CELL_DEFAULT_BG;
        return;
    }
    for (u8 i = 0; i < s_csi_nparams; i++) {
        int p = s_csi_params[i];
        switch (p) {
        case 0:
            s_cur_fg = CELL_DEFAULT_FG;
            s_cur_bg = CELL_DEFAULT_BG;
            break;
        case 1: /* bold: not modelled — keep fg */
            break;
        case 7: /* reverse video: swap fg/bg */
        {
            u8 t = s_cur_fg;
            s_cur_fg = s_cur_bg;
            s_cur_bg = t;
            break;
        }
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            s_cur_fg = (u8)(p - 30);
            break;
        case 39:
            s_cur_fg = CELL_DEFAULT_FG;
            break;
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            s_cur_bg = (u8)(p - 40);
            break;
        case 49:
            s_cur_bg = CELL_DEFAULT_BG;
            break;
        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            s_cur_fg = (u8)(p - 90 + 8); /* bright fg */
            break;
        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            s_cur_bg = (u8)(p - 100 + 8); /* bright bg */
            break;
        default:
            break; /* 256-col (38;5;n) etc.: ignore for now */
        }
    }
}

/* Redraw every live row from s_cells (forward decl; defined below the
 * input path so the bulk-edit CSI handlers can call it). */
static void term_redraw_screen(void);

/* Final byte of a CSI sequence dispatches. */
static void term_csi_final(u8 b) {
    if (b == 'm') {
        term_sgr();
        return;
    }
    switch (b) {
    case 'A': /* CUU: cursor up */
        if (s_csi_nparams == 0 || s_csi_params[0] == 0)
            s_csi_params[0] = 1;
        if (s_cursor_y >= s_csi_params[0])
            s_cursor_y -= s_csi_params[0];
        else
            s_cursor_y = 0;
        break;
    case 'B': /* CUD: cursor down */
        if (s_csi_nparams == 0 || s_csi_params[0] == 0)
            s_csi_params[0] = 1;
        s_cursor_y += s_csi_params[0];
        if (s_cursor_y >= s_rows)
            s_cursor_y = s_rows - 1;
        break;
    case 'C': /* CUF: cursor forward */
        if (s_csi_nparams == 0 || s_csi_params[0] == 0)
            s_csi_params[0] = 1;
        s_cursor_x += s_csi_params[0];
        if (s_cursor_x >= s_cols)
            s_cursor_x = s_cols - 1;
        break;
    case 'D': /* CUB: cursor back */
        if (s_csi_nparams == 0 || s_csi_params[0] == 0)
            s_csi_params[0] = 1;
        if (s_cursor_x >= s_csi_params[0])
            s_cursor_x -= s_csi_params[0];
        else
            s_cursor_x = 0;
        break;
    case 'H':
    case 'f': { /* CUP / HVP: cursor position (1-based) */
        u32 row = s_csi_nparams > 0 ? s_csi_params[0] : 1;
        u32 col = s_csi_nparams > 1 ? s_csi_params[1] : 1;
        if (row > 0)
            row--;
        if (col > 0)
            col--;
        if (row >= s_rows)
            row = s_rows - 1;
        if (col >= s_cols)
            col = s_cols - 1;
        s_cursor_y = row;
        s_cursor_x = col;
        break;
    }
    case 'G': /* CHA: column absolute (1-based) */
        if (s_csi_nparams == 0 || s_csi_params[0] == 0)
            s_csi_params[0] = 1;
        s_cursor_x = (s_csi_params[0] > 0) ? s_csi_params[0] - 1 : 0;
        if (s_cursor_x >= s_cols)
            s_cursor_x = s_cols - 1;
        break;
    case 'J': { /* ED: erase display */
        int mode = (s_csi_nparams > 0) ? s_csi_params[0] : 0;
        if (mode == 2) { /* whole screen */
            for (u32 r = 0; r < s_rows; r++)
                for (u32 c = 0; c < s_cols; c++)
                    CELL_SET(s_cells[r][c], ' ', CELL_DEFAULT_FG, CELL_DEFAULT_BG);
            term_fill_rect(0, 0, s_fb_width, s_fb_height, term_ansi_rgb(CELL_DEFAULT_BG));
            s_cursor_x = 0;
            s_cursor_y = 0;
        } else if (mode == 0) { /* cursor -> end of screen */
            for (u32 c = s_cursor_x; c < s_cols; c++)
                CELL_SET(s_cells[s_cursor_y][c], ' ', CELL_DEFAULT_FG, CELL_DEFAULT_BG);
            for (u32 r = s_cursor_y + 1; r < s_rows; r++)
                for (u32 c = 0; c < s_cols; c++)
                    CELL_SET(s_cells[r][c], ' ', CELL_DEFAULT_FG, CELL_DEFAULT_BG);
            term_redraw_screen();
        } else if (mode == 1) { /* start -> cursor */
            for (u32 r = 0; r <= s_cursor_y; r++)
                for (u32 c = 0; c < s_cols; c++) {
                    if (r == s_cursor_y && c > s_cursor_x)
                        continue;
                    CELL_SET(s_cells[r][c], ' ', CELL_DEFAULT_FG, CELL_DEFAULT_BG);
                }
            term_redraw_screen();
        }
        break;
    }
    case 'K': { /* EL: erase line */
        int mode = (s_csi_nparams > 0) ? s_csi_params[0] : 0;
        u32 start = (mode == 0) ? s_cursor_x : 0;
        u32 end   = (mode == 1) ? s_cursor_x + 1 : s_cols;
        if (mode == 2)
            start = 0, end = s_cols;
        if (start < end) {
            for (u32 c = start; c < end && c < s_cols; c++)
                CELL_SET(s_cells[s_cursor_y][c], ' ', CELL_DEFAULT_FG, CELL_DEFAULT_BG);
            for (u32 c = start; c < end && c < s_cols; c++)
                term_draw_cell(c, s_cursor_y, s_cells[s_cursor_y][c]);
        }
        break;
    }
    case 'h':
    case 'l':
        /* DECSET/DECRST private modes: only ?25 (cursor visibility). */
        if (s_csi_priv && (s_csi_nparams == 0 || s_csi_params[0] == 25)) {
            s_cursor_visible = (b == 'h') ? 1 : 0;
            if (!s_cursor_visible)
                term_draw_cell(s_cursor_x, s_cursor_y,
                               s_cells[s_cursor_y][s_cursor_x]);
            else
                term_draw_cursor();
        }
        break;
    default:
        break; /* unsupported CSI: discard */
    }
}

/*
 * Emit one decoded code point (from UTF-8 or ASCII): erase the cursor,
 * render, re-draw.  The caller must hold s_render_lock (term_write
 * does; single-threaded callers are fine too).
 */
static void term_emit_cp(u32 cp) {
    term_erase_cursor();
    term_put_code_point(cp);
    term_draw_cursor();
}

/*
 * Feed one input byte into the terminal: ANSI escape state machine.
 * ASCII printable bytes and the C0 controls act immediately; ESC
 * starts a CSI or two-byte sequence.  (UTF-8 is decoded upstream in
 * term_write and delivered as complete code points via term_emit_cp.)
 */
static void term_putc(u8 ch) {
    term_erase_cursor();

    switch (s_ansi_state) {
    case ANSI_STATE_TEXT:
        if (ch == 0x1B) {
            s_ansi_state = ANSI_STATE_ESC;
            return;
        }
        /* UTF-8 continuation/lead bytes are decoded before the code
         * point reaches term_put_code_point; single-byte ASCII falls
         * through directly. */
        if (ch >= 0x80) {
            /* Should not happen (the decoder consumes them), but a
             * stray continuation byte renders as a placeholder. */
            term_put_code_point('?');
            term_draw_cursor();
            return;
        }
        term_put_code_point(ch);
        break;

    case ANSI_STATE_ESC:
        if (ch == '[') {
            s_ansi_state = ANSI_STATE_CSI;
            s_csi_nparams = 0;
            s_csi_priv = 0;
            s_csi_inter = 0;
            s_csi_param = -1;
            return;
        }
        if (ch == ']') {
            /* OSC: consume until BEL or ST (ESC \) — just skip. */
            s_ansi_state = ANSI_STATE_ESC; /* reuse: ignore until BEL */
            return;
        }
        /* Two-byte escapes (ESC 7 save cursor, ESC 8 restore, ESC c
         * reset) — only the common ones. */
        if (ch == '7') { /* save cursor */
            s_cursor_x = s_cursor_x; /* keep simple: no saved state */
            s_ansi_state = ANSI_STATE_TEXT;
            return;
        }
        if (ch == '8') {
            s_ansi_state = ANSI_STATE_TEXT;
            return;
        }
        s_ansi_state = ANSI_STATE_TEXT;
        break;

    case ANSI_STATE_CSI:
        if (ch >= 0x30 && ch <= 0x3F) {
            if (ch == '?')
                s_csi_priv = 1;
            else if (ch == ';')
                s_csi_param = -1; /* separator */
            else if (ch >= '0' && ch <= '9') {
                if (s_csi_param < 0) {
                    if (s_csi_nparams >= 16)
                        break; /* too many params: drop extras */
                    s_csi_param = 0;
                }
                s_csi_param = s_csi_param * 10 + (ch - '0');
                if (s_csi_param > 9999)
                    s_csi_param = 9999;
            } else {
                s_csi_inter = ch;
            }
            return;
        }
        /* Final byte: commit the last parameter, then dispatch. */
        if (s_csi_param >= 0 && s_csi_nparams < 16) {
            s_csi_params[s_csi_nparams++] = (u8)s_csi_param;
        }
        term_csi_final(ch);
        s_ansi_state = ANSI_STATE_TEXT;
        break;
    }

    term_draw_cursor();
}

/*
 * Render len bytes of text.  UTF-8 sequences are decoded incrementally
 * (state persists across calls, so a multi-byte char split across two
 * WRITE messages still renders correctly).
 */
/* Redraw every live row from s_cells to the framebuffer (used to leave
 * the scrollback view, and after bulk cell edits). */
static void term_redraw_screen(void) {
    for (u32 r = 0; r < s_rows; r++)
        for (u32 c = 0; c < s_cols; c++)
            term_draw_cell(c, r, s_cells[r][c]);
}

/* Render the scrollback view: display the `view` most-recent scrollback
 * rows, oldest first, filling the screen.  The live s_cells buffer is
 * NOT touched — a later write resets the view and redraws live. */
static void term_show_scrollback(u32 view) {
    for (u32 r = 0; r < s_rows; r++) {
        for (u32 c = 0; c < s_cols; c++) {
            u32 cell;
            CELL_SET(cell, ' ', CELL_DEFAULT_FG, CELL_DEFAULT_BG);
            if (view + r <= s_sb_count) {
                u32 idx = (s_sb_next + r - view + SCROLLBACK_ROWS) % SCROLLBACK_ROWS;
                if (idx < SCROLLBACK_ROWS)
                    cell = s_sb[idx][c];
            }
            term_draw_cell(c, r, cell);
        }
    }
}

static void term_clear(void); /* forward decl: term_write resets the splash */

static i32 term_write(const u8 *data, u32 len) {
    if (s_splash_clear) {
        /* First write after the boot splash: clear it entirely so it
         * never lingers under/around later output. */
        s_splash_clear = 0;
        term_clear();
        s_cursor_x = 0;
        s_cursor_y = 0;
    }
    if (s_sb_view != 0) {
        /* Any write while paging through history returns to live. */
        s_sb_view = 0;
        term_redraw_screen();
    }
#ifdef TERM_DEBUG_SERIAL_MIRROR
    /* Debug-only mirror to the kernel serial log (COM1).  Disabled by
     * default to avoid racing the user-space serial service. */
    {
        char buf[64];
        u32  off = 0;
        while (off < len) {
            u32 n = len - off;
            if (n > sizeof(buf) - 1)
                n = sizeof(buf) - 1;
            for (u32 i = 0; i < n; i++) {
                char c = (char)data[off + i];
                buf[i] = (c == '\0') ? ' ' : c;
            }
            buf[n] = '\0';
            (void)debug_log(buf);
            off += n;
        }
    }
#endif

    if (s_render_lock >= 0)
        (void)mutex_lock(s_render_lock);
    /* Feed bytes through the ANSI state machine + incremental UTF-8
     * decoder (decoder state persists across calls, so a multi-byte
     * char split across WRITEs still renders correctly). */
    for (u32 i = 0; i < len; i++) {
        u8 b = data[i];
        if (s_utf8_left > 0) {
            if ((b & 0xC0) == 0x80) {
                s_utf8_cp = (s_utf8_cp << 6) | (b & 0x3F);
                if (--s_utf8_left == 0) {
                    /* Complete code point: bypass the byte state machine
                     * (UTF-8 text cannot appear inside an escape seq). */
                    if (s_ansi_state == ANSI_STATE_TEXT)
                        term_emit_cp(s_utf8_cp);
                    /* else: part of an escape sequence — dropped. */
                }
            } else {
                s_utf8_left = 0; /* malformed: drop the partial sequence */
            }
            continue;
        }
        if (b >= 0xC2 && b <= 0xDF) {
            s_utf8_cp = b & 0x1F;
            s_utf8_left = 1;
        } else if (b >= 0xE0 && b <= 0xEF) {
            s_utf8_cp = b & 0x0F;
            s_utf8_left = 2;
        } else if (b >= 0xF0 && b <= 0xF4) {
            s_utf8_cp = b & 0x07;
            s_utf8_left = 3;
        } else if (b < 0x80) {
            term_putc(b);
        } else {
            term_putc('?'); /* stray continuation byte */
        }
    }
    if (s_render_lock >= 0)
        (void)mutex_unlock(s_render_lock);
    return (i32)len;
}

/*
 * Clear the screen and reset the cursor to the top-left corner.
 * Blank cells store ' ' so the cursor erases to a clean background.
 * Serializes with the perm.ui thread on s_render_lock.
 */
static void term_clear(void) {
    if (s_render_lock >= 0)
        (void)mutex_lock(s_render_lock);

    for (u32 r = 0; r < s_rows; r++)
        for (u32 c = 0; c < s_cols; c++)
            CELL_SET(s_cells[r][c], ' ', CELL_DEFAULT_FG, CELL_DEFAULT_BG);

    if (s_vga_text) {
        u8            attr = term_cell_attr(term_ansi_rgb(CELL_DEFAULT_FG),
                                            term_ansi_rgb(CELL_DEFAULT_BG));
        volatile u16 *buf  = (volatile u16 *)s_fb_va;
        for (u32 i = 0; i < s_cols * s_rows; i++)
            buf[i] = (u16)((u16)attr << 8) | ' ';
    } else {
        term_fill_rect(0, 0, s_fb_width, s_fb_height,
                       term_ansi_rgb(CELL_DEFAULT_BG));
    }

    s_cursor_x = 0;
    s_cursor_y = 0;
    term_draw_cursor();
    s_clear_gen++; /* invalidate any perm-UI panel snapshot */

    if (s_render_lock >= 0)
        (void)mutex_unlock(s_render_lock);
}

/* ====================================================================
 * TUI Enhancement: Status bar, box drawing, cursor control
 * ==================================================================== */

/*
 * Render a status line at row (optionally the reserved status bar row).
 * Format: "prefix: msg [time]" where time is optional.
 * Used for displaying system status, connection state, etc.
 */
static void term_render_status(const char *prefix, const char *msg) {
    if (s_render_lock >= 0)
        (void)mutex_lock(s_render_lock);

    u32 row = TERM_STATUS_ROW;
    if (row >= s_rows)
        row = s_rows - 1;

    /* Clear the status row with highlight color */
    if (s_vga_text) {
        u8            status_attr = term_cell_attr(TERM_FG, TERM_STATUS_BG);
        volatile u16 *buf         = (volatile u16 *)s_fb_va;
        for (u32 c = 0; c < s_cols; c++)
            buf[row * s_cols + c] = (u16)((u16)status_attr << 8) | ' ';
    } else {
        term_fill_rect(0, row * 20, s_fb_width, 20, TERM_STATUS_BG);
    }

    /* Render prefix and message */
    u32         col = 0;
    const char *p   = prefix;
    while (*p && col < s_cols) {
        term_set_cell(col, row, (u8)*p, TERM_FG, TERM_STATUS_BG);
        col++;
        p++;
    }

    if (col < s_cols) {
        term_set_cell(col, row, ':', TERM_FG, TERM_STATUS_BG);
        col++;
    }
    if (col < s_cols) {
        term_set_cell(col, row, ' ', TERM_FG, TERM_STATUS_BG);
        col++;
    }

    p = msg;
    while (*p && col < s_cols - 1) {
        term_set_cell(col, row, (u8)*p, TERM_FG, TERM_STATUS_BG);
        col++;
        p++;
    }

    /* Pad to end of row */
    while (col < s_cols) {
        term_set_cell(col, row, ' ', TERM_FG, TERM_STATUS_BG);
        col++;
    }

    if (s_render_lock >= 0)
        (void)mutex_unlock(s_render_lock);
}

/*
 * Render a box (border) around a region using box-drawing characters
 * (simplified: uses ASCII +, -, | for compatibility with VGA text mode).
 * Region: (x, y) to (x+w-1, y+h-1).
 */
static void term_render_box(u32 x, u32 y, u32 w, u32 h, const char *title) {
    if (x >= s_cols || y >= s_rows || w == 0 || h == 0)
        return;
    if (x + w > s_cols)
        w = s_cols - x;
    if (y + h > s_rows)
        h = s_rows - y;

    if (s_render_lock >= 0)
        (void)mutex_lock(s_render_lock);

    /* Top border */
    term_draw_ch(x, y, '+', TERM_FG, TERM_BG);
    for (u32 i = 1; i < w - 1; i++)
        term_draw_ch(x + i, y, '-', TERM_FG, TERM_BG);
    term_draw_ch(x + w - 1, y, '+', TERM_FG, TERM_BG);

    /* Side borders */
    for (u32 i = 1; i < h - 1; i++) {
        term_draw_ch(x, y + i, '|', TERM_FG, TERM_BG);
        term_draw_ch(x + w - 1, y + i, '|', TERM_FG, TERM_BG);
    }

    /* Bottom border */
    if (h > 1) {
        term_draw_ch(x, y + h - 1, '+', TERM_FG, TERM_BG);
        for (u32 i = 1; i < w - 1; i++)
            term_draw_ch(x + i, y + h - 1, '-', TERM_FG, TERM_BG);
        term_draw_ch(x + w - 1, y + h - 1, '+', TERM_FG, TERM_BG);
    }

    /* Blank the box interior.  A menu/dialog is redrawn on every key
     * (tui_menu) and its item lines are shorter than the box width, so
     * WITHOUT this the residue of a longer previous frame stays visible
     * inside the box (the fm "字符覆盖" bug).  The caller snapshots the
     * region first, so restoring after the dialog still brings the old
     * content back intact. */
    for (u32 r = 1; r + 1 < h; r++)
        for (u32 c = 1; c + 1 < w; c++)
            term_draw_ch(x + c, y + r, ' ', TERM_FG, TERM_BG);

    /* Title bar (if provided) */
    if (title && h > 2) {
        u32 tlen = 0;
        while (title[tlen] && tlen < w - 4)
            tlen++;
        u32 start = x + (w - tlen) / 2;
        for (u32 i = 0; i < tlen && start + i < x + w - 1; i++)
            term_draw_ch(start + i, y, (u8)title[i], TERM_FG, TERM_BG);
    }

    if (s_render_lock >= 0)
        (void)mutex_unlock(s_render_lock);
}

/*
 * Render a line of text at row (x, y) without updating the global cursor.
 * Useful for status lines, dialog boxes, etc.
 */
static void term_render_line_at(u32 x, u32 y, const char *text, u32 maxlen) {
    if (x >= s_cols || y >= s_rows)
        return;

    if (s_render_lock >= 0)
        (void)mutex_lock(s_render_lock);

    u32 col = x;
    for (u32 i = 0; i < maxlen && text[i] && col < s_cols; i++) {
        term_set_cell(col, y, (u8)text[i], TERM_FG, TERM_BG);
        col++;
    }

    if (s_render_lock >= 0)
        (void)mutex_unlock(s_render_lock);
}

/*
 * Get the current cursor position (for interactive TUI elements).
 * Returns the cursor coordinates in cell units.
 */
static void term_get_cursor_pos(u32 *x, u32 *y) {
    if (s_render_lock >= 0)
        (void)mutex_lock(s_render_lock);
    if (x)
        *x = s_cursor_x;
    if (y)
        *y = s_cursor_y;
    if (s_render_lock >= 0)
        (void)mutex_unlock(s_render_lock);
}

/*
 * Set the cursor position (for programmatic control).
 * Used by TUI elements that need to position the cursor manually.
 */
static void term_set_cursor_pos(u32 x, u32 y) {
    if (x >= s_cols)
        x = s_cols - 1;
    if (y >= s_rows)
        y = s_rows - 1;

    if (s_render_lock >= 0)
        (void)mutex_lock(s_render_lock);

    term_erase_cursor();
    s_cursor_x = x;
    s_cursor_y = y;
    term_draw_cursor();

    if (s_render_lock >= 0)
        (void)mutex_unlock(s_render_lock);
}

/* ====================================================================
 * Server side
 * ==================================================================== */

static void term_reply(int token, i32 ret) {
    term_resp_t *resp = (term_resp_t *)s_resp_buf;
    resp->ret         = ret;
    int r             = ipc_reply(token, s_resp_buf, (int)TERM_RESP_HDR);
    if (r < 0)
        printf("term: ipc_reply failed (%d)\n", r);
}

/*
 * Interpret one client request and reply.  Never crashes on malformed
 * input: the opcode is validated and the payload length is capped at
 * TERM_MAX_DATA (and checked against what ipc_recv actually reported).
 */
static void term_handle_request(int token, int msg_len) {
    if (msg_len > (int)sizeof(s_req_buf))
        msg_len = (int)sizeof(s_req_buf);

    if (msg_len < (int)TERM_REQ_HDR) {
        term_reply(token, ERR_INVAL);
        return;
    }

    term_req_t *req = (term_req_t *)s_req_buf;

    if (req->op == TERM_OP_WRITE) {
        if (req->len > TERM_MAX_DATA || msg_len < (int)(TERM_REQ_HDR + req->len)) {
            term_reply(token, ERR_INVAL);
            return;
        }
        term_reply(token, term_write(req->data, req->len));
    } else if (req->op == TERM_OP_CLEAR) {
        term_clear();
        term_reply(token, 0);
    } else if (req->op == TERM_OP_STATUS) {
        /* STATUS: 2 + len pairs of (prefix_len, msg_len) + strings */
        if (req->len < 4 || msg_len < (int)(TERM_REQ_HDR + 4)) {
            term_reply(token, ERR_INVAL);
            return;
        }
        u32 *status_args  = (u32 *)req->data;
        u32  prefix_len   = status_args[0];
        u32  msg_len_arg  = status_args[1];
        u32  total_needed = 8 + prefix_len + msg_len_arg;
        if (req->len < total_needed || msg_len < (int)(TERM_REQ_HDR + total_needed)) {
            term_reply(token, ERR_INVAL);
            return;
        }
        char *prefix_str = (char *)(status_args + 2);
        char *msg_str    = prefix_str + prefix_len;

        /* NULL-terminate for safety */
        char prefix_tmp[64], msg_tmp[128];
        u32  plen = (prefix_len < sizeof(prefix_tmp) - 1) ? prefix_len : sizeof(prefix_tmp) - 1;
        u32  mlen = (msg_len_arg < sizeof(msg_tmp) - 1) ? msg_len_arg : sizeof(msg_tmp) - 1;
        memcpy(prefix_tmp, prefix_str, plen);
        prefix_tmp[plen] = '\0';
        memcpy(msg_tmp, msg_str, mlen);
        msg_tmp[mlen] = '\0';

        term_render_status(prefix_tmp, msg_tmp);
        term_reply(token, 0);
    } else if (req->op == TERM_OP_BOX) {
        /* BOX: x, y, w, h, title_len + title string */
        if (req->len < 16) {
            term_reply(token, ERR_INVAL);
            return;
        }
        u32 *box_args  = (u32 *)req->data;
        u32  x         = box_args[0];
        u32  y         = box_args[1];
        u32  w         = box_args[2];
        u32  h         = box_args[3];
        u32  title_len = box_args[4];

        if (req->len < 20 + title_len) {
            term_reply(token, ERR_INVAL);
            return;
        }
        char *title_str = (char *)(box_args + 5);
        char  title_tmp[64];
        u32   tlen = (title_len < sizeof(title_tmp) - 1) ? title_len : sizeof(title_tmp) - 1;
        if (title_len > 0)
            memcpy(title_tmp, title_str, tlen);
        title_tmp[tlen] = '\0';

        term_render_box(x, y, w, h, (title_len > 0) ? title_tmp : NULL);
        term_reply(token, 0);
    } else if (req->op == TERM_OP_RENDER_LINE) {
        /* RENDER_LINE: x, y + text data */
        if (req->len < 8 || msg_len < (int)(TERM_REQ_HDR + 8)) {
            term_reply(token, ERR_INVAL);
            return;
        }
        u32 *line_args = (u32 *)req->data;
        u32  x         = line_args[0];
        u32  y         = line_args[1];
        u32  textlen   = req->len - 8;
        if (textlen > TERM_MAX_DATA)
            textlen = TERM_MAX_DATA;
        char *text = (char *)(line_args + 2);

        term_render_line_at(x, y, text, textlen);
        term_reply(token, 0);
    } else if (req->op == TERM_OP_SET_CURSOR) {
        /* SET_CURSOR: x, y */
        if (req->len < 8) {
            term_reply(token, ERR_INVAL);
            return;
        }
        u32 *cursor_args = (u32 *)req->data;
        term_set_cursor_pos(cursor_args[0], cursor_args[1]);
        term_reply(token, 0);
    } else if (req->op == TERM_OP_GET_CURSOR) {
        /* GET_CURSOR: return cursor position in response */
        term_resp_t *resp      = (term_resp_t *)s_resp_buf;
        u32         *resp_data = (u32 *)(resp + 1);
        term_get_cursor_pos(&resp_data[0], &resp_data[1]);
        resp->ret    = 0;
        int resp_len = (int)(sizeof(term_resp_t) + 8);
        (void)ipc_reply(token, resp, resp_len);
        return;
    } else if (req->op == TERM_OP_SNAPSHOT) {
        /* SNAPSHOT: {x, y, w, h} -> response = {ret; cells[w*h]} */
        if (req->len < 16) {
            term_reply(token, ERR_INVAL);
            return;
        }
        u32 *snap    = (u32 *)req->data;
        u32  x       = snap[0];
        u32  y       = snap[1];
        u32  w       = snap[2];
        u32  h       = snap[3];
        if (w == 0 || h == 0 || (u64)w * h > TERM_MAX_REGION_CELLS) {
            term_reply(token, ERR_INVAL);
            return;
        }
        term_resp_t *resp = (term_resp_t *)s_resp_buf;
        resp->ret         = 0;
        u8 *dst           = (u8 *)(resp + 1);
        if (s_render_lock >= 0)
            (void)mutex_lock(s_render_lock);
        u32 n = 0;
        for (u32 r = 0; r < h && y + r < s_rows; r++)
            for (u32 c = 0; c < w && x + c < s_cols; c++) {
                u32 cp = CELL_CH(s_cells[y + r][x + c]);
                dst[n++] = (cp < 0x100) ? (u8)cp : (u8)'?';
            }
        if (s_render_lock >= 0)
            (void)mutex_unlock(s_render_lock);
        (void)ipc_reply(token, s_resp_buf, (int)(sizeof(term_resp_t) + n));
        return;
    } else if (req->op == TERM_OP_RESTORE) {
        /* RESTORE: {x, y, w, h, cells[w*h]} — redraw a saved region. */
        if (req->len < 16) {
            term_reply(token, ERR_INVAL);
            return;
        }
        u32 *snap = (u32 *)req->data;
        u32  x    = snap[0];
        u32  y    = snap[1];
        u32  w    = snap[2];
        u32  h    = snap[3];
        if (w == 0 || h == 0 || (u64)w * h > TERM_MAX_REGION_CELLS ||
            req->len < 16 + (u32)(w * h)) {
            term_reply(token, ERR_INVAL);
            return;
        }
        const u8 *cells = (const u8 *)(snap + 4);
        if (s_render_lock >= 0)
            (void)mutex_lock(s_render_lock);
        u32 n = 0;
        for (u32 r = 0; r < h && y + r < s_rows; r++)
            for (u32 c = 0; c < w && x + c < s_cols; c++) {
                u8 ch = cells[n++];
                if (ch < 0x20 || ch > 0x7E)
                    ch = ' ';
                term_set_cell(x + c, y + r, ch, TERM_FG, TERM_BG);
            }
        if (s_render_lock >= 0)
            (void)mutex_unlock(s_render_lock);
        term_reply(token, 0);
    } else if (req->op == TERM_OP_SCROLLVIEW) {
        /* SCROLLVIEW: {i32 delta}.  delta > 0 pages back (older),
         * delta < 0 pages forward, delta == 0 returns to live. */
        if (req->len < 4) {
            term_reply(token, ERR_INVAL);
            return;
        }
        i32 delta = (i32)((u32 *)req->data)[0];
        /* Clamp to the scrollback depth so the arithmetic cannot
         * overflow (s_sb_view <= SCROLLBACK_ROWS). */
        if (delta > (i32)SCROLLBACK_ROWS)
            delta = (i32)SCROLLBACK_ROWS;
        if (delta < -(i32)SCROLLBACK_ROWS)
            delta = -(i32)SCROLLBACK_ROWS;
        if (s_render_lock >= 0)
            (void)mutex_lock(s_render_lock);
        if (delta == 0) {
            s_sb_view = 0;
            term_redraw_screen();
        } else if (delta < 0) {
            u32 back = (u32)(-delta);
            s_sb_view = (back >= s_sb_view) ? 0 : s_sb_view - back;
            term_show_scrollback(s_sb_view);
        } else {
            s_sb_view += (u32)delta;
            if (s_sb_view > s_sb_count)
                s_sb_view = s_sb_count;
            term_show_scrollback(s_sb_view);
        }
        if (s_render_lock >= 0)
            (void)mutex_unlock(s_render_lock);
        term_reply(token, 0);
    } else if (req->op == TERM_OP_REDRAW) {
        /* REDRAW: repaint the whole screen from s_cells.  The GUI
         * compositor calls this after releasing the framebuffer so the
         * text screen reappears exactly as the shell left it. */
        if (s_render_lock >= 0)
            (void)mutex_lock(s_render_lock);
        for (u32 r = 0; r < s_rows; r++)
            for (u32 c = 0; c < s_cols; c++)
                term_draw_cell(c, r, s_cells[r][c]);
        term_draw_cursor();
        if (s_render_lock >= 0)
            (void)mutex_unlock(s_render_lock);
        term_reply(token, 0);
    } else if (req->op == TERM_OP_GET_SIZE) {
        /* GET_SIZE: reply {i32 ret; u32 cols; u32 rows}. */
        u32 *resp = (u32 *)s_resp_buf;
        resp[0]   = 0;
        resp[1]   = s_cols;
        resp[2]   = s_rows;
        (void)ipc_reply(token, s_resp_buf, 12);
    } else {
        term_reply(token, ERR_INVAL);
    }
}

static void term_server_loop(int port) {
    for (;;) {
        int msg_len = (int)sizeof(s_req_buf);
        int token   = 0;
        int ret     = ipc_recv(port, s_req_buf, &msg_len, &token);
        if (ret < 0) {
            printf("term: ipc_recv failed (%d)\n", ret);
            thread_exit(1);
        }

        term_handle_request(token, msg_len);
    }
}

/* ====================================================================
 * perm.ui Powerbox panel (design §8 决策 2: "UI 代理注册：term 启动时
 * 向 perm-manager 注册 port 名为 perm.ui")
 *
 * Two cooperating threads render and answer the permission panel:
 *
 *   perm_ui_main        (thread A) — owns the "perm.ui" port and is
 *                       ALWAYS parked in ipc_recv, so the perm-manager's
 *                       synchronous UI_SHOW push (ipc_call) is answered
 *                       immediately.  It snapshots the screen when a
 *                       PENDING query appears, renders the panel, shows
 *                       the verdict, holds it, then restores the area
 *                       the panel covered.
 *
 *   perm_ui_input_main  (thread B) — the keyboard front-end.  It polls
 *                       s_ui_await, takes the keyboard focus, reads a
 *                       y/n key, sends PERM_OP_ANSWER via ipc_send (NOT
 *                       ipc_call: perm-manager's do_answer synchronously
 *                       pushes the result UI_SHOW back here, so a
 *                       blocking call would deadlock), releases focus.
 *
 * The two-thread split is what makes a concurrent CHECK safe: even
 * while thread B is blocked waiting for a key, thread A is still in
 * ipc_recv, so the perm-manager's next UI_SHOW push completes and it
 * never gets stuck waiting for term (ipc_send is a rendezvous).
 *
 * The panel is a full-screen TUI dialog (62x9, centered, ASCII border
 * only — the VGA font covers just 0x20-0x7E).  The screen underneath
 * is snapshotted once when the first PENDING query arrives; after the
 * verdict has been shown, only the panel's own rectangle is restored,
 * so any shell output written while the panel was up (the -105 error
 * and the fresh prompt) stays on screen.  All rendering runs under
 * s_render_lock (the same mutex as term_write).
 * ==================================================================== */

/* Panel geometry (centered; clamped to the real screen) */
#define PERM_UI_PANEL_W 62
#define PERM_UI_PANEL_H 9

/* Verdict display hold time: ~1 s at the 100 Hz PIT tick. */
#define PERM_UI_RESULT_HOLD_TICKS 100

/* Keyboard protocol ops (mirror of keyboard.c — keyboard.h is not
 * shared with the term service). */
#define PERM_UI_KBD_READ           1
#define PERM_UI_KBD_READ_BLOCK    2
#define PERM_UI_KBD_TAKE_FOCUS    3
#define PERM_UI_KBD_RELEASE_FOCUS 4

/* Panel palette (extended colors, see the header comment). */
#define PERM_UI_TITLE_FG  TERM_WARN_FG
#define PERM_UI_OK_FG     TERM_SUCCESS_FG
#define PERM_UI_BAD_FG    TERM_ERROR_FG
#define PERM_UI_BORDER_FG TERM_INFO_FG

/* Panel state.  The snapshot is taken by thread A when the panel first
 * opens; s_ui_active tracks whether a panel is on screen (thread A
 * only).  s_ui_await and s_ui_query_id are the handoff to thread B:
 * A arms them, B clears s_ui_await after answering. */
static u32 s_ui_snapshot[TERM_MAX_ROWS][TERM_MAX_COLS];
static int          s_ui_active;         /* 1 = panel on screen (A only) */
static volatile u32 s_ui_snapshot_gen;   /* s_clear_gen at snapshot time  */
static volatile u32 s_ui_await;          /* 1 = input thread should run  */
static volatile u32 s_ui_query_id;       /* query being answered (u32)   */
static int          s_ui_perm_port = -1; /* lazy port_get("perm")        */
static int          s_ui_kbd_port  = -1; /* lazy port_get("keyboard")    */

/* Draw one cell into the panel (keeps s_cells in sync with the fb). */
static void perm_ui_cell(u32 x, u32 y, char ch, u32 fg) {
    if (x >= s_cols || y >= s_rows)
        return;
    if (ch < 0x20 || ch > 0x7E)
        ch = ' ';
    term_set_cell(x, y, (u8)ch, fg, TERM_BG);
}

/* Render a line of `width` cells, padded with spaces, sanitized. */
static void perm_ui_line(u32 x, u32 y, u32 fg, const char *s, u32 maxlen, u32 width) {
    for (u32 i = 0; i < width; i++) {
        char ch = (i < maxlen && s[i]) ? s[i] : ' ';
        perm_ui_cell(x + i, y, ch, fg);
    }
}

/* Write decimal of v into out; returns the digit count. */
static u32 perm_ui_dec(char *out, u32 v) {
    char tmp[12];
    u32  n = 0;
    if (v == 0)
        tmp[n++] = '0';
    while (v > 0 && n < sizeof(tmp) - 1) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (u32 i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

/* Access mask -> "R"/"W"/"RW"/"X"... (empty -> "-"). */
static u32 perm_ui_access(char *out, u32 access) {
    u32 n = 0;
    if (access & VFS_ACCESS_READ)
        out[n++] = 'R';
    if (access & VFS_ACCESS_WRITE)
        out[n++] = 'W';
    if (access & VFS_ACCESS_EXEC)
        out[n++] = 'X';
    if (n == 0)
        out[n++] = '-';
    return n;
}

/*
 * Draw the panel for req.  fresh=1: snapshot the screen underneath and
 * arm the input thread (first PENDING); fresh=0: re-draw in place
 * (another PENDING while active, or the ALLOWED/DENIED verdict).
 */
static void perm_ui_render_panel(const perm_req_ui_t *req, int fresh) {
    u32 w = PERM_UI_PANEL_W;
    u32 h = PERM_UI_PANEL_H;
    if (w > s_cols)
        w = s_cols;
    if (h > s_rows)
        h = s_rows;
    u32 px = (s_cols - w) / 2;
    u32 py = (s_rows - h) / 2;

    if (s_render_lock >= 0)
        (void)mutex_lock(s_render_lock);

    if (fresh) {
        for (u32 r = 0; r < s_rows; r++) {
            memcpy(s_ui_snapshot[r], s_cells[r], s_cols * sizeof(u32));
            for (u32 cc = s_cols; cc < TERM_MAX_COLS; cc++)
                CELL_SET(s_ui_snapshot[r][cc], ' ', CELL_DEFAULT_FG, CELL_DEFAULT_BG);
        }
        s_ui_snapshot_gen = s_clear_gen;
        s_ui_active       = 1;
    }

    /* Interior clear (no stale glyphs) */
    for (u32 r = 1; r < h - 1; r++)
        for (u32 c = 1; c < w - 1; c++)
            perm_ui_cell(px + c, py + r, ' ', TERM_FG);

    /* Border (+ - | only: the font has no box-drawing glyphs) */
    perm_ui_cell(px, py, '+', PERM_UI_BORDER_FG);
    for (u32 c = 1; c < w - 1; c++)
        perm_ui_cell(px + c, py, '-', PERM_UI_BORDER_FG);
    perm_ui_cell(px + w - 1, py, '+', PERM_UI_BORDER_FG);
    for (u32 r = 1; r < h - 1; r++) {
        perm_ui_cell(px, py + r, '|', PERM_UI_BORDER_FG);
        perm_ui_cell(px + w - 1, py + r, '|', PERM_UI_BORDER_FG);
    }
    perm_ui_cell(px, py + h - 1, '+', PERM_UI_BORDER_FG);
    for (u32 c = 1; c < w - 1; c++)
        perm_ui_cell(px + c, py + h - 1, '-', PERM_UI_BORDER_FG);
    perm_ui_cell(px + w - 1, py + h - 1, '+', PERM_UI_BORDER_FG);

    /* Title, centered on the top border */
    static const char k_title[] = "Permission Request";
    u32               tl        = (sizeof(k_title) - 1 < w - 4) ? sizeof(k_title) - 1 : w - 4;
    u32               tx        = px + (w - tl) / 2;
    for (u32 i = 0; i < tl; i++)
        perm_ui_cell(tx + i, py, k_title[i], PERM_UI_TITLE_FG);

    /* Requestor: "<name> (PID <pid>)" */
    {
        char line[96];
        u32  n = 0;
        u32  i = 0;
        while (i < sizeof(req->name) - 1 && req->name[i] && n < sizeof(line) - 1)
            line[n++] = req->name[i++];
        static const char k_pid[] = " (PID ";
        for (i = 0; k_pid[i] && n < sizeof(line) - 1; i++)
            line[n++] = k_pid[i];
        char dec[12];
        u32  dn = perm_ui_dec(dec, req->pid);
        for (i = 0; i < dn && n < sizeof(line) - 1; i++)
            line[n++] = dec[i];
        if (n < sizeof(line) - 1)
            line[n++] = ')';
        line[n] = '\0';
        perm_ui_line(px + 2, py + 1, TERM_FG, line, n, w - 4);
    }

    /* Resource URL (find its real length first: the field need not be
     * NUL-terminated within the struct). */
    {
        u32 ulen = 0;
        while (ulen < sizeof(req->url) - 1 && req->url[ulen])
            ulen++;
        perm_ui_line(px + 2, py + 2, TERM_FG, req->url, ulen, w - 4);
    }

    /* Access mask */
    {
        char              acc[8];
        u32               an = perm_ui_access(acc, req->access);
        char              line[32];
        u32               n       = 0;
        static const char k_acc[] = "Access: ";
        for (u32 i = 0; k_acc[i] && n < sizeof(line) - 1; i++)
            line[n++] = k_acc[i];
        for (u32 i = 0; i < an && n < sizeof(line) - 1; i++)
            line[n++] = acc[i];
        line[n] = '\0';
        perm_ui_line(px + 2, py + 3, TERM_FG, line, n, w - 4);
    }

    /* Aggregated description (PENDING only; sanitized to the ASCII font) */
    if (req->state == PERM_QUERY_PENDING)
        perm_ui_line(px + 2, py + 4, TERM_FG, req->label, sizeof(req->label), w - 4);

    /* Prompt or verdict line */
    if (req->state == PERM_QUERY_PENDING) {
        static const char k_q[] = "Allow? (y/n)";
        perm_ui_line(px + 2, py + 5, TERM_FG, k_q, sizeof(k_q) - 1, w - 4);
    } else if (req->state == PERM_QUERY_ALLOWED) {
        static const char k_ok[] = "Result: ALLOWED";
        perm_ui_line(px + 2, py + 5, PERM_UI_OK_FG, k_ok, sizeof(k_ok) - 1, w - 4);
    } else {
        static const char k_bad[] = "Result: DENIED";
        perm_ui_line(px + 2, py + 5, PERM_UI_BAD_FG, k_bad, sizeof(k_bad) - 1, w - 4);
    }

    if (s_render_lock >= 0)
        (void)mutex_unlock(s_render_lock);
}

/* Restore the screen area the panel covered (its centered rectangle),
 * leaving whatever the shell wrote while the panel was up — the -105
 * error and the fresh prompt — on screen.  The cursor stays where the
 * shell's last write left it: the shell never re-prints its prompt, so
 * resetting the cursor to the pre-panel position would orphan future
 * keystrokes off the visible prompt line. */
static void perm_ui_restore(void) {
    u32 w = PERM_UI_PANEL_W;
    u32 h = PERM_UI_PANEL_H;
    if (w > s_cols)
        w = s_cols;
    if (h > s_rows)
        h = s_rows;
    u32 px = (s_cols - w) / 2;
    u32 py = (s_rows - h) / 2;

    if (s_render_lock >= 0)
        (void)mutex_lock(s_render_lock);
    if (s_ui_snapshot_gen != s_clear_gen) {
        /* The screen was cleared after this snapshot was taken (e.g.
         * the boot-splash clear on the shell's first write, or an
         * explicit TERM_OP_CLEAR): restoring the snapshot would
         * resurrect pre-clear content (the splash rows).  Blank the
         * panel rectangle instead — it may hold a verdict render made
         * after the clear. */
        for (u32 r = py; r < py + h; r++)
            for (u32 c = px; c < px + w; c++) {
                term_set_cell(c, r, ' ', TERM_FG, TERM_BG);
            }
        term_draw_cursor();
        s_ui_active = 0;
        if (s_render_lock >= 0)
            (void)mutex_unlock(s_render_lock);
        return;
    }
    for (u32 r = py; r < py + h; r++) {
        memcpy(s_cells[r] + px, s_ui_snapshot[r] + px, w * sizeof(u32));
        for (u32 c = px; c < px + w; c++)
            term_draw_cell(c, r, s_cells[r][c]);
    }
    term_draw_cursor();
    s_ui_active = 0;
    if (s_render_lock >= 0)
        (void)mutex_unlock(s_render_lock);
}

/* --- keyboard helper: flat {u32 op; u32 len} -> {i32 ret; u8 data[]} --- */

static int perm_ui_kbd_port(void) {
    if (s_ui_kbd_port < 0)
        s_ui_kbd_port = port_get("keyboard");
    return s_ui_kbd_port;
}

/* One synchronous call to the keyboard service.  Returns 0 and fills
 * ret/data on success. */
static int perm_ui_kbd_req(u32 op, u32 len, i32 *ret, u8 *data, u32 data_cap) {
    int port = perm_ui_kbd_port();
    if (port < 0)
        return -1;
    static u8 s_req[8];
    static u8 s_resp[4 + 8];
    u32      *h  = (u32 *)s_req;
    h[0]         = op;
    h[1]         = len;
    int resp_len = (int)sizeof(s_resp);
    if (ipc_call(port, s_req, (int)sizeof(s_req), s_resp, &resp_len) < 0)
        return -1;
    if (resp_len < 4)
        return -1;
    i32 rret = *(i32 *)s_resp;
    if (ret)
        *ret = rret;
    if (data) {
        u32 n = (u32)(resp_len - 4);
        if (n > data_cap)
            n = data_cap;
        if (n > 0)
            memcpy(data, s_resp + 4, n);
    }
    return 0;
}

/* Take (1) or release (0) the keyboard focus.  < 0 on failure. */
static int perm_ui_kbd_focus(int take) {
    i32 ret = 0;
    if (perm_ui_kbd_req(
            take ? PERM_UI_KBD_TAKE_FOCUS : PERM_UI_KBD_RELEASE_FOCUS, 0, &ret, NULL, 0) < 0)
        return -1;
    return ret;
}

/* Send the user's verdict for the current query.  ipc_send only: the
 * perm-manager answers with a UI_SHOW result push synchronously inside
 * do_answer, so ipc_call here would deadlock. */
static void perm_ui_send_answer(int allow) {
    int port = s_ui_perm_port;
    if (port < 0) {
        s_ui_perm_port = port_get(PERM_PORT_NAME);
        port           = s_ui_perm_port;
    }
    if (port < 0)
        return;
    perm_req_answer_t ans;
    memset(&ans, 0, sizeof(ans));
    ans.op       = PERM_OP_ANSWER;
    ans.query_id = (u32)s_ui_query_id;
    ans.allow    = allow ? 1 : 0;
    (void)ipc_send(port, &ans, (int)sizeof(ans));
}

/*
 * Thread B: keyboard front-end.  Polls s_ui_await, answers the query
 * with y/n, then clears s_ui_await so thread A can run the verdict
 * hold + restore.
 */
static void perm_ui_input_main(void *arg) {
    (void)arg;
    for (;;) {
        while (!s_ui_await)
            (void)sleep(1);

        /* Grab the keyboard.  On failure answer deny so the query still
         * resolves and the panel can close. */
        if (perm_ui_kbd_focus(1) < 0) {
            perm_ui_send_answer(0);
            s_ui_await = 0;
            continue;
        }
        /* Poll for y/n instead of blocking forever: a verdict resolved
         * by a non-UI path (perm_answer) clears s_ui_await from thread A,
         * so this loop exits and the focus is released.  A real panel
         * keeps waiting as long as s_ui_await stays set. */
        int allow = -1;
        while (allow < 0 && s_ui_await) {
            u8 key = 0;
            i32 ret = 0;
            if (perm_ui_kbd_req(PERM_UI_KBD_READ, 1, &ret, &key, 1) < 0)
                break;
            if (ret > 0) {
                if (key == 'y' || key == 'Y')
                    allow = 1;
                else if (key == 'n' || key == 'N')
                    allow = 0;
            }
            (void)sleep(1);
        }
        (void)perm_ui_kbd_focus(0);
        if (allow < 0) {
            /* Resolved elsewhere or an error: nothing to answer. */
            s_ui_await = 0;
            continue;
        }
        perm_ui_send_answer(allow);
        s_ui_await = 0;
    }
}

/*
 * Thread A: "perm.ui" port server + panel renderer.  Always parked in
 * ipc_recv (never blocks on the user) so the perm-manager's UI_SHOW
 * pushes always complete — that is what makes a concurrent CHECK safe.
 */
static void perm_ui_main(void *arg) {
    (void)arg;

    int port = ipc_port_create();
    if (port < 0) {
        printf("term: perm.ui ipc_port_create failed (%d)\n", port);
        thread_exit(1);
    }
    int ret = port_register(PERM_UI_PORT_NAME, port);
    if (ret < 0) {
        printf("term: perm.ui port_register('%s') failed (%d)\n", PERM_UI_PORT_NAME, ret);
        thread_exit(1);
    }
    printf("term: perm.ui port %d registered\n", port);

    static u8 s_ui_req[sizeof(perm_req_ui_t)];
    static u8 s_ui_resp[sizeof(perm_resp_ui_t)];

    for (;;) {
        int msg_len = (int)sizeof(s_ui_req);
        int token   = 0;
        ret         = ipc_recv(port, s_ui_req, &msg_len, &token);
        if (ret < 0) {
            printf("term: perm.ui ipc_recv failed (%d)\n", ret);
            thread_exit(1);
        }

        perm_resp_ui_t *resp = (perm_resp_ui_t *)s_ui_resp;
        resp->ret            = 0;

        if (msg_len >= (int)sizeof(perm_req_ui_t)) {
            perm_req_ui_t *req = (perm_req_ui_t *)s_ui_req;
            if (req->op == PERM_OP_UI_SHOW) {
                if (req->state == PERM_QUERY_PENDING) {
                    int fresh = !s_ui_active;
                    perm_ui_render_panel(req, fresh);
                    if (fresh) {
                        s_ui_query_id = req->query_id;
                        s_ui_await    = 1;
                    } else {
                        /* New PENDING while a panel is up: redraw in place and
                         * re-point the input thread at the newest query. */
                        s_ui_query_id = req->query_id;
                    }
                } else if (s_ui_active && req->query_id == s_ui_query_id) {
                    /* Verdict for the panel on screen: redraw, ack first (so the
                     * perm-manager unblocks), hold, then restore.  The query was
                     * resolved WITHOUT the UI's y/n (e.g. init's perm_answer or a
                     * script) — clear s_ui_await so thread B stops waiting and
                     * RELEASES the keyboard focus; otherwise it would hold the
                     * focus forever and starve the shell's input. */
                    perm_ui_render_panel(req, 0);
                    (void)ipc_reply(token, s_ui_resp, (int)sizeof(perm_resp_ui_t));
                    (void)sleep(PERM_UI_RESULT_HOLD_TICKS);
                    perm_ui_restore();
                    s_ui_await = 0;
                    continue;
                }
            }
        }

        (void)ipc_reply(token, s_ui_resp, (int)sizeof(perm_resp_ui_t));
    }
}

/* ====================================================================
 * Entry point (term process main)
 * ==================================================================== */

/*
 * Server thread entry point.  Queries the framebuffer descriptor,
 * maps the framebuffer into this address space, registers the "term"
 * IPC port, blanks the screen, then serves clients forever.
 */
static void term_service_main(void *arg) {
    (void)arg;

    printf("term: starting framebuffer terminal service\n");

    /* 1. Query the framebuffer descriptor. */
    fb_user_info_t info;
    int            ret = fb_get_info(&info);
    if (ret < 0) {
        printf("term: fb_get_info failed (%d)\n", ret);
        thread_exit(1);
    }
    s_fb_width  = info.width;
    s_fb_height = info.height;
    s_fb_pitch  = info.pitch;
    s_fb_bpp    = info.bpp;
    s_vga_text  = info.vga_text;
    printf("term: fb %ux%u %ubpp pitch=%u%s\n",
           s_fb_width,
           s_fb_height,
           s_fb_bpp,
           s_fb_pitch,
           s_vga_text ? " (VGA text)" : " (linear)");

    /* 2. Map the framebuffer into THIS address space.
     *    Size must be page-aligned and no larger than the real fb
     *    (the kernel clamps: 1 page for VGA text, pitch*height for
     *    linear mode).  Mirror the kernel's computation exactly. */
    u64 fb_size;
    if (s_vga_text) {
        fb_size = 4096;
    } else {
        fb_size = (u64)s_fb_pitch * s_fb_height;
        fb_size = (fb_size + 4095) & ~(u64)4095;
    }
    void *va = fb_map((void *)TERM_FB_VA, fb_size);
    if ((long)va < 0) {
        printf("term: fb_map failed (%d)\n", (int)(long)va);
        thread_exit(1);
    }
    s_fb_va = (u64)va;
    /* libc printf has no %lx/%p and no width specifiers — print the
     * 64-bit address as two plain %x halves */
    printf("term: fb mapped at 0x%x%x (size=0x%x)\n",
           (u32)(s_fb_va >> 32),
           (u32)s_fb_va,
           (u32)fb_size);

    /* 3. Cell grid geometry: 9 px/col, 20 px/row (FB_COL/FB_ROW). */
    s_cols = s_fb_width / 9;
    s_rows = s_fb_height / 20;
    if (s_cols == 0 || s_rows == 0) {
        printf("term: degenerate fb geometry (%ux%u)\n", s_cols, s_rows);
        thread_exit(1);
    }
    if (s_cols > TERM_MAX_COLS)
        s_cols = TERM_MAX_COLS;
    if (s_rows > TERM_MAX_ROWS)
        s_rows = TERM_MAX_ROWS;
    printf("term: %ux%u cells\n", s_cols, s_rows);

    /* 4. IPC port, registered under the well-known name "term". */
    int port = ipc_port_create();
    if (port < 0) {
        printf("term: ipc_port_create failed (%d)\n", port);
        thread_exit(1);
    }

    /* 4b. Boot splash: rendered BEFORE the "term" port is registered so
     * no client can write while it is being drawn.  The clear flag is
     * armed only after the splash is complete; the FIRST client write
     * then clears the whole screen (s_splash_clear) and the shell
     * banner replaces the splash entirely — including the centered
     * rows, which a top-anchored banner would otherwise leave on
     * screen forever.  (This ordering also removes the startup race
     * where a concurrent first write could consume the clear flag and
     * then have the splash drawn over it.) */
    term_clear();
    s_splash_clear = 1;
    {
        static const char *const splash[] = {
            "OpSys Microkernel",
            "starting services...",
        };
        u32 nlines = sizeof(splash) / sizeof(splash[0]);
        u32 row    = (s_rows > nlines) ? (s_rows - nlines) / 2 : 0;
        s_cursor_x = 0;
        s_cursor_y = row; /* center vertically */
        for (u32 i = 0; i < nlines; i++) {
            u32 len = (u32)strlen(splash[i]);
            u32 col = (len < s_cols) ? (s_cols - len) / 2 : 0;
            for (u32 c = 0; c < col; c++)
                term_putc(' ');
            for (u32 c = 0; c < len; c++)
                term_putc(splash[i][c]);
            if (i + 1 < nlines)
                term_putc('\n');
        }
        /* Reset the cursor so the next writer starts at the top. */
        s_cursor_x = 0;
        s_cursor_y = 0;
        term_draw_cursor();
    }

    ret = port_register("term", port);
    if (ret < 0) {
        printf("term: port_register('term') failed (%d)\n", ret);
        thread_exit(1);
    }
    printf("term: port %d registered as 'term'\n", port);

    /* 5. Render lock (term loop ⇄ perm.ui thread) + Powerbox UI agent. */
    s_render_lock = mutex_create();
    if (s_render_lock < 0) {
        printf("term: mutex_create failed (%d)\n", s_render_lock);
        thread_exit(1);
    }
    int ui_tid = thread_create(perm_ui_main, NULL, 10);
    if (ui_tid < 0)
        printf("term: thread_create(perm.ui) failed (%d)\n", ui_tid);
    int ui_in_tid = thread_create(perm_ui_input_main, NULL, 10);
    if (ui_in_tid < 0)
        printf("term: thread_create(perm.ui input) failed (%d)\n", ui_in_tid);


    /* 7. Serve clients. */
    printf("term: serving on port %d\n", port);
    term_server_loop(port);
}

/* ====================================================================
 * Process entry point (crt0 calls main())
 * ==================================================================== */

int main(void) {
    term_service_main(NULL);
    return 0; /* unreachable */
}
