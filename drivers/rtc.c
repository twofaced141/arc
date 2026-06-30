#include "rtc.h"
#include "idt.h"
#include "debug.h"
#include "isr.h"

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_PORT_INDEX, reg);
    return inb(CMOS_PORT_DATA);
}

static void cmos_write(uint8_t reg, uint8_t val) {
    outb(CMOS_PORT_INDEX, reg);
    outb(CMOS_PORT_DATA, val);
}

static int cmos_is_update_in_progress(void) {
    return cmos_read(CMOS_REG_STAT_A) & CMOS_UIP_FLAG;
}

static int bcd_to_bin(uint8_t bcd) {
    return ((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F);
}

void rtc_read_time(rtc_time_t *time) {
    if (!time) return;

    uint8_t reg_b = cmos_read(CMOS_REG_STAT_B);
    int binary = (reg_b & CMOS_STAT_B_DM) ? 1 : 0;
    int hour24 = (reg_b & CMOS_STAT_B_24H) ? 1 : 0;

    uint8_t sec, min, hour, day, month, year;

    while (cmos_is_update_in_progress());

    sec    = cmos_read(CMOS_REG_SEC);
    min    = cmos_read(CMOS_REG_MIN);
    hour   = cmos_read(CMOS_REG_HOUR);
    day    = cmos_read(CMOS_REG_DAY);
    month  = cmos_read(CMOS_REG_MONTH);
    year   = cmos_read(CMOS_REG_YEAR);

    if (!binary) {
        sec   = (uint8_t)bcd_to_bin(sec);
        min   = (uint8_t)bcd_to_bin(min);
        day   = (uint8_t)bcd_to_bin(day);
        month = (uint8_t)bcd_to_bin(month);
        year  = (uint8_t)bcd_to_bin(year);

        if (!hour24) {
            int pm = (hour & 0x80) ? 1 : 0;
            hour = (uint8_t)(hour & 0x7F);
            hour = (uint8_t)bcd_to_bin(hour);
            if (pm) { if (hour != 12) hour += 12; }
            else    { if (hour == 12) hour = 0; }
        } else {
            hour = (uint8_t)bcd_to_bin(hour);
        }
    } else {
        if (!hour24) {
            int pm = (hour & 0x80) ? 1 : 0;
            hour = hour & 0x7F;
            if (pm) { if (hour != 12) hour += 12; }
            else    { if (hour == 12) hour = 0; }
        }
    }

    time->second = sec;
    time->minute = min;
    time->hour   = hour;
    time->day    = day;
    time->month  = month;

    uint16_t full_year;
    if (year >= 80) {
        full_year = 1900 + year;
    } else {
        full_year = 2000 + year;
    }

    time->year = full_year;
}

static volatile uint32_t rtc_ticks;

void rtc_irq_handler(registers_t *r) {
    (void)r;
    cmos_read(CMOS_REG_STAT_C);
    rtc_ticks++;
    pic_send_eoi(RTC_IRQ);
}

uint32_t rtc_get_ticks(void) {
    return rtc_ticks;
}

void rtc_init(void) {
    uint8_t prev;

    prev = cmos_read(CMOS_REG_STAT_B);
    cmos_write(CMOS_REG_STAT_B, prev | CMOS_STAT_B_PIE);

    uint8_t rate = cmos_read(CMOS_REG_STAT_A) & 0xF0;
    cmos_write(CMOS_REG_STAT_A, rate | 0x0C);

    cmos_read(CMOS_REG_STAT_C);

    register_interrupt_handler(RTC_INT_NO, rtc_irq_handler);

    debug_print("rtc: periodic interrupt enabled (~8 Hz)\r\n");
}
