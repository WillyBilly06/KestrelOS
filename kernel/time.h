#ifndef KESTREL_TIME_H
#define KESTREL_TIME_H

#include "kernel.h"

extern volatile u64 g_uptime_ms;

void timer_init(void);
u64  timer_cycles_per_us(void);     /* so user space can read the counter itself */
u64  timer_now_us(void);            /* microseconds, from the cycle counter */
void timer_udelay(u32 us);          /* busy wait, safe before the scheduler */
void timer_mdelay(u32 ms);

/* Wall clock, read from the CMOS RTC at boot and advanced by the tick. */
typedef struct {
    u16 year;
    u8  month, day, hour, minute, second;
} datetime_t;

void rtc_read(datetime_t *out);
u64  time_unix_seconds(void);       /* seconds since 1970-01-01T00:00:00Z */
/* Coherent millisecond snapshot in the scheduler's uptime clock domain.
 * Realtime uses the fixed boot RTC epoch; neither output implies ns precision. */
void time_clock_snapshot(u64 *monotonic_ms, u64 *realtime_ms);
void time_format(char *buf, size_t cap, u64 unix_seconds);

#endif
