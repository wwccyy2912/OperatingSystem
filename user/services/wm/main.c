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
 * wm.c - Window Manager service (v0.4)
 * Copyright (c) 2026 OpSys Project
 *
 * The window manager owns the DESKTOP:
 *   - Window registry: fixed table of windows, each with a title,
 *     geometry (term cell units), a body-content buffer, and an owner
 *     subject (the process that created it — unforgeable via
 *     ipc_recv_from).  Mutation ops (DESTROY/MOVE/WRITE) are gated to
 *     the owner or an admin caller.
 *   - Compositor: renders every window through the term service (the
 *     display owner).  wm never maps the framebuffer itself — it stays
 *     a pure IPC client of term, exactly like window_demo.  The focused
 *     window is drawn last (reads as "on top") with a '*' title marker.
 *   - Input routing: while a desktop session is active the wm holds the
 *     keyboard focus (TAKE_FOCUS) and drives the desktop: 1-9 focus
 *     windows, h/j/k/l move the focused window, q closes the session.
 *
 * Two threads (mirror of term's perm.ui split):
 *   - server thread: owns the "wm" port, ipc_recv_from loop, applies
 *     registry ops, recomposites after every mutation.
 *   - input thread: waits for s_active, takes the keyboard focus, reads
 *     keys, applies focus/move/quit, recomposites, releases on quit.
 * Both serialize registry + compositing on s_lock (libos mutex).
 *
 * Spawned by the manager before the shell; idles (no focus, no screen
 * changes) until a client calls ACTIVATE.
 *
 * ------------------------------------------------------------------
 * Structure (WmMain):
 *   main() spawns two threads:
 *     WmServerMain   "wm" port, ipc_recv_from, applies registry ops
 *     WmInputMain    TAKE_FOCUS, reads keys, focus/move/quit
 *   window registry (title, geometry, body buffer, owner subject)
 *   compositor -> term service (focused window drawn last, '*')
 *   registry + compositing serialized on s_lock
 * How it works:
 *   Clients create/mutate windows over the "wm" port; mutation ops are
 *   gated to the owning subject or an admin caller.  Every change
 *   recomposites through the term service; the input thread drives the
 *   desktop (1-9 focus, h/j/k/l move, q quit) while the session is on.
 * Purpose:
 *   Character-cell window manager: owns the DESKTOP window registry,
 *   term-backed compositing, and keyboard input routing for clients.
 * Caveats:
 *   Never maps the framebuffer — a pure IPC client of term.  Idles
 *   (no focus, no screen changes) until a client calls ACTIVATE.
 * ------------------------------------------------------------------
 */

#include "wm.h"

#include <libc/stdio.h>
#include <libc/string.h>
#include <stdint.h>
#include <libos/syscalls.h>
#include <libtui/tui.h>

typedef uint8_t  u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  i32;

/* Screen geometry (term cell grid, 113 x 38) */
#define WM_SCREEN_COLS 113
#define WM_SCREEN_ROWS 38

/* Keyboard protocol ops (mirror of keyboard.c) */
#define KBD_OP_READ_BLOCK    2
#define KBD_OP_TAKE_FOCUS    3
#define KBD_OP_RELEASE_FOCUS 4

/* ------------------------------------------------------------------ */
/*  Window registry                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    int   in_use;
    u32   win_id;
    u64   owner; /* creating subject (unforgeable) */
    char  title[WM_TITLE_MAX];
    u32   x, y, w, h;
    char  body[WM_CONTENT_ROWS][WM_CONTENT_COLS + 1];
} wm_win_t;

static wm_win_t s_wins[WM_MAX_WINDOWS];
static u32      s_win_seq;   /* next win_id */
static u32      s_focus;     /* focused win_id (0 = none) */
static int      s_active;    /* 1 = desktop session on screen */
static int      s_lock;      /* registry + compositor mutex */

/* ------------------------------------------------------------------ */
/*  Keyboard                                                          */
/* ------------------------------------------------------------------ */

static int s_kbd_port = -2; /* -2 unresolved, -1 failed, >=0 port */

static int WmKbdGet(void) {
    if (s_kbd_port >= -1)
        return s_kbd_port;
    s_kbd_port = PortGet("keyboard");
    return s_kbd_port;
}

static int WmKbdReq(u32 op, u8 *key) {
    int port = WmKbdGet();
    if (port < 0)
        return -1;
    u32 req[2]; /* { op; len } */
    u8  resp[8];
    req[0] = op;
    req[1] = 1;
    int resp_len = (int)sizeof(resp);
    if (IpcCall(port, req, 8, resp, &resp_len) < 0 || resp_len < 4)
        return -1;
    if (key)
        *key = resp[4];
    return (int)*(i32 *)resp;
}

