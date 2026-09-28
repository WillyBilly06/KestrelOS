/* ata.c - the legacy IDE/ATA controller, driven by programmed I/O.
 *
 * This system's driver-coverage report named two devices on the test machine
 * that nothing drove, and this is one of them: an Intel PIIX4 IDE controller,
 * 8086:7111, class 01.01.8a.  Every VMware, VirtualBox and QEMU machine
 * presents one, as does every PC built before about 2008 and a good many built
 * after it for the optical drive.
 *
 * ---------------------------------------------------------------------------
 * WHY PROGRAMMED I/O AND NOT DMA
 *
 * The same controller can move sectors by bus-master DMA, which is faster, and
 * this driver does not use it.  That is a deliberate choice rather than an
 * unfinished one.  PIO needs no descriptor table, no physical memory below any
 * boundary, no interrupt routing and no coherency argument, so the whole
 * driver is one file that can be read in a sitting and is correct on hardware
 * that predates every one of those things.  A disk on this controller is a
 * legacy disk; the machines that have fast storage have it on AHCI or NVMe,
 * and both of those already have drivers here that do use DMA.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS EASY TO GET WRONG, AND WHERE THIS GETS IT RIGHT
 *
 * The status register must be read once and used.  Reading it twice in a
 * condition - "while busy ... if error" - reads two different values, because
 * the register changes underneath and clears bits as a side effect of being
 * read.  Every wait here takes a single snapshot and decides from it.
 *
 * After writing a command the controller needs 400ns before its status means
 * anything.  The published way to spend it is four reads of the ALTERNATE
 * status port, which unlike the primary one does not acknowledge interrupts.
 *
 * Selecting a drive is not instant either, and the same 400ns applies; a
 * driver that selects and immediately reads gets the status of the drive it
 * just stopped talking to.  That failure looks exactly like a missing disk.
 *
 * A device that is not there floats.  All-ones and all-zeroes status both mean
 * "nobody home", and are checked before anything is believed.
 *
 * IDENTIFY returns 256 little-endian words, and the strings inside them are
 * byte-swapped in pairs.  The serial and model read as gibberish otherwise -
 * which is the sort of wrong that still looks like it worked.
 *
 * LBA48 exists because LBA28 tops out at 128 GiB.  Both are implemented, and
 * which is used is decided per command by the sector being asked for, not once
 * at startup: a 200 GiB disk still serves its first sectors by LBA28, and the
 * bootloader only ever asks for those.
 */
#include "kernel.h"
#include "klog.h"
#include "pci.h"
#include "block.h"
#include "time.h"

/* Register offsets from the command block base. */
#define ATA_DATA        0
#define ATA_ERROR       1       /* reading  */
#define ATA_FEATURES    1       /* writing  */
#define ATA_SECCOUNT    2
#define ATA_LBA_LOW     3
#define ATA_LBA_MID     4
#define ATA_LBA_HIGH    5
#define ATA_DRIVE       6
#define ATA_STATUS      7       /* reading; acknowledges the interrupt  */
#define ATA_COMMAND     7       /* writing  */

/* The control block sits at its own base; offset 0 is the alternate status,
 * which reads the same bits without acknowledging anything. */
#define ATA_ALTSTATUS   0
#define ATA_CONTROL     0

#define ST_ERR   0x01
#define ST_DRQ   0x08
#define ST_DF    0x20
#define ST_RDY   0x40
#define ST_BSY   0x80

#define CMD_READ_PIO        0x20
#define CMD_READ_PIO_EXT    0x24
#define CMD_WRITE_PIO       0x30
#define CMD_WRITE_PIO_EXT   0x34
#define CMD_FLUSH           0xE7
#define CMD_FLUSH_EXT       0xEA
#define CMD_IDENTIFY        0xEC

#define LBA28_LIMIT  (1ull << 28)

typedef struct {
    u16  io_base;           /* command block  */
    u16  ctrl_base;         /* control block  */
    u8   drive;             /* 0 master, 1 slave */
    bool lba48;
    u32  sector_size;
    u64  sectors;
    char model[41];
    char serial[21];
} ata_disk_t;

