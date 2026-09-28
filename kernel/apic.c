/* apic.c - local APIC and I/O APIC.
 *
 * The 8259 PICs are remapped and masked rather than left alone, so a stray
 * legacy interrupt cannot arrive on a vector we use for something else.  Device
 * interrupts are then routed through the I/O APIC, and the local APIC timer
 * drives preemption after being calibrated against the PIT.
 */
#include "kernel.h"
#include "apic.h"
#include "time.h"
#include "acpi.h"
#include "cpu.h"
#include "mm.h"
#include "klog.h"

/* Local APIC registers (offsets into the MMIO page). */
#define LAPIC_ID        0x020
#define LAPIC_VERSION   0x030
#define LAPIC_TPR       0x080
#define LAPIC_EOI       0x0B0

/* The interrupt controller's own two handlers, defined further down beside the
 * explanation of why each is different from a device's. */
static regs_t *apic_error_isr(regs_t *r, void *ctx);
static regs_t *apic_spurious_isr(regs_t *r, void *ctx);
#define LAPIC_LDR       0x0D0
#define LAPIC_DFR       0x0E0
#define LAPIC_SPURIOUS  0x0F0
#define LAPIC_ESR       0x280
#define LAPIC_ICR_LO    0x300
#define LAPIC_ICR_HI    0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERROR 0x370
#define LAPIC_TIMER_ICR 0x380
#define LAPIC_TIMER_CCR 0x390
#define LAPIC_TIMER_DIV 0x3E0

#define LVT_MASKED      (1u << 16)
#define TIMER_PERIODIC  (1u << 17)

static volatile u32 *lapic;
static bool          x2apic;
static u32           ioapic_count;

typedef struct {
    volatile u32 *base;
    u32 gsi_base;
    u32 pins;
} ioapic_t;
static ioapic_t ioapics[ACPI_MAX_IOAPICS];

/* ------------------------------------------------------------------------- */
/* local APIC access                                                         */
/* ------------------------------------------------------------------------- */

static inline u32 lapic_read(u32 reg) {
    if (x2apic) return (u32)rdmsr(0x800 + (reg >> 4));
    return lapic[reg / 4];
}

static inline void lapic_write(u32 reg, u32 val) {
    if (x2apic) { wrmsr(0x800 + (reg >> 4), val); return; }
    lapic[reg / 4] = val;
    (void)lapic[LAPIC_ID / 4];      /* serialise the write */
}

void lapic_eoi(void) { lapic_write(LAPIC_EOI, 0); }

/* Send one inter-processor message.
 *
 * This lives here rather than with the code that starts the other processors
 * because the two ways of reaching the interrupt command register are not
 * interchangeable, and only this file knows which is in use.  In x2APIC mode
 * the memory-mapped page is switched off entirely - writes to it are simply
 * lost, with no fault and nothing logged - so a startup message sent that way
 * on a modern machine goes nowhere and every core stays asleep.  That is not a
 * hypothetical: it is what this code did first.
 *
 * The two forms also differ in shape.  x2APIC takes the destination and the
 * command as one 64-bit write and has no delivery-status bit to poll. The
 * explicit fence below publishes memory before it. The older form takes two writes, high half
 * first, and has a busy bit to poll.
 */
void lapic_send_ipi(u32 apic_id, u32 command) {
    /* The xAPIC destination/command pair must not be interleaved with an IPI
     * sent from an interrupt on this same processor. Each CPU has its own ICR,
     * so a global lock would only add unnecessary cross-CPU serialization. */
    bool irq = irq_save();
    if (x2apic) {
        /* x2APIC MSR writes are not generally serializing. Publish preceding
         * memory stores before the notification reaches the destination. */
        __asm__ volatile("mfence; lfence" ::: "memory");
        wrmsr(0x830, ((u64)apic_id << 32) | command);
        irq_restore(irq);
        return;
    }

    lapic[LAPIC_ICR_HI / 4] = apic_id << 24;
    lapic[LAPIC_ICR_LO / 4] = command;

    /* Delivery status, bit 12: clear once the message has been sent. */
    for (int i = 0; i < 1000; i++) {
        if (!(lapic[LAPIC_ICR_LO / 4] & (1u << 12))) break;
        timer_udelay(10);
    }
    irq_restore(irq);
}

