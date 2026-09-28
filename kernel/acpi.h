#ifndef KESTREL_ACPI_H
#define KESTREL_ACPI_H

#include "kernel.h"

typedef struct __attribute__((packed)) {
    char signature[4];
    u32  length;
    u8   revision;
    u8   checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32  oem_revision;
    u32  creator_id;
    u32  creator_revision;
} acpi_sdt_t;

/* MADT (APIC) entry types. */
enum {
    MADT_LAPIC          = 0,
    MADT_IOAPIC         = 1,
    MADT_INT_OVERRIDE   = 2,
    MADT_NMI_SOURCE     = 3,
    MADT_LAPIC_NMI      = 4,
    MADT_LAPIC_OVERRIDE = 5,
    MADT_X2APIC         = 9,
};

typedef struct {
    u8  id;
    u8  apic_id;
    u32 flags;
} acpi_cpu_t;

typedef struct {
    u8  id;
    u64 address;
    u32 gsi_base;
    u32 gsi_count;
} acpi_ioapic_t;

typedef struct {
    u8  source_irq;
    u32 gsi;
    u16 flags;          /* bits 0-1 polarity, 2-3 trigger mode */
} acpi_override_t;

/* PCIe configuration space window from MCFG. */
typedef struct {
    u64 base;
    u16 segment;
    u8  start_bus, end_bus;
} acpi_mcfg_t;

#define ACPI_MAX_CPUS      64
#define ACPI_MAX_IOAPICS   8
#define ACPI_MAX_OVERRIDES 32
#define ACPI_MAX_MCFG      8

void acpi_init(void);
acpi_sdt_t *acpi_find_table(const char *sig);

extern acpi_cpu_t      g_acpi_cpus[ACPI_MAX_CPUS];
extern int             g_acpi_cpu_count;
extern acpi_ioapic_t   g_acpi_ioapics[ACPI_MAX_IOAPICS];
extern int             g_acpi_ioapic_count;
extern acpi_override_t g_acpi_overrides[ACPI_MAX_OVERRIDES];
extern int             g_acpi_override_count;
extern acpi_mcfg_t     g_acpi_mcfg[ACPI_MAX_MCFG];
extern int             g_acpi_mcfg_count;
extern u64             g_lapic_phys;

/* Translate a legacy ISA IRQ into a global system interrupt, applying any
 * MADT interrupt source override. */
u32  acpi_irq_to_gsi(u8 irq, bool *active_low, bool *level);

__attribute__((noreturn)) void acpi_poweroff(void);
__attribute__((noreturn)) void acpi_reboot(void);

/* The ACPI power-management timer: a fixed 3.579545 MHz counter, present on
 * every machine with ACPI.  Zero if this one has none. */
u32  acpi_pm_timer_port(void);
bool acpi_pm_timer_is_32bit(void);

/* False only when a valid modern FADT explicitly excludes the PS/2 controller. */
bool acpi_has_8042(void);

#endif
