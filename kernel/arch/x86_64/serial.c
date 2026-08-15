/*
 * serial.c - Serial port (COM1) debug driver
 * Copyright (c) 2026 OpSys Project
 *
 * Standard 16550 UART initialization for 115200 8N1.
 */

#include <kernel/serial.h>
#include <kernel/io.h>
#include <stdarg.h>

/* COM1 register offsets from base */
#define SERIAL_DATA        0 /* Data register (R/W) */
#define SERIAL_INT_ENABLE  1 /* Interrupt enable register */
#define SERIAL_FIFO_CTRL   2 /* FIFO control register */
#define SERIAL_LINE_CTRL   3 /* Line control register */
#define SERIAL_MODEM_CTRL  4 /* Modem control register */
#define SERIAL_LINE_STATUS 5 /* Line status register */

/* Line status register bits */
#define LSR_TX_EMPTY   (1 << 5)
#define LSR_DATA_READY (1 << 0)

/* Line control register bits */
#define LCR_DLAB  (1 << 7)
#define LCR_8BITS 0x03 /* 8 data bits, 1 stop, no parity */

void serial_init(void) {
    /* Disable all interrupts */
    io_outb(SERIAL_COM1_BASE + SERIAL_INT_ENABLE, 0x00);

    /* Enable DLAB to set baud rate divisor */
    io_outb(SERIAL_COM1_BASE + SERIAL_LINE_CTRL, LCR_DLAB);

    /* Set divisor to 1 for 115200 baud (115200 = 115200 / 1) */
    io_outb(SERIAL_COM1_BASE + SERIAL_DATA, 0x01);
    io_outb(SERIAL_COM1_BASE + SERIAL_INT_ENABLE, 0x00);

    /* 8 bits, no parity, 1 stop bit (8N1) */
    io_outb(SERIAL_COM1_BASE + SERIAL_LINE_CTRL, LCR_8BITS);

    /* Enable FIFO, clear them, 14-byte threshold */
    io_outb(SERIAL_COM1_BASE + SERIAL_FIFO_CTRL, 0xC7);

    /* IRQs enabled, RTS/DSR set */
    io_outb(SERIAL_COM1_BASE + SERIAL_MODEM_CTRL, 0x0B);
}

char serial_getchar(void) {
    /* Wait for data to be ready */
    while (!(io_inb(SERIAL_COM1_BASE + SERIAL_LINE_STATUS) & LSR_DATA_READY))
        ;
    return (char)io_inb(SERIAL_COM1_BASE + SERIAL_DATA);
}

void serial_putchar(char c) {
    /* Wait for transmit buffer to be empty */
    while (!(io_inb(SERIAL_COM1_BASE + SERIAL_LINE_STATUS) & LSR_TX_EMPTY))
        ;
    io_outb(SERIAL_COM1_BASE + SERIAL_DATA, (u8)c);
}

void serial_puts(const char *str) {
    while (*str) {
        if (*str == '\n')
            serial_putchar('\r');
        serial_putchar(*str);
        str++;
    }
}

/*
 * Current log level threshold.  Messages with a level above this are
 * suppressed.  Default INFO: ERROR/WARN/INFO print, DEBUG is hidden.
 */
static u8 s_log_level = SERIAL_LOG_INFO;

void serial_set_log_level(u8 level) {
    s_log_level = level;
}

u8 serial_get_log_level(void) {
    return s_log_level;
}

void serial_puts_level(u8 level, const char *str) {
    if (level <= s_log_level)
        serial_puts(str);
}

/*
 * Helper: write a number to serial with optional width and zero-padding.
 */
static void
serial_print_num(u64 val, int base, int is_signed, int width, int zero_pad, int is_upper) {
    char        buf[24];
    int         i        = 0;
    int         negative = 0;
    const char *digits   = is_upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (is_signed && (i64)val < 0) {
        negative = 1;
        val      = (u64)(-(i64)val);
    }

    if (val == 0) {
        buf[i++] = '0';
    } else {
        while (val > 0) {
            buf[i++] = digits[val % base];
            val /= base;
        }
    }

    if (negative)
        buf[i++] = '-';

    /* Pad with zeros or spaces */
    int pad_char  = zero_pad ? '0' : ' ';
    int total     = i;
    int pad_count = (width > total) ? width - total : 0;

    /* If zero-padding with sign, the sign goes first */
    if (zero_pad && negative && pad_count > 0) {
        serial_putchar('-');
        i--; /* Remove sign from buffer, we printed it */
        pad_count++;
        if (pad_count > 0 && width > total) {
            /* Recalculate: we want 'width' total chars including sign */
        }
    }

    /* Write padding */
    for (int p = 0; p < pad_count; p++)
        serial_putchar(pad_char);

    /* Write number in reverse */
    while (i > 0)
        serial_putchar(buf[--i]);
}

/*
 * Formatted output core.  Shared by serial_printf and
 * serial_printf_level; consumes the va_list.
 */
void serial_vprintf(const char *fmt, va_list ap) {
    while (*fmt) {
        if (*fmt != '%') {
            serial_putchar(*fmt++);
            continue;
        }

        fmt++; /* skip '%' */

        /* Parse zero-padding flag */
        int zero_pad = 0;
        if (*fmt == '0') {
            zero_pad = 1;
            fmt++;
        }

        /* Parse width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        switch (*fmt) {
        case 'd': {
            int ival = va_arg(ap, int);
            serial_print_num((u64)(i64)ival, 10, 1, width, zero_pad, 0);
            break;
        }
        case 'u': {
            u64 val = va_arg(ap, u64);
            serial_print_num(val, 10, 0, width, zero_pad, 0);
            break;
        }
        case 'x': {
            u64 val = va_arg(ap, u64);
            serial_print_num(val, 16, 0, width, zero_pad, 0);
            break;
        }
        case 'X': {
            u64 val = va_arg(ap, u64);
            serial_print_num(val, 16, 0, width, zero_pad, 1);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            while (*s)
                serial_putchar(*s++);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            serial_putchar(c);
            break;
        }
        case 'p': {
            u64 val = (u64)va_arg(ap, void *);
            serial_puts("0x");
            serial_print_num(val, 16, 0, 16, 1, 0);
            break;
        }
        case '%':
            serial_putchar('%');
            break;
        default:
            serial_putchar('%');
            serial_putchar(*fmt);
            break;
        }

        fmt++;
    }
}

void serial_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    serial_vprintf(fmt, ap);
    va_end(ap);
}

void serial_printf_level(u8 level, const char *fmt, ...) {
    if (level > s_log_level)
        return;
    va_list ap;
    va_start(ap, fmt);
    serial_vprintf(fmt, ap);
    va_end(ap);
}