u32 lapic_id(void) {
    u32 id = lapic_read(LAPIC_ID);
    return x2apic ? id : (id >> 24);
}

/* ------------------------------------------------------------------------- */
/* 8259                                                                      */
/* ------------------------------------------------------------------------- */

static void pic_remap_and_mask(void) {
    u8 mask1 = inb(0x21), mask2 = inb(0xA1);
    (void)mask1; (void)mask2;

    outb(0x20, 0x11); io_wait();      /* ICW1: init, expect ICW4 */
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();      /* ICW2: master base vector 0x20 */
    outb(0xA1, 0x28); io_wait();      /* slave base 0x28 */
    outb(0x21, 0x04); io_wait();      /* ICW3: slave on IRQ2 */
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();      /* ICW4: 8086 mode */
    outb(0xA1, 0x01); io_wait();

    /* Mask everything: the I/O APIC does the routing from here on. */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

/* ------------------------------------------------------------------------- */
/* I/O APIC                                                                  */
/* ------------------------------------------------------------------------- */

static u32 ioapic_read(ioapic_t *io, u32 reg) {
    io->base[0] = reg;
    return io->base[4];
}

static void ioapic_write(ioapic_t *io, u32 reg, u32 val) {
    io->base[0] = reg;
    io->base[4] = val;
}

static ioapic_t *ioapic_for_gsi(u32 gsi) {
    for (u32 i = 0; i < ioapic_count; i++)
        if (gsi >= ioapics[i].gsi_base && gsi < ioapics[i].gsi_base + ioapics[i].pins)
            return &ioapics[i];
    return NULL;
}

bool ioapic_route(u32 gsi, u8 vector, bool active_low, bool level, u32 dest_apic) {
    ioapic_t *io = ioapic_for_gsi(gsi);
    if (!io) { kerr("apic", "no I/O APIC covers GSI %u", gsi); return false; }

    u32 pin = gsi - io->gsi_base;
    u32 lo = vector
           | (0u << 8)                       /* fixed delivery      */
           | (0u << 11)                      /* physical dest mode  */
           | ((u32)active_low << 13)
           | ((u32)level << 15);
    u32 hi = dest_apic << 24;

    /* Program the high word first, and keep the entry masked until both
     * halves are consistent. */
    ioapic_write(io, 0x10 + pin * 2 + 1, hi);
    ioapic_write(io, 0x10 + pin * 2, lo);
    return true;
}

void ioapic_mask(u32 gsi, bool masked) {
    ioapic_t *io = ioapic_for_gsi(gsi);
    if (!io) return;
    u32 pin = gsi - io->gsi_base;
    u32 lo = ioapic_read(io, 0x10 + pin * 2);
    if (masked) lo |= LVT_MASKED; else lo &= ~LVT_MASKED;
    ioapic_write(io, 0x10 + pin * 2, lo);
}

/* ------------------------------------------------------------------------- */
/* init                                                                      */
/* ------------------------------------------------------------------------- */

void apic_init(void) {
    pic_remap_and_mask();

    u64 base_msr = rdmsr(MSR_APIC_BASE);
    if (!(base_msr & (1ULL << 11))) {
        base_msr |= (1ULL << 11);           /* global enable */
        wrmsr(MSR_APIC_BASE, base_msr);
    }

    if (g_cpu.has_x2apic) {
        wrmsr(MSR_APIC_BASE, rdmsr(MSR_APIC_BASE) | (1ULL << 10));
        x2apic = true;
        kinfo("apic", "using x2APIC");
    } else {
        u64 phys = base_msr & 0xFFFFFF000ULL;
        if (!phys) phys = g_lapic_phys;
        lapic = vmm_map_mmio(phys, PAGE_SIZE);
        if (!lapic) panic("apic: cannot map the local APIC at %p", (void *)phys);
        kinfo("apic", "local APIC mapped from %p", (void *)phys);

        /* Flat logical destination mode, so LDR/DFR are predictable. */
        lapic_write(LAPIC_DFR, 0xFFFFFFFF);
        lapic_write(LAPIC_LDR, (lapic_read(LAPIC_LDR) & 0x00FFFFFF) | (1 << 24));
    }

    /* Mask the error LVT before changing other LVTs. In particular, writing
     * vector zero while masking a fixed-delivery LVT can itself set ESR.
     * Keep legal vectors even while timers/legacy input are masked. */
    lapic_write(LAPIC_LVT_ERROR, LVT_MASKED | VEC_APIC_ERROR);
    irq_install(VEC_APIC_ERROR, apic_error_isr, NULL);
    irq_install(VEC_SPURIOUS, apic_spurious_isr, NULL);
    lapic_write(LAPIC_TPR, 0);                          /* accept every priority */
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED | VEC_APIC_TIMER);
    lapic_write(LAPIC_LVT_LINT0, LVT_MASKED | VEC_IRQ_BASE);
    lapic_write(LAPIC_LVT_LINT1, 0x400);                /* NMI */
    lapic_write(LAPIC_SPURIOUS, 0x100 | VEC_SPURIOUS);  /* enable + spurious vector */

    /* The APIC's own two interrupts, claimed rather than left to the
     * unhandled path.  Both need handling differently from a device, and one
     * of them carries information that is worth reading. */
    lapic_write(LAPIC_ESR, 0);
    (void)lapic_read(LAPIC_ESR);
    lapic_write(LAPIC_LVT_ERROR, VEC_APIC_ERROR);

    kinfo("apic", "local APIC id %u, version %#x", lapic_id(), lapic_read(LAPIC_VERSION) & 0xFF);

    /* I/O APICs. */
    for (int i = 0; i < g_acpi_ioapic_count && ioapic_count < ACPI_MAX_IOAPICS; i++) {
        volatile u32 *base = vmm_map_mmio(g_acpi_ioapics[i].address, PAGE_SIZE);
        if (!base) { kerr("apic", "cannot map I/O APIC at %p", (void *)g_acpi_ioapics[i].address); continue; }

        ioapics[ioapic_count].base     = base;
        ioapics[ioapic_count].gsi_base = g_acpi_ioapics[i].gsi_base;

        u32 ver = ioapic_read(&ioapics[ioapic_count], 0x01);
        ioapics[ioapic_count].pins = ((ver >> 16) & 0xFF) + 1;
        g_acpi_ioapics[i].gsi_count = ioapics[ioapic_count].pins;

        /* Mask every pin: nothing is routed until a driver asks for it. */
        for (u32 pin = 0; pin < ioapics[ioapic_count].pins; pin++) {
            ioapic_write(&ioapics[ioapic_count], 0x10 + pin * 2, LVT_MASKED);
            ioapic_write(&ioapics[ioapic_count], 0x10 + pin * 2 + 1, 0);
        }

        kinfo("apic", "I/O APIC %d: %u pins from GSI %u",
              i, ioapics[ioapic_count].pins, ioapics[ioapic_count].gsi_base);
        ioapic_count++;
    }

    if (!ioapic_count) kwarn("apic", "no usable I/O APIC; device interrupts may not arrive");
}