/* ------------------------------------------------------------------ */
/*  Registry helpers                                                  */
/* ------------------------------------------------------------------ */

static wm_win_t *win_find(u32 id) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (s_wins[i].in_use && s_wins[i].win_id == id)
            return &s_wins[i];
    return NULL;
}

static int WinCount(void) {
    int n = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (s_wins[i].in_use)
            n++;
    return n;
}

/* Is the caller allowed to mutate win w?  Owner, or admin (SERVICE_MANAGE
 * atom — the user account service grants OWNER/ADMIN roles; a management
 * caller may manage any window). */
static int WinCanMutate(const wm_win_t *w, u64 caller) {
    if (w->owner == caller)
        return 1;
    return CapHasAtom(caller, ATOM_SERVICE_MANAGE) == 1;
}

/* ------------------------------------------------------------------ */
/*  Compositor (renders through term — the display owner)             */
/* ------------------------------------------------------------------ */

static void WmComposite(void) {
    TuiClear();
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!s_wins[i].in_use)
            continue;
        wm_win_t *w = &s_wins[i];
        if (w->x >= WM_SCREEN_COLS || w->y >= WM_SCREEN_ROWS)
            continue;
        /* Clamp geometry to the screen. */
        u32 cw = w->w;
        u32 ch = w->h;
        if (w->x + cw > WM_SCREEN_COLS)
            cw = WM_SCREEN_COLS - w->x;
        if (w->y + ch > WM_SCREEN_ROWS)
            ch = WM_SCREEN_ROWS - w->y;
        if (cw < 4 || ch < 3)
            continue;

        char title[WM_TITLE_MAX + 4];
        int  focused = (w->win_id == s_focus);
        snprintf(title, sizeof(title), "%c %s", focused ? '*' : ' ', w->title);
        TuiRenderBox(w->x, w->y, cw, ch, title);

        /* Body rows (clamped to the box interior). */
        u32 rows = (ch >= 3) ? ch - 2 : 0;
        for (u32 r = 0; r < rows && r < WM_CONTENT_ROWS; r++) {
            u32 tlen = (u32)strlen(w->body[r]);
            if (tlen > cw - 2)
                tlen = cw - 2;
            if (tlen > 0)
                TuiRenderLineAt(w->x + 1, w->y + 1 + r, w->body[r], tlen);
        }
    }

    char st[96];
    if (s_focus != 0) {
        wm_win_t *f = win_find(s_focus);
        snprintf(st, sizeof(st),
                 "wm: %d window(s) - focus %s (1-9 focus, hjkl move, q quit)",
                 WinCount(), f ? f->title : "?");
    } else {
        snprintf(st, sizeof(st), "wm: %d window(s) - 1-9 focus, hjkl move, q quit",
                 WinCount());
    }
    TuiRenderStatus("System", st);
}

/* ------------------------------------------------------------------ */
/*  Server thread: IPC ops                                            */
/* ------------------------------------------------------------------ */

static void WmApplyCreate(wm_req_t *req, wm_resp_t *resp, u64 caller) {
    req->title[WM_TITLE_MAX - 1] = '\0';
    u32 w = req->w, h = req->h;
    if (w < 4)
        w = 4;
    if (h < 3)
        h = 3;
    if (w > WM_SCREEN_COLS)
        w = WM_SCREEN_COLS;
    if (h > WM_SCREEN_ROWS)
        h = WM_SCREEN_ROWS;

    int slot = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!s_wins[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        resp->ret = -1; /* ERR_NOMEM: registry full */
        return;
    }

    wm_win_t *n   = &s_wins[slot];
    n->in_use     = 1;
    n->win_id     = (++s_win_seq != 0) ? s_win_seq : ++s_win_seq;
    n->owner      = caller;
    strncpy(n->title, req->title[0] ? req->title : "Untitled", sizeof(n->title) - 1);
    n->title[sizeof(n->title) - 1] = '\0';
    n->x          = req->x;
    n->y          = req->y;
    n->w          = w;
    n->h          = h;
    for (int r = 0; r < WM_CONTENT_ROWS; r++)
        n->body[r][0] = '\0';

    s_focus      = n->win_id;
    resp->ret    = 0;
    resp->win_id = n->win_id;
}

static void WmApplyDestroy(wm_req_t *req, wm_resp_t *resp, u64 caller) {
    wm_win_t *w = win_find(req->win_id);
    if (!w) {
        resp->ret = -4; /* ERR_NOENT */
        return;
    }
    if (!WinCanMutate(w, caller)) {
        resp->ret = -9; /* ERR_DENIED */
        return;
    }
    if (s_focus == w->win_id)
        s_focus = 0;
    memset(w, 0, sizeof(*w));
    resp->ret = 0;
}

