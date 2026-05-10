/* CMOS real-time clock driver. See rtc.h. */

#include "rtc.h"
#include "io.h"

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define REG_SECOND  0x00
#define REG_MINUTE  0x02
#define REG_HOUR    0x04
#define REG_DAY     0x07
#define REG_MONTH   0x08
#define REG_YEAR    0x09
#define REG_STATUS_A 0x0A
#define REG_STATUS_B 0x0B

#define STATUS_A_UPDATING 0x80   /* an update is in progress   */
#define STATUS_B_24HOUR   0x02   /* else 12-hour with a PM bit */
#define STATUS_B_BINARY   0x04   /* else BCD                   */
#define STATUS_B_SET      0x80   /* halt updates while writing */

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

static void cmos_write(uint8_t reg, uint8_t val)
{
    outb(CMOS_INDEX, reg);
    outb(CMOS_DATA, val);
}

static uint8_t bcd_to_bin(uint8_t v) { return (uint8_t)((v & 0x0F) + (v >> 4) * 10); }
static uint8_t bin_to_bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static void wait_not_updating(void)
{
    for (int i = 0; i < 1000000; i++)
        if (!(cmos_read(REG_STATUS_A) & STATUS_A_UPDATING))
            return;
}

void rtc_read(wtime_t *out)
{
    uint8_t sec, min, hour, day, mon, year;
    uint8_t last_sec = 0xFF;

    /* Read twice and accept only when two reads agree, so we never catch the
     * clock mid-tick. */
    for (int tries = 0; tries < 100; tries++) {
        wait_not_updating();
        sec  = cmos_read(REG_SECOND);
        min  = cmos_read(REG_MINUTE);
        hour = cmos_read(REG_HOUR);
        day  = cmos_read(REG_DAY);
        mon  = cmos_read(REG_MONTH);
        year = cmos_read(REG_YEAR);
        if (sec == last_sec)
            break;
        last_sec = sec;
    }

    uint8_t status_b = cmos_read(REG_STATUS_B);
    int pm = (!(status_b & STATUS_B_24HOUR)) && (hour & 0x80);
    hour &= 0x7F;                        /* strip the PM flag before converting */

    if (!(status_b & STATUS_B_BINARY)) {
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        hour = bcd_to_bin(hour);
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        year = bcd_to_bin(year);
    }

    if (!(status_b & STATUS_B_24HOUR)) {
        if (pm)              hour = (uint8_t)((hour % 12) + 12);
        else if (hour == 12) hour = 0;
    }

    out->second = sec;
    out->minute = min;
    out->hour   = hour;
    out->day    = day;
    out->month  = mon;
    out->year   = 2000 + year;          /* two-digit register, this century */
}

void rtc_set(const wtime_t *t)
{
    uint8_t status_b = cmos_read(REG_STATUS_B);

    uint8_t sec  = (uint8_t)t->second;
    uint8_t min  = (uint8_t)t->minute;
    uint8_t hour = (uint8_t)t->hour;
    uint8_t day  = (uint8_t)t->day;
    uint8_t mon  = (uint8_t)t->month;
    uint8_t year = (uint8_t)(t->year % 100);

    /* Encode a 12-hour clock if that is the mode the RTC is in. */
    if (!(status_b & STATUS_B_24HOUR)) {
        int pm = hour >= 12;
        uint8_t h12 = (uint8_t)(hour % 12);
        if (h12 == 0) h12 = 12;
        hour = h12;
        if (!(status_b & STATUS_B_BINARY)) hour = bin_to_bcd(hour);
        if (pm) hour |= 0x80;
    } else if (!(status_b & STATUS_B_BINARY)) {
        hour = bin_to_bcd(hour);
    }

    if (!(status_b & STATUS_B_BINARY)) {
        sec  = bin_to_bcd(sec);
        min  = bin_to_bcd(min);
        day  = bin_to_bcd(day);
        mon  = bin_to_bcd(mon);
        year = bin_to_bcd(year);
    }

    /* Halt the clock's own updates while we write all the fields. */
    cmos_write(REG_STATUS_B, (uint8_t)(status_b | STATUS_B_SET));
    cmos_write(REG_SECOND, sec);
    cmos_write(REG_MINUTE, min);
    cmos_write(REG_HOUR,   hour);
    cmos_write(REG_DAY,    day);
    cmos_write(REG_MONTH,  mon);
    cmos_write(REG_YEAR,   year);
    cmos_write(REG_STATUS_B, status_b);   /* clears SET, updates resume */
}
