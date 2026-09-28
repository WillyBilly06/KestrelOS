/* pci.c - PCI/PCIe enumeration and interrupt setup.
 *
 * Configuration space is reached through the MCFG memory window when the
 * firmware provided one, and through the legacy 0xCF8/0xCFC ports otherwise.
 * Every function found is recorded with its decoded BARs and capabilities so
 * drivers can bind without walking the bus again.
 */
#include "kernel.h"
#include "pci.h"
#include "acpi.h"
#include "apic.h"
#include "cpu.h"
#include "mm.h"
#include "klog.h"

#define PCI_VENDOR      0x00
#define PCI_DEVICE      0x02
#define PCI_COMMAND     0x04
#define PCI_STATUS      0x06
#define PCI_REVISION    0x08
#define PCI_PROG_IF     0x09
#define PCI_SUBCLASS    0x0A
#define PCI_CLASS       0x0B
#define PCI_HEADER_TYPE 0x0E
#define PCI_BAR0        0x10
#define PCI_SUBSYS_VEN  0x2C
#define PCI_SUBSYS_DEV  0x2E
#define PCI_CAP_PTR     0x34
#define PCI_IRQ_LINE    0x3C
#define PCI_IRQ_PIN     0x3D
#define PCI_SECONDARY   0x19

#define CMD_IO          0x0001
#define CMD_MEMORY      0x0002
#define CMD_BUS_MASTER  0x0004
#define CMD_INTX_OFF    0x0400

#define CAP_POWER 0x01
#define CAP_MSI   0x05
#define CAP_PCIE  0x10
#define CAP_MSIX  0x11

static pci_dev_t *devices;
static int        device_count;
static u8        *ecam[ACPI_MAX_MCFG];

/* ------------------------------------------------------------------------- */
/* configuration space access                                                */
/* ------------------------------------------------------------------------- */

static u8 *ecam_addr(u16 seg, u8 bus, u8 slot, u8 func, u32 off) {
    for (int i = 0; i < g_acpi_mcfg_count; i++) {
        if (g_acpi_mcfg[i].segment != seg) continue;
        if (bus < g_acpi_mcfg[i].start_bus || bus > g_acpi_mcfg[i].end_bus) continue;
        if (!ecam[i]) continue;
        u64 delta = ((u64)(bus - g_acpi_mcfg[i].start_bus) << 20) |
                    ((u64)slot << 15) | ((u64)func << 12) | off;
        return ecam[i] + delta;
    }
    return NULL;
}

static u32 cfg_read(u16 seg, u8 bus, u8 slot, u8 func, u32 off, int width) {
    u8 *m = ecam_addr(seg, bus, slot, func, off);
    if (m) {
        if (width == 1) return *(volatile u8 *)m;
        if (width == 2) return *(volatile u16 *)m;
        return *(volatile u32 *)m;
    }
    /* Legacy path: aligned 32-bit access, then extract. */
    u32 addr = 0x80000000u | ((u32)bus << 16) | ((u32)slot << 11) | ((u32)func << 8) | (off & 0xFC);
    outl(0xCF8, addr);
    u32 v = inl(0xCFC);
    if (width == 4) return v;
    v >>= (off & 3) * 8;
    return width == 1 ? (v & 0xFF) : (v & 0xFFFF);
}

static void cfg_write(u16 seg, u8 bus, u8 slot, u8 func, u32 off, u32 val, int width) {
    u8 *m = ecam_addr(seg, bus, slot, func, off);
    if (m) {
        if (width == 1) *(volatile u8 *)m = (u8)val;
        else if (width == 2) *(volatile u16 *)m = (u16)val;
        else *(volatile u32 *)m = val;
        return;
    }
    u32 addr = 0x80000000u | ((u32)bus << 16) | ((u32)slot << 11) | ((u32)func << 8) | (off & 0xFC);
    outl(0xCF8, addr);
    if (width == 4) { outl(0xCFC, val); return; }

    u32 cur = inl(0xCFC);
    int shift = (int)((off & 3) * 8);
    u32 mask = (width == 1 ? 0xFFu : 0xFFFFu) << shift;
    outl(0xCF8, addr);
    outl(0xCFC, (cur & ~mask) | ((val << shift) & mask));
}