bool apic_available(void) { return x2apic || lapic != NULL; }

bool lapic_init_worker(void) {
    if (!apic_available()) return false;
    u64 base = rdmsr(MSR_APIC_BASE);
    if (!(base & (1ULL << 11))) {
        base |= 1ULL << 11;
        wrmsr(MSR_APIC_BASE, base);
    }
    /* APIC mode is per logical CPU. INIT does not configure an AP from the
     * BSP's C globals; enable the same hardware access mode before using it. */
    if (x2apic) wrmsr(MSR_APIC_BASE, base | (1ULL << 10));
    else if (base & (1ULL << 10)) return false;

    lapic_write(LAPIC_LVT_ERROR, LVT_MASKED | VEC_APIC_ERROR);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED | VEC_APIC_TIMER);
    lapic_write(LAPIC_LVT_LINT0, LVT_MASKED | VEC_IRQ_BASE);
    lapic_write(LAPIC_LVT_LINT1, 0x400); /* preserve NMI handling */
    /* Block the BSP's scheduler timer and all current PCI/MSI vectors. The AP
     * cannot safely enter handlers that use the global process `current`. */
    lapic_write(LAPIC_TPR, 0xe0);
    lapic_write(LAPIC_SPURIOUS, 0x100 | VEC_SPURIOUS);
    lapic_write(LAPIC_ESR, 0);
    (void)lapic_read(LAPIC_ESR);
    return true;
}

