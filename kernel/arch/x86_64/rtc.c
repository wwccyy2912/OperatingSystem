/*
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details: <https://www.gnu.org/licenses/>.
 *
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
 * registers twice and re-read while the value changes. *
 * ------------------------------------------------------------------
 * Structure (rtc):
 *   CMOS ports 0x70/0x71 -> BCD registers (sec/min/hour/day/...) ->
 *   RtcGetTime()/RtcSetTime() in 24h format.
 * How it works:
 *   Selects a register via 0x70 then reads 0x71; NMI disabled while
 *   selecting; BCD decoded to binary.
 * Purpose:
 *   Wall-clock for timestamps and the user set_time syscall.
 * Caveats:
 *   CMOS is volatile across power loss (RTC battery aside); update-
 *   in-progress bit (UIP) should be polled on real hardware.
 * ------------------------------------------------------------------
 */
#include <kernel/rtc.h>
#include <kernel/io.h>

/* CMOS registers */
#define RTC_INDEX_PORT 0x70
#define RTC_DATA_PORT  0x71

#define RTC_REG_SECONDS  0x00
#define RTC_REG_MINUTES  0x02
#define RTC_REG_HOURS    0x04
#define RTC_REG_DAY      0x07
#define RTC_REG_MONTH    0x08
#define RTC_REG_YEAR     0x09
#define RTC_REG_CENTURY  0x32
#define RTC_REG_STATUS_A 0x0A
#define RTC_REG_STATUS_B 0x0B

#define RTC_STATUS_A_UIP (1u << 7) /* update in progress */

#define RTC_STATUS_B_BINARY (1u << 2) /* values are binary, not BCD */
#define RTC_STATUS_B_24H    (1u << 1) /* 24-hour mode */

/* ------------------------------------------------------------------ */
/*  Low-level port access                                              */
/* ------------------------------------------------------------------ */

static u8 RtcReadReg(u8 reg) {
    /* Bit 7 set: disable NMIs around the access */
    IoOutb(RTC_INDEX_PORT, reg | 0x80);
    IoDelay();
    return IoInb(RTC_DATA_PORT);
}

static void RtcWriteReg(u8 reg, u8 val) {
    /* Bit 7 set: disable NMIs around the access (mirrors rtc_read_reg) */
    IoOutb(RTC_INDEX_PORT, reg | 0x80);
    IoDelay();
    IoOutb(RTC_DATA_PORT, val);
}

/* ------------------------------------------------------------------ */
/*  BCD conversion                                                     */
/* ------------------------------------------------------------------ */

static u8 RtcBcdToBin(u8 bcd) {
    return (u8)(((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F));
}

/* ------------------------------------------------------------------ */

/*
 * Wait for an in-progress update to finish.  Returns after UIP
 * clears (or a bounded number of tries — a stuck RTC must never
 * hang the kernel).  UIP is set for 244 µs of every second.
 */
static void RtcWaitUipClear(void) {
    for (int i = 0; i < 10000; i++) {
        if (!(RtcReadReg(RTC_REG_STATUS_A) & RTC_STATUS_A_UIP))
            return;
        IoDelay();
    }
}

/* ------------------------------------------------------------------ */

void RtcRead(rtc_time_t *out) {
    if (!out)
        return;

    u8   status_b = RtcReadReg(RTC_REG_STATUS_B);
    bool binary   = (status_b & RTC_STATUS_B_BINARY) != 0;

    u8 sec, min, hour, day, mon, year;
    u8 century;

    for (int attempt = 0; attempt < 4; attempt++) {
        RtcWaitUipClear();

        sec     = RtcReadReg(RTC_REG_SECONDS);
        min     = RtcReadReg(RTC_REG_MINUTES);
        hour    = RtcReadReg(RTC_REG_HOURS);
        day     = RtcReadReg(RTC_REG_DAY);
        mon     = RtcReadReg(RTC_REG_MONTH);
        year    = RtcReadReg(RTC_REG_YEAR);
        century = RtcReadReg(RTC_REG_CENTURY);

        /* Consistency check: re-read the first register; if the RTC
         * ticked over between reads, retry. */
        u8 sec2 = RtcReadReg(RTC_REG_SECONDS);
        if (sec2 == sec)
            break;
    }

    /* Convert BCD to binary unless the RTC is already in binary mode */
    if (!binary) {
        sec     = RtcBcdToBin(sec);
        min     = RtcBcdToBin(min);
        hour    = RtcBcdToBin(hour);
        day     = RtcBcdToBin(day);
        mon     = RtcBcdToBin(mon);
        year    = RtcBcdToBin(year);
        century = RtcBcdToBin(century);
    }

    /* 12-hour mode: bit 7 of the hours register is the PM flag */
    bool pm = false;
    if (!(status_b & RTC_STATUS_B_24H)) {
        pm = (hour & 0x80) != 0;
        hour &= 0x7F;
        if (hour == 12)
            hour = 0; /* 12:xx AM/PM -> 0:xx/12:xx */
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

void RtcWrite(const rtc_time_t *t) {
    if (!t)
        return;

    /* Force 24-hour BINARY mode up front (status B bit 1 = 24H,
     * bit 2 = binary).  This sidesteps two write hazards: the PM bit
     * (bit 7 of the hours register in 12-hour mode) and BCD-vs-binary
     * encoding.  After this every register below is written as a plain
     * binary value, and RtcRead(which already handles both modes)
     * reads them back consistently.  Wait for UIP before touching
     * status B so the mode switch does not race an update. */
    RtcWaitUipClear();
    u8 status_b = RtcReadReg(RTC_REG_STATUS_B);
    status_b |= RTC_STATUS_B_24H | RTC_STATUS_B_BINARY;
    RtcWriteReg(RTC_REG_STATUS_B, status_b);

    /* Now wait for UIP again and write the time-of-day registers while
     * no update is in progress (the RTC updates roughly once a second). */
    RtcWaitUipClear();

    RtcWriteReg(RTC_REG_SECONDS, (u8)t->second);
    RtcWriteReg(RTC_REG_MINUTES, (u8)t->minute);
    RtcWriteReg(RTC_REG_HOURS, (u8)t->hour);
    RtcWriteReg(RTC_REG_DAY, (u8)t->day);
    RtcWriteReg(RTC_REG_MONTH, (u8)t->month);
    RtcWriteReg(RTC_REG_YEAR, (u8)(t->year % 100));
    RtcWriteReg(RTC_REG_CENTURY, (u8)(t->year / 100));
}
