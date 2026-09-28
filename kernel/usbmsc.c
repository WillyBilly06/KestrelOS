/* usbmsc.c - USB storage: memory sticks, card readers, external drives.
 *
 * Nearly every USB storage device made since about 2000 speaks the same two
 * things stacked on each other, and neither is complicated:
 *
 *   Bulk-Only Transport is the envelope.  Three steps: a thirty-one byte
 *   command block goes out on the bulk OUT endpoint, then the data moves in
 *   whichever direction the command said, then a thirteen byte status block
 *   comes back on bulk IN.  Both blocks carry the same tag so a reply can be
 *   matched to its request.  That is the whole protocol.
 *
 *   SCSI is the letter inside.  The same command set disks have used since the
 *   1980s, of which a driver needs four commands: ask what you are, ask how
 *   big you are, read blocks, write blocks.
 *
 * This is why a memory stick works on every operating system without anybody
 * shipping a driver for that particular stick - there is nothing device
 * specific in here at all, and the same code drives a card reader, a portable
 * drive, and the stick this system boots from.
 *
 * ---------------------------------------------------------------------------
 * Why it matters here more than usual.  This system boots from a USB stick,
 * and until this file existed the kernel could not write to the thing it was
 * running from: the loader reads it through the firmware, but the firmware is
 * gone by the time the kernel starts.  Every account of what happened on a
 * real machine had to be photographed off the screen, because there was
 * nowhere to put a log.  With this, the volume it booted from is an ordinary
 * disk and the log goes on it.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "klog.h"
#include "mm.h"
#include "time.h"
#include "usb.h"
#include "block.h"
#include "proc.h"

/* ----------------------------------------------------------- the envelope */

#define CBW_SIGNATURE 0x43425355u          /* "USBC", little-endian */
#define CSW_SIGNATURE 0x53425355u          /* "USBS"                */

typedef struct {
    u32 signature;
    u32 tag;
    u32 transfer_length;
    u8  flags;                              /* 0x80 for a read              */
    u8  lun;
    u8  cb_length;                          /* how much of the command below */
    u8  cb[16];
} __attribute__((packed)) cbw_t;

typedef struct {
    u32 signature;
    u32 tag;
    u32 residue;                            /* what did not get transferred */
    u8  status;                             /* 0 good, 1 failed, 2 phase error */
} __attribute__((packed)) csw_t;

/* --------------------------------------------------------------- the letter */

#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_REQUEST_SENSE    0x03
#define SCSI_INQUIRY          0x12
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10          0x28
#define SCSI_WRITE_10         0x2A

/* ------------------------------------------------------------------ a disk */

#define MAX_DISKS 4

typedef struct {
    bool          used;
    bool          modelled;     /* answered by usbmsc_model.c, not a stick */
    usb_device_t *dev;
    u32           tag;
    u32           sector_size;
    u64           sectors;
    char          model[48];
    blockdev_t   *block;
} msc_t;

static msc_t disks[MAX_DISKS];
static int   disk_count;

/* How far the most recent attempt to bring up a storage device got.
 *
 * Every step below already logs when it fails, and on a machine with no
 * working keyboard those messages go to a console covered by a desktop nobody
 * can drive - which is to say nowhere.  Keeping the reason as a number lets it
 * reach a screen. */
static const char *msc_last_stage = "none seen";

const char *usbmsc_last_stage(void) { return msc_last_stage; }

/* Only the real ones.
 *
 * The desktop shows this next to "log on disk", and it is read by somebody
 * trying to work out why a stick they can see is not being written to.  A
 * modelled device counted here would tell them a stick is present on a machine
 * that has none, which is worse than saying nothing - it sends them looking at
 * the wrong half of the problem. */
static int modelled_disks;

int usbmsc_disk_count(void) { return disk_count - modelled_disks; }

/* Where the bytes actually go.
 *
 * A model stands in for the device at exactly this line and nowhere else.
 * That placement is deliberate: everything above it - the envelope, the tag
 * matching, the SCSI commands, the capacity arithmetic, the retry while the
 * device wakes up - is the same code whether a real stick or a model is
 * answering.  Putting the seam any higher would test a different driver from
 * the one that runs on hardware.
 */
