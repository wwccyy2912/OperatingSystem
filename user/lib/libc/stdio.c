/*
 * stdio.c - Standard I/O (C11 §7.21)
 * Copyright (c) 2026 OpSys Project
 *
 * Formatted output engine (vsnprintf core) supports:
 *   Conversions: d i u x X o c s p %
 *   Length:      l ll h hh z t j
 *   Width:       %8d %08x  (zero-pad and space-pad)
 *   Precision:   %.3s %.6d
 *   Flags:       0 - + space #
 *
 * Output goes through debug_log() for printf/fprintf.
 * sprintf/snprintf write into caller buffers.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "../libos/syscalls.h"

/* ====================================================================
 * Core formatter
 * ==================================================================== */

struct fmt_spec {
    int width;
    int precision;
    int base;  /* 8, 10, 16 */
    int upper; /* uppercase hex digits */
    int is_signed;
    int zero_pad;
    int left_align;
    int plus_sign;
    int space_sign;
    int alt_form; /* # flag */
    int has_precision;
};

static void emit_char(char *buf, int bufsize, int *pos, char ch) {
    if (buf && *pos < bufsize - 1)
        buf[*pos] = ch;
    (*pos)++;
}

static void emit_str(char *buf, int bufsize, int *pos, const char *s) {
    if (!s)
        s = "(null)";
    while (*s) {
        emit_char(buf, bufsize, pos, *s);
        s++;
    }
}

static void emit_pad(char *buf, int bufsize, int *pos, char ch, int count) {
    for (int i = 0; i < count; i++)
        emit_char(buf, bufsize, pos, ch);
}

/* Convert unsigned to string, returns length. */
static int u2str(unsigned long long val, int base, int upper, char *out) {
    const char *digits_lo = "0123456789abcdef";
    const char *digits_up = "0123456789ABCDEF";
    const char *digits    = upper ? digits_up : digits_lo;
    char        tmp[32];
    int         len = 0;

    if (val == 0) {
        out[0] = '0';
        out[1] = '\0';
        return 1;
    }
    while (val > 0) {
        tmp[len++] = digits[val % (unsigned)base];
        val /= (unsigned)base;
    }
    for (int i = 0; i < len; i++)
        out[i] = tmp[len - 1 - i];
    out[len] = '\0';
    return len;
}

static void format_number(
    char *buf, int bufsize, int *pos, unsigned long long val, int neg, const struct fmt_spec *s) {
    char numbuf[32];
    int  numlen;

    if (s->is_signed && neg) {
        /* negative: value already negated */
    }

    numlen = u2str(val, s->base, s->upper, numbuf);

    /* Apply precision (minimum digits). */
    int zeros = 0;
    if (s->has_precision) {
        if (s->precision == 0 && val == 0) {
            numlen = 0; /* "%.0d" of 0 = empty */
        } else if (s->precision > numlen) {
            zeros = s->precision - numlen;
        }
    }

    /* Sign / prefix */
    char sign = 0;
    if (s->is_signed && neg)
        sign = '-';
    else if (s->is_signed && s->plus_sign)
        sign = '+';
    else if (s->is_signed && s->space_sign)
        sign = ' ';

    const char *prefix     = "";
    int         prefix_len = 0;
    if (s->alt_form && val != 0) {
        if (s->base == 16) {
            prefix     = s->upper ? "0X" : "0x";
            prefix_len = 2;
        } else if (s->base == 8) {
            prefix     = "0";
            prefix_len = 1;
        }
    }

    int total = (sign ? 1 : 0) + prefix_len + zeros + numlen;

    /* Left-align or right-align with padding. */
    int pad = s->width - total;
    if (pad < 0)
        pad = 0;

    if (!s->left_align && pad > 0) {
        if (s->zero_pad && !s->has_precision) {
            /* Zero-pad after sign/prefix. */
            if (sign)
                emit_char(buf, bufsize, pos, sign);
            emit_str(buf, bufsize, pos, prefix);
            emit_pad(buf, bufsize, pos, '0', pad);
        } else {
            emit_pad(buf, bufsize, pos, ' ', pad);
            if (sign)
                emit_char(buf, bufsize, pos, sign);
            emit_str(buf, bufsize, pos, prefix);
        }
    } else {
        if (sign)
            emit_char(buf, bufsize, pos, sign);
        emit_str(buf, bufsize, pos, prefix);
    }

    /* Precision zeros. */
    emit_pad(buf, bufsize, pos, '0', zeros);

    /* The number itself. */
    for (int i = 0; i < numlen; i++)
        emit_char(buf, bufsize, pos, numbuf[i]);

    /* Right padding for left-align. */
    if (s->left_align && pad > 0)
        emit_pad(buf, bufsize, pos, ' ', pad);
}

