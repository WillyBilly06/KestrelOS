/* acpi.c - ACPI table discovery.
 *
 * Enough of ACPI to find the CPUs, the I/O APICs, the PCIe configuration
 * window, and the registers needed to power the machine off or reset it.
 * There is no AML interpreter here; the sleep type for S5 is recovered by
 * locating the \_S5_ package in the DSDT and reading the two small integers
 * out of it, which is all a power-off actually needs.
 */
#include "kernel.h"
#include "acpi.h"
#include "mm.h"
#include "klog.h"

acpi_cpu_t      g_acpi_cpus[ACPI_MAX_CPUS];
int             g_acpi_cpu_count;
acpi_ioapic_t   g_acpi_ioapics[ACPI_MAX_IOAPICS];
int             g_acpi_ioapic_count;
acpi_override_t g_acpi_overrides[ACPI_MAX_OVERRIDES];
int             g_acpi_override_count;
acpi_mcfg_t     g_acpi_mcfg[ACPI_MAX_MCFG];
int             g_acpi_mcfg_count;
u64             g_lapic_phys = 0xFEE00000;

/* Table pointers gathered from the RSDT/XSDT. */
#define MAX_TABLES 64
static acpi_sdt_t *tables[MAX_TABLES];
static int         table_count;

/* Power management registers from the FADT. */
static u32 pm1a_cnt, pm1b_cnt;
static u16 slp_typ_a, slp_typ_b;
static bool s5_known;
static u8  reset_space_id;
static u64 reset_addr;
static u8  reset_value;
static bool reset_known;

static bool checksum_ok(const void *p, size_t len) {
    const u8 *b = p;
    u8 sum = 0;
    for (size_t i = 0; i < len; i++) sum = (u8)(sum + b[i]);
    return sum == 0;
}