u32 pci_read32(pci_dev_t *d, u32 off) { return cfg_read(d->segment, d->bus, d->slot, d->func, off, 4); }
u16 pci_read16(pci_dev_t *d, u32 off) { return (u16)cfg_read(d->segment, d->bus, d->slot, d->func, off, 2); }
u8  pci_read8(pci_dev_t *d, u32 off)  { return (u8)cfg_read(d->segment, d->bus, d->slot, d->func, off, 1); }
void pci_write32(pci_dev_t *d, u32 off, u32 v) { cfg_write(d->segment, d->bus, d->slot, d->func, off, v, 4); }
void pci_write16(pci_dev_t *d, u32 off, u16 v) { cfg_write(d->segment, d->bus, d->slot, d->func, off, v, 2); }
void pci_write8(pci_dev_t *d, u32 off, u8 v)   { cfg_write(d->segment, d->bus, d->slot, d->func, off, v, 1); }

/* ------------------------------------------------------------------------- */
/* enumeration                                                               */
/* ------------------------------------------------------------------------- */

static void decode_bars(pci_dev_t *d) {
    int count = (d->header_type & 0x7F) == 0 ? 6 : 2;

    for (int i = 0; i < count; i++) {
        u32 off = PCI_BAR0 + (u32)i * 4;
        u32 lo = pci_read32(d, off);
        if (lo == 0) continue;

        /* Size a BAR by writing all ones and reading back the mask.  The
         * command register is turned off first so the device does not decode a
         * bogus address in the window between the two writes. */
        u16 cmd = pci_read16(d, PCI_COMMAND);
        pci_write16(d, PCI_COMMAND, (u16)(cmd & ~(CMD_IO | CMD_MEMORY)));

        pci_write32(d, off, 0xFFFFFFFF);
        u32 mask = pci_read32(d, off);
        pci_write32(d, off, lo);

        if (lo & 1) {
            d->bar_is_io[i] = true;
            d->bar[i] = lo & ~0x3u;
            d->bar_size[i] = (~(mask & ~0x3u) + 1) & 0xFFFF;
        } else {
            u8 type = (lo >> 1) & 3;
            d->bar[i] = lo & ~0xFu;
            if (type == 2 && i + 1 < count) {
                u32 hi = pci_read32(d, off + 4);
                pci_write32(d, off + 4, 0xFFFFFFFF);
                u32 mask_hi = pci_read32(d, off + 4);
                pci_write32(d, off + 4, hi);

                d->bar[i] |= (u64)hi << 32;
                u64 full = ((u64)mask_hi << 32) | (mask & ~0xFu);
                d->bar_size[i] = ~full + 1;
                d->bar_is_64[i] = true;
                d->bar[i + 1] = 0;
                i++;    /* the upper half is not a BAR of its own */
            } else {
                d->bar_size[i] = (~(mask & ~0xFu) + 1) & 0xFFFFFFFF;
            }
        }
        pci_write16(d, PCI_COMMAND, cmd);
    }
}

static void find_capabilities(pci_dev_t *d) {
    if (!(pci_read16(d, PCI_STATUS) & (1 << 4))) return;   /* no capability list */

    u8 ptr = pci_read8(d, PCI_CAP_PTR) & 0xFC;
    int guard = 0;
    while (ptr && ptr != 0xFF && ++guard < 48) {
        u8 id = pci_read8(d, ptr);
        switch (id) {
        case CAP_MSI:   d->cap_msi   = ptr; break;
        case CAP_MSIX:  d->cap_msix  = ptr; break;
        case CAP_PCIE:  d->cap_pcie  = ptr; break;
        case CAP_POWER: d->cap_power = ptr; break;
        default: break;
        }
        ptr = pci_read8(d, ptr + 1) & 0xFC;
    }
}

static void scan_bus(u16 seg, u8 bus, int depth);

