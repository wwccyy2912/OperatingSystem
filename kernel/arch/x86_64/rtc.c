/*
 * rtc.c - Real-time clock (CMOS RTC) wall-clock time
 * Copyright (c) 2026 OpSys Project
 *
 * CMOS register map (MC146818 / PC-AT):
 *   Index 0x00: seconds           Index 0x08: month
 *   Index 0x02: minutes           Index 0x09: year (0-99)
 *   Index 0x04: hours             Index 0x32: century (optional)
 *   Index 0x07: day of month
 *   Index 0x0A: status register A (bit 7 = UIP: update in progress)
 *   Index 0x0B: status register B (bit 1 = 24-hour mode,
 *                                   bit 2 = binary mode)
 *
 * Values are BCD-encoded by default; register B bit 2 selects
 * binary mode.  We handle both.
 *
 * The CMOS is behind ports 0x70 (index) / 0x71 (data).  Bit 7 of
 * the index byte disables NMIs for the duration of the access —
 * always set it so the read is atomic w.r.t. NMI delivery.
 *
 * Time-of-day registers update roughly once per second.  Reading
 * while an update is in progress (UIP = status A bit 7) can yield
 * torn values, so we wait for UIP to clear, then read all
 * registers twice and re-read while the value changes.
 */

#include <kernel/rtc.h>
#include <kernel/io.h>

/* CMOS registers */
#define RTC_INDEX_PORT      0x70
#define RTC_DATA_PORT       0x71

#define RTC_REG_SECONDS     0x00
#define RTC_REG_MINUTES     0x02
#define RTC_REG_HOURS       0x04
#define RTC_REG_DAY         0x07
#define RTC_REG_MONTH       0x08
#define RTC_REG_YEAR        0x09
#define RTC_REG_CENTURY     0x32
#define RTC_REG_STATUS_A    0x0A
#define RTC_REG_STATUS_B    0x0B

#define RTC_STATUS_A_UIP    (1u << 7)   /* update in progress */

#define RTC_STATUS_B_BINARY (1u << 2)   /* values are binary, not BCD */
#define RTC_STATUS_B_24H    (1u << 1)   /* 24-hour mode */

/* ------------------------------------------------------------------ */
/*  Low-level port access                                              */
/* ------------------------------------------------------------------ */

static u8 rtc_read_reg(u8 reg)
{
    /* Bit 7 set: disable NMIs around the access */
    io_outb(RTC_INDEX_PORT, reg | 0x80);
    io_delay();
    return io_inb(RTC_DATA_PORT);
}

/* ------------------------------------------------------------------ */
/*  BCD conversion                                                     */
/* ------------------------------------------------------------------ */

static u8 rtc_bcd_to_bin(u8 bcd)
{
    return (u8)(((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F));
}

/* ------------------------------------------------------------------ */

/*
 * Wait for an in-progress update to finish.  Returns after UIP
 * clears (or a bounded number of tries — a stuck RTC must never
 * hang the kernel).  UIP is set for 244 µs of every second.
 */
static void rtc_wait_uip_clear(void)
{
    for (int i = 0; i < 10000; i++) {
        if (!(rtc_read_reg(RTC_REG_STATUS_A) & RTC_STATUS_A_UIP))
            return;
        io_delay();
    }
}

/* ------------------------------------------------------------------ */

void rtc_read(rtc_time_t *out)
{
    if (!out)
        return;

    u8 status_b = rtc_read_reg(RTC_REG_STATUS_B);
    bool binary  = (status_b & RTC_STATUS_B_BINARY) != 0;

    u8 sec, min, hour, day, mon, year;
    u8 century;

    for (int attempt = 0; attempt < 4; attempt++) {
        rtc_wait_uip_clear();

        sec     = rtc_read_reg(RTC_REG_SECONDS);
        min     = rtc_read_reg(RTC_REG_MINUTES);
        hour    = rtc_read_reg(RTC_REG_HOURS);
        day     = rtc_read_reg(RTC_REG_DAY);
        mon     = rtc_read_reg(RTC_REG_MONTH);
        year    = rtc_read_reg(RTC_REG_YEAR);
        century = rtc_read_reg(RTC_REG_CENTURY);

        /* Consistency check: re-read the first register; if the RTC
         * ticked over between reads, retry. */
        u8 sec2 = rtc_read_reg(RTC_REG_SECONDS);
        if (sec2 == sec)
            break;
    }

    /* Convert BCD to binary unless the RTC is already in binary mode */
    if (!binary) {
        sec     = rtc_bcd_to_bin(sec);
        min     = rtc_bcd_to_bin(min);
        hour    = rtc_bcd_to_bin(hour);
        day     = rtc_bcd_to_bin(day);
        mon     = rtc_bcd_to_bin(mon);
        year    = rtc_bcd_to_bin(year);
        century = rtc_bcd_to_bin(century);
    }

    /* 12-hour mode: bit 7 of the hours register is the PM flag */
    bool pm = false;
    if (!(status_b & RTC_STATUS_B_24H)) {
        pm = (hour & 0x80) != 0;
        hour &= 0x7F;
        if (hour == 12)
            hour = 0;               /* 12:xx AM/PM -> 0:xx/12:xx */
        if (pm)
            hour += 12;
    }

    /* Year: low byte + century register.  The century register is
     * not present on all boards; fall back to 2000+year. */
    u16 full_year;
    if (century >= 19 && century <= 99)
        full_year = (u16)(century * 100 + year);
    else
        full_year = (u16)(2000 + year);

    out->year   = full_year;
    out->month  = mon;
    out->day    = day;
    out->hour   = hour;
    out->minute = min;
    out->second = sec;
}