acpi_sdt_t *acpi_find_table(const char *sig) {
    for (int i = 0; i < table_count; i++)
        if (!memcmp(tables[i]->signature, sig, 4)) return tables[i];
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* MADT                                                                      */
/* ------------------------------------------------------------------------- */

static void parse_madt(acpi_sdt_t *madt) {
    const u8 *p = (const u8 *)madt + sizeof(acpi_sdt_t);
    const u8 *end = (const u8 *)madt + madt->length;

    g_lapic_phys = *(const u32 *)p;
    u32 flags = *(const u32 *)(p + 4);
    p += 8;

    if (flags & 1) {
        /* The 8259s are wired up; mask them so they cannot inject interrupts
         * once we switch to the APIC. */
        outb(0x21, 0xFF);
        outb(0xA1, 0xFF);
    }

    while (p + 2 <= end) {
        u8 type = p[0], len = p[1];
        if (len < 2 || p + len > end) break;

        switch (type) {
        case MADT_LAPIC:
            if (g_acpi_cpu_count < ACPI_MAX_CPUS && len >= 8) {
                g_acpi_cpus[g_acpi_cpu_count].id      = p[2];
                g_acpi_cpus[g_acpi_cpu_count].apic_id = p[3];
                g_acpi_cpus[g_acpi_cpu_count].flags   = *(const u32 *)(p + 4);
                g_acpi_cpu_count++;
            }
            break;

        case MADT_IOAPIC:
            if (g_acpi_ioapic_count < ACPI_MAX_IOAPICS && len >= 12) {
                g_acpi_ioapics[g_acpi_ioapic_count].id       = p[2];
                g_acpi_ioapics[g_acpi_ioapic_count].address  = *(const u32 *)(p + 4);
                g_acpi_ioapics[g_acpi_ioapic_count].gsi_base = *(const u32 *)(p + 8);
                g_acpi_ioapics[g_acpi_ioapic_count].gsi_count = 0;  /* filled by apic_init */
                g_acpi_ioapic_count++;
            }
            break;

        case MADT_INT_OVERRIDE:
            if (g_acpi_override_count < ACPI_MAX_OVERRIDES && len >= 10) {
                g_acpi_overrides[g_acpi_override_count].source_irq = p[3];
                g_acpi_overrides[g_acpi_override_count].gsi        = *(const u32 *)(p + 4);
                g_acpi_overrides[g_acpi_override_count].flags      = *(const u16 *)(p + 8);
                g_acpi_override_count++;
            }
            break;

        case MADT_LAPIC_OVERRIDE:
            if (len >= 12) g_lapic_phys = *(const u64 *)(p + 4);
            break;

        default:
            break;
        }
        p += len;
    }

    kinfo("acpi", "MADT: %d CPU(s), %d I/O APIC(s), %d IRQ override(s), LAPIC at %p",
          g_acpi_cpu_count, g_acpi_ioapic_count, g_acpi_override_count, (void *)g_lapic_phys);
}

u32 acpi_irq_to_gsi(u8 irq, bool *active_low, bool *level) {
    /* ISA defaults: active high, edge triggered. */
    if (active_low) *active_low = false;
    if (level) *level = false;

    for (int i = 0; i < g_acpi_override_count; i++) {
        if (g_acpi_overrides[i].source_irq != irq) continue;
        u16 f = g_acpi_overrides[i].flags;
        if (active_low) *active_low = ((f & 3) == 3);
        if (level)      *level      = (((f >> 2) & 3) == 3);
        return g_acpi_overrides[i].gsi;
    }
    return irq;
}

/* ------------------------------------------------------------------------- */
/* MCFG                                                                      */
/* ------------------------------------------------------------------------- */

static void parse_mcfg(acpi_sdt_t *mcfg) {
    const u8 *p = (const u8 *)mcfg + sizeof(acpi_sdt_t) + 8;   /* 8 reserved bytes */
    const u8 *end = (const u8 *)mcfg + mcfg->length;

    while (p + 16 <= end && g_acpi_mcfg_count < ACPI_MAX_MCFG) {
        g_acpi_mcfg[g_acpi_mcfg_count].base      = *(const u64 *)p;
        g_acpi_mcfg[g_acpi_mcfg_count].segment   = *(const u16 *)(p + 8);
        g_acpi_mcfg[g_acpi_mcfg_count].start_bus = p[10];
        g_acpi_mcfg[g_acpi_mcfg_count].end_bus   = p[11];
        kinfo("acpi", "MCFG: segment %u buses %u-%u at %p",
              g_acpi_mcfg[g_acpi_mcfg_count].segment,
              g_acpi_mcfg[g_acpi_mcfg_count].start_bus,
              g_acpi_mcfg[g_acpi_mcfg_count].end_bus,
              (void *)g_acpi_mcfg[g_acpi_mcfg_count].base);
        g_acpi_mcfg_count++;
        p += 16;
    }
}

/* ------------------------------------------------------------------------- */
/* FADT and S5                                                               */
/* ------------------------------------------------------------------------- */

/* Locate \_S5_ in the DSDT and pull out the two sleep-type values.  The object
 * is a Name whose value is a package of small integers, so the byte pattern is
 * short and unambiguous enough to find without a full AML parser. */
static void find_s5(const u8 *aml, size_t len) {
    for (size_t i = 0; i + 8 < len; i++) {
        if (memcmp(aml + i, "_S5_", 4)) continue;

        const u8 *p = aml + i + 4;
        /* Optional NameOp (0x08) may precede the name; skip forward to PackageOp. */
        if (*p != 0x12) continue;                 /* PackageOp */
        p++;
        p += ((*p & 0xC0) >> 6) + 1;              /* PkgLength: lead byte + extra bytes */
        if (*p < 2) continue;                     /* element count */
        p++;

        /* Each element is a byte prefix (0x0A) followed by a value, or one of
         * the constant opcodes Zero/One. */
        for (int elem = 0; elem < 2; elem++) {
            u16 v;
            if (*p == 0x0A)      { p++; v = *p++; }
            else if (*p == 0x00) { p++; v = 0; }
            else if (*p == 0x01) { p++; v = 1; }
            else if (*p == 0x0B) { p++; v = (u16)(p[0] | (p[1] << 8)); p += 2; }
            else break;
            if (elem == 0) slp_typ_a = v; else slp_typ_b = v;
        }
        s5_known = true;
        kinfo("acpi", "S5 sleep types: a=%u b=%u", slp_typ_a, slp_typ_b);
        return;
    }
    kwarn("acpi", "no \\_S5_ object found; software power-off is unavailable");
}

/* The ACPI power-management timer.  A free-running counter that ticks at
 * exactly 3.579545 MHz - a third of the old colour-television subcarrier, for
 * reasons that stopped being good in about 1985 - and is present on every
 * machine with ACPI, which by now is every machine.
 *
 * It matters here because the thing it replaces is not.  Calibrating a timer
 * needs a second timer of known rate to calibrate against, and the one every
 * PC used for forty years was the 8254 programmable interval timer at port
 * 0x40.  Intel has been removing it: on the Core Ultra parts it is simply not
 * there, and code that waits for it waits forever.
 *
 * Reported in a generic address structure at offset 208 of the FADT on any
 * revision that is long enough, and as a plain port address at offset 76 on
 * every revision.  Zero means the machine does not have one.
 */
static u32 pm_timer_port;
static bool pm_timer_32bit;

u32  acpi_pm_timer_port(void) { return pm_timer_port; }
bool acpi_pm_timer_is_32bit(void) { return pm_timer_32bit; }

bool acpi_has_8042(void) {
    acpi_sdt_t *fadt = acpi_find_table("FACP");
    /* ACPI 1.0 reserved these bytes. Unknown/invalid firmware preserves the
     * legacy probe; only a valid ACPI 2.0+ table can explicitly rule it out.
     * ACPI 6.6 section 5.2.9.3: IAPC_BOOT_ARCH[1], at byte offset 109. */
    if (!fadt || fadt->revision < 3 || fadt->length < 111 ||
        !checksum_ok(fadt, fadt->length)) return true;
    return (((const u8 *)fadt)[109] & 2u) != 0;
}

static void parse_fadt(acpi_sdt_t *fadt) {
    const u8 *f = (const u8 *)fadt;

    u32 dsdt32 = *(const u32 *)(f + 40);
    pm1a_cnt = *(const u32 *)(f + 64);
    pm1b_cnt = *(const u32 *)(f + 68);

    /* PM_TMR_BLK, and the flag saying whether it counts to 24 bits or 32. */
    if (fadt->length >= 80) pm_timer_port = *(const u32 *)(f + 76);
    if (fadt->length >= 116) {
        u32 flags = *(const u32 *)(f + 112);
        pm_timer_32bit = (flags & (1u << 8)) != 0;
    }
    if (fadt->length >= 220) {
        /* X_PM_TMR_BLK: a 64-bit address, used in preference when it is set
         * and lives in I/O space, which it does on every machine so far. */
        u8  space = f[208];
        u64 addr  = *(const u64 *)(f + 208 + 4);
        if (addr && space == 1) pm_timer_port = (u32)addr;
    }

    u64 dsdt = dsdt32;
    if (fadt->length >= 148) {
        u64 x_dsdt = *(const u64 *)(f + 140);
        if (x_dsdt) dsdt = x_dsdt;
    }
    if (fadt->length >= 129) {
        /* RESET_REG is a generic address structure at offset 116. */
        reset_space_id = f[116];
        reset_addr     = *(const u64 *)(f + 116 + 4);
        reset_value    = f[128];
        u32 flags      = *(const u32 *)(f + 112);
        reset_known    = (flags & (1u << 10)) != 0 && reset_addr != 0;
    }

    if (dsdt) {
        acpi_sdt_t *d = phys_to_virt(dsdt);
        if (!memcmp(d->signature, "DSDT", 4) && d->length > sizeof(acpi_sdt_t))
            find_s5((const u8 *)d + sizeof(acpi_sdt_t), d->length - sizeof(acpi_sdt_t));
        else
            kwarn("acpi", "DSDT at %p does not look valid", (void *)dsdt);
    }
}

/* ------------------------------------------------------------------------- */
/* init                                                                      */
/* ------------------------------------------------------------------------- */

void acpi_init(void) {
    if (!g_boot.rsdp) {
        kwarn("acpi", "firmware provided no RSDP; falling back to legacy defaults");
        return;
    }

    const u8 *rsdp = phys_to_virt(g_boot.rsdp);
    if (memcmp(rsdp, "RSD PTR ", 8)) { kerr("acpi", "RSDP signature is wrong"); return; }
    if (!checksum_ok(rsdp, 20))      { kerr("acpi", "RSDP checksum failed"); return; }

    u8 revision = rsdp[15];
    u64 root = 0;
    bool xsdt = false;

    if (revision >= 2 && checksum_ok(rsdp, 36)) {
        root = *(const u64 *)(rsdp + 24);
        xsdt = true;
    }
    if (!root) { root = *(const u32 *)(rsdp + 16); xsdt = false; }
    if (!root) { kerr("acpi", "RSDP points at no root table"); return; }

    acpi_sdt_t *rt = phys_to_virt(root);
    if (!checksum_ok(rt, rt->length)) kwarn("acpi", "%s checksum failed; continuing anyway", xsdt ? "XSDT" : "RSDT");

    int entries = (int)((rt->length - sizeof(acpi_sdt_t)) / (xsdt ? 8 : 4));
    const u8 *ptr = (const u8 *)rt + sizeof(acpi_sdt_t);

    for (int i = 0; i < entries && table_count < MAX_TABLES; i++) {
        u64 addr = xsdt ? ((const u64 *)ptr)[i] : ((const u32 *)ptr)[i];
        if (!addr) continue;
        acpi_sdt_t *t = phys_to_virt(addr);
        if (t->length < sizeof(acpi_sdt_t) || t->length > (16u << 20)) {
            kwarn("acpi", "table %d has an implausible length %u; skipped", i, t->length);
            continue;
        }
        tables[table_count++] = t;
    }

    kinfo("acpi", "revision %u, %s with %d table(s)", revision, xsdt ? "XSDT" : "RSDT", table_count);

    acpi_sdt_t *t;
    if ((t = acpi_find_table("APIC"))) parse_madt(t);
    else kwarn("acpi", "no MADT: interrupt routing will fall back to the 8259 PIC");

    if ((t = acpi_find_table("MCFG"))) parse_mcfg(t);
    if ((t = acpi_find_table("FACP"))) parse_fadt(t);
    else kwarn("acpi", "no FADT: power management is unavailable");
}

/* ------------------------------------------------------------------------- */
/* power                                                                     */
/* ------------------------------------------------------------------------- */

void acpi_poweroff(void) {
    kinfo("acpi", "powering off");
    klog_persist_flush();
    cli();

    if (s5_known && pm1a_cnt) {
        /* SLP_TYP in bits 10-12, SLP_EN in bit 13. */
        outw((u16)pm1a_cnt, (u16)((slp_typ_a << 10) | (1 << 13)));
        if (pm1b_cnt) outw((u16)pm1b_cnt, (u16)((slp_typ_b << 10) | (1 << 13)));
        for (volatile int i = 0; i < 1000000; i++) { }
    }

    /* Hypervisor shutdown ports, harmless on real hardware. */
    outw(0x604, 0x2000);    /* QEMU / modern VMware  */
    outw(0xB004, 0x2000);   /* Bochs, older QEMU     */
    outw(0x4004, 0x3400);   /* VirtualBox            */

    kwarn("acpi", "power-off did not take effect; halting instead");
    console_set_color(C_LGRAY, C_BLACK);
    kprintf("\nIt is now safe to turn off the computer.\n");
    for (;;) { cli(); hlt(); }
}

void acpi_reboot(void) {
    kinfo("acpi", "rebooting");
    klog_persist_flush();
    cli();

    if (reset_known) {
        if (reset_space_id == 1) outb((u16)reset_addr, reset_value);          /* system I/O */
        else if (reset_space_id == 0) *(volatile u8 *)phys_to_virt(reset_addr) = reset_value;
        for (volatile int i = 0; i < 1000000; i++) { }
    }

    /* PCI reset control register. */
    outb(0xCF9, 0x02);
    outb(0xCF9, 0x06);
    for (volatile int i = 0; i < 1000000; i++) { }

    /* Pulse the keyboard controller's reset line. */
    for (int i = 0; i < 100 && (inb(0x64) & 2); i++) io_wait();
    outb(0x64, 0xFE);
    for (volatile int i = 0; i < 1000000; i++) { }

    /* Last resort: a triple fault via a null IDT. */
    struct __attribute__((packed)) { u16 limit; u64 base; } null_idt = { 0, 0 };
    __asm__ volatile("lidt %0; int3" :: "m"(null_idt));
    for (;;) { cli(); hlt(); }
}