static int format_string(char *buf, int bufsize, const char *fmt, va_list args) {
    int pos = 0;

    while (*fmt) {
        if (*fmt != '%') {
            emit_char(buf, bufsize, &pos, *fmt);
            fmt++;
            continue;
        }

        fmt++; /* skip '%' */

        if (*fmt == '%') {
            emit_char(buf, bufsize, &pos, '%');
            fmt++;
            continue;
        }

        struct fmt_spec s = {0};
        s.base            = 10;
        s.is_signed       = 1;

        /* Flags */
        for (;;) {
            if (*fmt == '0') {
                s.zero_pad = 1;
                fmt++;
            } else if (*fmt == '-') {
                s.left_align = 1;
                fmt++;
            } else if (*fmt == '+') {
                s.plus_sign = 1;
                fmt++;
            } else if (*fmt == ' ') {
                s.space_sign = 1;
                fmt++;
            } else if (*fmt == '#') {
                s.alt_form = 1;
                fmt++;
            } else
                break;
        }

        /* Width */
        if (*fmt >= '1' && *fmt <= '9') {
            while (*fmt >= '0' && *fmt <= '9') {
                s.width = s.width * 10 + (*fmt - '0');
                fmt++;
            }
        } else if (*fmt == '*') {
            s.width = va_arg(args, int);
            if (s.width < 0) {
                s.left_align = 1;
                s.width      = -s.width;
            }
            fmt++;
        }

        /* Precision */
        if (*fmt == '.') {
            fmt++;
            s.has_precision = 1;
            if (*fmt == '*') {
                s.precision = va_arg(args, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    s.precision = s.precision * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        /* Length modifiers */
        int len_l = 0, len_h = 0;
        int len_z = 0, len_j = 0, len_t = 0;
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 'j' || *fmt == 't') {
            if (*fmt == 'l') {
                len_l++;
                fmt++;
            } else if (*fmt == 'h') {
                len_h++;
                fmt++;
            } else if (*fmt == 'z') {
                len_z = 1;
                fmt++;
            } else if (*fmt == 'j') {
                len_j = 1;
                fmt++;
            } else if (*fmt == 't') {
                len_t = 1;
                fmt++;
            }
        }

        /* Conversion */
        switch (*fmt) {
        case 'd':
        case 'i': {
            long long val;
            if (len_l >= 2 || len_j)
                val = va_arg(args, long long);
            else if (len_l == 1)
                val = va_arg(args, long);
            else if (len_z || len_t)
                val = va_arg(args, long);
            else
                val = va_arg(args, int);
            if (len_h == 1)
                val = (short)val;
            if (len_h >= 2)
                val = (signed char)val;

            int                neg = 0;
            unsigned long long uval;
            if (val < 0) {
                neg  = 1;
                uval = (unsigned long long)(-(val + 1)) + 1;
            } else
                uval = (unsigned long long)val;

            s.base      = 10;
            s.is_signed = 1;
            format_number(buf, bufsize, &pos, uval, neg, &s);
            break;
        }
        case 'u': {
            unsigned long long val;
            if (len_l >= 2 || len_j)
                val = va_arg(args, unsigned long long);
            else if (len_l == 1 || len_z || len_t)
                val = va_arg(args, unsigned long);
            else
                val = va_arg(args, unsigned int);
            if (len_h == 1)
                val = (unsigned short)val;
            if (len_h >= 2)
                val = (unsigned char)val;
            s.base      = 10;
            s.is_signed = 0;
            format_number(buf, bufsize, &pos, val, 0, &s);
            break;
        }
        case 'x':
        case 'X': {
            unsigned long long val;
            if (len_l >= 2 || len_j)
                val = va_arg(args, unsigned long long);
            else if (len_l == 1 || len_z || len_t)
                val = va_arg(args, unsigned long);
            else
                val = va_arg(args, unsigned int);
            if (len_h == 1)
                val = (unsigned short)val;
            if (len_h >= 2)
                val = (unsigned char)val;
            s.base      = 16;
            s.is_signed = 0;
            s.upper     = (*fmt == 'X');
            format_number(buf, bufsize, &pos, val, 0, &s);
            break;
        }
        case 'o': {
            unsigned long long val;
            if (len_l >= 2 || len_j)
                val = va_arg(args, unsigned long long);
            else if (len_l == 1 || len_z || len_t)
                val = va_arg(args, unsigned long);
            else
                val = va_arg(args, unsigned int);
            if (len_h == 1)
                val = (unsigned short)val;
            if (len_h >= 2)
                val = (unsigned char)val;
            s.base      = 8;
            s.is_signed = 0;
            format_number(buf, bufsize, &pos, val, 0, &s);
            break;
        }
        case 'p': {
            unsigned long long val = (unsigned long long)(uintptr_t)va_arg(args, void *);
            s.base                 = 16;
            s.is_signed            = 0;
            s.alt_form             = 1;
            if (s.width == 0)
                s.width = 18;
            s.zero_pad = 1;
            format_number(buf, bufsize, &pos, val, 0, &s);
            break;
        }
        case 'c': {
            int c   = va_arg(args, int);
            int pad = s.width - 1;
            if (!s.left_align && pad > 0)
                emit_pad(buf, bufsize, &pos, ' ', pad);
            emit_char(buf, bufsize, &pos, (char)c);
            if (s.left_align && pad > 0)
                emit_pad(buf, bufsize, &pos, ' ', pad);
            break;
        }
        case 's': {
            const char *str = va_arg(args, const char *);
            if (!str)
                str = "(null)";
            int slen = 0;
            if (s.has_precision) {
                while (str[slen] && slen < s.precision)
                    slen++;
            } else {
                slen = (int)strlen(str);
            }
            int pad = s.width - slen;
            if (!s.left_align && pad > 0)
                emit_pad(buf, bufsize, &pos, ' ', pad);
            for (int i = 0; i < slen; i++)
                emit_char(buf, bufsize, &pos, str[i]);
            if (s.left_align && pad > 0)
                emit_pad(buf, bufsize, &pos, ' ', pad);
            break;
        }
        case 'n': {
            int *p = va_arg(args, int *);
            if (p)
                *p = pos;
            break;
        }
        default:
            emit_char(buf, bufsize, &pos, '%');
            emit_char(buf, bufsize, &pos, *fmt);
            break;
        }

        if (*fmt)
            fmt++;
    }

    if (buf && bufsize > 0) {
        if (pos < bufsize)
            buf[pos] = '\0';
        else
            buf[bufsize - 1] = '\0';
    }

    return pos;
}

/* ====================================================================
 * Public API
 * ==================================================================== */

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    return format_string(buf, (int)n, fmt, ap);
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    return format_string(buf, 0x7FFFFFFF, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}

int vprintf(const char *fmt, va_list ap) {
    char buf[512];
    int  len = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (len > 0)
        debug_log(buf);
    return len;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int vfprintf(void *stream, const char *fmt, va_list ap) {
    (void)stream;
    return vprintf(fmt, ap);
}

int fprintf(void *stream, const char *fmt, ...) {
    (void)stream;
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int puts(const char *str) {
    if (!str)
        return debug_log("(null)\n");
    int r = debug_log(str);
    debug_log("\n");
    return r;
}

int putchar(int c) {
    char buf[2] = {(char)c, '\0'};
    return debug_log(buf);
}

int getchar(void) {
    return debug_getchar();
}