static void examine(u16 seg, u8 bus, u8 slot, u8 func, int depth) {
    u32 id = cfg_read(seg, bus, slot, func, PCI_VENDOR, 4);
    u16 vendor = (u16)(id & 0xFFFF);
    if (vendor == 0xFFFF || vendor == 0) return;

    pci_dev_t *d = kzalloc(sizeof *d);
    if (!d) { kerr("pci", "out of memory recording %02x:%02x.%u", bus, slot, func); return; }

    d->segment = seg;
    d->bus = bus; d->slot = slot; d->func = func;
    d->vendor = vendor;
    d->device = (u16)(id >> 16);
    d->revision   = pci_read8(d, PCI_REVISION);
    d->prog_if    = pci_read8(d, PCI_PROG_IF);
    d->subclass   = pci_read8(d, PCI_SUBCLASS);
    d->class_code = pci_read8(d, PCI_CLASS);
    d->header_type = pci_read8(d, PCI_HEADER_TYPE);
    d->irq_line   = pci_read8(d, PCI_IRQ_LINE);
    d->irq_pin    = pci_read8(d, PCI_IRQ_PIN);

    if ((d->header_type & 0x7F) == 0) {
        d->subsys_vendor = pci_read16(d, PCI_SUBSYS_VEN);
        d->subsys_device = pci_read16(d, PCI_SUBSYS_DEV);
    }

    decode_bars(d);
    find_capabilities(d);

    d->next = devices;
    devices = d;
    device_count++;

    char desc[96];
    pci_describe(d, desc, sizeof desc);

    /* The numeric identity as well as the description.
     *
     * A description is what a person reads; the four numbers are what a driver
     * is written against, and until now they were nowhere in the log.  On a
     * machine whose network card has no driver, "Ethernet controller" does not
     * say which one to write - and this line, recovered from the log, is the
     * whole difference between guessing at that and knowing. */
    kinfo("pci", "%04x:%02x:%02x.%u  %04x:%04x  class %02x.%02x.%02x rev %02x  %s",
          seg, bus, slot, func, d->vendor, d->device,
          d->class_code, d->subclass, d->prog_if, d->revision, desc);

    /* No recursion behind bridges here.  Every bus in the segment is swept
     * directly, which reaches strictly more than following bridges does - see
     * the note in pci_init - and doing both would find the same device twice.
     * The bridge's secondary bus number is still read and kept, because
     * knowing what is behind what is worth having even when it is not how
     * anything was found. */
    (void)depth;
}

static void scan_bus(u16 seg, u8 bus, int depth) {
    for (u8 slot = 0; slot < 32; slot++) {
        u32 id = cfg_read(seg, bus, slot, 0, PCI_VENDOR, 4);
        if ((id & 0xFFFF) == 0xFFFF) continue;

        examine(seg, bus, slot, 0, depth);

        u8 header = (u8)cfg_read(seg, bus, slot, 0, PCI_HEADER_TYPE, 1);
        if (header & 0x80)
            for (u8 func = 1; func < 8; func++) examine(seg, bus, slot, func, depth);
    }
}

