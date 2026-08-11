/*
 * stdio.c - Minimal user-space standard I/O
 * Copyright (c) 2026 OpSys Project
 *
 * All output goes through the SYS_DEBUG_LOG syscall.
 * printf supports: %d, %u, %x, %s, %c, %%
 * Buffer limited to 512 bytes for v0.1.
 */

#include <stdarg.h>
#include <stddef.h>
#include "stdio.h"
#include "stdlib.h"
#include "../libos/syscalls.h"

/* Fixed-size output buffer for formatted printing */
#define PRINTF_BUF_SIZE 512

/* ---- Internal helpers ---- */

/*
 * Write a formatted string into buf, returning the number of characters
 * written (excluding null terminator). If buf is NULL, only count.
 * va_list is forwarded as pointer (array decay on x86-64).
 */
static int format_string(char *buf, int bufsize, const char *fmt, va_list args)
{
    int pos = 0;

    #define EMIT(ch) do {                    \
        if (buf && pos < bufsize - 1)        \
            buf[pos] = (ch);                  \
        pos++;                                \
    } while (0)

    #define EMIT_STR(s) do {                 \
        const char *_s = (s);                 \
        while (*_s) {                         \
            EMIT(*_s);                        \
            _s++;                             \
        }                                     \
    } while (0)

    while (*fmt != '\0') {
        if (*fmt != '%') {
            EMIT(*fmt);
            fmt++;
            continue;
        }

        fmt++; /* skip '%' */

        switch (*fmt) {
        case 'd': {
            int val = va_arg(args, int);
            char numbuf[16];
            int npos = 0;
            unsigned int uval;

            if (val < 0) {
                EMIT('-');
                uval = (unsigned int)(-(val + 1)) + 1;
            } else {
                uval = (unsigned int)val;
            }

            if (uval == 0) {
                numbuf[npos++] = '0';
            } else {
                while (uval > 0) {
                    numbuf[npos++] = '0' + (char)(uval % 10);
                    uval /= 10;
                }
            }

            for (int i = npos - 1; i >= 0; i--) {
                EMIT(numbuf[i]);
            }
            break;
        }

        case 'u': {
            unsigned int val = va_arg(args, unsigned int);
            char numbuf[16];
            int npos = 0;

            if (val == 0) {
                numbuf[npos++] = '0';
            } else {
                while (val > 0) {
                    numbuf[npos++] = '0' + (char)(val % 10);
                    val /= 10;
                }
            }

            for (int i = npos - 1; i >= 0; i--) {
                EMIT(numbuf[i]);
            }
            break;
        }

        case 'x': {
            unsigned int val = va_arg(args, unsigned int);
            char numbuf[16];
            int npos = 0;
            const char *hex = "0123456789abcdef";

            if (val == 0) {
                numbuf[npos++] = '0';
            } else {
                while (val > 0) {
                    numbuf[npos++] = hex[val & 0xf];
                    val >>= 4;
                }
            }

            for (int i = npos - 1; i >= 0; i--) {
                EMIT(numbuf[i]);
            }
            break;
        }

        case 's': {
            const char *s = va_arg(args, const char *);
            if (s == NULL) {
                EMIT_STR("(null)");
            } else {
                EMIT_STR(s);
            }
            break;
        }

        case 'c': {
            int c = va_arg(args, int);
            EMIT((char)c);
            break;
        }

        case '%':
            EMIT('%');
            break;

        default:
            EMIT('%');
            EMIT(*fmt);
            break;
        }

        fmt++;
    }

    /* Null-terminate */
    if (buf && pos < bufsize) {
        buf[pos] = '\0';
    } else if (buf && bufsize > 0) {
        buf[bufsize - 1] = '\0';
    }

    #undef EMIT
    #undef EMIT_STR

    return pos;
}

/* ---- Public API ---- */

int printf(const char *fmt, ...)
{
    char buf[PRINTF_BUF_SIZE];
    va_list args;
    int len;

    va_start(args, fmt);
    len = format_string(buf, PRINTF_BUF_SIZE, fmt, args);
    va_end(args);

    if (len > 0) {
        debug_log(buf);
    }

    return len;
}

int puts(const char *str)
{
    if (str == NULL) {
        return debug_log("(null)\n");
    }
    return debug_log(str);
}

int putchar(int c)
{
    char buf[2] = { (char)c, '\0' };
    return debug_log(buf);
}
