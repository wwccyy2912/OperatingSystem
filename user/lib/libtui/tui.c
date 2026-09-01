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
 * tui.c - TUI (Text User Interface) client library implementation
 * Copyright (c) 2026 OpSys Project
 *
 * IPC wrappers for terminal service operations.
 *
 * ------------------------------------------------------------------
 * Structure (IPC client, rendering, snapshots, dialogs):
 *   TuiWrite/RenderBox/RenderLineAt/... -> TuiCall(op, payload) -> term service
 *   TuiRegionSave -> cells[w*h] snapshot ; TuiRegionRestore -> write back
 *   TuiInputLine/TuiConfirm/TuiMenu -> TuiKbdReadKey + render + restore
 * How it works:
 *   The service port is resolved once and cached; every operation packs
 *   a request into s_req and ships it through TuiCall, reading the reply
 *   into s_resp.  Region snapshots copy a rectangle of cells into a
 *   caller buffer so popups can restore what they covered.
 * Purpose:
 *   Client-side TUI for the terminal service: output, boxes, menus,
 *   prompts, and snapshot/restore of screen regions.
 * Caveats:
 *   Static request/response buffers (4 KiB) and a single cached port
 *   make the library non-reentrant; oversized payloads do not fit.
 *   Snapshot cell format (uint16_t per cell) must match the service.
 * ------------------------------------------------------------------
 */

#include "tui.h"
#include "../libc/stdio.h"
#include "../libc/string.h"
#include "../libos/syscalls.h"
#include <stdarg.h>

typedef uint8_t  u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  i32;

/* Cached port (resolved on first use) */
static int s_tui_port = -2; /* -2 = not yet resolved, -1 = failed, >=0 = port */

/* Request/response buffers */
static u8 s_req[4096];
static u8 s_resp[4096];
static int s_resp_len; /* actual reply bytes from the last tui_call */

/* ====================================================================
 * Internal: port resolution and IPC call
 * ==================================================================== */

int TuiPortGet(void) {
    if (s_tui_port >= -1)
        return s_tui_port;

    s_tui_port = PortGet(TUI_PORT_NAME);
    return s_tui_port;
}

static int TuiCall(u32 op, const void *payload, u32 payload_len) {
    int port = TuiPortGet();
    if (port < 0)
        return port;

    /* Build request: op + len + payload */
    u32 *req = (u32 *)s_req;
    req[0]   = op;
    req[1]   = payload_len;
    if (payload_len > 0 && payload)
        memcpy(s_req + 8, payload, payload_len);

    /* Call and get response */
    int resp_len = (int)sizeof(s_resp);
    int ret      = IpcCall(port, s_req, 8 + payload_len, s_resp, &resp_len);
    if (ret < 0)
        return ret;
    s_resp_len = resp_len;

    /* Response is at least 4 bytes (ret field) */
    if (resp_len < 4)
        return -1; /* malformed response */

    i32 *resp = (i32 *)s_resp;
    return resp[0];
}

/* ====================================================================
 * Basic output
 * ==================================================================== */

int TuiWrite(const char *text, uint32_t len) {
    if (!text || len == 0)
        return 0;
    if (len > TUI_MAX_TEXT)
        len = TUI_MAX_TEXT;

    return TuiCall(TUI_OP_WRITE, text, len);
}

int TuiWriteStr(const char *str) {
    if (!str)
        return 0;
    uint32_t len = 0;
    while (str[len] && len < TUI_MAX_TEXT)
        len++;
    return TuiWrite(str, len);
}

/* ====================================================================
 * Screen control
 * ==================================================================== */

int TuiClear(void) {
    return TuiCall(TUI_OP_CLEAR, NULL, 0);
}

int TuiRenderStatus(const char *prefix, const char *msg) {
    if (!prefix)
        prefix = "";
    if (!msg)
        msg = "";

    uint32_t plen = 0, mlen = 0;
    while (prefix[plen] && plen < 63)
        plen++;
    while (msg[mlen] && mlen < 127)
        mlen++;

    /* Payload: prefix_len(4) + msg_len(4) + prefix + msg */
    u8   payload[8 + 64 + 128];
    u32 *lengths = (u32 *)payload;
    lengths[0]   = plen;
    lengths[1]   = mlen;
    memcpy(payload + 8, prefix, plen);
    memcpy(payload + 8 + plen, msg, mlen);

    return TuiCall(TUI_OP_STATUS, payload, 8 + plen + mlen);
}