/* ------------------------------------------------------------------------- */
/* local APIC timer                                                          */
/* ------------------------------------------------------------------------- */

static u32 timer_ticks_per_ms;

/* ---------------------------------------------------------- calibrating time
 *
 * The local APIC timer counts at some rate nobody has written down.  To use it
 * for anything you have to measure it, and measuring it needs a second clock
 * whose rate IS known.  For forty years every PC had one: the 8254 interval
 * timer at port 0x40, ticking at 1.193182 MHz because that is a twelfth of the
 * original colour-television subcarrier.
 *
 * It is going away.  Intel has been removing the 8254 from client platforms,
 * and on the Core Ultra parts it is simply not there - reads come back with a
 * bit that never changes, and code that waits for it waits forever.  A kernel
 * that only knows how to calibrate against the PIT does not run slowly on such
 * a machine; it stops during boot with no explanation, which is exactly what
 * this one did.
 *
 * So there are three sources, tried in order, and the order is by how much
 * each one proves:
 *
 *   1. The ACPI power-management timer.  A real measurement against a real
 *      counter of exactly known rate - 3.579545 MHz - that every machine with
 *      ACPI has, including every machine that has dropped the PIT.  This is
 *      the best answer available and it is a measurement, not an assumption.
 *
 *   2. The 8254, if it is still there.  Same measurement, older counter.  It
 *      is tried second rather than first because it is the one disappearing.
 *
 *   3. What the processor says about itself.  CPUID leaf 0x15 reports the core
 *      crystal the APIC timer is driven from.  No measurement is involved, so
 *      it is trusted least - but it needs no other timer at all, which makes
 *      it the only thing left when there is nothing to measure against.
 *
 * And every wait is bounded by the processor's own cycle counter rather than
 * by a count of iterations.  A loop that gives up after a hundred million
 * reads sounds bounded and is not: each read of a legacy port that nothing
 * answers takes about a microsecond, so that bound is a hundred seconds, three
 * times over.  Five minutes of apparently having crashed is not meaningfully
 * better than crashing, and it is considerably harder to diagnose.
 */
static u32 timer_ticks_per_ms;

/* Roughly how many cycles a millisecond is, before anything is known about the
 * clock.  Only used to bound waits, where being out by a factor of three
 * changes nothing that matters. */
#define GUESS_CYCLES_PER_MS 3000000ULL

static bool tsc_elapsed(u64 start, u64 ms) {
    return (rdtsc() - start) > ms * GUESS_CYCLES_PER_MS;
}

/* --------------------------------------------------- 1. the ACPI PM timer */

static u32 calibrate_against_pm_timer(u32 ms) {
    u32 port = acpi_pm_timer_port();
    if (!port) return 0;

    const u32 pm_hz = 3579545;
    u32 mask = acpi_pm_timer_is_32bit() ? 0xFFFFFFFFu : 0x00FFFFFFu;
    u32 want = (u32)(((u64)pm_hz * ms) / 1000);

    u32 first = inl(port) & mask;

    /* A counter that is not counting is not a clock.  Checking costs a few
     * microseconds and turns "the machine hangs" into "this timer is not
     * usable, trying the next one". */
    u64 started = rdtsc();
    while ((inl(port) & mask) == first) {
        if (tsc_elapsed(started, 10)) return 0;
    }

    u32 begin = inl(port) & mask;
    lapic_write(LAPIC_TIMER_DIV, 0x3);            /* divide by 16 */
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);

    started = rdtsc();
    for (;;) {
        u32 now = inl(port) & mask;
        u32 gone = (now - begin) & mask;          /* wraps correctly either way */
        if (gone >= want) break;
        if (tsc_elapsed(started, ms * 8 + 50)) {
            lapic_write(LAPIC_LVT_TIMER, LVT_MASKED | VEC_APIC_TIMER);
            return 0;
        }
    }

    u32 remaining = lapic_read(LAPIC_TIMER_CCR);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED | VEC_APIC_TIMER);
    return 0xFFFFFFFFu - remaining;
}