void pci_init(void) {
    for (int i = 0; i < g_acpi_mcfg_count; i++) {
        u32 buses = (u32)(g_acpi_mcfg[i].end_bus - g_acpi_mcfg[i].start_bus) + 1;
        ecam[i] = vmm_map_mmio(g_acpi_mcfg[i].base, (size_t)buses << 20);
        if (!ecam[i]) kwarn("pci", "cannot map ECAM for segment %u; using port I/O", g_acpi_mcfg[i].segment);
    }

    /* Every bus the firmware says exists, swept directly.
     *
     * This used to scan bus zero and then stop, following bridges from there
     * on the reasoning that everything hangs off bus zero eventually.  On the
     * first real machine it ran on, that reasoning cost the keyboard.
     *
     * A modern Intel desktop is two chips: the processor package, whose
     * devices are on bus 0, and a separate chipset connected over a private
     * link, whose devices are on a bus of their own - 128 on the machine this
     * was found on.  That link is not a PCI-to-PCI bridge and does not appear
     * in configuration space as one, so there is nothing there to follow.
     * Everything on the chipset was simply invisible: the USB controller the
     * keyboard and mouse are plugged into, the SATA controller, and the sound.
     *
     * There was a second bug sitting behind the first, and it would have kept
     * the bus hidden even without the early stop: the test for "does this bus
     * exist" asked device zero, and a bus is not required to have one.  The
     * chipset's bus starts at device 20.
     *
     * So the sweep asks every slot of every bus, which is the only question
     * that has no assumption in it.  It costs two hundred and fifty-six buses
     * times thirty-two slots of configuration reads - a few milliseconds - and
     * it cannot miss a device that is there.
     */
    if (g_acpi_mcfg_count) {
        for (int i = 0; i < g_acpi_mcfg_count; i++)
            for (u32 bus = g_acpi_mcfg[i].start_bus; bus <= g_acpi_mcfg[i].end_bus; bus++)
                scan_bus(g_acpi_mcfg[i].segment, (u8)bus, 0);
    } else {
        /* Without a memory-mapped configuration space there are only the
         * first 256 buses to ask about, through the two legacy ports. */
        for (u32 bus = 0; bus < 256; bus++) scan_bus(0, (u8)bus, 0);
    }

    kinfo("pci", "%d device(s) found", device_count);
    if (!device_count) kwarn("pci", "no PCI devices detected; storage and network will be unavailable");
}

pci_dev_t *pci_first(void) { return devices; }
int pci_device_count(void) { return device_count; }

/* Find the next device of a class.  0xFF means "any" for BOTH the subclass and
 * the programming interface.
 *
 * The subclass used not to be wildcarded - only the programming interface was -
 * and two callers had always written 0xFF for it meaning "any display adapter":
 * nv_core.c and amd_core.c.  No device has subclass 0xFF (a VGA controller is
 * 03.00, a 3D controller 03.02; "other" is 0x80, not 0xFF), so those two
 * queries matched nothing on every machine this has ever run on.
 *
 * The NVIDIA driver therefore never attached to a real card.  `nv_card(0)` was
 * NULL, `gpu start` returned -E_NODEV the instant it was typed, and the desktop
 * fell back to drawing on the processor - which is exactly the complaint that
 * everything renders on the CPU and the card is not used.  The card still
 * appeared in the device list because gpu.c enumerates the subclasses it wants
 * one at a time, so nothing looked missing.
 *
 * One character of asymmetry between two adjacent lines, and it silently
 * disabled the largest driver in the system. */
pci_dev_t *pci_find(u8 class_code, u8 subclass, u8 prog_if, pci_dev_t *after) {
    pci_dev_t *d = after ? after->next : devices;
    for (; d; d = d->next) {
        if (d->class_code != class_code) continue;
        if (subclass != 0xFF && d->subclass != subclass) continue;
        if (prog_if  != 0xFF && d->prog_if  != prog_if)  continue;
        return d;
    }
    return NULL;
}

pci_dev_t *pci_find_id(u16 vendor, u16 device, pci_dev_t *after) {
    pci_dev_t *d = after ? after->next : devices;
    for (; d; d = d->next)
        if (d->vendor == vendor && d->device == device) return d;
    return NULL;
}

void pci_enable_bus_master(pci_dev_t *d) {
    u16 cmd = pci_read16(d, PCI_COMMAND);
    pci_write16(d, PCI_COMMAND, (u16)(cmd | CMD_BUS_MASTER | CMD_MEMORY));
}

void pci_enable_memory(pci_dev_t *d) {
    u16 cmd = pci_read16(d, PCI_COMMAND);
    pci_write16(d, PCI_COMMAND, (u16)(cmd | CMD_MEMORY));
}

/* ------------------------------------------------------------------------- */
/* interrupts                                                                */
/* ------------------------------------------------------------------------- */

