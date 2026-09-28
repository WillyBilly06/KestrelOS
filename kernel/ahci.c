/* ahci.c - AHCI SATA controller driver.
 *
 * Each port gets one command slot's worth of DMA structures allocated below
 * 4 GiB, so the driver works whether or not the controller reports 64-bit
 * addressing.  Commands are issued and polled to completion: interrupts would
 * buy throughput the rest of the system cannot yet use, and polling keeps the
 * failure modes obvious.
 */
#include "kernel.h"
#include "block.h"
#include "pci.h"
#include "mm.h"
#include "klog.h"
#include "time.h"
#include "vfs.h"
#include "proc.h"

/* Host control registers. */
#define HBA_CAP     0x00
#define HBA_GHC     0x04
#define HBA_IS      0x08
#define HBA_PI      0x0C
#define HBA_VS      0x10
#define HBA_CAP2    0x24
#define HBA_BOHC    0x28

#define GHC_HR      (1u << 0)
#define GHC_IE      (1u << 1)
#define GHC_AE      (1u << 31)

/* Per-port registers, at 0x100 + port * 0x80. */
#define PORT_CLB    0x00
#define PORT_CLBU   0x04
#define PORT_FB     0x08
#define PORT_FBU    0x0C
#define PORT_IS     0x10
#define PORT_IE     0x14
#define PORT_CMD    0x18
#define PORT_TFD    0x20
#define PORT_SIG    0x24
#define PORT_SSTS   0x28
#define PORT_SCTL   0x2C
#define PORT_SERR   0x30
#define PORT_CI     0x38

#define CMD_ST      (1u << 0)
#define CMD_SUD     (1u << 1)
#define CMD_POD     (1u << 2)
#define CMD_FRE     (1u << 4)
#define CMD_FR      (1u << 14)
#define CMD_CR      (1u << 15)

#define TFD_BSY     (1u << 7)
#define TFD_DRQ     (1u << 3)
#define TFD_ERR     (1u << 0)

#define SIG_ATA     0x00000101
#define SIG_ATAPI   0xEB140101

#define FIS_TYPE_H2D 0x27

#define ATA_IDENTIFY      0xEC
#define ATA_READ_DMA_EX   0x25
#define ATA_WRITE_DMA_EX  0x35
#define ATA_FLUSH_EX      0xEA

typedef struct __attribute__((packed)) {
    u8  fis_type;
    u8  pmport_c;        /* bit 7 = command, low nibble = port multiplier */
    u8  command;
    u8  featurel;
    u8  lba0, lba1, lba2, device;
    u8  lba3, lba4, lba5, featureh;
    u8  countl, counth, icc, control;
    u8  reserved[4];
} fis_h2d_t;

typedef struct __attribute__((packed)) {
    u32 dba, dbau, reserved;
    u32 dbc;             /* bits 0-21 byte count - 1, bit 31 interrupt */
} prdt_entry_t;

typedef struct __attribute__((packed)) {
    u8  cfis[64];
    u8  acmd[16];
    u8  reserved[48];
    prdt_entry_t prdt[8];
} cmd_table_t;

typedef struct __attribute__((packed)) {
    u16 flags;           /* bits 0-4 command FIS length in dwords, bit 6 write */
    u16 prdtl;
    u32 prdbc;
    u32 ctba, ctbau;
    u32 reserved[4];
} cmd_header_t;

typedef struct {
    volatile u8 *abar;
    int          port;
    volatile u8 *regs;

    cmd_header_t *clb;       /* command list, 1 KiB aligned  */
    u8           *fb;        /* received FIS area, 256 aligned */
    cmd_table_t  *ct;        /* one command table            */
    u64           clb_phys, fb_phys, ct_phys;

    u8           *bounce;    /* DMA staging buffer below 4 GiB */
    u64           bounce_phys;
    size_t        bounce_size;

    bool          lba48;
    bool          busy;      /* one transfer at a time per port */
} ahci_port_t;

/* A transfer can be preempted, so a second process must wait rather than
 * overwrite the command table and staging buffer underneath it. */
static void port_lock(ahci_port_t *p) {
    for (;;) {
        bool irq = irq_save();
        if (!p->busy) { p->busy = true; irq_restore(irq); return; }
        irq_restore(irq);
        sched_yield();
    }
}

static void port_unlock(ahci_port_t *p) {
    bool irq = irq_save();
    p->busy = false;
    irq_restore(irq);
}

#define BOUNCE_BYTES (128 * 1024)

static inline u32 rd(volatile u8 *base, u32 off) { return *(volatile u32 *)(base + off); }
static inline void wr(volatile u8 *base, u32 off, u32 v) { *(volatile u32 *)(base + off) = v; }

/* ------------------------------------------------------------------------- */
/* port engine                                                               */
/* ------------------------------------------------------------------------- */