#define MAX_DISKS 4
static ata_disk_t disks[MAX_DISKS];
static int disk_count;

/* --------------------------------------------------------------- the basics */

/* 400 nanoseconds, spent the way the specification says to spend it.  The
 * alternate status port is used precisely because reading it has no effect on
 * the controller's interrupt state. */
static void delay400(const ata_disk_t *d) {
    for (int i = 0; i < 4; i++) (void)inb(d->ctrl_base + ATA_ALTSTATUS);
}

/* Wait for the controller to stop being busy, then report what it settled on.
 *
 * One read, one decision.  `*status` is the snapshot the caller must judge
 * from - handing back the register to be read again would reintroduce exactly
 * the race this exists to avoid. */
static bool wait_ready(const ata_disk_t *d, u8 *status, int ms) {
    for (int spent = 0; spent < ms * 1000; spent += 10) {
        u8 s = inb(d->ctrl_base + ATA_ALTSTATUS);
        if (s == 0xFF || s == 0x00) { if (status) *status = s; return false; }
        if (!(s & ST_BSY)) { if (status) *status = s; return true; }
        timer_udelay(10);
    }
    if (status) *status = inb(d->ctrl_base + ATA_ALTSTATUS);
    return false;
}

/* Wait for the drive to want data moved, which is a different question from
 * whether it is busy: a command can complete without ever asking. */
static bool wait_drq(const ata_disk_t *d, int ms) {
    u8 s;
    if (!wait_ready(d, &s, ms)) return false;
    if (s & (ST_ERR | ST_DF)) return false;
    return (s & ST_DRQ) != 0;
}

static void select_drive(const ata_disk_t *d, u8 head_bits) {
    outb(d->io_base + ATA_DRIVE, (u8)(0xA0 | (d->drive << 4) | head_bits));
    delay400(d);
}

/* The strings IDENTIFY returns come back with each pair of bytes the wrong way
 * round, and with trailing spaces rather than a terminator. */
static void unswap(char *out, const u16 *words, int count) {
    int n = 0;
    for (int i = 0; i < count; i++) {
        out[n++] = (char)(words[i] >> 8);
        out[n++] = (char)(words[i] & 0xFF);
    }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\0')) n--;
    out[n] = '\0';
}

/* ------------------------------------------------------------- reading data */

/* One command, for however many sectors fit in a single request.
 *
 * The drive is re-selected for every command rather than once at startup: two
 * disks share these ports, and something else may have talked to the other one
 * in between.  A driver that assumes its selection survived reads the wrong
 * disk, silently and with entirely plausible data.
 */