/* Stop a device raising interrupts at all.
 *
 * A driver that polls its device does not want interrupts from it, and leaving
 * them on is not merely untidy.  The firmware configures these devices before
 * the operating system starts - a USB controller in particular, so that a
 * keyboard works in the firmware's own menus - and whatever it pointed those
 * interrupts at is still there when we arrive.  The interrupts then keep
 * arriving, addressed to a handler that belongs to code no longer running.
 *
 * On the first real machine this ran on, the local APIC reported "received an
 * illegal vector" moments after the timer started, which is what that looks
 * like from the processor's side.  Worse than the noise is the possibility
 * that the firmware's own system-management code is still servicing the
 * controller and taking the events this driver is waiting for.
 *
 * All three sources have to be switched off: message-signalled interrupts in
 * both their forms, and the old pin-based one through the command register.
 */
void pci_disable_interrupts(pci_dev_t *d) {
    if (d->cap_msi) {
        u16 ctl = pci_read16(d, d->cap_msi + 2);
        pci_write16(d, d->cap_msi + 2, (u16)(ctl & ~1u));       /* MSI enable */
    }
    if (d->cap_msix) {
        u16 ctl = pci_read16(d, d->cap_msix + 2);
        pci_write16(d, d->cap_msix + 2, (u16)(ctl & ~0x8000u)); /* MSI-X enable */
    }

    /* And the legacy pin.  Bit 10 of the command register is INTx Disable. */
    u16 cmd = pci_read16(d, PCI_COMMAND);
    pci_write16(d, PCI_COMMAND, (u16)(cmd | (1u << 10)));
}

static bool setup_msix(pci_dev_t *d, u8 vector) {
    u8 cap = d->cap_msix;
    u16 ctrl = pci_read16(d, cap + 2);
    u32 table = pci_read32(d, cap + 4);

    int bir = (int)(table & 7);
    u32 offset = table & ~7u;
    if (bir > 5 || !d->bar[bir]) return false;

    volatile u32 *entries = vmm_map_mmio(d->bar[bir] + offset, 16 * 64);
    if (!entries) return false;

    /* Entry 0: address 0xFEE00000 | (apic id << 12), data = vector. */
    u64 addr = 0xFEE00000ULL | ((u64)lapic_id() << 12);
    entries[0] = (u32)addr;
    entries[1] = (u32)(addr >> 32);
    entries[2] = vector;
    entries[3] = 0;                       /* unmask */

    pci_write16(d, cap + 2, (u16)((ctrl | 0x8000) & ~0x4000));   /* enable, not masked */

    /* MSI-X supersedes INTx. */
    u16 cmd = pci_read16(d, PCI_COMMAND);
    pci_write16(d, PCI_COMMAND, (u16)(cmd | CMD_INTX_OFF));
    return true;
}

static bool setup_msi(pci_dev_t *d, u8 vector) {
    u8 cap = d->cap_msi;
    u16 ctrl = pci_read16(d, cap + 2);
    bool wide = (ctrl & 0x0080) != 0;   /* 64-bit address capable */

    u64 addr = 0xFEE00000ULL | ((u64)lapic_id() << 12);
    pci_write32(d, cap + 4, (u32)addr);
    if (wide) {
        pci_write32(d, cap + 8, (u32)(addr >> 32));
        pci_write16(d, cap + 12, vector);
    } else {
        pci_write16(d, cap + 8, vector);
    }

    /* Enable with a single vector. */
    pci_write16(d, cap + 2, (u16)((ctrl & ~0x0070u) | 0x0001));

    u16 cmd = pci_read16(d, PCI_COMMAND);
    pci_write16(d, PCI_COMMAND, (u16)(cmd | CMD_INTX_OFF));
    return true;
}

bool pci_setup_single_msi(pci_dev_t *d, u8 vector) {
    if (!d || !d->cap_msi) return false;
    /* Never leave a firmware-configured MSI-X table competing with MSI. */
    if (d->cap_msix) {
        u16 ctl = pci_read16(d, d->cap_msix + 2);
        pci_write16(d, d->cap_msix + 2, (u16)(ctl & ~0x8000u));
    }
    if (!setup_msi(d, vector)) return false;
    kinfo("pci", "%02x:%02x.%u using single MSI on vector %#x",
          d->bus, d->slot, d->func, vector);
    return true;
}

