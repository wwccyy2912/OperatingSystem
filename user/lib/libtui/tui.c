/*
 * tui.c - TUI (Text User Interface) client library implementation
 * Copyright (c) 2026 OpSys Project
 *
 * IPC wrappers for terminal service operations.
 */

#include "tui.h"
#include "../libc/stdio.h"
#include "../libc/string.h"
#include "../libos/syscalls.h"
#include <stdarg.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t i32;

/* Cached port (resolved on first use) */
static int s_tui_port = -2; /* -2 = not yet resolved, -1 = failed, >=0 = port */

/* Request/response buffers */
static u8 s_req[4096];
static u8 s_resp[4096];

/* ====================================================================
 * Internal: port resolution and IPC call
 * ==================================================================== */

int tui_port_get(void) {
  if (s_tui_port >= -1)
    return s_tui_port;

  s_tui_port = port_get(TUI_PORT_NAME);
  return s_tui_port;
}

static int tui_call(u32 op, const void *payload, u32 payload_len) {
  int port = tui_port_get();
  if (port < 0)
    return port;

  /* Build request: op + len + payload */
  u32 *req = (u32 *)s_req;
  req[0] = op;
  req[1] = payload_len;
  if (payload_len > 0 && payload)
    memcpy(s_req + 8, payload, payload_len);

  /* Call and get response */
  int resp_len = (int)sizeof(s_resp);
  int ret = ipc_call(port, s_req, 8 + payload_len, s_resp, &resp_len);
  if (ret < 0)
    return ret;

  /* Response is at least 4 bytes (ret field) */
  if (resp_len < 4)
    return -1; /* malformed response */

  i32 *resp = (i32 *)s_resp;
  return resp[0];
}

/* ====================================================================
 * Basic output
 * ==================================================================== */

int tui_write(const char *text, uint32_t len) {
  if (!text || len == 0)
    return 0;
  if (len > TUI_MAX_TEXT)
    len = TUI_MAX_TEXT;

  return tui_call(TUI_OP_WRITE, text, len);
}

int tui_write_str(const char *str) {
  if (!str)
    return 0;
  uint32_t len = 0;
  while (str[len] && len < TUI_MAX_TEXT)
    len++;
  return tui_write(str, len);
}

/* ====================================================================
 * Screen control
 * ==================================================================== */

int tui_clear(void) { return tui_call(TUI_OP_CLEAR, NULL, 0); }

int tui_render_status(const char *prefix, const char *msg) {
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
  u8 payload[8 + 64 + 128];
  u32 *lengths = (u32 *)payload;
  lengths[0] = plen;
  lengths[1] = mlen;
  memcpy(payload + 8, prefix, plen);
  memcpy(payload + 8 + plen, msg, mlen);

  return tui_call(TUI_OP_STATUS, payload, 8 + plen + mlen);
}

/* ====================================================================
 * Box/border drawing
 * ==================================================================== */

int tui_render_box(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   const char *title) {
  if (!title)
    title = "";

  uint32_t tlen = 0;
  while (title[tlen] && tlen < 63)
    tlen++;

  /* Payload: x(4) + y(4) + w(4) + h(4) + title_len(4) + title */
  u8 payload[20 + 64];
  u32 *args = (u32 *)payload;
  args[0] = x;
  args[1] = y;
  args[2] = w;
  args[3] = h;
  args[4] = tlen;
  if (tlen > 0)
    memcpy(payload + 20, title, tlen);

  return tui_call(TUI_OP_BOX, payload, 20 + tlen);
}

/* ====================================================================
 * Text rendering without cursor change
 * ==================================================================== */

int tui_render_line_at(uint32_t x, uint32_t y, const char *text, uint32_t len) {
  if (!text || len == 0)
    return 0;
  if (len > TUI_MAX_TEXT)
    len = TUI_MAX_TEXT;

  /* Payload: x(4) + y(4) + text */
  u8 payload[8 + TUI_MAX_TEXT];
  u32 *coords = (u32 *)payload;
  coords[0] = x;
  coords[1] = y;
  memcpy(payload + 8, text, len);

  return tui_call(TUI_OP_RENDER_LINE, payload, 8 + len);
}

/* ====================================================================
 * Cursor control
 * ==================================================================== */

int tui_set_cursor(uint32_t x, uint32_t y) {
  u32 coords[2] = {x, y};
  return tui_call(TUI_OP_SET_CURSOR, coords, 8);
}

int tui_get_cursor(uint32_t *x, uint32_t *y) {
  int ret = tui_call(TUI_OP_GET_CURSOR, NULL, 0);
  if (ret < 0)
    return ret;

  /* Response contains cursor position in the first 8 bytes after ret */
  if (x)
    *x = ((u32 *)s_resp)[1];
  if (y)
    *y = ((u32 *)s_resp)[2];

  return 0;
}

/* ====================================================================
 * Utility: formatted output
 * ==================================================================== */

int tui_printf(const char *fmt, ...) {
  if (!fmt)
    return 0;

  va_list ap;
  char buf[512];
  int len = 0;

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
      int val = va_arg(ap, int);
      char tmp[12];
      int neg = (val < 0);
      if (neg)
        val = -val;
      int i = 0;
      if (val == 0)
        tmp[i++] = '0';
      while (val > 0) {
        tmp[i++] = (char)('0' + (val % 10));
        val /= 10;
      }
      if (neg && len < (int)sizeof(buf) - 1)
        buf[len++] = '-';
      while (i > 0 && len < (int)sizeof(buf) - 1)
        buf[len++] = tmp[--i];
      break;
    }
    case 'x': {
      u32 val = va_arg(ap, u32);
      char tmp[9];
      int i = 0;
      if (val == 0)
        tmp[i++] = '0';
      while (val > 0 && i < 8) {
        u32 d = val & 0xF;
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
  return tui_write(buf, (u32)len);
}