/* ====================================================================
 * Box/border drawing
 * ==================================================================== */

int TuiRenderBox(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const char *title) {
    if (!title)
        title = "";

    uint32_t tlen = 0;
    while (title[tlen] && tlen < 63)
        tlen++;

    /* Payload: x(4) + y(4) + w(4) + h(4) + title_len(4) + title */
    u8   payload[20 + 64];
    u32 *args = (u32 *)payload;
    args[0]   = x;
    args[1]   = y;
    args[2]   = w;
    args[3]   = h;
    args[4]   = tlen;
    if (tlen > 0)
        memcpy(payload + 20, title, tlen);

    return TuiCall(TUI_OP_BOX, payload, 20 + tlen);
}

/* ====================================================================
 * Text rendering without cursor change
 * ==================================================================== */

int TuiRenderLineAt(uint32_t x, uint32_t y, const char *text, uint32_t len) {
    if (!text || len == 0)
        return 0;
    if (len > TUI_MAX_TEXT)
        len = TUI_MAX_TEXT;

    /* Payload: x(4) + y(4) + text */
    u8   payload[8 + TUI_MAX_TEXT];
    u32 *coords = (u32 *)payload;
    coords[0]   = x;
    coords[1]   = y;
    memcpy(payload + 8, text, len);

    return TuiCall(TUI_OP_RENDER_LINE, payload, 8 + len);
}

/* ====================================================================
 * Cursor control
 * ==================================================================== */

int TuiSetCursor(uint32_t x, uint32_t y) {
    u32 coords[2] = {x, y};
    return TuiCall(TUI_OP_SET_CURSOR, coords, 8);
}

int TuiGetCursor(uint32_t *x, uint32_t *y) {
    int ret = TuiCall(TUI_OP_GET_CURSOR, NULL, 0);
    if (ret < 0)
        return ret;
    /* Response: ret + x + y (12 bytes).  Guard against a truncated
     * reply so we never read stale bytes. */
    if (s_resp_len < 12)
        return -1;
    if (x)
        *x = ((u32 *)s_resp)[1];
    if (y)
        *y = ((u32 *)s_resp)[2];

    return 0;
}

int TuiGetSize(uint32_t *cols, uint32_t *rows) {
    int ret = TuiCall(TUI_OP_GET_SIZE, NULL, 0);
    if (ret < 0)
        return ret;
    if (s_resp_len < 12)
        return -1;
    if (cols)
        *cols = ((u32 *)s_resp)[1];
    if (rows)
        *rows = ((u32 *)s_resp)[2];
    return 0;
}

/* ====================================================================
 * Utility: formatted output
 * ==================================================================== */

int TuiPrintf(const char *fmt, ...) {
    if (!fmt)
        return 0;

    va_list ap;
    char    buf[512];
    int     len = 0;

    va_start(ap, fmt);
    for (const char *p = fmt; *p != '\0' && len < (int)sizeof(buf) - 1; p++) {
        if (*p != '%') {
            buf[len++] = *p;
            continue;
        }
        p++;
        if (*p == '\0')
            break;

        switch (*p) {
        case '%':
            buf[len++] = '%';
            break;
        case 'c':
            if (len < (int)sizeof(buf) - 1)
                buf[len++] = (char)va_arg(ap, int);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s) {
                while (*s && len < (int)sizeof(buf) - 1)
                    buf[len++] = *s++;
            }
            break;
        }
        case 'd': {
            int  val = va_arg(ap, int);
            char tmp[12];
            /* INT_MIN-safe: negate in the unsigned domain. */
            u32 uv = (val < 0) ? (u32)0 - (u32)val : (u32)val;
            int  neg = (val < 0);
            int  i = 0;
            if (uv == 0)
                tmp[i++] = '0';
            while (uv > 0) {
                tmp[i++] = (char)('0' + (uv % 10));
                uv /= 10;
            }
            if (neg && len < (int)sizeof(buf) - 1)
                buf[len++] = '-';
            while (i > 0 && len < (int)sizeof(buf) - 1)
                buf[len++] = tmp[--i];
            break;
        }
        case 'x': {
            u32  val = va_arg(ap, u32);
            char tmp[9];
            int  i = 0;
            if (val == 0)
                tmp[i++] = '0';
            while (val > 0 && i < 8) {
                u32 d    = val & 0xF;
                tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                val >>= 4;
            }
            while (i > 0 && len < (int)sizeof(buf) - 1)
                buf[len++] = tmp[--i];
            break;
        }
        default:
            if (len < (int)sizeof(buf) - 1)
                buf[len++] = '%';
            if (len < (int)sizeof(buf) - 1)
                buf[len++] = *p;
            break;
        }
    }
    va_end(ap);

    buf[len] = '\0';
    return TuiWrite(buf, (u32)len);
}