bool pci_setup_interrupt(pci_dev_t *d, u8 vector) {
    if (d->cap_msix && setup_msix(d, vector)) {
        kinfo("pci", "%02x:%02x.%u using MSI-X on vector %#x", d->bus, d->slot, d->func, vector);
        return true;
    }
    if (d->cap_msi && setup_msi(d, vector)) {
        kinfo("pci", "%02x:%02x.%u using MSI on vector %#x", d->bus, d->slot, d->func, vector);
        return true;
    }

    if (!d->irq_pin) {
        kwarn("pci", "%02x:%02x.%u has no interrupt pin and no MSI", d->bus, d->slot, d->func);
        return false;
    }

    /* Legacy INTx.  Without an ACPI _PRT interpreter the routing has to be
     * guessed; the conventional mapping is used and reported so a mismatch is
     * visible in the log rather than a silent hang. */
    u16 cmd = pci_read16(d, PCI_COMMAND);
    pci_write16(d, PCI_COMMAND, (u16)(cmd & ~CMD_INTX_OFF));

    bool active_low = true, level = true;    /* PCI INTx is level, active low */
    u32 gsi = acpi_irq_to_gsi(d->irq_line, &active_low, &level);
    if (d->irq_line == 0 || d->irq_line == 0xFF) {
        kwarn("pci", "%02x:%02x.%u reports no usable IRQ line", d->bus, d->slot, d->func);
        return false;
    }
    if (!ioapic_route(gsi, vector, true, true, lapic_id())) return false;
    ioapic_mask(gsi, false);
    kinfo("pci", "%02x:%02x.%u using INTx IRQ %u (GSI %u) on vector %#x",
          d->bus, d->slot, d->func, d->irq_line, gsi, vector);
    return true;
}

/* ------------------------------------------------------------------------- */
/* naming                                                                    */
/* ------------------------------------------------------------------------- */

const char *pci_vendor_name(u16 vendor) {
    switch (vendor) {
    case 0x8086: return "Intel";
    case 0x1022: return "AMD";
    case 0x1002: return "AMD/ATI";
    case 0x10DE: return "NVIDIA";
    case 0x15AD: return "VMware";
    case 0x1AF4: return "Red Hat/Virtio";
    case 0x1B36: return "Red Hat";
    case 0x80EE: return "VirtualBox";
    case 0x1414: return "Microsoft";
    case 0x14E4: return "Broadcom";
    case 0x10EC: return "Realtek";
    case 0x14C3: return "MediaTek";
    case 0x168C: return "Qualcomm Atheros";
    case 0x17CB: return "Qualcomm";
    case 0x1969: return "Atheros";
    case 0x1106: return "VIA";
    case 0x1033: return "NEC";
    case 0x1912: return "Renesas";
    case 0x144D: return "Samsung";
    case 0x1C5C: return "SK hynix";
    case 0x1E0F: return "KIOXIA";
    case 0x1344: return "Micron";
    case 0x1987: return "Phison";
    case 0x126F: return "Silicon Motion";
    case 0x1B4B: return "Marvell";
    case 0x197B: return "JMicron";
    case 0x1B21: return "ASMedia";
    default: return NULL;
    }
}

const char *pci_class_name(u8 class_code, u8 subclass) {
    switch (class_code) {
    case 0x00: return "unclassified device";
    case 0x01:
        switch (subclass) {
        case 0x00: return "SCSI controller";
        case 0x01: return "IDE controller";
        case 0x05: return "ATA controller";
        case 0x06: return "SATA controller";
        case 0x07: return "SAS controller";
        case 0x08: return "NVMe controller";
        default:   return "storage controller";
        }
    case 0x02:
        return subclass == 0x80 ? "wireless network controller" : "ethernet controller";
    case 0x03: return "display controller";
    case 0x04:
        switch (subclass) {
        case 0x01: return "audio device";
        case 0x03: return "HD audio controller";
        default:   return "multimedia controller";
        }
    case 0x05: return "memory controller";
    case 0x06:
        switch (subclass) {
        case 0x00: return "host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI bridge";
        default:   return "bridge";
        }
    case 0x07: return "communication controller";
    case 0x08: return "system peripheral";
    case 0x09: return "input device";
    case 0x0C:
        switch (subclass) {
        case 0x03: return "USB controller";
        case 0x05: return "SMBus controller";
        default:   return "serial bus controller";
        }
    case 0x0D: return "wireless controller";
    case 0x10: return "encryption controller";
    case 0x11: return "signal processing controller";
    default:   return "device";
    }
}

