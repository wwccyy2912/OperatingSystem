/*
 * time.c - Date and time functions (C11 §7.27)
 * Copyright (c) 2026 OpSys Project
 *
 * Backed by kernel GetTime() (tick count) and OsGetRtcTime()
 * (wall clock).  No timezone support — localtime == gmtime.
 
 *
 * ------------------------------------------------------------------
 * Structure (time):
 *   time()/clock()/GetTime() -> kernel tick/RTC syscalls;
 *   localtime/gmtime/strftime -> civil-time conversion.
 * How it works:
 *   Reads the kernel tick counter or RTC; date math is standard
 *   days-from-epoch conversion.
 * Purpose:
 *   Timestamps and calendar formatting for services and the shell.
 * Caveats:
 *   No timezone database; localtime is fixed to the RTC timezone.
 * ------------------------------------------------------------------
 */

#include "time.h"
#include "string.h"
#include "../libos/syscalls.h"

/* ====================================================================
 * Static result buffers (POSIX: localtime/gmtime return static storage)
 * ==================================================================== */

static struct tm s_tm_buf;
static char      s_time_str[32];

/* ====================================================================
 * clock / time
 * ==================================================================== */

clock_t clock(void) {
    /* Kernel GetTime() returns a tick counter at 1 kHz. */
    return (clock_t)GetTime();
}

time_t time(time_t *timer) {
    rtc_time_t rtc;
    if (OsGetRtcTime(&rtc) != 0) {
        /* RTC unavailable — fall back to tick count */
        time_t t = (time_t)GetTime() / (time_t)CLOCKS_PER_SEC;
        if (timer)
            *timer = t;
        return t;
    }

    /* Convert RTC to epoch seconds (simplified leap-year calc). */
    int year = rtc.year;
    int mon  = rtc.month;
    int day  = rtc.day;

    /* Days from 1970-01-01 to year-01-01 (excluding leap-day of `year`). */
    int years_since = year - 1970;
    int leap_days   = (years_since + 1) / 4 - (years_since + 69) / 100 + (years_since + 369) / 400;
    int days        = years_since * 365 + leap_days;

    /* Days from Jan 1 to current month. */
    static const int mdays[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    days += mdays[mon - 1];
    /* Add leap day if past February in a leap year. */
    if (mon > 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)))
        days++;
    days += day - 1;

    time_t t = (time_t)days * 86400 + (time_t)rtc.hour * 3600 + (time_t)rtc.minute * 60 +
               (time_t)rtc.second;
    if (timer)
        *timer = t;
    return t;
}

double difftime(time_t t1, time_t t0) {
    return (double)t1 - (double)t0;
}

/* ====================================================================
 * Broken-down time conversion
 * ==================================================================== */

