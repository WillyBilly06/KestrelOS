#ifndef KESTREL_PCI_H
#define KESTREL_PCI_H

#include "kernel.h"

typedef struct pci_dev {
    u16 segment;
    u8  bus, slot, func;
    u16 vendor, device;
    u8  class_code, subclass, prog_if, revision;
    u16 subsys_vendor, subsys_device;
    u8  header_type;
    u8  irq_line, irq_pin;

    /* Decoded base address registers. */
    u64  bar[6];
    u64  bar_size[6];
    bool bar_is_io[6];
    bool bar_is_64[6];

    /* Capability offsets, 0 when the capability is absent. */
    u8 cap_msi, cap_msix, cap_pcie, cap_power;

    /* The driver that took this device on, or NULL because nothing did.
     *
     * Set at the point a driver commits to a device rather than where it finds
     * one: a driver that recognises a card and then gives up - no usable BAR,
     * a model it does not know - has not driven it, and saying otherwise would
     * hide exactly the gap this field exists to show. */
    const char *driver;

    struct pci_dev *next;
} pci_dev_t;

void pci_init(void);
pci_dev_t *pci_first(void);
pci_dev_t *pci_find(u8 class_code, u8 subclass, u8 prog_if, pci_dev_t *after);
pci_dev_t *pci_find_id(u16 vendor, u16 device, pci_dev_t *after);
int  pci_device_count(void);

u32  pci_read32(pci_dev_t *d, u32 offset);
u16  pci_read16(pci_dev_t *d, u32 offset);
u8   pci_read8(pci_dev_t *d, u32 offset);
void pci_write32(pci_dev_t *d, u32 offset, u32 val);
void pci_write16(pci_dev_t *d, u32 offset, u16 val);
void pci_write8(pci_dev_t *d, u32 offset, u8 val);

void pci_enable_bus_master(pci_dev_t *d);
/* Silence a device's interrupts, for a driver that polls it instead. */
void pci_disable_interrupts(pci_dev_t *d);
void pci_enable_memory(pci_dev_t *d);

/* Route this device's interrupt to `vector`.  Prefers MSI-X, then MSI, then the
 * legacy INTx line through the I/O APIC.  Returns false when nothing worked. */
bool pci_setup_interrupt(pci_dev_t *d, u8 vector);
/* Configure exactly one MSI vector.  Large devices such as GPUs expose MSI-X
 * tables with several vectors, but a driver that owns one RM ISR must not
 * advertise a partially configured MSI-X topology. */
bool pci_setup_single_msi(pci_dev_t *d, u8 vector);

const char *pci_class_name(u8 class_code, u8 subclass);
const char *pci_vendor_name(u16 vendor);
void pci_describe(pci_dev_t *d, char *buf, size_t cap);

/* A driver saying "this one is mine". */
void pci_claim(pci_dev_t *d, const char *driver);
/* Every device, and what drives it - written to the log at startup. */
void pci_report_coverage(void);
int  pci_unclaimed_count(void);

#endif