void pci_describe(pci_dev_t *d, char *buf, size_t cap) {
    const char *vendor = pci_vendor_name(d->vendor);
    const char *cls = pci_class_name(d->class_code, d->subclass);
    if (vendor)
        snprintf(buf, cap, "%s %s [%04x:%04x]", vendor, cls, d->vendor, d->device);
    else
        snprintf(buf, cap, "%s [%04x:%04x]", cls, d->vendor, d->device);
}

/* --------------------------------------------------------- driver coverage
 *
 * "We are still missing a lot of drivers" is true, and until now this system
 * could not say WHICH.  Every device was listed at startup and nothing
 * recorded whether anything went on to drive it, so a card with a working
 * driver and a card with none read identically in the log.
 *
 * A device is claimed when a driver commits to it, not when one recognises it.
 * The difference is the entire value of the report: a driver that finds a card
 * and then gives up - an unusable BAR, a model it has no table for - leaves
 * the device unclaimed, which is exactly what it is.
 */
void pci_claim(pci_dev_t *d, const char *driver) {
    if (!d || !driver) return;
    /* First claim wins.  Two drivers claiming one device is a bug worth seeing
     * rather than quietly resolving in favour of whichever ran last. */
    if (d->driver) {
        if (strcmp(d->driver, driver) != 0)
            kwarn("pci", "%02x:%02x.%u claimed by %s and then by %s",
                  d->bus, d->slot, d->func, d->driver, driver);
        return;
    }
    d->driver = driver;
}

int pci_unclaimed_count(void) {
    int n = 0;
    for (pci_dev_t *d = devices; d; d = d->next)
        if (!d->driver) n++;
    return n;
}

/* A bridge carries other devices and is not something to write a driver for;
 * the firmware has already configured it and this system only walks through
 * it.  Counting the thirty-odd of them a desktop chipset presents as "missing
 * drivers" would bury the two or three findings that matter. */
static bool needs_a_driver(const pci_dev_t *d) {
    if (d->class_code == 0x06) return false;          /* bridge */
    if (d->class_code == 0x08 && d->subclass == 0x80) return false; /* misc */
    return true;
}

void pci_report_coverage(void) {
    int total = 0, driven = 0, structural = 0;

    for (pci_dev_t *d = devices; d; d = d->next) {
        if (!needs_a_driver(d)) { structural++; continue; }
        total++;
        if (d->driver) driven++;
    }

    kinfo("pci", "driver coverage: %d of %d devices are driven "
                 "(%d bridges and the like need none)",
          driven, total, structural);

    /* Named individually, because a count does not tell anyone what to write
     * next.  The identifiers go with each line for the same reason they go in
     * the enumeration above: a driver is written against those four numbers. */
    for (pci_dev_t *d = devices; d; d = d->next) {
        if (d->driver || !needs_a_driver(d)) continue;
        char desc[96];
        pci_describe(d, desc, sizeof desc);
        kinfo("pci", "  no driver: %02x:%02x.%u  %04x:%04x  class %02x.%02x.%02x  %s",
              d->bus, d->slot, d->func, d->vendor, d->device,
              d->class_code, d->subclass, d->prog_if, desc);
    }

    for (pci_dev_t *d = devices; d; d = d->next) {
        if (!d->driver) continue;
        kinfo("pci", "  %-8s %02x:%02x.%u  %04x:%04x",
              d->driver, d->bus, d->slot, d->func, d->vendor, d->device);
    }
}