static void WmApplyList(wm_resp_t *resp) {
    resp->count = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!s_wins[i].in_use)
            continue;
        if (resp->count >= WM_MAX_WINDOWS)
            break;
        snprintf(resp->lines[resp->count], sizeof(resp->lines[0]),
                 "%u %s", s_wins[i].win_id, s_wins[i].title);
        resp->count++;
    }
    resp->ret = 0;
}

static void WmApplyFocus(wm_req_t *req, wm_resp_t *resp) {
    if (req->win_id != 0 && !win_find(req->win_id)) {
        resp->ret = -4; /* ERR_NOENT */
        return;
    }
    s_focus   = req->win_id;
    resp->ret = 0;
}

static void WmApplyMove(wm_req_t *req, wm_resp_t *resp, u64 caller) {
    wm_win_t *w = win_find(req->win_id);
    if (!w) {
        resp->ret = -4;
        return;
    }
    if (!WinCanMutate(w, caller)) {
        resp->ret = -9;
        return;
    }
    /* Clamp so the window stays on screen. */
    i32 nx = (i32)req->mx, ny = (i32)req->my;
    if (nx < 0)
        nx = 0;
    if (ny < 0)
        ny = 0;
    if ((u32)nx + w->w > WM_SCREEN_COLS)
        nx = (i32)(WM_SCREEN_COLS - w->w);
    if ((u32)ny + w->h > WM_SCREEN_ROWS)
        ny = (i32)(WM_SCREEN_ROWS - w->h);
    w->x      = (u32)nx;
    w->y      = (u32)ny;
    resp->ret = 0;
}

static void WmApplyWrite(wm_req_t *req, wm_resp_t *resp, u64 caller) {
    wm_win_t *w = win_find(req->win_id);
    if (!w) {
        resp->ret = -4;
        return;
    }
    if (!WinCanMutate(w, caller)) {
        resp->ret = -9;
        return;
    }
    if (req->row >= WM_CONTENT_ROWS) {
        resp->ret = -2; /* ERR_INVAL */
        return;
    }
    req->text[WM_CONTENT_COLS - 1] = '\0';
    /* Strip control chars for the cell grid (0x20-0x7E only). */
    u32 n = 0;
    for (u32 i = 0; req->text[i] && i < WM_CONTENT_COLS - 1; i++) {
        char c = req->text[i];
        if (c >= ' ' && c <= 0x7E)
            w->body[req->row][n++] = c;
    }
    w->body[req->row][n] = '\0';
    resp->ret = 0;
}

static void WmApplyActivate(wm_resp_t *resp) {
    s_active  = 1;
    resp->ret = 0;
}

static void WmApplyDeactivate(wm_resp_t *resp) {
    s_active  = 0;
    s_focus   = 0;
    resp->ret = 0;
}

static void WmApplyGetState(wm_resp_t *resp) {
    resp->active = (u32)s_active;
    resp->focus  = s_focus;
    resp->count  = (u32)WinCount();
    resp->ret    = 0;
}

/* Server thread entry. */
static void WmServerMain(void *arg) {
    (void)arg;

    int port = IpcPortCreate();
    if (port < 0) {
        printf("wm: ipc_port_create failed (%d)\n", port);
        ThreadExit(1);
    }
    int ret = PortRegister(WM_PORT_NAME, port);
    if (ret < 0) {
        printf("wm: PortRegister('%s') failed (%d)\n", WM_PORT_NAME, ret);
        ThreadExit(1);
    }
    printf("wm: port %d registered as '%s'\n", port, WM_PORT_NAME);

    for (;;) {
        wm_req_t  req;
        wm_resp_t resp;
        int       msg_len = (int)sizeof(req);
        int       token   = 0;
        u64       caller  = 0;
        int       r       = IpcRecvFrom(port, &req, &msg_len, &token, &caller);
        if (r < 0) {
            printf("wm: ipc_recv failed (%d)\n", r);
            continue;
        }
        memset(&resp, 0, sizeof(resp));
        resp.ret = -2; /* ERR_INVAL until an op fills it */

        if (s_lock >= 0)
            (void)MutexLock(s_lock);
        switch (req.op) {
        case WM_OP_CREATE:
            WmApplyCreate(&req, &resp, caller);
            break;
        case WM_OP_DESTROY:
            WmApplyDestroy(&req, &resp, caller);
            break;
        case WM_OP_LIST:
            WmApplyList(&resp);
            break;
        case WM_OP_FOCUS:
            WmApplyFocus(&req, &resp);
            break;
        case WM_OP_MOVE:
            WmApplyMove(&req, &resp, caller);
            break;
        case WM_OP_WRITE:
            WmApplyWrite(&req, &resp, caller);
            break;
        case WM_OP_ACTIVATE:
            WmApplyActivate(&resp);
            break;
        case WM_OP_DEACTIVATE:
            WmApplyDeactivate(&resp);
            break;
        case WM_OP_GET_STATE:
            WmApplyGetState(&resp);
            break;
        default:
            break;
        }
        /* Recomposite whenever a mutation may have changed the screen
         * (skipped for LIST/GET_STATE/DEACTIVATE no-ops is not worth
         * the branch — a full redraw is cheap and idempotent). */
        WmComposite();
        if (s_lock >= 0)
            (void)MutexUnlock(s_lock);

        (void)IpcReply(token, &resp, (int)sizeof(resp));
    }
}

