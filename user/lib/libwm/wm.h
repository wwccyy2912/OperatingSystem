/*
 * libwm - Window Manager client library (v0.4)
 * Copyright (c) 2026 OpSys Project
 *
 * High-level wrappers for the wm service IPC.  Lets an app create
 * windows, write content rows, set focus, move windows, and manage the
 * desktop session — everything the window manager exposes to clients.
 *
 * All functions serialize IPC calls through the "wm" port and return
 * the service's reply code (0 on success, negative error otherwise).
 */

#ifndef LIBWM_WM_H
#define LIBWM_WM_H

#include <stdint.h>
#include "wm_proto.h"

/* Resolve the wm port (cached).  Returns >= 0 on success. */
int wm_port_get(void);

/**
 * Create a window.  title is copied (truncated to WM_TITLE_MAX-1).
 * Returns 0 and fills *win_id on success, negative error otherwise.
 * The new window becomes focused.
 */
int wm_create(const char *title, uint32_t x, uint32_t y,
              uint32_t w, uint32_t h, uint32_t *win_id);

/**
 * Destroy a window (owner or admin only).
 * Returns 0 on success, negative error otherwise.
 */
int wm_destroy(uint32_t win_id);

/**
 * List the registry.  Fills lines[0..*count) with "id title" entries.
 * Returns 0 on success, negative error otherwise.
 */
int wm_list(char lines[WM_MAX_WINDOWS][WM_TITLE_MAX + 24], uint32_t *count);

/**
 * Set keyboard focus to win_id (0 = none).  Returns 0 on success.
 */
int wm_focus(uint32_t win_id);

/**
 * Move a window to (x, y) in cell units (owner or admin only).
 * Returns 0 on success, negative error otherwise.
 */
int wm_move(uint32_t win_id, uint32_t x, uint32_t y);

/**
 * Write one body row (row < WM_CONTENT_ROWS) into a window's content
 * buffer (owner or admin only).  Non-printable bytes are stripped.
 * Returns 0 on success, negative error otherwise.
 */
int wm_write(uint32_t win_id, uint32_t row, const char *text);

/**
 * Start a desktop session (the wm takes the keyboard focus and renders
 * the desktop).  Returns 0 on success.
 */
int wm_activate(void);

/**
 * End the desktop session (the wm releases the keyboard focus and
 * clears the screen).  Returns 0 on success.
 */
int wm_deactivate(void);

/**
 * Query desktop state: *active (1 = session running), *focus (focused
 * win_id, 0 = none), *count (windows in the registry).
 * Returns 0 on success.
 */
int wm_get_state(uint32_t *active, uint32_t *focus, uint32_t *count);

#endif /* LIBWM_WM_H */
