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
 * Two threads: the server thread (term_server_loop) serves the "term"
 * port, and the perm.ui thread (perm_ui_main) serves the "perm.ui"
 * port so the Powerbox prompt from the perm-manager appears on screen.
 * Both render through term_write()/term_clear(), which serialize on
 * s_render_lock so the cursor state machine never races.
 */

#include "../lib/libc/stdio.h"
#include "../lib/libos/syscalls.h"
#include "../lib/libc/string.h"
#include <stdint.h>

/* Fixed-width types.  kernel/types.h is not includable from user space:
 * its error_t enum collides with the OK/ERR_* macros in syscalls.h. */
typedef uint8_t     u8;
typedef uint16_t    u16;
typedef uint32_t    u32;
typedef uint64_t    u64;
typedef int32_t     i32;

#include "font.h"   /* 8x16 glyphs, s_font[95][16], 0x20..0x7E */
#include "../perm/perm.h"   /* perm.ui port: Powerbox prompt rendering */

/* ====================================================================
 * Constants
 * ==================================================================== */

/* Protocol ops */
#define TERM_OP_WRITE      1
#define TERM_OP_CLEAR      2
#define TERM_MAX_DATA      256         /* max payload bytes per request */

/* Virtual address for the framebuffer mapping.  Must be page-aligned,
 * below USER_PTR_MAX (0x0000800000000000), and must not collide with
 * the user-address-space regions the kernel already lays out:
 *   ELF           @ 0x400000
 *   heap          @ [0x70000000, 0x78000000) + 256 MB region
 *   stack         @ [0x90000000, 0x100000000) (ASLR, 1 MB blocks)
 *   0x400000000 (16 GiB) is far above all of them. */
#define TERM_FB_VA          0x400000000ULL

/* Palette (kernel boot-screen colors) */
#define TERM_FG             0x00FFFFFF      /* white text     */
#define TERM_BG             0x00082860      /* dark blue bg   */

/* Terminal geometry limits (static buffer sizing) */
#define TERM_MAX_COLS       256
#define TERM_MAX_ROWS       128

/* ====================================================================
 * Protocol structures (flat, raw copy — see header comment)
 * ==================================================================== */

typedef struct {
    u32 op;
    u32 len;
    u8  data[];             /* payload (WRITE) */
} term_req_t;

typedef struct {
    i32 ret;
} term_resp_t;

#define TERM_REQ_HDR      ((u32)sizeof(term_req_t))
#define TERM_RESP_HDR     ((u32)sizeof(term_resp_t))

/* ====================================================================
 * Terminal state
 * ==================================================================== */

static u8  s_req_buf[TERM_REQ_HDR + TERM_MAX_DATA];
static u8  s_resp_buf[TERM_RESP_HDR];

/* Framebuffer descriptor (from SYS_FB_GET_INFO) */
static u64  s_fb_va;            /* mapped virtual address        */
static u32  s_fb_width;         /* logical px (VGA text: scaled) */
static u32  s_fb_height;        /* logical px (VGA text: scaled) */
static u32  s_fb_pitch;         /* bytes per scanline (linear)   */
static u8   s_fb_bpp;           /* bits per pixel (linear)       */
static u8   s_vga_text;         /* 1 = 0xB8000 text buffer       */

/* Character grid (text cells) */
static u32  s_cols;             /* columns = width  / 9          */
static u32  s_rows;             /* rows    = height / 20         */
static u8   s_cells[TERM_MAX_ROWS][TERM_MAX_COLS];   /* screen buffer */
static u32  s_cursor_x;         /* cell coordinates              */
static u32  s_cursor_y;
static int  s_render_lock = -1; /* mutex: term loop ⇄ perm.ui     */

/* ====================================================================
 * Color helpers (mirror of kernel rgb_to_vga_attr)
 * ==================================================================== */

static u8 term_rgb_to_vga_attr(u32 rgb, int bg)
{
    u8 r = (u8)(rgb >> 16);
    u8 g = (u8)(rgb >> 8);
    u8 b = (u8)(rgb);

    static const u8 vga_r[16] = {0,0,0,0,170,170,85,255,85,85,85,85,255,255,255,255};
    static const u8 vga_g[16] = {0,0,170,170,0,0,85,255,85,85,255,255,85,85,255,255};
    static const u8 vga_b[16] = {0,170,0,170,0,170,0,255,85,255,85,255,85,255,85,255};

    u8 best = 7;
    u32 best_dist = 0xFFFFFFFF;
    for (int i = 0; i < 16; i++) {
        i32 dr = (i32)r - (i32)vga_r[i];
        i32 dg = (i32)g - (i32)vga_g[i];
        i32 db = (i32)b - (i32)vga_b[i];
        u32 dist = (u32)(dr*dr + dg*dg + db*db);
        if (dist < best_dist) {
            best_dist = dist;
            best = (u8)i;
        }
    }
    if (bg)
        return best & 7;            /* no bright backgrounds */
    return best;
}