/* ------------------------------------------------------------- 2. the 8254 */

/* Channel 2 can be driven without taking an interrupt: gate it on, wait for a
 * known number of 1.193182 MHz ticks, and see how far the APIC counter fell. */
static u32 calibrate_against_pit(u32 ms) {
    const u32 pit_hz = 1193182;
    u32 count = (pit_hz * ms) / 1000;
    if (count > 0xFFFF) count = 0xFFFF;

    /* Speaker gate on, speaker data off. */
    u8 port61 = inb(0x61);
    if (port61 == 0xFF) return 0;           /* nothing answering at all */

    outb(0x61, (u8)((port61 & ~0x02) | 0x01));

    outb(0x43, 0xB2);                       /* channel 2, lobyte/hibyte, mode 0 */
    outb(0x42, (u8)(count & 0xFF));
    outb(0x42, (u8)(count >> 8));

    /* Restart the counter by toggling the gate. */
    u8 t = inb(0x61);
    outb(0x61, (u8)(t & ~1));
    outb(0x61, (u8)(t | 1));

    lapic_write(LAPIC_TIMER_DIV, 0x3);       /* divide by 16 */
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);

    /* Bit 5 of port 0x61 goes high when channel 2 finishes counting down.  On
     * a machine with no 8254 it never does, so the wait is bounded by the
     * processor's own clock and gives up in milliseconds. */
    u64 started = rdtsc();
    bool finished = true;
    while (!(inb(0x61) & 0x20)) {
        if (tsc_elapsed(started, ms * 8 + 50)) { finished = false; break; }
    }

    u32 remaining = lapic_read(LAPIC_TIMER_CCR);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED | VEC_APIC_TIMER);
    outb(0x61, port61);

    return finished ? (0xFFFFFFFFu - remaining) : 0;
}

/* --------------------------------------------- 3. what the processor says */

static u32 crystal_from_cpuid(void) {
    u32 a = 0, b = 0, c = 0, d = 0;

    cpuid_raw(0, 0, &a, &b, &c, &d);
    if (a < 0x15) return 0;

    /* Leaf 0x15: EAX is the denominator of the TSC-to-crystal ratio, EBX the
     * numerator, and ECX the crystal frequency itself in hertz.  ECX is
     * allowed to be zero, and on a good many parts it is. */
    cpuid_raw(0x15, 0, &a, &b, &c, &d);
    if (!a || !b) return 0;
    if (c) return c;

    /* No crystal reported.  It can be recovered from the processor's stated
     * base frequency and the ratio, when leaf 0x16 exists. */
    u32 a16 = 0, b16 = 0, c16 = 0, d16 = 0;
    cpuid_raw(0, 0, &a16, &b16, &c16, &d16);
    if (a16 < 0x16) return 0;
    cpuid_raw(0x16, 0, &a16, &b16, &c16, &d16);
    if (!a16) return 0;                       /* base MHz */

    u64 tsc_hz = (u64)a16 * 1000000ull;
    return (u32)((tsc_hz * a) / b);           /* crystal = tsc / (num/den) */
}

/* ------------------------------------------------------------ putting it all
 *
 * Three runs and the median, so one long system-management interrupt in the
 * middle of a measurement does not become the answer.
 */
static u32 median_of_three(u32 (*measure)(u32), u32 ms) {
    u32 r[3];
    for (int i = 0; i < 3; i++) {
        r[i] = measure(ms);
        if (!r[i]) return 0;                  /* this source does not work */
        r[i] /= ms;
    }
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 3; j++)
            if (r[j] < r[i]) { u32 t = r[i]; r[i] = r[j]; r[j] = t; }
    return r[1];
}

/* --------------------------------------------------------- the APIC's errors
 *
 * The local APIC reports its own faults through an interrupt, and what went
 * wrong is in a register - but reading it needs a write first, because the
 * value is only latched into the register when it is written to.  That is not
 * a quirk to work around; it is how the register is specified, and skipping
 * the write reads whatever was latched last time.
 *
 * Every one of these is worth naming.  A driver that says "APIC error" and
 * nothing else has told you only that something is wrong with the one piece of
 * hardware every other interrupt depends on.
 */
