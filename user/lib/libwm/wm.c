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
 * libwm - Window Manager client library (v0.4)
 * Copyright (c) 2026 OpSys Project
 *
 * IPC wrappers for the wm service.  See wm.h for the API contract.
 */

#include "wm.h"

#include <libc/string.h>
#include <libos/syscalls.h>

static int s_wm_port = -2; /* -2 unresolved, -1 failed, >=0 port */

int WmPortGet(void) {
    if (s_wm_port >= -1)
        return s_wm_port;
    s_wm_port = PortGet(WM_PORT_NAME);
    return s_wm_port;
}

static int WmCall(wm_req_t *req, wm_resp_t *resp) {
    int port = WmPortGet();
    if (port < 0)
        return port;
    int resp_len = (int)sizeof(*resp);
    int r        = IpcCall(port, req, (int)sizeof(*req), resp, &resp_len);
    if (r < 0)
        return r;
    if (resp_len < 4)
        return -1;
    return resp->ret;
}

int WmCreate(const char *title, uint32_t x, uint32_t y,
              uint32_t w, uint32_t h, uint32_t *win_id) {
    wm_req_t req;
    memset(&req, 0, sizeof(req));
    req.op = WM_OP_CREATE;
    if (title)
        strncpy(req.title, title, sizeof(req.title) - 1);
    req.x = x;
    req.y = y;
    req.w = w;
    req.h = h;
    wm_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = WmCall(&req, &resp);
    if (r == 0 && win_id)
        *win_id = resp.win_id;
    return r;
}

int WmDestroy(uint32_t win_id) {
    wm_req_t req;
    memset(&req, 0, sizeof(req));
    req.op     = WM_OP_DESTROY;
    req.win_id = win_id;
    wm_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    return WmCall(&req, &resp);
}

int WmList(char lines[WM_MAX_WINDOWS][WM_TITLE_MAX + 24], uint32_t *count) {
    wm_req_t req;
    memset(&req, 0, sizeof(req));
    req.op = WM_OP_LIST;
    wm_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = WmCall(&req, &resp);
    if (r == 0) {
        if (count)
            *count = resp.count;
        if (lines) {
            for (uint32_t i = 0; i < resp.count && i < WM_MAX_WINDOWS; i++)
                strncpy(lines[i], resp.lines[i], WM_TITLE_MAX + 23);
        }
    }
    return r;
}

int WmFocus(uint32_t win_id) {
    wm_req_t req;
    memset(&req, 0, sizeof(req));
    req.op     = WM_OP_FOCUS;
    req.win_id = win_id;
    wm_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    return WmCall(&req, &resp);
}

int WmMove(uint32_t win_id, uint32_t x, uint32_t y) {
    wm_req_t req;
    memset(&req, 0, sizeof(req));
    req.op     = WM_OP_MOVE;
    req.win_id = win_id;
    req.mx     = x;
    req.my     = y;
    wm_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    return WmCall(&req, &resp);
}

int WmWrite(uint32_t win_id, uint32_t row, const char *text) {
    wm_req_t req;
    memset(&req, 0, sizeof(req));
    req.op     = WM_OP_WRITE;
    req.win_id = win_id;
    req.row    = row;
    if (text)
        strncpy(req.text, text, sizeof(req.text) - 1);
    wm_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    return WmCall(&req, &resp);
}

int WmActivate(void) {
    wm_req_t req;
    memset(&req, 0, sizeof(req));
    req.op = WM_OP_ACTIVATE;
    wm_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    return WmCall(&req, &resp);
}

int WmDeactivate(void) {
    wm_req_t req;
    memset(&req, 0, sizeof(req));
    req.op = WM_OP_DEACTIVATE;
    wm_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    return WmCall(&req, &resp);
}

int WmGetState(uint32_t *active, uint32_t *focus, uint32_t *count) {
    wm_req_t req;
    memset(&req, 0, sizeof(req));
    req.op = WM_OP_GET_STATE;
    wm_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = WmCall(&req, &resp);
    if (r == 0) {
        if (active)
            *active = resp.active;
        if (focus)
            *focus = resp.focus;
        if (count)
            *count = resp.count;
    }
    return r;
}