/* ====================================================================
 * Pixel / rectangle drawing (linear RGB mode)
 * ==================================================================== */

static void term_pixel(u32 x, u32 y, u32 c)
{
    if (x >= s_fb_width || y >= s_fb_height)
        return;

    if (s_fb_bpp == 32) {
        *(volatile u32 *)(s_fb_va + (u64)y * s_fb_pitch + (u64)x * 4) = c;
    } else if (s_fb_bpp == 24) {
        /* Framebuffer expects BGR byte order (byte 0 = Blue, byte 2 = Red) */
        volatile u8 *p = (volatile u8 *)(s_fb_va + (u64)y * s_fb_pitch + (u64)x * 3);
        p[0] = (u8)(c);
        p[1] = (u8)(c >> 8);
        p[2] = (u8)(c >> 16);
    }
}

static void term_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 c)
{
    if (x >= s_fb_width || y >= s_fb_height)
        return;
    if (x + w > s_fb_width)  w = s_fb_width - x;
    if (y + h > s_fb_height) h = s_fb_height - y;

    if (s_fb_bpp == 32) {
        for (u32 row = 0; row < h; row++) {
            volatile u32 *line = (volatile u32 *)(s_fb_va + (u64)(y + row) * s_fb_pitch);
            for (u32 col = 0; col < w; col++)
                line[x + col] = c;
        }
    } else if (s_fb_bpp == 24) {
        u8 b_val = (u8)(c);
        u8 g = (u8)(c >> 8);
        u8 r = (u8)(c >> 16);
        for (u32 row = 0; row < h; row++) {
            volatile u8 *line = (volatile u8 *)(s_fb_va + (u64)(y + row) * s_fb_pitch);
            for (u32 col = 0; col < w; col++) {
                u32 off = (x + col) * 3;
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
static u8 term_cell_attr(u32 fg, u32 bg)
{
    return (u8)((term_rgb_to_vga_attr(bg, 1) << 4) |
                term_rgb_to_vga_attr(fg, 0));
}

/*
 * Draw one text cell (s_cells coordinate) on the framebuffer.
 * Cell grid is 9 px wide x 20 px tall (8x16 glyph + 1px right spacing
 * + 4px bottom spacing), matching the kernel's FB_COL/FB_ROW macros.
 */
static void term_draw_cell(u32 cx, u32 cy, u8 ch, u32 fg, u32 bg)
{
    if (cx >= s_cols || cy >= s_rows)
        return;

    if (s_vga_text) {
        if (ch < 0x20 || ch > 0x7E)
            ch = ' ';
        u8 attr = term_cell_attr(fg, bg);
        volatile u16 *buf = (volatile u16 *)s_fb_va;
        buf[cy * s_cols + cx] = (u16)((u16)attr << 8) | ch;
        return;
    }

    /* Linear mode: fill the cell with bg, then stamp the glyph */
    u32 px = cx * 9;
    u32 py = cy * 20;
    term_fill_rect(px, py, 9, 20, bg);
    if (ch < 0x20 || ch > 0x7E)
        return;                     /* no glyph for control chars */

    const u8 *glyph = s_font[ch - 0x20];
    for (int row = 0; row < 16; row++) {
        u8 bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col))
                term_pixel(px + col, py + row, fg);
        }
    }
}

/* ====================================================================
 * Cursor rendering
 *
 * The cursor is drawn as an inverted-color cell: the stored character
 * is re-rendered with foreground/background swapped.  To erase it we
 * re-render the same cell with the normal palette (s_cells holds the
 * character, so no pixel read-back is needed).
 * ==================================================================== */

static void term_draw_cursor(void)
{
    u8 ch = s_cells[s_cursor_y][s_cursor_x];
    term_draw_cell(s_cursor_x, s_cursor_y, ch, TERM_BG, TERM_FG);
}

static void term_erase_cursor(void)
{
    u8 ch = s_cells[s_cursor_y][s_cursor_x];
    term_draw_cell(s_cursor_x, s_cursor_y, ch, TERM_FG, TERM_BG);
}

/* ====================================================================
 * Scrolling
 * ==================================================================== */

/*
 * Scroll the whole screen up by one text row (20 px / one cell row).
 * Both the pixel buffer and the s_cells screen buffer are shifted;
 * the new bottom row is cleared with the background color.
 */
static void term_scroll(void)
{
    /* Shift the character buffer up by one row */
    for (u32 r = 1; r < s_rows; r++)
        memcpy(s_cells[r - 1], s_cells[r], s_cols);
    memset(s_cells[s_rows - 1], ' ', s_cols);

    /* Shift the framebuffer up by one cell row */
    if (s_vga_text) {
        u32 row_bytes = s_cols * 2;
        memmove((void *)s_fb_va, (const void *)(s_fb_va + row_bytes),
                row_bytes * (s_rows - 1));
        /* Clear the bottom row with background attribute + space */
        u8 attr = term_cell_attr(TERM_FG, TERM_BG);
        volatile u16 *buf = (volatile u16 *)s_fb_va;
        for (u32 c = 0; c < s_cols; c++)
            buf[(s_rows - 1) * s_cols + c] = (u16)((u16)attr << 8) | ' ';
    } else {
        u32 row_px = 20;
        u32 shift = row_px * s_fb_pitch;
        u32 bytes = (s_rows - 1) * row_px * s_fb_pitch;
        memmove((void *)s_fb_va, (const void *)(s_fb_va + shift), bytes);
        term_fill_rect(0, (s_rows - 1) * row_px, s_fb_width, row_px, TERM_BG);
    }
}

/* ====================================================================
 * Text output
 * ==================================================================== */

/*
 * Render one character at the cursor and advance it.
 * Handles \n (CRLF), \r, \b (backspace), \t (tab stops of 8),
 * printable ASCII; other control chars are ignored.  Wraps at the
 * right edge and scrolls at the bottom edge.
 */
static void term_putc(char ch)
{
    term_erase_cursor();

    switch (ch) {
    case '\n':
        s_cursor_x = 0;
        s_cursor_y++;
        break;
    case '\r':
        s_cursor_x = 0;
        break;
    case '\b':
        if (s_cursor_x > 0) {
            s_cursor_x--;
            s_cells[s_cursor_y][s_cursor_x] = ' ';
            term_draw_cell(s_cursor_x, s_cursor_y, ' ', TERM_FG, TERM_BG);
        }
        break;
    case '\t':
        s_cursor_x = (s_cursor_x / 8 + 1) * 8;
        break;
    default:
        if (ch >= 0x20 && ch <= 0x7E) {
            s_cells[s_cursor_y][s_cursor_x] = (u8)ch;
            term_draw_cell(s_cursor_x, s_cursor_y, (u8)ch, TERM_FG, TERM_BG);
            s_cursor_x++;
        }
        break;
    }

    /* Wrap at the right edge, scroll at the bottom */
    if (s_cursor_x >= s_cols) {
        s_cursor_x = 0;
        s_cursor_y++;
    }
    if (s_cursor_y >= s_rows) {
        term_scroll();
        s_cursor_y = s_rows - 1;
    }

    term_draw_cursor();
}

/*
 * Render len bytes of text.  Returns the number of bytes consumed
 * (all of them — the terminal is a sink, it never blocks or drops).
 * Serializes with the perm.ui thread on s_render_lock.
 *
 * Serial mirror: every byte rendered to the framebuffer is also
 * written to the kernel debug log (COM1) so headless QEMU sessions
 * (`-nographic`, serial log) can observe the full shell session —
 * prompts, echoed input, command output, and the Powerbox prompt
 * lines from the perm.ui thread (which carry the query id needed by
 * `perm_answer`).  This is a debugging aid; the framebuffer remains
 * the primary console.
 */
static i32 term_write(const u8 *data, u32 len)
{
    /* Mirror to the serial console via SYS_DEBUG_LOG.  debug_log()
     * takes a NUL-terminated string, so copy into a small local
     * buffer (chunked) rather than printing byte-by-byte. */
    {
        char buf[64];
        u32 off = 0;
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

    if (s_render_lock >= 0)
        (void)mutex_lock(s_render_lock);
    for (u32 i = 0; i < len; i++)
        term_putc((char)data[i]);
    if (s_render_lock >= 0)
        (void)mutex_unlock(s_render_lock);
    return (i32)len;
}

/*
 * Clear the screen and reset the cursor to the top-left corner.
 * Blank cells store ' ' so the cursor erases to a clean background.
 * Serializes with the perm.ui thread on s_render_lock.
 */
static void term_clear(void)
{
    if (s_render_lock >= 0)
        (void)mutex_lock(s_render_lock);

    for (u32 r = 0; r < s_rows; r++)
        memset(s_cells[r], ' ', s_cols);

    if (s_vga_text) {
        u8 attr = term_cell_attr(TERM_FG, TERM_BG);
        volatile u16 *buf = (volatile u16 *)s_fb_va;
        for (u32 i = 0; i < s_cols * s_rows; i++)
            buf[i] = (u16)((u16)attr << 8) | ' ';
    } else {
        term_fill_rect(0, 0, s_fb_width, s_fb_height, TERM_BG);
    }

    s_cursor_x = 0;
    s_cursor_y = 0;
    term_draw_cursor();

    if (s_render_lock >= 0)
        (void)mutex_unlock(s_render_lock);
}

/* ====================================================================
 * Server side
 * ==================================================================== */

static void term_reply(int token, i32 ret)
{
    term_resp_t *resp = (term_resp_t *)s_resp_buf;
    resp->ret = ret;
    int r = ipc_reply(token, s_resp_buf, (int)TERM_RESP_HDR);
    if (r < 0)
        printf("term: ipc_reply failed (%d)\n", r);
}

/*
 * Interpret one client request and reply.  Never crashes on malformed
 * input: the opcode is validated and the payload length is capped at
 * TERM_MAX_DATA (and checked against what ipc_recv actually reported).
 */
static void term_handle_request(int token, int msg_len)
{
    if (msg_len > (int)sizeof(s_req_buf))
        msg_len = (int)sizeof(s_req_buf);

    if (msg_len < (int)TERM_REQ_HDR) {
        term_reply(token, ERR_INVAL);
        return;
    }

    term_req_t *req = (term_req_t *)s_req_buf;

    if (req->op == TERM_OP_WRITE) {
        if (req->len > TERM_MAX_DATA ||
            msg_len < (int)(TERM_REQ_HDR + req->len)) {
            term_reply(token, ERR_INVAL);
            return;
        }
        term_reply(token, term_write(req->data, req->len));
    } else if (req->op == TERM_OP_CLEAR) {
        term_clear();
        term_reply(token, 0);
    } else {
        term_reply(token, ERR_INVAL);
    }
}

static void term_server_loop(int port)
{
    for (;;) {
        int msg_len = (int)sizeof(s_req_buf);
        int token = 0;
        int ret = ipc_recv(port, s_req_buf, &msg_len, &token);
        if (ret < 0) {
            printf("term: ipc_recv failed (%d)\n", ret);
            thread_exit(1);
        }

        term_handle_request(token, msg_len);
    }
}

/* ====================================================================
 * perm.ui agent thread (design §8 决策 2: "UI 代理注册：term 启动时
 * 向 perm-manager 注册 port 名为 perm.ui")
 *
 * The perm-manager PUSHES PERM_OP_UI_SHOW notifications here (one per
 * Powerbox query state change).  This thread renders each as a text
 * prompt line on the terminal — e.g.:
 *
 *   perm: app 0x1234 requests /Users/a.txt (R) - perm_answer 3 y/n
 *
 * Answers are given in the shell (perm_answer <id> y/n), which talks
 * to the perm-manager directly; this thread is display-only.  Every
 * message is acknowledged so the perm-manager's ipc_call completes.
 * ==================================================================== */

static void perm_ui_main(void *arg)
{
    (void)arg;

    int port = ipc_port_create();
    if (port < 0) {
        printf("term: perm.ui ipc_port_create failed (%d)\n", port);
        thread_exit(1);
    }
    int ret = port_register(PERM_UI_PORT_NAME, port);
    if (ret < 0) {
        printf("term: perm.ui port_register('%s') failed (%d)\n",
               PERM_UI_PORT_NAME, ret);
        thread_exit(1);
    }
    printf("term: perm.ui port %d registered\n", port);

    static u8 s_ui_req[sizeof(perm_req_ui_t)];
    static u8 s_ui_resp[sizeof(perm_resp_ui_t)];

    for (;;) {
        int msg_len = (int)sizeof(s_ui_req);
        int token = 0;
        ret = ipc_recv(port, s_ui_req, &msg_len, &token);
        if (ret < 0) {
            printf("term: perm.ui ipc_recv failed (%d)\n", ret);
            thread_exit(1);
        }

        perm_resp_ui_t *resp = (perm_resp_ui_t *)s_ui_resp;
        resp->ret = 0;

        if (msg_len >= (int)sizeof(perm_req_ui_t)) {
            perm_req_ui_t *req = (perm_req_ui_t *)s_ui_req;
            if (req->op == PERM_OP_UI_SHOW) {
                /* One status line: state, app, url, access, query id. */
                char line[512];
                char acc[4];
                u32 n = 0;

                if (req->state == PERM_QUERY_PENDING) {
                    memcpy(line + n, "perm: ", 6); n += 6;
                } else if (req->state == PERM_QUERY_ALLOWED) {
                    memcpy(line + n, "perm: [ALLOWED] ", 16); n += 16;
                } else {
                    memcpy(line + n, "perm: [DENIED] ", 15); n += 15;
                }

                line[n++] = 'a';
                line[n++] = 'p';
                line[n++] = 'p';
                line[n++] = ' ';
                line[n++] = '0';
                line[n++] = 'x';
                /* app_id_hash as hex */
                {
                    char tmp[12];
                    int tn = 0;
                    u32 v = req->app_id_hash;
                    if (v == 0) tmp[tn++] = '0';
                    while (v > 0 && tn < 10) {
                        u32 d = v & 0xF;
                        tmp[tn++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                        v >>= 4;
                    }
                    while (tn > 0) line[n++] = tmp[--tn];
                }

                memcpy(line + n, " requests ", 10); n += 10;
                u32 ul = 0;
                while (req->url[ul] && ul < sizeof(req->url) - 1 &&
                       n < sizeof(line) - 16)
                    line[n++] = req->url[ul++];

                /* access mask → "R"/"W"/"RW" */
                u32 an = 0;
                if (req->access & VFS_ACCESS_READ)  acc[an++] = 'R';
                if (req->access & VFS_ACCESS_WRITE) acc[an++] = 'W';
                if (req->access & VFS_ACCESS_EXEC)  acc[an++] = 'X';
                acc[an] = '\0';
                line[n++] = ' ';
                line[n++] = '(';
                memcpy(line + n, acc, an); n += an;
                line[n++] = ')';

                if (req->state == PERM_QUERY_PENDING) {
                    memcpy(line + n, " - perm_answer ", 15); n += 15;
                    /* query_id as decimal */
                    {
                        char tmp[12];
                        int tn = 0;
                        u32 v = req->query_id;
                        if (v == 0) tmp[tn++] = '0';
                        while (v > 0 && tn < 10) {
                            tmp[tn++] = (char)('0' + (v % 10));
                            v /= 10;
                        }
                        while (tn > 0) line[n++] = tmp[--tn];
                    }
                    memcpy(line + n, " y/n\n", 5); n += 5;
                } else {
                    line[n++] = '\n';
                }

                term_write((u8 *)line, n);
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
static void term_service_main(void *arg)
{
    (void)arg;

    printf("term: starting framebuffer terminal service\n");

    /* 1. Query the framebuffer descriptor. */
    fb_user_info_t info;
    int ret = fb_get_info(&info);
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
           s_fb_width, s_fb_height, s_fb_bpp, s_fb_pitch,
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
           (u32)(s_fb_va >> 32), (u32)s_fb_va, (u32)fb_size);

    /* 3. Cell grid geometry: 9 px/col, 20 px/row (FB_COL/FB_ROW). */
    s_cols = s_fb_width / 9;
    s_rows = s_fb_height / 20;
    if (s_cols == 0 || s_rows == 0) {
        printf("term: degenerate fb geometry (%ux%u)\n", s_cols, s_rows);
        thread_exit(1);
    }
    if (s_cols > TERM_MAX_COLS) s_cols = TERM_MAX_COLS;
    if (s_rows > TERM_MAX_ROWS) s_rows = TERM_MAX_ROWS;
    printf("term: %ux%u cells\n", s_cols, s_rows);

    /* 4. IPC port, registered under the well-known name "term". */
    int port = ipc_port_create();
    if (port < 0) {
        printf("term: ipc_port_create failed (%d)\n", port);
        thread_exit(1);
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

    /* 6. Blank the screen and show the cursor. */
    term_clear();

    /* 7. Serve clients. */
    printf("term: serving on port %d\n", port);
    term_server_loop(port);
}

/* ====================================================================
 * Process entry point (crt0 calls main())
 * ==================================================================== */

int main(void)
{
    term_service_main(NULL);
    return 0;   /* unreachable */
}