static int transfer(ata_disk_t *d, u64 lba, u32 count, void *buf, bool write) {
    if (!count) return 0;
    if (lba + count > d->sectors) return -1;

    bool ext = d->lba48 && (lba + count > LBA28_LIMIT || count > 256);
    u16 io = d->io_base;

    if (ext) {
        select_drive(d, 0x40);                       /* LBA, head bits unused */
        outb(io + ATA_SECCOUNT, (u8)(count >> 8));
        outb(io + ATA_LBA_LOW,  (u8)(lba >> 24));
        outb(io + ATA_LBA_MID,  (u8)(lba >> 32));
        outb(io + ATA_LBA_HIGH, (u8)(lba >> 40));
        outb(io + ATA_SECCOUNT, (u8)count);
        outb(io + ATA_LBA_LOW,  (u8)lba);
        outb(io + ATA_LBA_MID,  (u8)(lba >> 8));
        outb(io + ATA_LBA_HIGH, (u8)(lba >> 16));
        outb(io + ATA_COMMAND, write ? CMD_WRITE_PIO_EXT : CMD_READ_PIO_EXT);
    } else {
        if (lba + count > LBA28_LIMIT) return -1;
        /* The top four bits of the address ride in the drive register, which
         * is why the drive is selected after they are known and not before. */
        select_drive(d, (u8)(0x40 | ((lba >> 24) & 0x0F)));
        outb(io + ATA_SECCOUNT, (u8)(count == 256 ? 0 : count));
        outb(io + ATA_LBA_LOW,  (u8)lba);
        outb(io + ATA_LBA_MID,  (u8)(lba >> 8));
        outb(io + ATA_LBA_HIGH, (u8)(lba >> 16));
        outb(io + ATA_COMMAND, write ? CMD_WRITE_PIO : CMD_READ_PIO);
    }

    delay400(d);

    u16 *p = (u16 *)buf;
    const u16 words = (u16)(d->sector_size / 2);

    /* Every sector is its own handshake.  The drive raises DRQ once per
     * sector, and a driver that waits once and then moves the lot gets ahead
     * of the disk on the second sector onwards. */
    for (u32 s = 0; s < count; s++) {
        if (!wait_drq(d, 5000)) {
            u8 st = inb(io + ATA_STATUS);
            kerr("ata", "%s sector %llu failed (status %02x, error %02x)",
                 write ? "writing" : "reading", (unsigned long long)(lba + s),
                 st, inb(io + ATA_ERROR));
            return -1;
        }

        if (write) for (u16 i = 0; i < words; i++) outw(io + ATA_DATA, *p++);
        else       for (u16 i = 0; i < words; i++) *p++ = inw(io + ATA_DATA);

        /* Between sectors of a write the drive needs a moment before it will
         * accept the next; the read of alternate status provides it. */
        if (write) delay400(d);
    }

    if (write) {
        outb(io + ATA_COMMAND, d->lba48 ? CMD_FLUSH_EXT : CMD_FLUSH);
        delay400(d);
        u8 st;
        if (!wait_ready(d, &st, 30000) || (st & (ST_ERR | ST_DF))) {
            kerr("ata", "the cache flush after a write did not complete "
                        "(status %02x)", st);
            return -1;
        }
    }

    return 0;
}

/* The published limit for one command is 256 sectors under LBA28 and 65536
 * under LBA48; requests larger than that are split rather than refused. */
static int ata_read(blockdev_t *dev, u64 lba, u32 count, void *buf) {
    ata_disk_t *d = dev->priv;
    u8 *out = buf;
    while (count) {
        u32 chunk = count;
        u32 limit = d->lba48 ? 65536u : 256u;
        if (chunk > limit) chunk = limit;
        if (transfer(d, lba, chunk, out, false) < 0) return -1;
        lba += chunk;
        out += (size_t)chunk * d->sector_size;
        count -= chunk;
    }
    return 0;
}

static int ata_write(blockdev_t *dev, u64 lba, u32 count, const void *buf) {
    ata_disk_t *d = dev->priv;
    const u8 *in = buf;
    while (count) {
        u32 chunk = count;
        u32 limit = d->lba48 ? 65536u : 256u;
        if (chunk > limit) chunk = limit;
        if (transfer(d, lba, chunk, (void *)in, true) < 0) return -1;
        lba += chunk;
        in += (size_t)chunk * d->sector_size;
        count -= chunk;
    }
    return 0;
}

static int ata_flush(blockdev_t *dev) {
    ata_disk_t *d = dev->priv;
    select_drive(d, 0x40);
    outb(d->io_base + ATA_COMMAND, d->lba48 ? CMD_FLUSH_EXT : CMD_FLUSH);
    delay400(d);
    u8 st;
    if (!wait_ready(d, &st, 30000) || (st & (ST_ERR | ST_DF))) return -1;
    return 0;
}

static const block_ops_t ata_ops = {
    .read = ata_read,
    .write = ata_write,
    .flush = ata_flush,
};

/* ------------------------------------------------------------- finding them */

/* Ask one drive what it is.  Returns false for an empty socket, which is the
 * ordinary case for three of the four positions on a typical machine. */