/* ====================================================================
 * Region snapshot/restore (v1.2)
 * ==================================================================== */

int TuiRegionSave(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t *cells) {
    if (!cells || w == 0 || h == 0 || (uint64_t)w * h > TUI_MAX_REGION_CELLS)
        return -2; /* ERR_INVAL */

    int port = TuiPortGet();
    if (port < 0)
        return port;

    u32 *req = (u32 *)s_req;
    req[0]   = TUI_OP_SNAPSHOT;
    req[1]   = 16; /* payload: x,y,w,h */
    req[2]   = x;
    req[3]   = y;
    req[4]   = w;
    req[5]   = h;

    int resp_len = (int)sizeof(s_resp);
    int ret      = IpcCall(port, s_req, 8 + 16, s_resp, &resp_len);
    if (ret < 0)
        return ret;
    if (resp_len < 4)
        return -1;
    i32 r = *(i32 *)s_resp;
    if (r < 0)
        return r;
    memcpy(cells, s_resp + 4, (size_t)(w * h) * 2); /* u16 code points */
    return 0;
}

int TuiRegionRestore(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *cells) {
    if (!cells || w == 0 || h == 0 || (uint64_t)w * h > TUI_MAX_REGION_CELLS)
        return -2; /* ERR_INVAL */

    int port = TuiPortGet();
    if (port < 0)
        return port;

    u32 *req = (u32 *)s_req;
    req[0]   = TUI_OP_RESTORE;
    req[1]   = 16 + (u32)(w * h) * 2;
    req[2]   = x;
    req[3]   = y;
    req[4]   = w;
    req[5]   = h;
    memcpy(s_req + 8 + 16, cells, (size_t)(w * h) * 2);

    int resp_len = (int)sizeof(s_resp);
    int ret      = IpcCall(port, s_req, 8 + 16 + (int)(w * h) * 2, s_resp, &resp_len);
    if (ret < 0)
        return ret;
    if (resp_len < 4)
        return -1;
    return *(i32 *)s_resp;
}

/* ====================================================================
 * Interactive components (v1.1)
 * ==================================================================== */

static int s_tui_kbd_port = -2; /* -2 unresolved, -1 failed, >=0 port */

static int TuiKbdGet(void) {
    if (s_tui_kbd_port >= -1)
        return s_tui_kbd_port;
    s_tui_kbd_port = PortGet("keyboard");
    return s_tui_kbd_port;
}

/* Read one key (READ_BLOCK, ASCII char in data[0]). */
static int TuiKbdReadKey(u8 *key) {
    int port = TuiKbdGet();
    if (port < 0)
        return -1;
    u32 *req = (u32 *)s_req;
    req[0]   = 2; /* KBD_OP_READ_BLOCK */
    req[1]   = 1; /* max data bytes */
    int  resp_len = (int)sizeof(s_resp);
    if (IpcCall(port, s_req, 8, s_resp, &resp_len) < 0 || resp_len < 4)
        return -1;
    if (key)
        *key = s_resp[4];
    return *(i32 *)s_resp;
}

