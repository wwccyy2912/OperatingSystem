/*
 * stdio.h - Minimal user-space standard I/O
 * Copyright (c) 2026 OpSys Project
 *
 * These functions output via the SYS_DEBUG_LOG syscall.
 * No file descriptors, no buffering - just serial output.
 */

#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H

/**
 * Print a formatted string to the debug log.
 * Supported format specifiers: %d, %u, %x, %s, %c, %%
 * @param fmt  Format string.
 * @param ...  Format arguments.
 * @return Number of characters written, or -1 on error.
 */
int printf(const char *fmt, ...);

/**
 * Write a string followed by a newline to the debug log.
 * @param str  Null-terminated string.
 * @return 0 on success, -1 on error.
 */
int puts(const char *str);

/**
 * Write a single character to the debug log.
 * @param c  Character to write.
 * @return The character written, or -1 on error.
 */
int putchar(int c);

#endif /* LIBC_STDIO_H */