static bool identify(ata_disk_t *d) {
    select_drive(d, 0);

    /* Zero the address registers first: a non-zero LBA mid/high after the
     * command is how an ATAPI device announces itself, and that signature is
     * only meaningful if this driver did not put the values there. */
    outb(d->io_base + ATA_SECCOUNT, 0);
    outb(d->io_base + ATA_LBA_LOW,  0);
    outb(d->io_base + ATA_LBA_MID,  0);
    outb(d->io_base + ATA_LBA_HIGH, 0);

    outb(d->io_base + ATA_COMMAND, CMD_IDENTIFY);
    delay400(d);

    u8 st = inb(d->ctrl_base + ATA_ALTSTATUS);
    if (st == 0 || st == 0xFF) return false;          /* nothing in the socket */

    if (!wait_ready(d, &st, 1000)) return false;

    /* An ATAPI device - an optical drive, most often - rejects IDENTIFY and
     * leaves its signature in the address registers.  Reporting it as a disk
     * with no sectors would be worse than passing over it, because everything
     * above here would then try to read a partition table off a CD-ROM. */
    u8 mid = inb(d->io_base + ATA_LBA_MID), high = inb(d->io_base + ATA_LBA_HIGH);
    if (mid == 0x14 && high == 0xEB) {
        kinfo("ata", "%s at %03x is a packet device (ATAPI); this driver "
                     "handles disks only",
              d->drive ? "slave" : "master", d->io_base);
        return false;
    }
    if (mid || high) return false;                    /* SATA or something else */

    if (st & ST_ERR) return false;
    if (!(st & ST_DRQ)) return false;

    u16 id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(d->io_base + ATA_DATA);

    unswap(d->serial, id + 10, 10);
    unswap(d->model, id + 27, 20);

    /* Word 83 bit 10 is the drive saying it understands the 48-bit commands.
     * Words 100..103 then hold the real capacity; words 60..61 hold the 28-bit
     * one, which saturates rather than wrapping and so cannot be told apart
     * from a genuine 128 GiB disk without asking this question first. */
    d->lba48 = (id[83] & (1u << 10)) != 0;
    if (d->lba48) {
        d->sectors = (u64)id[100] | ((u64)id[101] << 16) |
                     ((u64)id[102] << 32) | ((u64)id[103] << 48);
    }
    if (!d->lba48 || !d->sectors)
        d->sectors = (u64)id[60] | ((u64)id[61] << 16);

    /* Word 106 describes the physical sector layout; bit 14 set and bit 12 set
     * together mean the logical sector is larger than 512 bytes, and words
     * 117..118 then give its size in words. */
    d->sector_size = 512;
    if ((id[106] & 0xC000) == 0x4000 && (id[106] & (1u << 12))) {
        u32 words = (u32)id[117] | ((u32)id[118] << 16);
        if (words >= 256 && words <= 4096) d->sector_size = words * 2;
    }

    return d->sectors != 0;
}

/* Does a read of sector N actually return sector N?
 *
 * Only says anything when the disk carries the marker; a real disk is left
 * alone and reported without comment. */
static void verify_addressing(ata_disk_t *d, blockdev_t *dev, const char *name) {
    static u8 sector[4096];
    if (d->sector_size > sizeof sector) return;

    const u64 probes[] = { 0, 1, 1000, d->sectors - 1 };
    int checked = 0, wrong = 0;

    for (unsigned i = 0; i < ARRAY_LEN(probes); i++) {
        u64 lba = probes[i];
        if (lba >= d->sectors) continue;

        memset(sector, 0xA5, d->sector_size);
        if (ata_read(dev, lba, 1, sector) < 0) {
            kerr("ata", "%s: reading sector %llu failed", name,
                 (unsigned long long)lba);
            wrong++;
            continue;
        }
        if (memcmp(sector + 4, "KESTREL-ATA", 11) != 0) continue;  /* not marked */

        checked++;
        u32 says = (u32)sector[0] | ((u32)sector[1] << 8) |
                   ((u32)sector[2] << 16) | ((u32)sector[3] << 24);
        if (says != (u32)lba) {
            kerr("ata", "%s: asked for sector %llu and got sector %u",
                 name, (unsigned long long)lba, says);
            wrong++;
        }
    }

    if (checked && !wrong)
        kinfo("ata", "%s: read back %d marked sectors, each the one asked for",
              name, checked);
    else if (wrong)
        kerr("ata", "%s: addressing is wrong - %d of %d probes disagreed",
             name, wrong, checked + wrong);
}

