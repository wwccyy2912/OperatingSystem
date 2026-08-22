/*
 * libtui.h - TUI (Text User Interface) client library
 * Copyright (c) 2026 OpSys Project
 *
 * High-level wrappers for terminal service operations.
 * Provides functions for:
 *   - Basic text output (term_write)
 *   - Screen clearing (term_clear)
 *   - Status bar rendering (term_render_status)
 *   - Box/border drawing (term_render_box)
 *   - Text rendering at arbitrary positions (term_render_line_at)
 *   - Cursor control (term_set_cursor, term_get_cursor)
 *
 * All functions serialize IPC calls through the "term" port.
 * Return 0 on success, negative error on failure.
 */

#ifndef LIBTUI_TUI_H
#define LIBTUI_TUI_H

#include <stdint.h>

/* Terminal service port name (registered by term.c at startup) */
#define TUI_PORT_NAME "term"

/* TUI operation codes (must match term.c TERM_OP_*) */
#define TUI_OP_WRITE       1 /* render text at cursor */
#define TUI_OP_CLEAR       2 /* clear screen + reset cursor */
#define TUI_OP_STATUS      3 /* render status bar */
#define TUI_OP_BOX         4 /* render box border + title */
#define TUI_OP_RENDER_LINE 5 /* render line at (x,y) */
#define TUI_OP_SET_CURSOR  6 /* set cursor position */
#define TUI_OP_GET_CURSOR  7 /* query cursor position */
#define TUI_OP_SNAPSHOT    8 /* save a cell region: {x,y,w,h} -> cells[] */
#define TUI_OP_RESTORE     9 /* redraw a saved cell region: {x,y,w,h,cells} */

#define TUI_MAX_TEXT 256 /* max payload per operation */

/* Snapshot/restore region cap (must match term.c TERM_MAX_REGION_CELLS). */
#define TUI_MAX_REGION_CELLS 2048

/* ====================================================================
 * Basic output
 * ==================================================================== */

/**
 * Write text at the current cursor position.
 * Supports \n (newline), \r (carriage return), \b (backspace), \t (tab).
 * Returns bytes written on success, or negative error.
 */
int tui_write(const char *text, uint32_t len);

/**
 * Write a NUL-terminated string.
 */
int tui_write_str(const char *str);

/* ====================================================================
 * Screen control
 * ==================================================================== */

/**
 * Clear the entire screen and reset cursor to (0, 0).
 * Returns 0 on success, negative error on failure.
 */
int tui_clear(void);

/**
 * Render a status bar at the bottom of the screen.
 * Format: "prefix: msg"
 * Returns 0 on success, negative error on failure.
 */
int tui_render_status(const char *prefix, const char *msg);

/* ====================================================================
 * Box/border drawing
 * ==================================================================== */

/**
 * Render a box border with optional title.
 * Box occupies cells (x, y) to (x+w-1, y+h-1).
 * title may be NULL for no title bar.
 * Returns 0 on success, negative error on failure.
 */
int tui_render_box(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const char *title);

/* ====================================================================
 * Text rendering without cursor change
 * ==================================================================== */

/**
 * Render text at a specific (x, y) position without changing the cursor.
 * Useful for status lines, dialog boxes, overlays.
 * Returns 0 on success, negative error on failure.
 */
int tui_render_line_at(uint32_t x, uint32_t y, const char *text, uint32_t len);

/* ====================================================================
 * Cursor control
 * ==================================================================== */

/**
 * Set the cursor position to (x, y) in cell coordinates.
 * Next tui_write() will render at this position.
 * Returns 0 on success, negative error on failure.
 */
int tui_set_cursor(uint32_t x, uint32_t y);

/**
 * Query the current cursor position.
 * Returns 0 on success and fills x/y, or negative error.
 */
int tui_get_cursor(uint32_t *x, uint32_t *y);

/* ====================================================================
 * Utility functions
 * ==================================================================== */

/**
 * Resolve the terminal service port.
 * Called automatically by other functions, but can be used
 * to verify the terminal is available before rendering.
 * Returns the port number on success, negative error if unavailable.
 */
int tui_port_get(void);

/**
 * Print formatted string to the terminal (like printf, but to term).
 * Supports %d, %x, %s, %c, %% — enough for most TUI use cases.
 * Returns 0 on success, negative error on failure.
 */
int tui_printf(const char *fmt, ...);

/* ====================================================================
 * Region snapshot/restore (v1.2): save a rectangular block of screen
 * cells and redraw it later.  The basis for non-destructive dialog
 * overlays: save the area under a dialog, render the dialog, then
 * restore the area when it closes.  Cells are plain ASCII.
 * ==================================================================== */

/**
 * Save the w*h cells of the region at (x,y) into cells[] (which must
 * hold at least w*h bytes; w*h must not exceed TUI_MAX_REGION_CELLS).
 * Returns 0 on success, negative error on failure.
 */
int tui_region_save(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t *cells);

/**
 * Redraw the w*h cells previously saved by tui_region_save at (x,y).
 * Returns 0 on success, negative error on failure.
 */
int tui_region_restore(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *cells);

#endif /* LIBTUI_TUI_H */

/* ====================================================================
 * Interactive components (v1.1): input line, password line, confirm box.
 * These read the keyboard service directly (READ_BLOCK).  They are
 * intended for use while the caller has NO parked keyboard read (e.g.
 * a shell between commands); with focus free, READ_BLOCK delivers keys.
 * ==================================================================== */

/* Render a labelled input line at (x,y) and read a line from the
 * keyboard.  When mask != 0 every typed character is echoed as '*'
 * (password entry).  Returns the number of characters read (>=0), or
 * a negative error.  buf receives the NUL-terminated line. */
int tui_input_line(int x, int y, const char *prompt, char *buf, int maxlen, int mask);

/* Render a titled confirmation dialog centered around (x,y) with a
 * message and a hint line, then read y/n.  Returns 1 (yes), 0 (no),
 * or a negative error. */
int tui_confirm(int x, int y, int w, const char *title, const char *msg,
                const char *hint);
