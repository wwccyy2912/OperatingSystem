/*
 * stdio.h - Standard I/O (C11 §7.21)
 * Copyright (c) 2026 OpSys Project
 *
 * Formatted output via the SYS_DEBUG_LOG syscall (printf/fprintf) and
 * into caller-provided buffers (sprintf/snprintf).  No FILE* streams
 * in v0.1 — all output goes to the debug log.
 *
 * Supported format specifiers:
 *   %d %i %u %x %X %o %c %s %p %%
 *   Length modifiers: l ll h hh z t
 *   Width / precision (limited): %8d %08x %.3s
 *   Flags: 0 (zero-pad), - (left-align), + (force sign), space
 */

#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H

#include <stdarg.h>
#include <stddef.h>

/* ====================================================================
 * Formatted output (C11 §7.21.6)
 * ==================================================================== */

/* Print to the debug log.  Returns chars written. */
int printf(const char *fmt, ...);

/* Print to the debug log from a va_list. */
int vprintf(const char *fmt, va_list ap);

/* Print to the debug log (same as printf — no FILE* in v0.1).
 * The `stream` argument is ignored. */
int fprintf(void *stream, const char *fmt, ...);
int vfprintf(void *stream, const char *fmt, va_list ap);

/* Write into buf (unbounded — use snprintf in production). */
int sprintf(char *buf, const char *fmt, ...);
int vsprintf(char *buf, const char *fmt, va_list ap);

/* Write at most n-1 chars into buf, NUL-terminated.  Returns the
 * total number of chars that WOULD be written (C11 semantics). */
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

/* ====================================================================
 * Character / string output (C11 §7.21.7)
 * ==================================================================== */

int puts(const char *str);
int putchar(int c);

/* ====================================================================
 * Character input (C11 §7.21.7) — limited
 * ==================================================================== */

int getchar(void);

#endif /* LIBC_STDIO_H */
