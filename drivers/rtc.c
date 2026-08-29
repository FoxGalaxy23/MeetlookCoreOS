#include "rtc.h"
#include "io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static u8 cmos_read(u8 reg) {
    outb(CMOS_ADDR, reg);
    io_wait();
    return inb(CMOS_DATA);
}

static bool_t update_in_progress(void) {
    outb(CMOS_ADDR, 0x0A);
    return (inb(CMOS_DATA) & 0x80) != 0;
}

static u8 bcd_to_bin(u8 v) {
    return (v & 0x0F) + ((v / 16) * 10);
}

void rtc_read(rtc_time_t *t) {
    while (update_in_progress());

    u8 second = cmos_read(0x00);
    u8 minute = cmos_read(0x02);
    u8 hour   = cmos_read(0x04);
    u8 day    = cmos_read(0x07);
    u8 month  = cmos_read(0x08);
    u8 year   = cmos_read(0x09);
    u8 regB   = cmos_read(0x0B);

    if (!(regB & 0x04)) { /* формат BCD */
        second = bcd_to_bin(second);
        minute = bcd_to_bin(minute);
        hour   = bcd_to_bin(hour & 0x7F) | (hour & 0x80);
        day    = bcd_to_bin(day);
        month  = bcd_to_bin(month);
        year   = bcd_to_bin(year);
    }

    if (!(regB & 0x02) && (hour & 0x80)) {
        hour = ((hour & 0x7F) + 12) % 24;
    }

    t->second = second;
    t->minute = minute;
    t->hour = hour;
    t->day = day;
    t->month = month;
    t->year = 2000 + year;
}