static bool stop_engine(ahci_port_t *p) {
    u32 cmd = rd(p->regs, PORT_CMD);
    wr(p->regs, PORT_CMD, cmd & ~(CMD_ST | CMD_FRE));

    for (int i = 0; i < 1000; i++) {
        cmd = rd(p->regs, PORT_CMD);
        if (!(cmd & (CMD_CR | CMD_FR))) return true;
        timer_mdelay(1);
    }
    kerr("ahci", "port %d did not stop (CMD=%#x)", p->port, cmd);
    return false;
}

static void start_engine(ahci_port_t *p) {
    for (int i = 0; i < 1000 && (rd(p->regs, PORT_CMD) & CMD_CR); i++) timer_mdelay(1);
    u32 cmd = rd(p->regs, PORT_CMD);
    wr(p->regs, PORT_CMD, cmd | CMD_FRE | CMD_ST);
}

static bool wait_not_busy(ahci_port_t *p, int ms) {
    for (int i = 0; i < ms * 100; i++) {
        u32 tfd = rd(p->regs, PORT_TFD);
        if (!(tfd & (TFD_BSY | TFD_DRQ))) return true;
        timer_udelay(10);
    }
    return false;
}

/* Issue the command in slot 0 and wait for it to retire. */
static int run_command(ahci_port_t *p, int timeout_ms) {
    if (!wait_not_busy(p, 1000)) {
        kerr("ahci", "port %d stuck busy before issue (TFD=%#x)", p->port, rd(p->regs, PORT_TFD));
        return -E_IO;
    }
    wr(p->regs, PORT_SERR, rd(p->regs, PORT_SERR));    /* clear sticky errors */
    wr(p->regs, PORT_IS, rd(p->regs, PORT_IS));
    wr(p->regs, PORT_CI, 1);

    for (int i = 0; i < timeout_ms * 100; i++) {
        if (!(rd(p->regs, PORT_CI) & 1)) {
            u32 tfd = rd(p->regs, PORT_TFD);
            if (tfd & TFD_ERR) {
                kerr("ahci", "port %d command failed, TFD=%#x SERR=%#x", p->port, tfd, rd(p->regs, PORT_SERR));
                return -E_IO;
            }
            return 0;
        }
        u32 is = rd(p->regs, PORT_IS);
        if (is & 0x40000000u) {     /* task file error */
            kerr("ahci", "port %d task file error, TFD=%#x", p->port, rd(p->regs, PORT_TFD));
            wr(p->regs, PORT_IS, is);
            return -E_IO;
        }
        timer_udelay(10);
    }

    kerr("ahci", "port %d command timed out after %d ms", p->port, timeout_ms);
    /* Recover the port so the next request has a chance. */
    stop_engine(p);
    start_engine(p);
    return -E_IO;
}

static void build_command(ahci_port_t *p, u8 command, u64 lba, u32 count, u64 buf_phys,
                          size_t bytes, bool write) {
    memset(p->ct, 0, sizeof *p->ct);

    fis_h2d_t *fis = (fis_h2d_t *)p->ct->cfis;
    fis->fis_type = FIS_TYPE_H2D;
    fis->pmport_c = 0x80;
    fis->command  = command;
    fis->device   = 0x40;                     /* LBA mode */
    fis->lba0 = (u8)lba;
    fis->lba1 = (u8)(lba >> 8);
    fis->lba2 = (u8)(lba >> 16);
    fis->lba3 = (u8)(lba >> 24);
    fis->lba4 = (u8)(lba >> 32);
    fis->lba5 = (u8)(lba >> 40);
    fis->countl = (u8)count;
    fis->counth = (u8)(count >> 8);

    int prdtl = 0;
    if (bytes) {
        /* One PRD is enough: transfers are capped at the bounce buffer size,
         * which is well under the 4 MiB a single entry can describe. */
        p->ct->prdt[0].dba  = (u32)buf_phys;
        p->ct->prdt[0].dbau = (u32)(buf_phys >> 32);
        p->ct->prdt[0].dbc  = (u32)(bytes - 1);
        prdtl = 1;
    }

    memset(p->clb, 0, sizeof(cmd_header_t));
    p->clb[0].flags = (u16)((sizeof(fis_h2d_t) / 4) | (write ? (1 << 6) : 0));
    p->clb[0].prdtl = (u16)prdtl;
    p->clb[0].prdbc = 0;
    p->clb[0].ctba  = (u32)p->ct_phys;
    p->clb[0].ctbau = (u32)(p->ct_phys >> 32);
}

/* ------------------------------------------------------------------------- */
/* block operations                                                          */
/* ------------------------------------------------------------------------- */

