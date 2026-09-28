/* timer.c - the millisecond tick, delays and the wall clock.
 *
 * The local APIC timer is the tick source at 1 kHz; the TSC provides
 * fine-grained delays once its frequency is known, and the CMOS RTC seeds the
 * wall clock at boot.
 */
#include "kernel.h"
#include "time.h"
#include "apic.h"
#include "cpu.h"
#include "klog.h"

volatile u64 g_uptime_ms;

static u64  tsc_per_us;
static u64  boot_unix;          /* wall clock at the moment uptime was 0 */
static bool tick_running;

/* Defined by the scheduler; weak so the timer works before it exists. */
__attribute__((weak)) regs_t *sched_tick(regs_t *r) { return r; }

static regs_t *timer_isr(regs_t *r, void *ctx) {
    (void)ctx;
    g_uptime_ms++;
    cpu_frequency_sample(cpu_current_slot());
    lapic_eoi();
    return sched_tick(r);
}

/* ------------------------------------------------------------------------- */
/* CMOS RTC                                                                  */
/* ------------------------------------------------------------------------- */

static u8 cmos_read(u8 reg) {
    outb(0x70, (u8)((inb(0x70) & 0x80) | (reg & 0x7F)));
    io_wait();
    return inb(0x71);
}

static bool rtc_updating(void) { return (cmos_read(0x0A) & 0x80) != 0; }

static u8 bcd_to_bin(u8 v) { return (u8)((v & 0x0F) + ((v >> 4) * 10)); }

void rtc_read(datetime_t *out) {
    /* Read twice and accept only when two consecutive reads agree, which is
     * how the RTC's lack of an atomic snapshot is normally worked around. */
    datetime_t a, b;
    int guard = 0;

    do {
        while (rtc_updating() && ++guard < 1000000) { }
        a.second = cmos_read(0x00);
        a.minute = cmos_read(0x02);
        a.hour   = cmos_read(0x04);
        a.day    = cmos_read(0x07);
        a.month  = cmos_read(0x08);
        a.year   = cmos_read(0x09);

        while (rtc_updating() && ++guard < 2000000) { }
        b.second = cmos_read(0x00);
        b.minute = cmos_read(0x02);
        b.hour   = cmos_read(0x04);
        b.day    = cmos_read(0x07);
        b.month  = cmos_read(0x08);
        b.year   = cmos_read(0x09);
    } while (guard < 2000000 &&
             (a.second != b.second || a.minute != b.minute || a.hour != b.hour ||
              a.day != b.day || a.month != b.month || a.year != b.year));

    u8 status = cmos_read(0x0B);
    bool bcd = !(status & 0x04);
    bool h12 = !(status & 0x02);
    bool pm  = (a.hour & 0x80) != 0;

    if (bcd) {
        a.second = bcd_to_bin(a.second);
        a.minute = bcd_to_bin(a.minute);
        a.hour   = bcd_to_bin((u8)(a.hour & 0x7F));
        a.day    = bcd_to_bin(a.day);
        a.month  = bcd_to_bin(a.month);
        a.year   = bcd_to_bin((u8)a.year);
    } else {
        a.hour &= 0x7F;
    }

    if (h12) {
        if (pm && a.hour != 12) a.hour = (u8)(a.hour + 12);
        else if (!pm && a.hour == 12) a.hour = 0;
    }

    /* Two-digit year: the century register is not reliable across firmwares,
     * so pivot instead. */
    out->year   = (u16)(a.year < 70 ? 2000 + a.year : 1900 + a.year);
    out->month  = a.month ? a.month : 1;
    out->day    = a.day ? a.day : 1;
    out->hour   = a.hour;
    out->minute = a.minute;
    out->second = a.second;
}

