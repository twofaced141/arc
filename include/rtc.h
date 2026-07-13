#ifndef RTC_H
#define RTC_H

#include <stdint.h>
#include "isr.h"

#define CMOS_PORT_INDEX 0x70
#define CMOS_PORT_DATA  0x71

#define CMOS_REG_SEC        0x00
#define CMOS_REG_MIN        0x02
#define CMOS_REG_HOUR       0x04
#define CMOS_REG_WEEKDAY    0x06
#define CMOS_REG_DAY        0x07
#define CMOS_REG_MONTH      0x08
#define CMOS_REG_YEAR       0x09
#define CMOS_REG_STAT_A     0x0A
#define CMOS_REG_STAT_B     0x0B

#define CMOS_REG_STAT_C     0x0C
#define CMOS_REG_STAT_D     0x0D

#define CMOS_UIP_FLAG       (1 << 7)
#define CMOS_STAT_B_DM      (1 << 2)
#define CMOS_STAT_B_24H     (1 << 1)
#define CMOS_STAT_B_PIE     (1 << 6)

#define RTC_IRQ             8
#define RTC_INT_NO          40

typedef struct {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
} rtc_time_t;

void         rtc_init(void);
void         rtc_read_time(rtc_time_t *time);
uint32_t     rtc_get_ticks(void);
void         rtc_irq_handler(registers_t *r);

#endif