int usbmsc_model_bulk(bool in, void *data, u32 length);

static int msc_bulk(msc_t *d, bool in, void *data, u32 length, u32 timeout) {
    if (d->modelled) return usbmsc_model_bulk(in, data, length);
    return usb_bulk(d->dev, in, data, length, timeout);
}

/* Run one command all the way through: envelope out, data, status back.
 *
 * Returns the number of data bytes moved, or negative.  A device that reports
 * a failure in the status block is a different thing from one that did not
 * answer at all, and both are distinguished from a short transfer, because a
 * short read on a disk is normal and a failure is not.
 */
static int transact(msc_t *d, const u8 *cmd, int cmd_len, bool in,
                    void *data, u32 length) {
    cbw_t cbw;
    memset(&cbw, 0, sizeof cbw);
    cbw.signature = CBW_SIGNATURE;
    cbw.tag = ++d->tag;
    cbw.transfer_length = length;
    cbw.flags = in ? 0x80 : 0x00;
    cbw.lun = 0;
    cbw.cb_length = (u8)cmd_len;
    memcpy(cbw.cb, cmd, (size_t)cmd_len);

    if (msc_bulk(d, false, &cbw, sizeof cbw, 1000) != (int)sizeof cbw)
        return -1;

    int moved = 0;
    if (length) {
        moved = msc_bulk(d, in, data, length, 5000);
        if (moved < 0) return -1;
    }

    csw_t csw;
    memset(&csw, 0, sizeof csw);
    if (msc_bulk(d, true, &csw, sizeof csw, 1000) != (int)sizeof csw)
        return -1;

    if (csw.signature != CSW_SIGNATURE || csw.tag != cbw.tag) {
        /* The device answered something that is not a reply to this - the two
         * sides have lost track of each other, and continuing would attribute
         * one command's data to another. */
        kwarn("usbmsc", "%s: out of step (signature %08x, tag %u wanted %u)",
              d->model, csw.signature, csw.tag, cbw.tag);
        return -1;
    }
    if (csw.status != 0) return -1;

    return moved;
}

/* Some devices answer the first command or two with "not ready" while they
 * spin up or check their media.  Asking a few times is what every other
 * operating system does and is why a card reader with no card in it does not
 * hold up a boot. */