static const int s_mon_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static int IsLeap(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int DayOfWeek(int y, int m, int d) {
    /* Zeller's congruence (0 = Sunday). */
    if (m < 3) {
        m += 12;
        y--;
    }
    int k = y % 100;
    int j = y / 100;
    int h = (d + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    return (h + 6) % 7;
}

struct tm *gmtime(const time_t *timer) {
    if (!timer)
        return NULL;
    time_t t = *timer;

    int days = (int)(t / 86400);
    int secs = (int)(t % 86400);
    if (secs < 0) {
        secs += 86400;
        days--;
    }

    s_tm_buf.tm_hour = secs / 3600;
    s_tm_buf.tm_min  = (secs % 3600) / 60;
    s_tm_buf.tm_sec  = secs % 60;

    /* Compute year. */
    int year = 1970;
    while (1) {
        int yd = IsLeap(year) ? 366 : 365;
        if (days < yd)
            break;
        days -= yd;
        year++;
    }
    s_tm_buf.tm_year = year - 1900;

    /* Compute month. */
    int mon = 0;
    while (mon < 11) {
        int md = s_mon_days[mon];
        if (mon == 1 && IsLeap(year))
            md++;
        if (days < md)
            break;
        days -= md;
        mon++;
    }
    s_tm_buf.tm_mon  = mon;
    s_tm_buf.tm_mday = days + 1;

    s_tm_buf.tm_wday = DayOfWeek(year, mon + 1, s_tm_buf.tm_mday);

    /* Day of year. */
    int yday = s_tm_buf.tm_mday - 1;
    for (int i = 0; i < mon; i++) {
        yday += s_mon_days[i];
        if (i == 1 && IsLeap(year))
            yday++;
    }
    s_tm_buf.tm_yday  = yday;
    s_tm_buf.tm_isdst = 0;

    return &s_tm_buf;
}

struct tm *localtime(const time_t *timer) {
    return gmtime(timer);
}

time_t mktime(struct tm *t) {
    if (!t)
        return (time_t)-1;

    int year = t->tm_year + 1900;
    int mon  = t->tm_mon;

    /* Normalise month. */
    if (mon < 0) {
        int y = (-mon - 1) / 12 + 1;
        year -= y;
        mon += y * 12;
    } else if (mon > 11) {
        int y = mon / 12;
        year += y;
        mon -= y * 12;
    }

    /* Days from epoch. */
    int days = 0;
    for (int y = 1970; y < year; y++)
        days += IsLeap(y) ? 366 : 365;
    for (int m = 0; m < mon; m++) {
        int md = s_mon_days[m];
        if (m == 1 && IsLeap(year))
            md++;
        days += md;
    }
    days += t->tm_mday - 1;

    time_t result = (time_t)days * 86400 + (time_t)t->tm_hour * 3600 + (time_t)t->tm_min * 60 +
                    (time_t)t->tm_sec;

    /* Update fields. */
    t->tm_wday = DayOfWeek(year, mon + 1, t->tm_mday);
    t->tm_year = year - 1900;
    t->tm_mon  = mon;

    int yday = t->tm_mday - 1;
    for (int m = 0; m < mon; m++) {
        int md = s_mon_days[m];
        if (m == 1 && IsLeap(year))
            md++;
        yday += md;
    }
    t->tm_yday = yday;

    return result;
}

/* ====================================================================
 * Textual formatting
 * ==================================================================== */

static const char *s_wday[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *s_mon[]  = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

char *asctime(const struct tm *t) {
    /* "Wed Jun 30 21:49:08 2026\n" — 25 chars + NUL */
    s_time_str[0]  = s_wday[t->tm_wday][0];
    s_time_str[1]  = s_wday[t->tm_wday][1];
    s_time_str[2]  = s_wday[t->tm_wday][2];
    s_time_str[3]  = ' ';
    s_time_str[4]  = s_mon[t->tm_mon][0];
    s_time_str[5]  = s_mon[t->tm_mon][1];
    s_time_str[6]  = s_mon[t->tm_mon][2];
    s_time_str[7]  = ' ';
    s_time_str[8]  = '0' + (t->tm_mday / 10);
    s_time_str[9]  = '0' + (t->tm_mday % 10);
    s_time_str[10] = ' ';
    s_time_str[11] = '0' + (t->tm_hour / 10);
    s_time_str[12] = '0' + (t->tm_hour % 10);
    s_time_str[13] = ':';
    s_time_str[14] = '0' + (t->tm_min / 10);
    s_time_str[15] = '0' + (t->tm_min % 10);
    s_time_str[16] = ':';
    s_time_str[17] = '0' + (t->tm_sec / 10);
    s_time_str[18] = '0' + (t->tm_sec % 10);
    s_time_str[19] = ' ';
    int year       = t->tm_year + 1900;
    s_time_str[20] = '0' + (year / 1000) % 10;
    s_time_str[21] = '0' + (year / 100) % 10;
    s_time_str[22] = '0' + (year / 10) % 10;
    s_time_str[23] = '0' + year % 10;
    s_time_str[24] = '\n';
    s_time_str[25] = '\0';
    return s_time_str;
}

char *ctime(const time_t *timer) {
    return asctime(localtime(timer));
}

/* ====================================================================
 * strftime — simplified (supports %Y %m %d %H %M %S %a %b %j %w %%)
 * ==================================================================== */

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *t) {
    size_t pos = 0;

#define EMIT(ch)           \
    do {                   \
        if (pos + 1 < max) \
            s[pos] = (ch); \
        pos++;             \
    } while (0)

#define EMIT2(v)              \
    do {                      \
        EMIT('0' + (v) / 10); \
        EMIT('0' + (v) % 10); \
    } while (0)

    while (*fmt && pos < max) {
        if (*fmt != '%') {
            EMIT(*fmt);
            fmt++;
            continue;
        }
        fmt++;
        switch (*fmt) {
        case 'Y': {
            int y = t->tm_year + 1900;
            EMIT('0' + (y / 1000) % 10);
            EMIT('0' + (y / 100) % 10);
            EMIT('0' + (y / 10) % 10);
            EMIT('0' + y % 10);
            break;
        }
        case 'm':
            EMIT2(t->tm_mon + 1);
            break;
        case 'd':
            EMIT2(t->tm_mday);
            break;
        case 'H':
            EMIT2(t->tm_hour);
            break;
        case 'M':
            EMIT2(t->tm_min);
            break;
        case 'S':
            EMIT2(t->tm_sec);
            break;
        case 'j':
            EMIT2((t->tm_yday / 100) % 10);
            EMIT2(t->tm_yday % 100);
            break;
        case 'w':
            EMIT('0' + t->tm_wday);
            break;
        case 'a':
            for (int i = 0; i < 3; i++)
                EMIT(s_wday[t->tm_wday][i]);
            break;
        case 'A': {
            const char *p = s_wday[t->tm_wday];
            while (*p)
                EMIT(*p++);
            break;
        }
        case 'b':
            for (int i = 0; i < 3; i++)
                EMIT(s_mon[t->tm_mon][i]);
            break;
        case 'B': {
            const char *p = s_mon[t->tm_mon];
            while (*p)
                EMIT(*p++);
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
#undef EMIT
#undef EMIT2

    if (max > 0)
        s[pos < max ? pos : max - 1] = '\0';
    return pos;
}

/* ====================================================================
 * timespec_get (C11 §7.27.2.5)
 * ==================================================================== */

int timespec_get(struct timespec *ts, int base) {
    if (!ts || base != TIME_UTC)
        return 0;
    time_t t    = time(NULL);
    ts->tv_sec  = t;
    ts->tv_nsec = 0; /* kernel tick granularity is 1 ms */
    return base;
}
