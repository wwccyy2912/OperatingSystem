/*
 * time.h - Date and time (C11 §7.27)
 * Copyright (c) 2026 OpSys Project
 *
 * Provides time_t, struct tm, clock_t and the standard time functions.
 * Backed by the kernel's get_time() (tick count) and os_get_rtc_time()
 * (wall clock) syscalls.
 */

#ifndef LIBC_TIME_H
#define LIBC_TIME_H

#include <stddef.h>

/* ====================================================================
 * Types (C11 §7.27.1)
 * ==================================================================== */

typedef long long          time_t;  /* epoch seconds */
typedef unsigned long long clock_t; /* processor time in ticks */

struct tm {
    int tm_sec;   /* seconds after the minute [0, 60] */
    int tm_min;   /* minutes after the hour [0, 59] */
    int tm_hour;  /* hours since midnight [0, 23] */
    int tm_mday;  /* day of the month [1, 31] */
    int tm_mon;   /* months since January [0, 11] */
    int tm_year;  /* years since 1900 */
    int tm_wday;  /* days since Sunday [0, 6] */
    int tm_yday;  /* days since January 1 [0, 365] */
    int tm_isdst; /* daylight saving time flag (<0 = unknown) */
};

/* C11 §7.27.1: timer and signal support */
struct timespec {
    time_t tv_sec;  /* seconds */
    long   tv_nsec; /* nanoseconds [0, 999999999] */
};

/* ====================================================================
 * Constants
 * ==================================================================== */

#define CLOCKS_PER_SEC 1000U /* kernel tick rate (1 kHz) */
#define TIME_UTC       1     /* timespec_get base */

/* ====================================================================
 * Functions (C11 §7.27.2-7.27.3)
 * ==================================================================== */

/* Returns the number of processor ticks since an epoch, or (clock_t)-1
 * if unavailable.  Dividing by CLOCKS_PER_SEC yields seconds. */
clock_t clock(void);

/* Returns the current calendar time as seconds since the epoch.
 * If timer is non-NULL, the value is also stored through it. */
time_t time(time_t *timer);

/* Returns the difference t1 - t0 in seconds. */
double difftime(time_t t1, time_t t0);

/* Normalises a broken-down time and returns the epoch seconds, or
 * (time_t)-1 on failure.  The fields of *t are adjusted to their
 * canonical ranges. */
time_t mktime(struct tm *t);

/* Converts calendar time to broken-down local time. */
struct tm *localtime(const time_t *timer);

/* Converts calendar time to broken-down UTC time (same as localtime
 * in v0.1 — no timezone support). */
struct tm *gmtime(const time_t *timer);

/* Converts broken-down time to a textual representation:
 *   "Wed Jun 30 21:49:08 2026\n" */
char *asctime(const struct tm *t);

/* Converts calendar time to the same textual representation. */
char *ctime(const time_t *timer);

/* Formats broken-down time according to a format string (strftime). */
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *t);

/* C11 §7.27.2.5: timespec_get — fills ts with the current calendar
 * time relative to base.  Only TIME_UTC is supported. */
int timespec_get(struct timespec *ts, int base);

#endif /* LIBC_TIME_H */