static int ahci_transfer(blockdev_t *dev, u64 lba, u32 count, void *buf, bool write) {
    ahci_port_t *p = dev->priv;
    u8 *user = buf;

    port_lock(p);
    while (count) {
        u32 chunk = count;
        if ((u64)chunk * dev->sector_size > p->bounce_size)
            chunk = (u32)(p->bounce_size / dev->sector_size);
        size_t bytes = (size_t)chunk * dev->sector_size;

        if (write) memcpy(p->bounce, user, bytes);

        build_command(p, write ? ATA_WRITE_DMA_EX : ATA_READ_DMA_EX,
                      lba, chunk, p->bounce_phys, bytes, write);
        int r = run_command(p, 5000);
        if (r < 0) { port_unlock(p); return r; }

        if (!write) memcpy(user, p->bounce, bytes);

        user += bytes;
        lba += chunk;
        count -= chunk;
    }
    port_unlock(p);
    return 0;
}

static int ahci_read(blockdev_t *dev, u64 lba, u32 count, void *buf) {
    return ahci_transfer(dev, lba, count, buf, false);
}

static int ahci_write(blockdev_t *dev, u64 lba, u32 count, const void *buf) {
    return ahci_transfer(dev, lba, count, (void *)buf, true);
}

static int ahci_flush(blockdev_t *dev) {
    ahci_port_t *p = dev->priv;
    port_lock(p);
    build_command(p, ATA_FLUSH_EX, 0, 0, 0, 0, false);
    int r = run_command(p, 10000);
    port_unlock(p);
    return r;
}

static const block_ops_t ahci_ops = { ahci_read, ahci_write, ahci_flush };

/* ------------------------------------------------------------------------- */
/* identify and attach                                                       */
/* ------------------------------------------------------------------------- */

/* ATA strings are byte-swapped within each 16-bit word. */
static void ata_string(const u16 *src, int words, char *out, size_t cap) {
    size_t n = 0;
    for (int i = 0; i < words && n + 2 < cap; i++) {
        out[n++] = (char)(src[i] >> 8);
        out[n++] = (char)(src[i] & 0xFF);
    }
    out[n] = 0;
    while (n && (out[n - 1] == ' ' || out[n - 1] == 0)) out[--n] = 0;
}



static void attach_port(volatile u8 *abar, int port_no) {
    volatile u8 *regs = abar + 0x100 + (u32)port_no * 0x80;

    u32 ssts = rd(regs, PORT_SSTS);
    if ((ssts & 0x0F) != 3) return;                 /* no device present */
    if (((ssts >> 8) & 0x0F) != 1) return;          /* interface not active */

    u32 sig = rd(regs, PORT_SIG);
    if (sig == SIG_ATAPI) { kinfo("ahci", "port %d: ATAPI device, not supported", port_no); return; }
    if (sig != SIG_ATA)   { kinfo("ahci", "port %d: unrecognised signature %#x", port_no, sig); return; }

    ahci_port_t *p = kzalloc(sizeof *p);
    if (!p) return;
    p->abar = abar;
    p->port = port_no;
    p->regs = regs;

    if (!stop_engine(p)) { kfree(p); return; }

    /* All DMA structures go below 4 GiB so a 32-bit-only controller works. */
    u64 dma = pmm_alloc_pages_below(2, 0x100000000ULL);
    if (!dma) { kerr("ahci", "port %d: no DMA memory", port_no); kfree(p); return; }
    memset(phys_to_virt(dma), 0, 2 * PAGE_SIZE);

    p->clb_phys = dma;                  /* 1 KiB, needs 1 KiB alignment  */
    p->fb_phys  = dma + 0x400;          /* 256 B, needs 256 B alignment  */
    p->ct_phys  = dma + 0x1000;         /* 128 B alignment               */
    p->clb = phys_to_virt(p->clb_phys);
    p->fb  = phys_to_virt(p->fb_phys);
    p->ct  = phys_to_virt(p->ct_phys);

    size_t bounce_pages = BOUNCE_BYTES / PAGE_SIZE;
    p->bounce_phys = pmm_alloc_pages_below(bounce_pages, 0x100000000ULL);
    if (!p->bounce_phys) {
        kerr("ahci", "port %d: no DMA staging buffer", port_no);
        pmm_free_pages(dma, 2);
        kfree(p);
        return;
    }
    p->bounce = phys_to_virt(p->bounce_phys);
    p->bounce_size = BOUNCE_BYTES;

    wr(regs, PORT_CLB,  (u32)p->clb_phys);
    wr(regs, PORT_CLBU, (u32)(p->clb_phys >> 32));
    wr(regs, PORT_FB,   (u32)p->fb_phys);
    wr(regs, PORT_FBU,  (u32)(p->fb_phys >> 32));
    wr(regs, PORT_SERR, 0xFFFFFFFF);
    wr(regs, PORT_IS,   0xFFFFFFFF);
    wr(regs, PORT_IE,   0);              /* polled, not interrupt driven */

    start_engine(p);

    /* IDENTIFY DEVICE. */
    build_command(p, ATA_IDENTIFY, 0, 0, p->bounce_phys, 512, false);
    if (run_command(p, 3000) < 0) {
        kerr("ahci", "port %d: IDENTIFY failed", port_no);
        goto fail;
    }

    u16 *id = (u16 *)p->bounce;
    char model[48], serial[24];
    ata_string(id + 27, 20, model, sizeof model);
    ata_string(id + 10, 10, serial, sizeof serial);

    u64 sectors;
    if (id[83] & (1 << 10)) {              /* LBA48 supported */
        sectors = *(u64 *)(id + 100) & 0x0000FFFFFFFFFFFFULL;
        p->lba48 = true;
    } else {
        sectors = *(u32 *)(id + 60);
    }

    u32 sector_size = 512;
    if ((id[106] & 0xC000) == 0x4000 && (id[106] & (1 << 12)))
        sector_size = (*(u32 *)(id + 117)) * 2;
    if (sector_size < 512 || sector_size > 4096) sector_size = 512;

    if (!sectors) { kerr("ahci", "port %d: device reports zero capacity", port_no); goto fail; }

    char name[BLOCK_NAME_MAX];
    snprintf(name, sizeof name, "disk%d", block_next_disk_index());

    blockdev_t *dev = block_register(name, &ahci_ops, p, sector_size, sectors, model);
    if (!dev) goto fail;
    kinfo("ahci", "port %d: %s (serial %s)%s", port_no, model, serial, p->lba48 ? ", LBA48" : "");
    return;

fail:
    stop_engine(p);
    pmm_free_pages(p->bounce_phys, bounce_pages);
    pmm_free_pages(dma, 2);
    kfree(p);
}