/* ------------------------------------------------------------------ */
/*  Input thread: keyboard focus routing while the session is active  */
/* ------------------------------------------------------------------ */

static void WmInputMain(void *arg) {
    (void)arg;

    for (;;) {
        /* Wait for an active session. */
        for (;;) {
            int active = 0;
            if (s_lock >= 0)
                (void)MutexLock(s_lock);
            active = s_active;
            if (s_lock >= 0)
                (void)MutexUnlock(s_lock);
            if (active)
                break;
            ThreadYield();
        }

        if (WmKbdReq(KBD_OP_TAKE_FOCUS, NULL) < 0) {
            printf("wm: TAKE_FOCUS failed\n");
            Sleep(50);
            continue;
        }
        printf("wm: desktop session active, keyboard focus taken\n");

        for (;;) {
            u8 key = 0;
            if (WmKbdReq(KBD_OP_READ_BLOCK, &key) < 0)
                break;

            if (s_lock >= 0)
                (void)MutexLock(s_lock);

            if (key >= '1' && key <= '9') {
                /* Focus the Nth existing window (1-based). */
                int n    = key - '0';
                int seen = 0;
                for (int i = 0; i < WM_MAX_WINDOWS; i++) {
                    if (!s_wins[i].in_use)
                        continue;
                    if (++seen == n) {
                        s_focus = s_wins[i].win_id;
                        break;
                    }
                }
                WmComposite();
            } else if (key == 'h' || key == 'j' || key == 'k' || key == 'l') {
                wm_win_t *f = win_find(s_focus);
                if (f) {
                    i32 dx = 0, dy = 0;
                    if (key == 'h')
                        dx = -1;
                    else if (key == 'l')
                        dx = 1;
                    else if (key == 'k')
                        dy = -1;
                    else if (key == 'j')
                        dy = 1;
                    i32 nx = (i32)f->x + dx;
                    i32 ny = (i32)f->y + dy;
                    if (nx < 0)
                        nx = 0;
                    if (ny < 0)
                        ny = 0;
                    if ((u32)nx + f->w > WM_SCREEN_COLS)
                        nx = (i32)(WM_SCREEN_COLS - f->w);
                    if ((u32)ny + f->h > WM_SCREEN_ROWS)
                        ny = (i32)(WM_SCREEN_ROWS - f->h);
                    f->x = (u32)nx;
                    f->y = (u32)ny;
                    WmComposite();
                }
            } else if (key == 'q' || key == 'Q') {
                s_active = 0;
                s_focus  = 0;
                WmComposite();
                if (s_lock >= 0)
                    (void)MutexUnlock(s_lock);
                (void)WmKbdReq(KBD_OP_RELEASE_FOCUS, NULL);
                TuiClear();
                printf("wm: desktop session closed, focus released\n");
                Sleep(30); /* let the server observe deactivation */
                break;
            }

            if (s_lock >= 0)
                (void)MutexUnlock(s_lock);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Entry                                                             */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("wm: starting window manager\n");
    memset(s_wins, 0, sizeof(s_wins));
    s_win_seq = 0;
    s_focus   = 0;
    s_active  = 0;
    s_lock    = -1;

    if (TuiPortGet() < 0) {
        printf("wm: term service unavailable\n");
        return 1;
    }
    s_lock = MutexCreate();
    if (s_lock < 0) {
        printf("wm: mutex_create failed (%d)\n", s_lock);
        return 1;
    }

    if (ThreadCreate(WmServerMain, NULL, 10) < 0) {
        printf("wm: server thread_create failed\n");
        return 1;
    }
    if (ThreadCreate(WmInputMain, NULL, 10) < 0) {
        printf("wm: input thread_create failed\n");
        return 1;
    }

    for (;;)
        ThreadYield();
    return 0;
}