static bool wait_ready(msc_t *d) {
    /* Bounded by the clock, not by a count of attempts.
     *
     * Each attempt can take as long as its transfers are allowed to - several
     * seconds if the device is not answering at all - so twenty attempts is
     * not "about two seconds", it is up to three minutes.  That matters more
     * than it sounds: this runs inside the thread that enumerates the bus, so
     * a memory stick that is slow to wake would hold up every other device on
     * the same controller behind it, including the keyboard.
     *
     * Three seconds is generous for a device that is going to work and short
     * enough that one that is not costs nothing worth noticing. */
    u64 deadline = g_uptime_ms + 3000;
    u8 cmd[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };

    while (g_uptime_ms < deadline) {
        if (transact(d, cmd, sizeof cmd, false, NULL, 0) >= 0) return true;

        /* A failure has to be collected before the device will accept
         * anything else; leaving it uncollected wedges the next command. */
        u8 sense[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
        u8 data[18];
        transact(d, sense, sizeof sense, true, data, sizeof data);
        sched_sleep_ms(50);
    }
    return false;
}

/* ----------------------------------------------------------- block interface */

static int msc_read(blockdev_t *bd, u64 lba, u32 count, void *buf) {
    msc_t *d = bd->priv;
    if (!d || !d->used) return -E_IO;

    u8 *out = buf;
    while (count) {
        /* One page at a time, because that is the buffer the transfer layer
         * has; eight sectors of five hundred and twelve bytes. */
        u32 batch = PAGE_SIZE / d->sector_size;
        if (!batch) batch = 1;
        if (batch > count) batch = count;

        u8 cmd[10] = { SCSI_READ_10, 0,
                       (u8)(lba >> 24), (u8)(lba >> 16), (u8)(lba >> 8), (u8)lba,
                       0, (u8)(batch >> 8), (u8)batch, 0 };

        u32 want = batch * d->sector_size;
        if (transact(d, cmd, sizeof cmd, true, out, want) != (int)want)
            return -E_IO;

        out += want;
        lba += batch;
        count -= batch;
    }
    return 0;
}

static int msc_write(blockdev_t *bd, u64 lba, u32 count, const void *buf) {
    msc_t *d = bd->priv;
    if (!d || !d->used) return -E_IO;

    const u8 *in = buf;
    while (count) {
        u32 batch = PAGE_SIZE / d->sector_size;
        if (!batch) batch = 1;
        if (batch > count) batch = count;

        u8 cmd[10] = { SCSI_WRITE_10, 0,
                       (u8)(lba >> 24), (u8)(lba >> 16), (u8)(lba >> 8), (u8)lba,
                       0, (u8)(batch >> 8), (u8)batch, 0 };

        u32 want = batch * d->sector_size;
        if (transact(d, cmd, sizeof cmd, false, (void *)in, want) != (int)want)
            return -E_IO;

        in += want;
        lba += batch;
        count -= batch;
    }
    return 0;
}

static const block_ops_t msc_ops = {
    .read = msc_read,
    .write = msc_write,
};

/* ------------------------------------------------------------------- probe */

static bool bring_up(msc_t *d);

bool usbmsc_probe(usb_device_t *dev, const usb_interface_t *ifc) {
    /* Subclass 6 is the SCSI command set and protocol 0x50 is Bulk-Only.
     * Nearly everything is this pair; the handful of devices that are not are
     * older than the machines this runs on. */
    msc_last_stage = "found, checking what it speaks";

    if (ifc->subclass != 0x06 || ifc->protocol != 0x50) {
        msc_last_stage = "speaks a protocol this driver does not";
        kinfo("usbmsc", "storage device with an interface this driver does not "
                        "speak (subclass %u, protocol %#x)",
              ifc->subclass, ifc->protocol);
        return false;
    }

    if (disk_count >= MAX_DISKS) {
        kwarn("usbmsc", "no room for another storage device");
        return false;
    }

    msc_t *d = NULL;
    for (int i = 0; i < MAX_DISKS; i++) if (!disks[i].used) { d = &disks[i]; break; }
    if (!d) return false;

    memset(d, 0, sizeof *d);
    d->dev = dev;
    d->sector_size = 512;
    strlcpy(d->model, "USB storage", sizeof d->model);

    msc_last_stage = "opening its bulk endpoints";
    if (!usb_bulk_open(dev, ifc)) {
        msc_last_stage = "its bulk endpoints would not open";
        kwarn("usbmsc", "cannot open the bulk endpoints");
        return false;
    }
    d->used = true;
    return bring_up(d);
}

/* Everything after the endpoints are open: what it is, how big, and getting it
 * registered as a disk.  Separated so that a model drives exactly this and not
 * a paraphrase of it. */
static bool bring_up(msc_t *d) {
    msc_last_stage = "waiting for it to be ready";
    if (!wait_ready(d)) {
        msc_last_stage = "it never answered that it was ready";
        kwarn("usbmsc", "the device never became ready");
        d->used = false;
        return false;
    }

    /* What it is.  The name is three fields of padded text, which is how SCSI
     * has always reported it. */
    u8 inq[36];
    u8 cmd_inq[6] = { SCSI_INQUIRY, 0, 0, 0, sizeof inq, 0 };
    if (transact(d, cmd_inq, sizeof cmd_inq, true, inq, sizeof inq) >= 36) {
        char name[32];
        int n = 0;
        for (int i = 8; i < 32 && n < (int)sizeof name - 1; i++) {
            if (inq[i] < 0x20 || inq[i] > 0x7E) continue;
            if (inq[i] == ' ' && (n == 0 || name[n - 1] == ' ')) continue;
            name[n++] = (char)inq[i];
        }
        while (n > 0 && name[n - 1] == ' ') n--;
        name[n] = 0;
        if (n) strlcpy(d->model, name, sizeof d->model);
    }

    /* How big.  Read Capacity gives the number of the LAST block, not the
     * count - a difference of one that shows up as a disk one sector short
     * and nothing else, which is a horrible thing to debug later. */
    u8 cap[8];
    u8 cmd_cap[10] = { SCSI_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    msc_last_stage = "asking how big it is";
    if (transact(d, cmd_cap, sizeof cmd_cap, true, cap, sizeof cap) != 8) {
        msc_last_stage = "it would not say how big it is";
        kwarn("usbmsc", "%s: will not say how big it is", d->model);
        d->used = false;
        return false;
    }

    u32 last = ((u32)cap[0] << 24) | ((u32)cap[1] << 16) |
               ((u32)cap[2] << 8) | cap[3];
    u32 size = ((u32)cap[4] << 24) | ((u32)cap[5] << 16) |
               ((u32)cap[6] << 8) | cap[7];

    if (size < 512 || size > 4096 || (size & (size - 1))) size = 512;
    d->sector_size = size;
    d->sectors = (u64)last + 1;

    if (!d->sectors || d->sectors == 1) {
        kwarn("usbmsc", "%s: reports no capacity", d->model);
        d->used = false;
        return false;
    }

    char name[16];
    snprintf(name, sizeof name, "usb%d", disk_count);
    d->block = block_register(name, &msc_ops, d, d->sector_size, d->sectors,
                              d->model);
    if (!d->block) {
        kerr("usbmsc", "%s: cannot register as a disk", d->model);
        d->used = false;
        return false;
    }
    disk_count++;

    msc_last_stage = "working";
    kinfo("usbmsc", "%s: %llu MiB (%llu sectors of %u bytes) as %s",
          d->model,
          (unsigned long long)((d->sectors * d->sector_size) >> 20),
          (unsigned long long)d->sectors, d->sector_size, name);
    return true;
}

/* ------------------------------------------------------ driven by a model --
 *
 * The reason this exists at all: this system's primary use is a bootable USB
 * stick, and until now the code that makes the stick a writable disk had never
 * run once.  Not "was believed to work" - had never been executed, because
 * every machine it is tested on boots from something else.  The log the user
 * would read off the stick afterwards depends entirely on this path.
 */
void usbmsc_model_attach(void);
void usbmsc_model_detach(void);

bool usbmsc_attach_model(void) {
    if (disk_count >= MAX_DISKS) return false;

    msc_t *d = NULL;
    for (int i = 0; i < MAX_DISKS; i++) if (!disks[i].used) { d = &disks[i]; break; }
    if (!d) return false;

    memset(d, 0, sizeof *d);
    d->modelled = true;
    d->sector_size = 512;
    strlcpy(d->model, "USB storage", sizeof d->model);
    d->used = true;

    /* What the last real attempt reported, so that running a test does not
     * overwrite the diagnosis of a stick that actually failed. */
    const char *stage_before = msc_last_stage;

    usbmsc_model_attach();
    bool ok = bring_up(d);
    if (!ok) d->used = false;
    else modelled_disks++;

    msc_last_stage = stage_before;
    return ok;
}

/* And put it away again.
 *
 * A modelled stick left registered is a disk that every later survey finds, on
 * a machine where no such disk exists.  The test proves the driver works; it
 * must not leave a device behind that nothing can read. */
void usbmsc_detach_model(void) {
    for (int i = 0; i < MAX_DISKS; i++) {
        if (!disks[i].used || !disks[i].modelled) continue;
        if (disks[i].block) block_unregister(disks[i].block);
        disks[i].used = false;
        disks[i].block = NULL;
        disk_count--;
        modelled_disks--;
    }
    usbmsc_model_detach();
}

/* The block device the model registered.  The self-test must ask for THIS and
 * not for "usb0": on a machine that really did boot from a USB stick, the stick
 * is usb0 and the model is usb1, and a test that looked up usb0 would check the
 * real stick against the model's expected size and report a working stick as
 * broken - which is exactly the misleading "USB storage is not working" seen on
 * the first boot from real hardware. */
blockdev_t *usbmsc_model_block(void) {
    for (int i = 0; i < MAX_DISKS; i++)
        if (disks[i].used && disks[i].modelled) return disks[i].block;
    return NULL;
}

/* ================================================ a USB stick, driven ====
 *
 * The path that makes this system's primary medium into a writable disk, run
 * for the first time.
 *
 * What is being proved is narrow and worth stating exactly: the envelope, the
 * tag matching, the SCSI command set, the capacity arithmetic and the block
 * registration all work against a device that behaves the way sticks behave.
 * It is not proof that any particular stick works - that needs the stick.  It
 * IS proof that the code is not simply broken, which is the state it was in
 * before, having never been executed.
 */
bool usbmsc_attach_model(void);
void usbmsc_detach_model(void);
int  usbmsc_model_commands(void);
bool usbmsc_model_bad_signature(void);
int  usbmsc_model_wedged(void);
bool usbmsc_model_wrote_past_the_end(void);
u32  usbmsc_model_sector_size(void);
u64  usbmsc_model_sectors(void);
bool usbmsc_model_peek(u64 lba, void *out);


/* ==================================== a filesystem on the stick ==========
 *
 * Everything above works in sectors, which proves the transport and the SCSI
 * commands and stops there.  This is the question the user actually asked.
 *
 * "Nothing logs on the USB stick" is not about sectors.  It is about whether a
 * volume can be MOUNTED from the device, a file created on it, written, and
 * the write pushed all the way back down through the filesystem and the block
 * cache to the hardware.  Every one of those layers exists and works; none of
 * them had ever been run together on a USB device, because there is no USB
 * disk on any machine this is tested on.
 *
 * The volume the model arrives with was formatted by the build, not by this
 * kernel - a filesystem this system created and then read back would only
 * prove it agrees with itself.
 */
static int check_stick_filesystem(blockdev_t *disk) {
    int bad = 0;

    filesystem_t *fs = vfs_probe(disk);
    if (!fs) {
        kwarn("usbmsc", "selftest: nothing on the stick looks like a "
                        "filesystem, so nothing could ever be written to it");
        return 1;
    }

    vfs_mkdir("/stick");
    if (vfs_mount("/stick", fs) < 0) {
        kwarn("usbmsc", "selftest: the stick holds %s and it would not mount",
              fs->name);
        if (fs->unmount) fs->unmount(fs);
        return 1;
    }

    /* Something the build put there, read back.  A volume that mounts and is
     * empty would pass a check that only created files - and an empty volume
     * is what a driver reading the wrong sectors produces. */
    static u8 want[600], got[600];
    for (int i = 0; i < 600; i++) want[i] = (u8)((i * 37 + 11) & 0xFF);

    s64 n = vfs_read_file("/stick/KESTREL/STICK.CHK", got, sizeof got);
    if (n != (s64)sizeof want) {
        kwarn("usbmsc", "selftest: the file the build put on the stick came "
                        "back as %lld bytes, not %u",
              (long long)n, (unsigned)sizeof want);
        bad++;
    } else if (memcmp(got, want, sizeof want) != 0) {
        kwarn("usbmsc", "selftest: the file on the stick reads back wrong - "
                        "the driver is finding the right file in the wrong "
                        "place");
        bad++;
    }

    /* And the thing a log does: create, write, flush, read back.  Written
     * through the ordinary path, not a special one, because the ordinary path
     * is what klog uses. */
    static char line[220];
    snprintf(line, sizeof line,
             "KestrelOS wrote this to a USB stick through its own driver.\n");

    if (vfs_append("/stick/logs/usbtest.log", line, strlen(line)) < 0) {
        kwarn("usbmsc", "selftest: a file could not be written to the stick - "
                        "this is what \"nothing logs on the USB stick\" is");
        bad++;
    } else {
        /* Flushed all the way to the device, not merely into the cache.  A log
         * that reaches the block cache and no further is not on the stick when
         * it is unplugged, and everything upstream looks like it worked. */
        block_flush(disk);

        static char back[220];
        s64 got_n = vfs_read_file("/stick/logs/usbtest.log", back, sizeof back);
        if (got_n != (s64)strlen(line) ||
            memcmp(back, line, strlen(line)) != 0) {
            kwarn("usbmsc", "selftest: what was written to the stick is not "
                            "what comes back from it");
            bad++;
        }
    }

    if (!bad)
        kinfo("usbmsc", "a filesystem works on the stick: it mounted as %s, a "
                        "file the build put there reads back byte for byte, "
                        "and a log line written to it survives a flush to the "
                        "device", fs->name);

    vfs_unmount("/stick");
    return bad;
}

int usbmsc_selftest(void) {
    int bad = 0;

    if (!usbmsc_attach_model()) {
        kwarn("usbmsc", "selftest: the modelled stick did not come up");
        return 1;
    }

    /* The model refuses the first two readiness checks and then demands the
     * failure be collected before it will answer anything else - which is what
     * a stick does while it wakes up.  Coming up at all means the driver
     * waited and collected the sense between attempts. */
    if (usbmsc_model_wedged()) {
        kwarn("usbmsc", "selftest: %d command(s) were sent while a failure "
                        "was still uncollected - a real stick wedges there",
              usbmsc_model_wedged());
        bad++;
    }
    if (usbmsc_model_bad_signature()) {
        kwarn("usbmsc", "selftest: the envelope was sent with the wrong "
                        "signature");
        bad++;
    }

    /* It has to have registered as a disk, with the size the device reported.
     * Read Capacity gives the number of the LAST block; a driver that takes it
     * as the count is one sector short and nothing says so until something
     * reads off the end. */
    blockdev_t *disk = usbmsc_model_block();
    if (!disk) {
        kwarn("usbmsc", "selftest: it never appeared as a disk, so nothing "
                        "above the driver can reach it");
        usbmsc_detach_model();
        return bad + 1;
    }
    if (disk->sector_count != usbmsc_model_sectors()) {
        kwarn("usbmsc", "selftest: it registered %llu sectors where the device "
                        "reports %llu - Read Capacity gives the LAST block, "
                        "not the count",
              (unsigned long long)disk->sector_count,
              (unsigned long long)usbmsc_model_sectors());
        bad++;
    }
    if (disk->sector_size != usbmsc_model_sector_size()) {
        kwarn("usbmsc", "selftest: it registered %u byte sectors, not %u",
              disk->sector_size, usbmsc_model_sector_size());
        bad++;
    }

    /* Reading what is there.  The model puts a pattern in the first sector so
     * that a read returning zeroes cannot pass. */
    static u8 got[512];
    if (block_read(disk, 0, 1, got) != 0) {
        kwarn("usbmsc", "selftest: the first sector would not read");
        bad++;
    } else {
        /* Against what the device actually holds, rather than a pattern this
         * test also knows: the question is whether the driver brought back
         * THAT sector, and asking the device is the only way to tell a correct
         * read from a read of the wrong place that happens to look right. */
        static u8 held[512];
        bool empty = true;
        for (int i = 0; i < 512; i++) if (got[i]) { empty = false; break; }

        if (empty) {
            kwarn("usbmsc", "selftest: the first sector came back as nothing");
            bad++;
        } else if (!usbmsc_model_peek(0, held)) {
            kwarn("usbmsc", "selftest: the device cannot be asked what it "
                            "holds");
            bad++;
        } else if (memcmp(got, held, sizeof held) != 0) {
            kwarn("usbmsc", "selftest: the first sector came back as "
                            "something else");
            bad++;
        }
    }

    /* Writing, and checking it landed where it was addressed rather than
     * merely landing.  A driver that gets the address bytes of READ_10 or
     * WRITE_10 in the wrong order writes a real sector, just not that one. */
    static u8 pattern[512];
    for (int i = 0; i < 512; i++) pattern[i] = (u8)(0xC3 + i);

    const u64 where = 42;
    if (block_write(disk, where, 1, pattern) != 0) {
        kwarn("usbmsc", "selftest: a sector would not write");
        bad++;
    } else {
        static u8 landed[512];

        /* Written, but not yet on the device: the block layer holds it.
         *
         * This is checked rather than assumed because it is the whole reason a
         * log can be missing from a stick that was working.  Everything above
         * this line succeeds, the file is there when read back, and the stick
         * carried away has nothing on it - because nothing flushed before the
         * power went.  A write that has not reached the device is not a write
         * yet, and only the device can say so. */
        bool before = usbmsc_model_peek(where, landed) &&
                      memcmp(landed, pattern, sizeof pattern) == 0;

        if (block_flush(disk) != 0) {
            kwarn("usbmsc", "selftest: the disk would not flush, so nothing "
                            "written to it is guaranteed to be on it");
            bad++;
        }

        if (!usbmsc_model_peek(where, landed)) {
            kwarn("usbmsc", "selftest: the device cannot be asked what it "
                            "holds");
            bad++;
        } else if (memcmp(landed, pattern, sizeof pattern) != 0) {
            kwarn("usbmsc", "selftest: what the stick holds at sector %llu is "
                            "not what was written there - even after a flush",
                  (unsigned long long)where);
            bad++;
        } else if (!before) {
            kinfo("usbmsc", "a write reaches the stick when the disk is "
                            "flushed and not before - which is why a log that "
                            "is never flushed is not on the stick");
        }

        /* And back through the driver, which is the round trip the log
         * depends on. */
        static u8 reread[512];
        if (block_read(disk, where, 1, reread) != 0 ||
            memcmp(reread, pattern, sizeof pattern) != 0) {
            kwarn("usbmsc", "selftest: a sector written and read back is not "
                            "the same sector");
            bad++;
        }
    }

    /* More than one sector at a time, which is what a filesystem does and what
     * the driver splits into batches - the loop that advances the address and
     * the count is only exercised by a transfer larger than one batch. */
    static u8 many[512 * 12];
    for (unsigned i = 0; i < sizeof many; i++) many[i] = (u8)(i * 7 + 1);
    if (block_write(disk, 100, 12, many) != 0) {
        kwarn("usbmsc", "selftest: a twelve-sector write was refused");
        bad++;
    } else {
        block_flush(disk);
        static u8 back[512 * 12];
        if (block_read(disk, 100, 12, back) != 0 ||
            memcmp(back, many, sizeof many) != 0) {
            kwarn("usbmsc", "selftest: a transfer spanning several batches "
                            "came back wrong - the address or the count is "
                            "not advancing with it");
            bad++;
        }
    }

    /* Off the end must be refused, not clamped.  A clamped write corrupts
     * whatever sector it lands on instead. */
    if (block_write(disk, usbmsc_model_sectors() - 1, 4, many) == 0) {
        kwarn("usbmsc", "selftest: a write running off the end of the device "
                        "was accepted");
        bad++;
    }

    /* And a filesystem on top of all that, which is the question that was
     * actually asked.  Runs last because it needs the device registered and
     * the guard lifted, and writing sectors above is what lifts it. */
    /* The volume, put back before it is mounted.  The writes above landed
     * inside it on purpose, and the cache still holds them - so both the
     * device and the memory in front of it have to be restored, or the
     * filesystem reads the damage rather than the volume. */
    void usbmsc_model_reformat(void);
    usbmsc_model_reformat();
    block_cache_invalidate(disk);

    bad += check_stick_filesystem(disk);

    if (!bad)
        kinfo("usbmsc", "a USB stick works: it woke up, said what it is and "
                        "how big, and sectors written to it come back - %d "
                        "command(s), %llu sectors of %u bytes",
              usbmsc_model_commands(),
              (unsigned long long)usbmsc_model_sectors(),
              usbmsc_model_sector_size());

    usbmsc_detach_model();
    return bad;
}
