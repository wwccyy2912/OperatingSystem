/*
 * libwm - Window Manager client library (v0.4)
 * Copyright (c) 2026 OpSys Project
 *
 * IPC wrappers for the wm service.  See wm.h for the API contract.
 */

#include "wm.h"

#include <libc/string.h>
#include <libos/syscalls.h>

static int s_wm_port = -2; /* -2 unresolved, -1 failed, >=0 port */

int wm_port_get(void) {
    if (s_wm_port >= -1)
        return s_wm_port;
    s_wm_port = port_get(WM_PORT_NAME);
    return s_wm_port;
}

static int wm_call(wm_req_t *req, wm_resp_t *resp) {
    int port = wm_port_get();
    if (port < 0)
        return port;
    int resp_len = (int)sizeof(*resp);
    int r        = ipc_call(port, req, (int)sizeof(*req), resp, &resp_len);
    if (r < 0)
        return r;
    if (resp_len < 4)
        return -1;
    return resp->ret;
}

int wm_create(const char *title, uint32_t x, uint32_t y,
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
    int r = wm_call(&req, &resp);
    if (r == 0 && win_id)
        *win_id = resp.win_id;
    return r;
}

int wm_destroy(uint32_t win_id) {
    wm_req_t req;
    memset(&req, 0, sizeof(req));
    req.op     = WM_OP_DESTROY;
    req.win_id = win_id;
    wm_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    return wm_call(&req, &resp);
}

int wm_list(char lines[WM_MAX_WINDOWS][WM_TITLE_MAX + 24], uint32_t *count) {
    wm_req_t req;
    memset(&req, 0, sizeof(req));
    req.op = WM_OP_LIST;
    wm_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = wm_call(&req, &resp);
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

int wm_focus(uint32_t win_id) {
    wm_req_t req;
    memset(&req, 0, sizeof(req));
    req.op     = WM_OP_FOCUS;
    req.win_id = win_id;
    wm_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    return wm_call(&req, &resp);
}

int wm_move(uint32_t win_id, uint32_t x, uint32_t y) {
    wm_req_t req;
    memset(&req, 0, sizeof(req));
    req.op     = WM_OP_MOVE;
    req.win_id = win_id;
    req.mx     = x;
    req.my     = y;
    wm_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    return wm_call(&req, &resp);
}

int wm_write(uint32_t win_id, uint32_t row, const char *text) {
    wm_req_t req;
    memset(&req, 0, sizeof(req));
    req.op     = WM_OP_WRITE;
    req.win_id = win_id;
    req.row    = row;
    if (text)
        strncpy(req.text, text, sizeof(req.text) - 1);
    wm_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    return wm_call(&req, &resp);
}

int wm_activate(void) {
    wm_req_t req;
    memset(&req, 0, sizeof(req));
    req.op = WM_OP_ACTIVATE;
    wm_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    return wm_call(&req, &resp);
}

int wm_deactivate(void) {
    wm_req_t req;
    memset(&req, 0, sizeof(req));
    req.op = WM_OP_DEACTIVATE;
    wm_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    return wm_call(&req, &resp);
}

int wm_get_state(uint32_t *active, uint32_t *focus, uint32_t *count) {
    wm_req_t req;
    memset(&req, 0, sizeof(req));
    req.op = WM_OP_GET_STATE;
    wm_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = wm_call(&req, &resp);
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