int TuiInputLine(int x, int y, const char *prompt, char *buf, int maxlen, int mask) {
    if (!buf || maxlen <= 1)
        return -1;

    /* Non-destructive overlay: save the cells the prompt line will
     * cover plus the cursor, and restore both before returning. */
    int  plen = prompt ? (int)strlen(prompt) : 0;
    u32  rw   = (u32)(plen + maxlen + 1);
    uint16_t saved[TUI_MAX_REGION_CELLS];
    int  has_saved = 0;
    u32  cx = 0, cy = 0;
    int  have_cursor = 0;
    if (rw <= TUI_MAX_REGION_CELLS && TuiRegionSave((u32)x, (u32)y, rw, 1, saved) == 0) {
        has_saved = 1;
        if (TuiGetCursor(&cx, &cy) == 0)
            have_cursor = 1;
    }

    int pos = 0;
    buf[0]  = '\0';
    for (;;) {
        /* Render prompt + masked content + cursor marker. */
        char disp[256];
        int  d = 0;
        if (prompt) {
            while (prompt[d] && d < (int)sizeof(disp) - 2) {
                disp[d] = prompt[d];
                d++;
            }
        }
        for (int i = 0; i < pos && d < (int)sizeof(disp) - 2; i++)
            disp[d++] = mask ? '*' : buf[i];
        disp[d++] = '_'; /* cursor */
        /* Pad to the saved row width so a shorter frame (e.g. after
         * backspace) erases the tail instead of leaving residue. */
        while (d < (int)rw && d < (int)sizeof(disp) - 1)
            disp[d++] = ' ';
        disp[d]   = '\0';
        TuiRenderLineAt(x, y, disp, (u32)d);
        TuiSetCursor((u32)(x + d), (u32)y);

        u8 key = 0;
        if (TuiKbdReadKey(&key) < 0) {
            if (has_saved) {
                (void)TuiRegionRestore((u32)x, (u32)y, rw, 1, saved);
                if (have_cursor)
                    (void)TuiSetCursor(cx, cy);
            }
            return -1;
        }
        if (key == '\r' || key == '\n') {
            break;
        } else if (key == '\b' || key == 0x7F) {
            if (pos > 0)
                pos--;
        } else if (key >= ' ' && key < 0x7F) {
            if (pos < maxlen - 1)
                buf[pos++] = (char)key;
        }
        buf[pos] = '\0';
    }
    buf[pos] = '\0';

    /* Restore the covered region and the previous cursor position. */
    if (has_saved) {
        (void)TuiRegionRestore((u32)x, (u32)y, rw, 1, saved);
        if (have_cursor)
            (void)TuiSetCursor(cx, cy);
    }
    return pos;
}

int TuiConfirm(int x, int y, int w, const char *title, const char *msg, const char *hint) {
    if (w < 16)
        w = 16;
    if (w > 100)
        w = 100;

    /* Non-destructive overlay: snapshot the dialog rectangle. */
    uint16_t saved[TUI_MAX_REGION_CELLS];
    int has_saved = 0;
    u32 cx = 0, cy = 0;
    int have_cursor = 0;
    if ((u64)w * 5 <= TUI_MAX_REGION_CELLS &&
        TuiRegionSave((u32)x, (u32)y, (u32)w, 5, saved) == 0) {
        has_saved = 1;
        if (TuiGetCursor(&cx, &cy) == 0)
            have_cursor = 1;
    }

    TuiRenderBox(x, y, (u32)w, 5, title);
    if (msg)
        TuiRenderLineAt(x + 2, (u32)(y + 2), msg, (u32)strlen(msg));
    if (hint)
        TuiRenderLineAt(x + 2, (u32)(y + 3), hint, (u32)strlen(hint));

    int result = -1;
    for (;;) {
        u8 key = 0;
        if (TuiKbdReadKey(&key) < 0) {
            result = -1;
            break;
        }
        if (key == 'y' || key == 'Y') {
            result = 1;
            break;
        }
        if (key == 'n' || key == 'N') {
            result = 0;
            break;
        }
    }

    /* Restore the covered region and the previous cursor position. */
    if (has_saved) {
        (void)TuiRegionRestore((u32)x, (u32)y, (u32)w, 5, saved);
        if (have_cursor)
            (void)TuiSetCursor(cx, cy);
    }
    return result;
}

