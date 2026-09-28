#ifndef KESTREL_APIC_H
#define KESTREL_APIC_H

#include "kernel.h"

void apic_init(void);
bool apic_available(void);
/* AP-only local setup. Device vectors stay blocked while the processor is a
 * restricted worker; only vectors in priority class 0xf are admitted. */
bool lapic_init_worker(void);
void lapic_eoi(void);
u32  lapic_id(void);

/* One inter-processor message, sent whichever way this machine's local
 * APIC is being reached.  See the note in apic.c: the two ways are not
 * interchangeable and the wrong one fails silently. */
void lapic_send_ipi(u32 apic_id, u32 command);

u32  lapic_timer_calibrate(void);       /* ticks per millisecond */
void lapic_timer_start(u32 hz, u8 vector);
/* Uses the BSP's completed calibration, without recalibration or logging.
 * Must only be called on the processor whose timer is being configured. */
bool lapic_timer_start_local(u32 hz, u8 vector);
void lapic_timer_stop(void);

bool ioapic_route(u32 gsi, u8 vector, bool active_low, bool level, u32 dest_apic);
void ioapic_mask(u32 gsi, bool masked);

#endif