static bool is_leap(u32 y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static u64 to_unix(const datetime_t *t) {
    static const u16 cumulative[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    if (t->year < 1970) return 0;

    u64 days = 0;
    for (u32 y = 1970; y < t->year; y++) days += is_leap(y) ? 366 : 365;
    days += cumulative[(t->month - 1) % 12];
    if (t->month > 2 && is_leap(t->year)) days++;
    days += t->day - 1;

    return days * 86400ULL + t->hour * 3600ULL + t->minute * 60ULL + t->second;
}

u64 time_unix_seconds(void) { return boot_unix + g_uptime_ms / 1000; }

void time_clock_snapshot(u64 *monotonic_ms, u64 *realtime_ms) {
    u64 now = __atomic_load_n(&g_uptime_ms, __ATOMIC_RELAXED);
    if (monotonic_ms) *monotonic_ms = now;
    if (realtime_ms) *realtime_ms = boot_unix > (~0ull - now) / 1000u
        ? ~0ull : boot_unix * 1000u + now;
}

void time_format(char *buf, size_t cap, u64 unix_seconds) {
    static const u16 cumulative[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

    u64 days = unix_seconds / 86400;
    u32 rem  = (u32)(unix_seconds % 86400);

    u32 year = 1970;
    for (;;) {
        u32 len = is_leap(year) ? 366 : 365;
        if (days < len) break;
        days -= len;
        year++;
    }
    u32 month = 11;
    for (u32 m = 0; m < 12; m++) {
        u32 start = cumulative[m] + ((m >= 2 && is_leap(year)) ? 1 : 0);
        if ((u32)days < start) { month = m - 1; break; }
        if (m == 11) month = 11;
    }
    u32 mstart = cumulative[month] + ((month >= 2 && is_leap(year)) ? 1 : 0);
    u32 day = (u32)days - mstart + 1;

    snprintf(buf, cap, "%04u-%02u-%02u %02u:%02u:%02u",
             year, month + 1, day, rem / 3600, (rem / 60) % 60, rem % 60);
}

/* ------------------------------------------------------------------------- */
/* delays                                                                    */
/* ------------------------------------------------------------------------- */

static void calibrate_tsc(void) {
    /* Measure the TSC across a known APIC-timer interval.  Any error here only
     * affects short delays, so a single pass is enough. */
    u64 start_ms = g_uptime_ms;
    while (g_uptime_ms == start_ms) pause_cpu();     /* align to a tick edge */

    u64 t0 = rdtsc();
    u64 t0_ms = g_uptime_ms;
    while (g_uptime_ms - t0_ms < 20) pause_cpu();
    u64 elapsed = rdtsc() - t0;

    tsc_per_us = elapsed / (20 * 1000);
    if (tsc_per_us == 0) tsc_per_us = 1;
    kinfo("time", "TSC runs at about %lu MHz", tsc_per_us);
}

/* Microseconds since start-up, from the cycle counter.
 *
 * The tick is a millisecond, which is far too coarse to measure anything that
 * happens inside one frame - and a frame is exactly what most of the questions
 * about how fast this system feels come down to. */
u64 timer_cycles_per_us(void) { return tsc_per_us; }

u64 timer_now_us(void) {
    if (!tsc_per_us) return g_uptime_ms * 1000;
    return rdtsc() / tsc_per_us;
}

void timer_udelay(u32 us) {
    if (tsc_per_us) {
        u64 target = rdtsc() + (u64)us * tsc_per_us;
        while ((s64)(rdtsc() - target) < 0) pause_cpu();
        return;
    }
    /* Before calibration, fall back to the PIT's 1.193182 MHz channel 2, which
     * is available whether or not interrupts are running. */
    u32 ticks = (u32)(((u64)us * 1193182) / 1000000);
    if (!ticks) ticks = 1;
    while (ticks) {
        u32 chunk = ticks > 0xFFF0 ? 0xFFF0 : ticks;
        ticks -= chunk;

        u8 port61 = inb(0x61);
        outb(0x61, (u8)((port61 & ~0x02) | 0x01));
        outb(0x43, 0xB0);
        outb(0x42, (u8)(chunk & 0xFF));
        outb(0x42, (u8)(chunk >> 8));
        u8 t = inb(0x61);
        outb(0x61, (u8)(t & ~1));
        outb(0x61, (u8)(t | 1));

        u64 guard = 0;
        while (!(inb(0x61) & 0x20) && ++guard < 50000000ULL) { }
        outb(0x61, port61);
    }
}

void timer_mdelay(u32 ms) {
    if (tick_running) {
        u64 target = g_uptime_ms + ms;
        while (g_uptime_ms < target) { __asm__ volatile("hlt"); }
        return;
    }
    while (ms--) timer_udelay(1000);
}

void timer_init(void) {
    datetime_t now;
    rtc_read(&now);
    boot_unix = to_unix(&now);

    char stamp[32];
    time_format(stamp, sizeof stamp, boot_unix);
    kinfo("time", "wall clock from RTC: %s UTC", stamp);

    irq_install(VEC_APIC_TIMER, timer_isr, NULL);
    lapic_timer_calibrate();
    lapic_timer_start(1000, VEC_APIC_TIMER);
    tick_running = true;

    sti();
    calibrate_tsc();
}