static void probe_channel(u16 io_base, u16 ctrl_base, const char *which) {
    for (u8 drive = 0; drive < 2; drive++) {
        if (disk_count >= MAX_DISKS) return;

        ata_disk_t *d = &disks[disk_count];
        memset(d, 0, sizeof *d);
        d->io_base = io_base;
        d->ctrl_base = ctrl_base;
        d->drive = drive;

        if (!identify(d)) continue;

        char name[BLOCK_NAME_MAX];
        snprintf(name, sizeof name, "disk%d", block_next_disk_index());

        blockdev_t *dev = block_register(name, &ata_ops, d, d->sector_size,
                                         d->sectors, d->model);
        if (!dev) { block_release_disk_index(); continue; }
        disk_count++;

        /* Read the disk back before claiming it works.
         *
         * A driver that reports a disk it cannot actually read is the exact
         * failure this system's honesty rule exists to prevent, and it is easy
         * to write: IDENTIFY succeeding says the drive answers, not that the
         * addressing is right.  The test machine's image carries its own
         * sector number at four known places, so a read that returns the wrong
         * sector - or nothing - is caught rather than logged as success.
         *
         * On a disk with no such pattern the check simply says nothing. */
        verify_addressing(d, dev, name);

        u64 mib = (d->sectors * d->sector_size) >> 20;
        kinfo("ata", "%s %s -> %s: %s (serial %s), %llu MiB, %u-byte sectors, "
                     "%s addressing",
              which, drive ? "slave" : "master", name, d->model, d->serial,
              (unsigned long long)mib, d->sector_size,
              d->lba48 ? "48-bit" : "28-bit");
    }
}

void ata_init(void) {
    pci_dev_t *pci = NULL;
    int controllers = 0;

    /* Class 1 subclass 1 is an IDE controller.  The programming interface byte
     * says, per channel, whether it answers at the legacy fixed ports or at
     * the addresses in its BARs; bit 0 covers the primary channel and bit 2
     * the secondary.  Both arrangements are common and a driver that assumes
     * either one finds nothing on half the machines it runs on. */
    while ((pci = pci_find(0x01, 0x01, 0xFF, pci)) != NULL) {
        controllers++;

        bool primary_native   = (pci->prog_if & 0x01) != 0;
        bool secondary_native = (pci->prog_if & 0x04) != 0;

        u16 p_cmd  = primary_native   ? (u16)(pci->bar[0] & ~3u) : 0x1F0;
        u16 p_ctl  = primary_native   ? (u16)((pci->bar[1] & ~3u) + 2) : 0x3F6;
        u16 s_cmd  = secondary_native ? (u16)(pci->bar[2] & ~3u) : 0x170;
        u16 s_ctl  = secondary_native ? (u16)((pci->bar[3] & ~3u) + 2) : 0x376;

        if (!p_cmd || !s_cmd) {
            kwarn("ata", "%02x:%02x.%u reports native mode with no port BAR",
                  pci->bus, pci->slot, pci->func);
            continue;
        }

        int before = disk_count;

        /* Interrupts off: every transfer here is polled, and a controller left
         * asserting an unhandled line would spin the machine. */
        outb(p_ctl, 0x02);
        outb(s_ctl, 0x02);

        probe_channel(p_cmd, p_ctl, "primary");
        probe_channel(s_cmd, s_ctl, "secondary");

        /* Claimed whether or not a disk was found: the controller is being
         * driven either way, and an empty IDE controller is not a gap in this
         * system's hardware support. */
        pci_claim(pci, "ata");

        if (disk_count == before)
            kinfo("ata", "%02x:%02x.%u has no disks attached",
                  pci->bus, pci->slot, pci->func);
    }

    if (!controllers) { kdebug("ata", "no IDE controller present"); return; }
    kinfo("ata", "%d IDE controller(s), %d disk(s)", controllers, disk_count);
}