static void attach_controller(pci_dev_t *d) {
    /* BAR5 is the AHCI register window. */
    if (!d->bar[5]) { kerr("ahci", "controller has no ABAR"); return; }

    pci_enable_bus_master(d);

    volatile u8 *abar = vmm_map_mmio(d->bar[5], d->bar_size[5] ? (size_t)d->bar_size[5] : 0x1100);
    if (!abar) { kerr("ahci", "cannot map ABAR at %p", (void *)d->bar[5]); return; }

    /* Take ownership from the firmware if BIOS/OS handoff is supported. */
    u32 cap2 = rd(abar, HBA_CAP2);
    if (cap2 & 1) {
        u32 bohc = rd(abar, HBA_BOHC);
        wr(abar, HBA_BOHC, bohc | (1u << 1));       /* OS ownership */
        for (int i = 0; i < 500; i++) {
            if (!(rd(abar, HBA_BOHC) & (1u << 0))) break;
            timer_mdelay(1);
        }
        if (rd(abar, HBA_BOHC) & 1) kwarn("ahci", "firmware did not release the controller");
    }

    /* Mapped, owned, and about to be enabled: this driver is driving it,
     * whether or not any port turns out to have a disk on it. */
    pci_claim(d, "ahci");

    wr(abar, HBA_GHC, rd(abar, HBA_GHC) | GHC_AE);

    /* Reset so we start from a known state whatever the firmware left behind. */
    wr(abar, HBA_GHC, rd(abar, HBA_GHC) | GHC_HR);
    bool reset_ok = false;
    for (int i = 0; i < 1000; i++) {
        if (!(rd(abar, HBA_GHC) & GHC_HR)) { reset_ok = true; break; }
        timer_mdelay(1);
    }
    if (!reset_ok) { kerr("ahci", "controller reset did not complete"); return; }

    wr(abar, HBA_GHC, rd(abar, HBA_GHC) | GHC_AE);
    wr(abar, HBA_IS, 0xFFFFFFFF);

    u32 cap = rd(abar, HBA_CAP);
    u32 pi  = rd(abar, HBA_PI);
    int max_ports = (int)(cap & 0x1F) + 1;
    u32 version = rd(abar, HBA_VS);

    kinfo("ahci", "controller %04x:%04x, AHCI %u.%u, %d port(s), %d command slot(s)%s",
          d->vendor, d->device, (version >> 16) & 0xFFFF, (version >> 8) & 0xFF,
          max_ports, (int)((cap >> 8) & 0x1F) + 1,
          (cap & (1u << 31)) ? ", 64-bit DMA" : "");

    for (int i = 0; i < 32; i++) {
        if (!(pi & (1u << i))) continue;
        attach_port(abar, i);
    }
}

void ahci_init(void) {
    int found = 0;
    for (pci_dev_t *d = pci_find(0x01, 0x06, 0x01, NULL); d; d = pci_find(0x01, 0x06, 0x01, d)) {
        attach_controller(d);
        found++;
    }
    if (!found) kinfo("ahci", "no AHCI controllers present");
}
