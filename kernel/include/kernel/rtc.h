/*
 * rtc.h - Real-time clock (CMOS RTC) wall-clock time
 * Copyright (c) 2026 OpSys Project
 *
 * The MC146818-compatible CMOS RTC keeps the wall-clock time even
 * when the machine is off (battery backed).  It is accessed through
 * I/O ports 0x70 (index, bit 7 = NMI disable) and 0x71 (data).
 * See rtc.c for the register map.
 */

#ifndef KERNEL_RTC_H
#define KERNEL_RTC_H

#include <kernel/types.h>

/* Wall-clock time as read from the CMOS RTC */
typedef struct {
    u16 year;   /* full year, e.g. 2026 */
    u8  month;  /* 1-12 */
    u8  day;    /* 1-31 */
    u8  hour;   /* 0-23 */
    u8  minute; /* 0-59 */
    u8  second; /* 0-59 */
} rtc_time_t;

/**
 * Read the current wall-clock time from the CMOS RTC.
 * Converts BCD to binary, waits out an in-progress update (UIP),
 * and handles both 12-hour and 24-hour mode.
 * @param out  Filled with the current time.
 */
void rtc_read(rtc_time_t *out);

/**
 * Set the wall-clock time on the CMOS RTC (P2 地基: SYS_SET_TIME).
 * Mirrors rtc_read's discipline: waits out an in-progress update (UIP)
 * and uses NMI-disabled port access.  To sidestep the 12/24-hour mode
 * PM-bit and BCD-vs-binary complications entirely, status register B is
 * forced into 24-hour BINARY mode first (bits 1+2 set), then all
 * registers are written as plain binary values.  rtc_read already
 * handles either mode, so the permanent mode switch is harmless.
 * @param t  Time to write; ignored (no-op) when NULL.
 */
void rtc_write(const rtc_time_t *t);

#endif /* KERNEL_RTC_H */
