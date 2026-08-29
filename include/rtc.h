#ifndef RTC_H
#define RTC_H

#include "types.h"

typedef struct {
    u8 second;
    u8 minute;
    u8 hour;
    u8 day;
    u8 month;
    u16 year;
} rtc_time_t;

void rtc_read(rtc_time_t *t);

#endif
