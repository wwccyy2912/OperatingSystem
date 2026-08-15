/*
 * serial.h - Serial port (COM1) debug driver
 * Copyright (c) 2026 OpSys Project
 */

#ifndef KERNEL_SERIAL_H
#define KERNEL_SERIAL_H

#include <kernel/types.h>
#include <stdarg.h>

/* Standard COM1 base address */
#define SERIAL_COM1_BASE 0x3F8

/*
 * Log levels for gated serial output (higher = more verbose).
 * serial_puts_level()/serial_printf_level() emit only when the given
 * level is <= the current threshold, so verbose diagnostics can be
 * silenced at runtime without touching call sites.  Critical paths
 * (exceptions, stack smash, reboot) use the ungated functions below
 * and always print.
 */
#define SERIAL_LOG_ERROR 0
#define SERIAL_LOG_WARN  1
#define SERIAL_LOG_INFO  2
#define SERIAL_LOG_DEBUG 3

/**
 * Set the current log level threshold.
 * @param level  One of SERIAL_LOG_*; messages above it are suppressed.
 */
void serial_set_log_level(u8 level);

/**
 * Get the current log level threshold.
 * @return One of SERIAL_LOG_*.
 */
u8 serial_get_log_level(void);

/**
 * Write a string to serial if level <= current threshold.
 * @param level  Log level of this message.
 * @param str    The string to send.
 */
void serial_puts_level(u8 level, const char *str);

/**
 * Write formatted output to serial if level <= current threshold.
 * @param level  Log level of this message.
 * @param fmt    Format string (same specifiers as serial_printf).
 * @param ...    Variable arguments.
 */
void serial_printf_level(u8 level, const char *fmt, ...);

/**
 * Initialize the COM1 serial port for 115200 8N1.
 */
void serial_init(void);

/**
 * Read a single character from serial (blocking).
 * @return The character read.
 */
char serial_getchar(void);

/**
 * Write a single character to serial.
 * @param c  The character to send.
 */
void serial_putchar(char c);

/**
 * Write a null-terminated string to serial.
 * @param str  The string to send.
 */
void serial_puts(const char *str);

/**
 * Write formatted output to serial (minimal printf).
 * Supports: %d, %u, %x, %s, %c, %p, %%
 * @param fmt  Format string.
 * @param ...  Variable arguments.
 */
void serial_printf(const char *fmt, ...);

/**
 * Formatted output core (consumes a va_list).  Public so panic() can
 * format its reason without re-parsing the format string.
 * @param fmt  Format string (same specifiers as serial_printf).
 * @param ap   Initialized va_list; consumed.
 */
void serial_vprintf(const char *fmt, va_list ap);

#endif /* KERNEL_SERIAL_H */
