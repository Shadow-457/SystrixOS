/* ================================================================
 *  Systrix OS — drivers/rtc.c
 *  CMOS Real-Time Clock (RTC) driver
 *
 *  Reads wall-clock time from CMOS registers via I/O ports
 *  0x70 (index) and 0x71 (data).  All values are BCD-decoded.
 *
 *  Also provides FAT16 date/time word builders used by the
 *  FAT32 driver when creating directory entries.
 * ================================================================ */
#include "../include/kernel.h"

/* CMOS register indices */
#define CMOS_SECONDS    0x00
#define CMOS_MINUTES    0x02
#define CMOS_HOURS      0x04
#define CMOS_DAY        0x07
#define CMOS_MONTH      0x08
#define CMOS_YEAR       0x09
#define CMOS_CENTURY    0x32    /* may not exist on all hardware */
#define CMOS_STATUS_A   0x0A
#define CMOS_STATUS_B   0x0B

/* Read one CMOS register (BCD → binary) */
static u8 cmos_read(u8 reg) {
    outb(0x70, reg);
    u8 v = inb(0x71);
    /* Convert BCD to binary */
    return (u8)(((v >> 4) * 10) + (v & 0x0F));
}

/* Wait until RTC update-in-progress bit clears */
static void rtc_wait_ready(void) {
    u32 t = 100000;
    while ((inb(0x71) & 0x80) && --t) {
        outb(0x70, CMOS_STATUS_A);
    }
}

/* Read the current date and time */
void rtc_read(RtcTime *t) {
    rtc_wait_ready();
    outb(0x70, CMOS_STATUS_A);
    t->second = cmos_read(CMOS_SECONDS);
    t->minute = cmos_read(CMOS_MINUTES);
    t->hour   = cmos_read(CMOS_HOURS);
    t->day    = cmos_read(CMOS_DAY);
    t->month  = cmos_read(CMOS_MONTH);
    t->year   = cmos_read(CMOS_YEAR) + 2000;   /* assumes 21st century */
}

/* Get current second (fast path for timestamps) */
u8 rtc_seconds(void) {
    outb(0x70, CMOS_SECONDS);
    u8 v = inb(0x71);
    return (u8)(((v >> 4) * 10) + (v & 0x0F));
}

/* Build FAT16 date word: [15:9]=year-1980, [8:5]=month, [4:0]=day */
u16 rtc_fat_date(void) {
    RtcTime t;
    rtc_read(&t);
    u16 year  = (u16)(t.year - 1980);
    u16 month = (u16)t.month;
    u16 day   = (u16)t.day;
    return (u16)((year << 9) | (month << 5) | day);
}

/* Build FAT16 time word: [15:11]=hour, [10:5]=min, [4:0]=sec/2 */
u16 rtc_fat_time(void) {
    RtcTime t;
    rtc_read(&t);
    u16 hour = (u16)t.hour;
    u16 min  = (u16)t.minute;
    u16 sec  = (u16)(t.second / 2);
    return (u16)((hour << 11) | (min << 5) | sec);
}

/* Return seconds since midnight as a simple uptime proxy */
u32 rtc_time_of_day_secs(void) {
    RtcTime t;
    rtc_read(&t);
    return (u32)t.hour * 3600 + (u32)t.minute * 60 + t.second;
}