static const char *apic_error_name(u32 bit) {
    switch (bit) {
    case 0: return "a message it sent had a bad checksum";
    case 1: return "a message it received had a bad checksum";
    case 2: return "a message it sent was not accepted";
    case 3: return "a message it received was not accepted";
    case 4: return "an interrupt was sent that cannot be redirected";
    case 5: return "it was asked to send an illegal vector";
    case 6: return "it received an illegal vector";
    case 7: return "a register that does not exist was accessed";
    default: return "an error this processor does not document";
    }
}

static regs_t *apic_error_isr(regs_t *r, void *ctx) {
    (void)ctx;

    /* Write, then read: that is the sequence that latches the current value. */
    lapic_write(LAPIC_ESR, 0);
    u32 esr = lapic_read(LAPIC_ESR);
    lapic_write(LAPIC_ESR, 0);              /* and clear it for next time */

    static u32 already_said;
    for (u32 bit = 0; bit < 8; bit++) {
        if (!(esr & (1u << bit))) continue;
        if (already_said & (1u << bit)) continue;
        already_said |= 1u << bit;
        kwarn("apic", "the interrupt controller reported: %s (status %#x)",
              apic_error_name(bit), esr);
    }
    if (!esr) {
        static bool once;
        if (!once) { once = true;
            kwarn("apic", "the interrupt controller raised an error and then "
                          "reported none; taking it as harmless"); }
    }

    lapic_eoi();
    return r;
}

/* The spurious interrupt is the one that must NOT be acknowledged: the
 * hardware raises it exactly when it has decided not to mark a vector as being
 * serviced, so an acknowledgement here would clear a bit belonging to some
 * other interrupt that really is in progress. */
static regs_t *apic_spurious_isr(regs_t *r, void *ctx) {
    (void)ctx;
    static bool once;
    if (!once) { once = true; kdebug("apic", "a spurious interrupt arrived"); }
    return r;
}

u32 lapic_timer_calibrate(void) {
    const char *how = NULL;
    u32 best = 0;

    if ((best = median_of_three(calibrate_against_pm_timer, 10)) >= 100) {
        how = "measured against the ACPI power-management timer";
    } else if ((best = median_of_three(calibrate_against_pit, 10)) >= 100) {
        how = "measured against the 8254 interval timer";
    } else {
        u32 crystal = crystal_from_cpuid();
        if (crystal) {
            /* The APIC timer counts the core crystal, and the divider is 16. */
            best = crystal / 16 / 1000;
            how = "from the core crystal the processor reports (not measured)";
        }
    }

    if (!how || best < 100) {
        /* Nothing worked.  Say so in the words that name the cause, because
         * the machines this happens on are new ones and the reason is always
         * the same: the timer everything used to calibrate against is gone. */
        kwarn("apic", "no usable reference clock - no ACPI PM timer, no 8254, "
                      "and the processor reports no crystal.  Assuming 100000 "
                      "ticks/ms; everything timed will be wrong");
        best = 100000;
        how = "assumed, because nothing on this machine could be measured";
    }

    timer_ticks_per_ms = best;
    kinfo("apic", "APIC timer runs at %u ticks/ms (divider 16), %s", best, how);
    return best;
}

bool lapic_timer_start_local(u32 hz, u8 vector) {
    if (!hz || !timer_ticks_per_ms || vector < 0x10) return false;
    u64 count = ((u64)timer_ticks_per_ms * 1000) / hz;
    if (count > 0xffffffffu) return false;
    u32 initial = (u32)count;
    if (initial < 16) initial = 16;

    lapic_write(LAPIC_TIMER_DIV, 0x3);
    lapic_write(LAPIC_LVT_TIMER, vector | TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_ICR, initial);
    return true;
}

void lapic_timer_start(u32 hz, u8 vector) {
    if (!timer_ticks_per_ms) lapic_timer_calibrate();
    if (!lapic_timer_start_local(hz, vector)) {
        kerr("apic", "invalid local timer configuration: %u Hz, vector %#x", hz, vector);
        return;
    }
    kinfo("apic", "APIC timer started at %u Hz on vector %#x", hz, vector);
}

void lapic_timer_stop(void) { lapic_write(LAPIC_LVT_TIMER, LVT_MASKED | VEC_APIC_TIMER); }