/* ====================================================================
 * Menu component (v1.3)
 *
 * tui_menu: a titled, boxed, scrollable item list with keyboard
 * selection.  Keys: j/k or up/down-style (we map j/k and w/s), Enter
 * selects, q/Esc cancels.  Non-destructive: the covered region and
 * cursor are saved and restored, exactly like tui_confirm.
 *
 * Returns the selected index (0-based) on Enter, -1 on cancel/error.
 * ==================================================================== */

int TuiMenu(int x, int y, int w, int h, const char *title,
             const char *const *items, int count, int *scroll_out) {
    if (!items || count <= 0 || h < 3)
        return -1;
    if (w < 16)
        w = 16;
    if (w > 100)
        w = 100;

    /* Save the covered region + cursor (non-destructive overlay). */
    uint16_t saved[TUI_MAX_REGION_CELLS];
    int has_saved = 0;
    u32 cx = 0, cy = 0;
    int have_cursor = 0;
    if ((uint64_t)w * h <= TUI_MAX_REGION_CELLS &&
        TuiRegionSave((u32)x, (u32)y, (u32)w, (u32)h, saved) == 0) {
        has_saved = 1;
        if (TuiGetCursor(&cx, &cy) == 0)
            have_cursor = 1;
    }

    int sel    = 0;   /* selected item index */
    int scroll = 0;   /* first visible row */
    int rows   = h - 2; /* box interior rows (title + 2 borders) */

    int result = -1;
    for (;;) {
        /* Redraw the menu each key. */
        TuiRenderBox(x, y, (u32)w, (u32)h, title);
        int visible = (count - scroll < rows) ? count - scroll : rows;
        for (int r = 0; r < visible; r++) {
            int idx = scroll + r;
            char line[TUI_MAX_TEXT];
            int  ln = 0;
            if (idx == sel) {
                line[ln++] = '>';
                line[ln++] = ' ';
            } else {
                line[ln++] = ' ';
                line[ln++] = ' ';
            }
            const char *it = items[idx];
            while (*it && ln < w - 3 && ln < (int)sizeof(line) - 1)
                line[ln++] = *it++;
            /* Scroll indicators in the right margin: '^' on the first
             * visible row when scrolled down, 'v' on the last visible
             * row when more items remain below. */
            if (r == 0 && scroll > 0)
                line[ln++] = '^';
            else if (r == visible - 1 && scroll + visible < count)
                line[ln++] = 'v';
            line[ln] = '\0';
            TuiRenderLineAt((u32)(x + 1), (u32)(y + 1 + r), line, (u32)ln);
        }

        u8 key = 0;
        if (TuiKbdReadKey(&key) < 0) {
            result = -1;
            break;
        }
        if (key == 'j' || key == 's' || key == 0x0C) {
            /* down (j/s or Down arrow -> FF) */
            if (sel + 1 < count) {
                sel++;
                if (sel >= scroll + rows)
                    scroll = sel - rows + 1;
            }
        } else if (key == 'k' || key == 'w' || key == 0x0B) {
            /* up (k/w or Up arrow -> VT) */
            if (sel > 0) {
                sel--;
                if (sel < scroll)
                    scroll = sel;
            }
        } else if (key == 0x01 || key == 0x02) {
            /* Home (SOH) / PgUp (STX) — first item */
            sel    = 0;
            scroll = 0;
        } else if (key == 0x05 || key == 0x06) {
            /* End (ENQ) / PgDn (ACK) — last item */
            sel    = count - 1;
            scroll = (sel - rows + 1 > 0) ? sel - rows + 1 : 0;
        } else if (key == 'q' || key == 'Q') {
            result = -1; /* cancel */
            break;
        } else if (key == '\r' || key == '\n') {
            result = sel; /* select */
            break;
        }
    }

    /* Restore the covered region and the previous cursor position. */
    if (has_saved) {
        (void)TuiRegionRestore((u32)x, (u32)y, (u32)w, (u32)h, saved);
        if (have_cursor)
            (void)TuiSetCursor(cx, cy);
    }
    if (scroll_out)
        *scroll_out = scroll;
    return result;
}
