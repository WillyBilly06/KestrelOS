/* block.c - the block device registry, sector cache and partition scanner.
 *
 * Disks register themselves here; partitions are registered the same way with a
 * parent pointer and an LBA offset, so a filesystem never needs to know whether
 * it sits on a whole disk or a slice of one.  A small write-back cache absorbs
 * the many small reads that FAT directory walks produce.
 */
#include "kernel.h"
#include "block.h"
#include "vfs.h"
#include "mm.h"
#include "klog.h"
#include "usb.h"

bool cmdline_has(const char *flag);
#include "time.h"

const u8 GPT_TYPE_ESP[GUID_LEN] = {
    0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11, 0xBA,0x4B, 0x00,0xA0,0xC9,0x3E,0xC9,0x3B
};
const u8 GPT_TYPE_MSDATA[GUID_LEN] = {
    0xA2,0xA0,0xD0,0xEB, 0xE5,0xB9, 0x33,0x44, 0x87,0xC0, 0x68,0xB6,0xB7,0x26,0x99,0xC7
};
/* KestrelOS data partition: a private, randomly chosen type GUID. */
const u8 GPT_TYPE_KESTREL[GUID_LEN] = {
    0x7E,0x1A,0x4C,0xB3, 0x62,0x9D, 0x47,0x4E, 0x9C,0x31, 0x5A,0x6F,0x2E,0x88,0xD1,0x40
};

static blockdev_t *devices;

/* Set once block_init()'s one-off partition scan has run, after which any disk
 * that turns up has its own table read as it registers.  See block_register. */
static bool partitions_scanned_at_init;
static int         device_count;

/* ------------------------------------------------------------------------- */
/* GUID helpers                                                              */
/* ------------------------------------------------------------------------- */

/* GPT stores the first three fields little-endian and the rest big-endian,
 * which is what makes the canonical text form interleave byte orders. */
void guid_format(const u8 g[GUID_LEN], char *buf, size_t cap) {
    snprintf(buf, cap, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
             g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool guid_parse(const char *text, u8 out[GUID_LEN]) {
    u8 raw[GUID_LEN];
    int n = 0;
    for (const char *p = text; *p && n < GUID_LEN * 2; p++) {
        if (*p == '-') continue;
        int v = hexval(*p);
        if (v < 0) return false;
        if (n % 2 == 0) raw[n / 2] = (u8)(v << 4);
        else raw[n / 2] |= (u8)v;
        n++;
    }
    if (n != GUID_LEN * 2) return false;

    out[3] = raw[0]; out[2] = raw[1]; out[1] = raw[2]; out[0] = raw[3];
    out[5] = raw[4]; out[4] = raw[5];
    out[7] = raw[6]; out[6] = raw[7];
    for (int i = 8; i < 16; i++) out[i] = raw[i];
    return true;
}

bool guid_equal(const u8 a[GUID_LEN], const u8 b[GUID_LEN]) { return memcmp(a, b, GUID_LEN) == 0; }

static bool guid_is_zero(const u8 g[GUID_LEN]) {
    for (int i = 0; i < GUID_LEN; i++) if (g[i]) return false;
    return true;
}

/* ------------------------------------------------------------------------- */
/* sector cache                                                              */
/* ------------------------------------------------------------------------- */

#define CACHE_BLOCK   4096
#define CACHE_ENTRIES 256

typedef struct {
    blockdev_t *dev;
    u64  block;
    bool valid, dirty;
    u64  stamp;
    u8  *data;
} cache_entry_t;

static cache_entry_t cache[CACHE_ENTRIES];
static u64  cache_clock;
static bool cache_ready;

/* Keeping other threads out of the cache, WITHOUT switching interrupts off.
 *
 * This used to be `irq_save()` around each of the blocks below, and that was a
 * deadlock rather than a lock.  Every one of those regions ends up calling a
 * driver - `flush_entry` writes a dirty block back, `cache_lookup` reads a
 * missing one in - and a driver waits for its device.  How xhci.c waits is:
 *
 *     u64 deadline = g_uptime_ms + timeout_ms;
 *
 * and `g_uptime_ms` is incremented in exactly one place, `timer_isr`.  With
 * interrupts off that clock does not move, so the deadline is never reached
 * and the loop that was written to give up after five seconds gives up never.
 * The transfer is issued - the light on the stick flashes - and then the
 * machine stops for good, needing the power pulled.  That is precisely what
 * shutting down did on the target hardware.
 *
 * The bug hid for as long as it did because NVMe counts its own iterations
 * rather than reading the clock, so a machine that boots from NVMe - which is
 * every machine this is developed and tested on - never waits on a timer while
 * holding this, and never hangs.  USB storage does, on hardware, once.
 *
 * A plain lock is what was wanted.  The cache is only ever entered from thread
 * context (nothing in any interrupt handler reads or writes a block), so
 * spinning here with interrupts ENABLED is safe: the holder keeps being
 * scheduled, time keeps advancing, and driver timeouts mean what they say.
 */
static volatile u32 cache_held;

static void cache_lock(void) {
    while (__atomic_exchange_n(&cache_held, 1u, __ATOMIC_ACQUIRE)) pause_cpu();
}

static void cache_unlock(void) {
    __atomic_store_n(&cache_held, 0u, __ATOMIC_RELEASE);
}

void block_cache_init(void) {
    u64 phys = pmm_alloc_pages(CACHE_ENTRIES * CACHE_BLOCK / PAGE_SIZE);
    if (!phys) { kwarn("block", "no memory for the sector cache; running uncached"); return; }
    u8 *base = phys_to_virt(phys);
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        cache[i].data = base + (size_t)i * CACHE_BLOCK;
        cache[i].valid = false;
        cache[i].dirty = false;
    }
    cache_ready = true;
    kinfo("block", "sector cache: %d blocks of %d bytes", CACHE_ENTRIES, CACHE_BLOCK);
}

/* How much has actually gone to and from the disks.  Counted here, at the one
 * place every read and write passes through, rather than in each driver -
 * where it would be five copies of the same two lines and one of them would be
 * missing. */
u64 g_disk_read_bytes;
u64 g_disk_write_bytes;

/* One disk's driver, entered by one thread at a time.  Spun with interrupts
 * ENABLED - the same reasoning as cache_lock above: the holder keeps being
 * scheduled, the driver waits on a wall-clock deadline the timer advances, and
 * nothing in an interrupt handler touches a disk.  This is what stops the
 * flush and large-transfer paths (which run the driver OUTSIDE cache_lock)
 * from entering the same controller as another thread's cached I/O and
 * corrupting a transfer - the bug that truncated the log on real hardware the
 * moment a second thread touched the same USB. */
static void disk_io_lock(blockdev_t *disk) {
    while (__atomic_exchange_n(&disk->io_held, 1u, __ATOMIC_ACQUIRE)) pause_cpu();
}
static void disk_io_unlock(blockdev_t *disk) {
    __atomic_store_n(&disk->io_held, 0u, __ATOMIC_RELEASE);
}

static int raw_read(blockdev_t *dev, u64 lba, u32 count, void *buf) {
    blockdev_t *disk = dev;
    u64 off = 0;
    while (disk->parent) { off += disk->lba_offset; disk = disk->parent; }
    if (lba + count > dev->sector_count) return -E_INVAL;
    disk_io_lock(disk);
    int r = disk->ops->read(disk, lba + off, count, buf);
    disk_io_unlock(disk);
    if (r >= 0) g_disk_read_bytes += (u64)count * dev->sector_size;
    return r;
}

static int raw_write(blockdev_t *dev, u64 lba, u32 count, const void *buf) {
    blockdev_t *disk = dev;
    u64 off = 0;
    while (disk->parent) { off += disk->lba_offset; disk = disk->parent; }
    if (dev->readonly) return -E_ROFS;
    if (lba + count > dev->sector_count) return -E_INVAL;
    if (!disk->ops->write) return -E_ROFS;
    disk_io_lock(disk);
    int r = disk->ops->write(disk, lba + off, count, buf);
    disk_io_unlock(disk);
    if (r >= 0) g_disk_write_bytes += (u64)count * dev->sector_size;
    return r;
}

static int flush_entry(cache_entry_t *e) {
    if (!e->valid || !e->dirty) return 0;
    u32 spb = CACHE_BLOCK / e->dev->sector_size;
    u64 lba = e->block * spb;
    u32 count = spb;
    /* The last block of a device may be short. */
    if (lba + count > e->dev->sector_count) count = (u32)(e->dev->sector_count - lba);
    int r = raw_write(e->dev, lba, count, e->data);
    if (r < 0) {
        kerr("block", "write-back failed on %s block %lu: %s", e->dev->name, e->block, vfs_strerror(-r));
        return r;
    }
    e->dirty = false;
    return 0;
}

static cache_entry_t *cache_lookup(blockdev_t *dev, u64 block, bool load) {
    if (!cache_ready) return NULL;

    cache_entry_t *victim = NULL;
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        cache_entry_t *e = &cache[i];
        if (e->valid && e->dev == dev && e->block == block) { e->stamp = ++cache_clock; return e; }
        if (!e->valid) { if (!victim) victim = e; continue; }
        if (!victim || (victim->valid && e->stamp < victim->stamp)) victim = e;
    }
    if (!victim) return NULL;

    if (victim->valid && victim->dirty && flush_entry(victim) < 0) return NULL;

    victim->dev = dev;
    victim->block = block;
    victim->dirty = false;
    victim->valid = false;

    if (load) {
        u32 spb = CACHE_BLOCK / dev->sector_size;
        u64 lba = block * spb;
        u32 count = spb;
        if (lba >= dev->sector_count) return NULL;
        if (lba + count > dev->sector_count) count = (u32)(dev->sector_count - lba);
        memset(victim->data, 0, CACHE_BLOCK);
        if (raw_read(dev, lba, count, victim->data) < 0) return NULL;
    } else {
        memset(victim->data, 0, CACHE_BLOCK);
    }
    victim->valid = true;
    victim->stamp = ++cache_clock;
    return victim;
}

void block_cache_flush_all(void) {
    cache_lock();
    for (int i = 0; i < CACHE_ENTRIES; i++) flush_entry(&cache[i]);
    cache_unlock();
    for (blockdev_t *d = devices; d; d = d->next)
        if (!d->parent && d->ops->flush) {
            disk_io_lock(d);
            d->ops->flush(d);
            disk_io_unlock(d);
        }
}

void block_cache_invalidate(blockdev_t *dev) {
    cache_lock();
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        cache_entry_t *e = &cache[i];
        if (!e->valid) continue;
        /* Invalidate the device itself and anything layered on it. */
        bool related = (e->dev == dev);
        for (blockdev_t *p = e->dev; !related && p; p = p->parent) if (p == dev) related = true;
        if (!related) continue;
        flush_entry(e);
        e->valid = false;
    }
    cache_unlock();
}

/* ------------------------------------------------------------------------- */
/* public I/O                                                                */
/* ------------------------------------------------------------------------- */

#define DIRECT_THRESHOLD (64 * 1024)

int block_read(blockdev_t *dev, u64 lba, u32 count, void *buf) {
    if (!dev || !count) return 0;
    if (lba + count > dev->sector_count) {
        kerr("block", "read past the end of %s (lba %lu + %u > %lu)", dev->name, lba, count, dev->sector_count);
        return -E_INVAL;
    }

    if (!cache_ready || (u64)count * dev->sector_size >= DIRECT_THRESHOLD) {
        /* Large transfer: make sure the cache holds nothing newer, then go
         * straight to the device. */
        if (cache_ready) {
            cache_lock();
            u32 spb = CACHE_BLOCK / dev->sector_size;
            for (int i = 0; i < CACHE_ENTRIES; i++) {
                cache_entry_t *e = &cache[i];
                if (!e->valid || e->dev != dev || !e->dirty) continue;
                u64 first = e->block * spb;
                if (first + spb <= lba || first >= lba + count) continue;
                flush_entry(e);
            }
            cache_unlock();
        }
        return raw_read(dev, lba, count, buf);
    }

    u32 spb = CACHE_BLOCK / dev->sector_size;
    u8 *out = buf;
    cache_lock();
    for (u32 done = 0; done < count; ) {
        u64 abs = lba + done;
        u64 block = abs / spb;
        u32 within = (u32)(abs % spb);
        u32 n = spb - within;
        if (n > count - done) n = count - done;

        cache_entry_t *e = cache_lookup(dev, block, true);
        if (!e) { cache_unlock(); return raw_read(dev, lba, count, buf); }
        memcpy(out + (size_t)done * dev->sector_size,
               e->data + (size_t)within * dev->sector_size,
               (size_t)n * dev->sector_size);
        done += n;
    }
    cache_unlock();
    return 0;
}

int block_write(blockdev_t *dev, u64 lba, u32 count, const void *buf) {
    if (!dev || !count) return 0;
    if (dev->readonly) return -E_ROFS;
    if (lba + count > dev->sector_count) return -E_INVAL;

    /* Somebody is writing sectors to this disk, which is a deliberate act -
     * the installer, after it has been told which disk and had that confirmed.
     * From here on the volumes on it are writable too, so that the files can
     * be copied onto what was just partitioned. */
    block_note_write_intent(dev);

    if (!cache_ready || (u64)count * dev->sector_size >= DIRECT_THRESHOLD) {
        if (cache_ready) {
            cache_lock();
            u32 spb = CACHE_BLOCK / dev->sector_size;
            for (int i = 0; i < CACHE_ENTRIES; i++) {
                cache_entry_t *e = &cache[i];
                if (!e->valid || e->dev != dev) continue;
                u64 first = e->block * spb;
                if (first + spb <= lba || first >= lba + count) continue;
                e->valid = false;      /* the device copy is about to win */
                e->dirty = false;
            }
            cache_unlock();
        }
        return raw_write(dev, lba, count, buf);
    }

    u32 spb = CACHE_BLOCK / dev->sector_size;
    const u8 *in = buf;
    cache_lock();
    for (u32 done = 0; done < count; ) {
        u64 abs = lba + done;
        u64 block = abs / spb;
        u32 within = (u32)(abs % spb);
        u32 n = spb - within;
        if (n > count - done) n = count - done;

        /* Only read the block first when the write is partial. */
        bool whole = (within == 0 && n == spb);
        cache_entry_t *e = cache_lookup(dev, block, !whole);
        if (!e) { cache_unlock(); return raw_write(dev, lba, count, buf); }

        memcpy(e->data + (size_t)within * dev->sector_size,
               in + (size_t)done * dev->sector_size,
               (size_t)n * dev->sector_size);
        e->dirty = true;
        done += n;
    }
    cache_unlock();
    return 0;
}

int block_flush(blockdev_t *dev) {
    cache_lock();
    for (int i = 0; i < CACHE_ENTRIES; i++)
        if (cache[i].valid && cache[i].dev == dev) flush_entry(&cache[i]);
    cache_unlock();

    blockdev_t *disk = dev;
    while (disk->parent) disk = disk->parent;
    if (!disk->ops->flush) return 0;
    disk_io_lock(disk);
    int r = disk->ops->flush(disk);
    disk_io_unlock(disk);
    return r;
}

int block_read_bytes(blockdev_t *dev, u64 offset, void *buf, size_t len) {
    u32 ss = dev->sector_size;
    u8 *out = buf;

    while (len) {
        u64 lba = offset / ss;
        u32 within = (u32)(offset % ss);
        size_t n = ss - within;
        if (n > len) n = len;

        if (within == 0 && n == ss) {
            int r = block_read(dev, lba, 1, out);
            if (r < 0) return r;
        } else {
            u8 tmp[4096];
            if (ss > sizeof tmp) return -E_INVAL;
            int r = block_read(dev, lba, 1, tmp);
            if (r < 0) return r;
            memcpy(out, tmp + within, n);
        }
        out += n; offset += n; len -= n;
    }
    return 0;
}

int block_write_bytes(blockdev_t *dev, u64 offset, const void *buf, size_t len) {
    u32 ss = dev->sector_size;
    const u8 *in = buf;

    while (len) {
        u64 lba = offset / ss;
        u32 within = (u32)(offset % ss);
        size_t n = ss - within;
        if (n > len) n = len;

        if (within == 0 && n == ss) {
            int r = block_write(dev, lba, 1, in);
            if (r < 0) return r;
        } else {
            u8 tmp[4096];
            if (ss > sizeof tmp) return -E_INVAL;
            int r = block_read(dev, lba, 1, tmp);
            if (r < 0) return r;
            memcpy(tmp + within, in, n);
            r = block_write(dev, lba, 1, tmp);
            if (r < 0) return r;
        }
        in += n; offset += n; len -= n;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* registry                                                                  */
/* ------------------------------------------------------------------------- */

blockdev_t *block_register(const char *name, const block_ops_t *ops, void *priv,
                           u32 sector_size, u64 sector_count, const char *model) {
    if (sector_size < 512 || sector_size > 4096 || (sector_size & (sector_size - 1))) {
        kerr("block", "%s has an unsupported sector size %u", name, sector_size);
        return NULL;
    }

    blockdev_t *d = kzalloc(sizeof *d);
    if (!d) return NULL;

    strlcpy(d->name, name, sizeof d->name);
    strlcpy(d->model, model ? model : "unknown", sizeof d->model);
    d->ops = ops;
    d->priv = priv;
    d->sector_size = sector_size;
    d->sector_count = sector_count;

    /* Append rather than prepend: everything that lists devices - the shell,
     * the installer, /dev - should show them in discovery order, so disk0
     * comes before disk1 rather than after it. */
    blockdev_t **link = &devices;
    while (*link) link = &(*link)->next;
    *link = d;
    device_count++;

    u64 mb = (sector_count * sector_size) / (1024 * 1024);
    kinfo("block", "%s: %s, %lu MiB (%lu sectors of %u bytes)", d->name, d->model, mb, sector_count, sector_size);

    /* Read its partition table, if the one-off scan at start-up has already
     * been and gone.
     *
     * That scan runs inside block_init(), over whatever AHCI, NVMe and ATA
     * have found by then.  A USB stick is not among them: enumerating a device
     * behind a hub takes seconds, and on the machine this was found on the
     * stick appeared between ten and twenty seconds after start-up - long
     * after the only scan there was.
     *
     * So the disk existed and its partitions did not.  Nothing had read the
     * table, `usb0` had no `usb0p1`, and the search for something to write a
     * log on probed the raw disk instead - where a GPT keeps a protective
     * master boot record and no filesystem at all.  It found nothing and said
     * so, on a stick whose first partition holds the very kernel that was
     * running.  A disk that arrives late is not a special case; it is the
     * normal case for the medium this system is meant to boot from. */
    if (partitions_scanned_at_init && !d->parent)
        block_scan_partitions(d);

    return d;
}

void block_unregister(blockdev_t *dev) {
    block_cache_invalidate(dev);
    blockdev_t **link = &devices;
    while (*link && *link != dev) link = &(*link)->next;
    if (*link) { *link = dev->next; device_count--; kfree(dev); }
}

blockdev_t *block_find(const char *name) {
    for (blockdev_t *d = devices; d; d = d->next)
        if (!strcmp(d->name, name)) return d;
    return NULL;
}

blockdev_t *block_first(void) { return devices; }

/* Whether this device has already been described in the log.
 *
 * Probing is not a one-off: the boot-volume search retries, and opening a file
 * manager or the System window probes every partition again.  Each pass had
 * every filesystem driver reannouncing the same unchanging facts - size,
 * cluster size, that dates are fixed - four lines per partition per pass.  On
 * a machine with five partitions that is twenty lines of nothing new, and it
 * is what pushed a `gpu start` report out of an eight-kilobyte log tail.
 *
 * The facts are still logged, once, the first time each device is seen. */
static blockdev_t *described[32];
static int described_count;

bool block_described(blockdev_t *dev) {
    for (int i = 0; i < described_count; i++)
        if (described[i] == dev) return true;
    return false;
}

void block_mark_described(blockdev_t *dev) {
    if (!dev || block_described(dev)) return;
    if (described_count < 32) described[described_count++] = dev;
}

/* Say, in one place and at a moment that cannot scroll away, why there is or
 * is not a disk to keep a log on.
 *
 * This exists because the answer kept being destroyed before it could be read.
 * The log ring holds 512 entries; the storage drivers have all had their say
 * within the first ten seconds, and by the time anybody asks a machine to shut
 * down those lines are long gone. A snapshot taken at shutdown could describe
 * the last twenty seconds of a desktop being clicked around and nothing else.
 *
 * So the verdict is not recovered from the log - it is re-derived from state
 * the drivers still hold, at the moment it is wanted. `usbmsc_last_stage()` is
 * the exact rung of the ladder the storage driver fell off, and the USB walk
 * below distinguishes the three cases that look identical from outside: no
 * device on any controller at all, a device present that nothing claimed, and
 * a device claimed by this driver that then failed.
 */
void storage_report_verdict(void) {
    kinfo("verdict", "---- why there is or is not a disk to write to ----");

    static usb_devinfo_t seen[24];
    int n = usb_snapshot(seen, 24);
    kinfo("verdict", "USB: %d device(s) enumerated across every controller", n);
    for (int i = 0; i < n; i++) {
        kinfo("verdict", "  port %u slot %u  %04x:%04x  %-28s  %s",
              seen[i].port, seen[i].slot, seen[i].vendor, seen[i].product,
              seen[i].name[0] ? seen[i].name : "(unnamed)",
              seen[i].driver[0] ? seen[i].driver : "NO DRIVER CLAIMED IT");
    }

    const char *usbmsc_last_stage(void);
    kinfo("verdict", "storage driver got as far as: %s", usbmsc_last_stage());

    /* Partitions as well as whole disks.  Listing only the disks hid the fault
     * this was written to find: `usb0` was there and `usb0p1` was not, because
     * nothing had read the stick's partition table, and a report that showed
     * only disks could not tell that from a stick with no partitions on it. */
    int disks = 0, parts = 0;
    for (blockdev_t *d = devices; d; d = d->next) {
        if (d->parent) continue;
        disks++;
        kinfo("verdict", "  disk %-8s %-24s %llu MiB%s%s",
              d->name, d->model[0] ? d->model : "(no model)",
              (unsigned long long)(d->sector_count * d->sector_size / (1024 * 1024)),
              d->readonly ? ", read-only" : "",
              d->removable ? ", removable" : "");
        for (blockdev_t *q = devices; q; q = q->next) {
            if (q->parent != d) continue;
            parts++;
            kinfo("verdict", "    part %-8s %-20s %llu MiB%s",
                  q->name, q->part_label[0] ? q->part_label : "(no label)",
                  (unsigned long long)(q->sector_count * q->sector_size / (1024 * 1024)),
                  q->readonly ? ", read-only" : "");
        }
    }
    kinfo("verdict", "%d whole disk(s) and %d partition(s) known to the block "
                     "layer", disks, parts);

    if (klog_persist_active())
        kinfo("verdict", "the log is being written to %s", klog_persist_path());
    else
        kwarn("verdict", "NO log is being written: nothing writable was found");

    kinfo("verdict", "---- end ----");
}


int block_count(void) { return device_count; }

/* Whole disks only, which is what a summary line should report. */
int block_disk_count(void) {
    int n = 0;
    for (blockdev_t *d = devices; d; d = d->next) if (!d->parent) n++;
    return n;
}

/* ------------------------------------------------------------------------- */
/* partitions                                                                */
/* ------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    char signature[8];      /* "EFI PART" */
    u32  revision;
    u32  header_size;
    u32  header_crc;
    u32  reserved;
    u64  current_lba, backup_lba;
    u64  first_usable, last_usable;
    u8   disk_guid[16];
    u64  entries_lba;
    u32  entry_count;
    u32  entry_size;
    u32  entries_crc;
} gpt_header_t;

typedef struct __attribute__((packed)) {
    u8  type_guid[16];
    u8  part_guid[16];
    u64 first_lba, last_lba;
    u64 attributes;
    u16 name[36];
} gpt_entry_t;

static void register_partition(blockdev_t *disk, u32 index, u64 first, u64 count,
                               const u8 *type_guid, const u8 *part_guid, const char *label) {
    char name[BLOCK_NAME_MAX];
    snprintf(name, sizeof name, "%sp%u", disk->name, index);

    blockdev_t *p = kzalloc(sizeof *p);
    if (!p) return;

    strlcpy(p->name, name, sizeof p->name);
    strlcpy(p->model, disk->model, sizeof p->model);
    p->ops          = disk->ops;
    p->priv         = disk->priv;
    p->sector_size  = disk->sector_size;
    p->sector_count = count;
    p->readonly     = disk->readonly;
    p->parent       = disk;
    p->lba_offset   = first;
    p->part_index   = index;
    if (type_guid) memcpy(p->part_type_guid, type_guid, GUID_LEN);
    if (part_guid) memcpy(p->part_guid, part_guid, GUID_LEN);
    if (label) strlcpy(p->part_label, label, sizeof p->part_label);

    blockdev_t **link = &devices;
    while (*link) link = &(*link)->next;
    *link = p;
    device_count++;

    u64 mb = (count * p->sector_size) / (1024 * 1024);
    if (label && label[0])
        kinfo("block", "%s: %lu MiB, \"%s\"", p->name, mb, label);
    else
        kinfo("block", "%s: %lu MiB at LBA %lu", p->name, mb, first);
}

static int scan_gpt(blockdev_t *disk) {
    u8 sector[4096];
    if (disk->sector_size > sizeof sector) return 0;
    if (block_read(disk, 1, 1, sector) < 0) return 0;

    gpt_header_t *h = (gpt_header_t *)sector;
    if (memcmp(h->signature, "EFI PART", 8)) return 0;
    if (h->entry_size < sizeof(gpt_entry_t) || h->entry_size > 1024) {
        kwarn("block", "%s: GPT entry size %u is implausible", disk->name, h->entry_size);
        return 0;
    }
    u32 count = h->entry_count;
    if (count > 256) count = 256;

    u64 entries_lba = h->entries_lba;
    u32 entry_size = h->entry_size;

    u8 *table = kmalloc((size_t)count * entry_size);
    if (!table) return 0;

    u32 bytes = count * entry_size;
    u32 sectors = (bytes + disk->sector_size - 1) / disk->sector_size;
    if (block_read(disk, entries_lba, sectors, table) < 0) { kfree(table); return 0; }

    int found = 0;
    for (u32 i = 0; i < count; i++) {
        gpt_entry_t *e = (gpt_entry_t *)(table + (size_t)i * entry_size);
        if (guid_is_zero(e->type_guid)) continue;
        if (e->last_lba < e->first_lba) continue;
        if (e->last_lba >= disk->sector_count) continue;

        char label[40];
        int n = 0;
        for (int j = 0; j < 36 && e->name[j] && n < (int)sizeof label - 1; j++)
            if (e->name[j] < 128) label[n++] = (char)e->name[j];
        label[n] = 0;

        register_partition(disk, i + 1, e->first_lba, e->last_lba - e->first_lba + 1,
                           e->type_guid, e->part_guid, label);
        found++;
    }

    kfree(table);
    if (found) kinfo("block", "%s: GPT with %d partition(s)", disk->name, found);
    return found;
}

static int scan_mbr(blockdev_t *disk) {
    u8 sector[4096];
    if (disk->sector_size > sizeof sector) return 0;
    if (block_read(disk, 0, 1, sector) < 0) return 0;
    if (sector[510] != 0x55 || sector[511] != 0xAA) return 0;

    int found = 0;
    for (int i = 0; i < 4; i++) {
        u8 *e = sector + 446 + i * 16;
        u8 type = e[4];
        if (!type || type == 0xEE) continue;      /* 0xEE is a protective MBR */

        u32 first = (u32)(e[8] | (e[9] << 8) | (e[10] << 16) | ((u32)e[11] << 24));
        u32 count = (u32)(e[12] | (e[13] << 8) | (e[14] << 16) | ((u32)e[15] << 24));
        if (!count || first >= disk->sector_count) continue;
        if ((u64)first + count > disk->sector_count) count = (u32)(disk->sector_count - first);

        register_partition(disk, (u32)(i + 1), first, count, NULL, NULL, NULL);
        found++;
    }
    if (found) kinfo("block", "%s: MBR with %d partition(s)", disk->name, found);
    return found;
}

int block_scan_partitions(blockdev_t *disk) {
    if (disk->parent) return 0;
    int n = scan_gpt(disk);
    if (!n) n = scan_mbr(disk);
    if (!n) kinfo("block", "%s: no partition table", disk->name);
    return n;
}

/* ------------------------------------------------------------------------- */
/* bring-up                                                                  */
/* ------------------------------------------------------------------------- */

/* Disk names are handed out from here rather than from each driver.
 *
 * Every storage driver used to keep its own counter, which worked only for as
 * long as a machine had one kind of controller.  This one has NVMe and IDE
 * both, so two different disks were both called "disk0" - and block_find()
 * answers with whichever happens to be nearer the head of the list, meaning a
 * name could quietly refer to the wrong disk.  The IDE driver made it visible;
 * NVMe and AHCI together would have done the same thing. */
static int next_disk_index;

int block_next_disk_index(void) { return next_disk_index++; }
void block_release_disk_index(void) { if (next_disk_index) next_disk_index--; }

void ahci_init(void);
void nvme_init(void);
void ata_init(void);

void block_init(void) {
    block_cache_init();
    ahci_init();
    nvme_init();
    /* Last of the three, so that disk numbering puts the fast controllers
     * first on a machine that has both - and because a legacy IDE controller
     * is very often present with nothing attached to it. */
    ata_init();

    /* Snapshot the disk list first: scanning adds partitions to the same list. */
    blockdev_t *disks[32];
    int n = 0;
    for (blockdev_t *d = devices; d && n < 32; d = d->next)
        if (!d->parent) disks[n++] = d;
    for (int i = 0; i < n; i++) block_scan_partitions(disks[i]);

    /* From here on a disk scans itself as it arrives - USB storage always
     * turns up after this point. */
    partitions_scanned_at_init = true;

    if (!device_count) kwarn("block", "no storage devices found");
}

/* Find the partition the boot configuration named, so the system mounts the
 * same volume it was installed to rather than guessing. */
static blockdev_t *find_data_partition(void) {
    const char *spec = g_boot.bootdev[0] ? g_boot.bootdev : NULL;

    if (spec && !strncmp(spec, "PARTUUID=", 9)) {
        u8 want[GUID_LEN];
        if (guid_parse(spec + 9, want)) {
            for (blockdev_t *d = devices; d; d = d->next)
                if (d->parent && guid_equal(d->part_guid, want)) return d;
            kwarn("block", "no partition matches %s", spec);
        } else {
            kwarn("block", "cannot parse %s", spec);
        }
    } else if (spec && !strncmp(spec, "/dev/", 5)) {
        blockdev_t *d = block_find(spec + 5);
        if (d) return d;
        kwarn("block", "no such device %s", spec);
    }

    /* Fall back to the first partition carrying our own type GUID. */
    for (blockdev_t *d = devices; d; d = d->next)
        if (d->parent && guid_equal(d->part_type_guid, GPT_TYPE_KESTREL)) return d;
    return NULL;
}

/* Everything, not just the warnings.  On an installed system the log is a
 * record kept over months and the warnings are the useful part; on a stick
 * booted onto unfamiliar hardware it is the only account of what happened, and
 * the ordinary lines - which card, how much memory, how far each driver got -
 * are the entire point. */
static void start_boot_log(void) {
    klog_persist_level(KLOG_DEBUG);
    klog_persist_enable("/boot/KESTREL/KERNEL.LOG");
    kinfo("log", "writing this boot to KESTREL\\KERNEL.LOG on the boot "
                 "volume; read it on another machine afterwards");
}

/* The volume the system was booted from, when there is no installed one.
 *
 * A machine booted from a USB stick has no data partition, so until now it ran
 * entirely from memory and everything the kernel had to say died with the
 * power.  That is exactly backwards for the boot that matters most: the first
 * one on hardware nobody has run this on, where the whole question is what the
 * drivers found and there is no serial cable to ask down.
 *
 * The stick itself is writable and was written by the loader on the way in, so
 * it is somewhere to put the answer.  Finding it does not need the firmware's
 * boot device plumbed through: the volume this system booted from is the one
 * with this system's kernel on it, and checking for the file is both simpler
 * and harder to get wrong than matching identifiers.
 */
/* ---------------------------------------------------- not our disks
 *
 * This machine boots from a stick and the disks already in it belong to
 * somebody else's operating system.  Until recently that was academic: the
 * NTFS driver could change bytes in a file that already existed and could not
 * create one, so the worst a mistake could do was bounded.
 *
 * It is not academic now.  The driver creates files, allocates clusters,
 * rewrites directory indexes and grows the MFT - on any NTFS volume it
 * mounts, and a PC that dual boots is full of NTFS volumes holding an
 * installed Windows.
 *
 * So the default is inverted.  A volume is mounted read-only unless it is on
 * the disk this system was booted from; everything else can be read and
 * nothing else can be touched.  The installer is unaffected: it writes
 * sectors through its own path after asking, which is a deliberate act, not
 * an accident of having mounted something.
 *
 * `writeanywhere` on the command line turns the guard off, for the case where
 * somebody means it.
 */
static char boot_disk_name[24];

/* "disk0p2" and "disk0" are the same disk; the partition suffix is dropped. */
static void whole_disk_name(const char *partition, char *out, size_t cap) {
    size_t n = 0;
    while (partition[n] && n + 1 < cap) {
        if (partition[n] == 'p' && n > 0 && partition[n + 1] >= '0' &&
            partition[n + 1] <= '9')
            break;
        out[n] = partition[n];
        n++;
    }
    out[n] = 0;
}

#define MAX_UNLOCKED 4
static char unlocked[MAX_UNLOCKED][24];
static int unlocked_count;

void block_note_write_intent(const blockdev_t *d) {
    if (!d) return;

    char mine[24];
    whole_disk_name(d->name, mine, sizeof mine);

    for (int i = 0; i < unlocked_count; i++)
        if (!strcmp(unlocked[i], mine)) return;
    if (unlocked_count >= MAX_UNLOCKED) return;

    strlcpy(unlocked[unlocked_count++], mine, sizeof unlocked[0]);
    kinfo("block", "%s has had sectors written to it directly, so volumes on "
                   "it are no longer held read-only - that only happens when "
                   "something was told to write to this disk", mine);
}

void block_note_boot_disk(const char *devname) {
    if (boot_disk_name[0]) return;
    whole_disk_name(devname, boot_disk_name, sizeof boot_disk_name);
    kinfo("block", "booted from %s; every other disk is mounted read-only so "
                   "that nothing here can write to an operating system that "
                   "is not this one", boot_disk_name);
}

bool block_writes_allowed(const blockdev_t *d) {
    if (cmdline_has("writeanywhere")) return true;
    if (!d) return false;

    /* Before the boot disk is known, nothing is writable.  That order is
     * deliberate: the window where it is unknown is exactly the window where
     * a wrong guess would be unnoticed. */
    if (!boot_disk_name[0]) return false;

    char mine[24];
    whole_disk_name(d->name, mine, sizeof mine);
    if (!strcmp(mine, boot_disk_name)) return true;

    for (int i = 0; i < unlocked_count; i++)
        if (!strcmp(unlocked[i], mine)) return true;
    return false;
}

static filesystem_t *find_boot_volume(blockdev_t **which) {
    for (blockdev_t *d = devices; d; d = d->next) {
        /* Every partition, and every whole disk that has no partition table
         * of its own.  This used to insist on the EFI-System partition type,
         * which is a GPT idea - and a USB stick formatted the ordinary way has
         * a master boot record, where that type does not exist and the test
         * could never match.  It found nothing on the first machine it ran on,
         * which is the sort of thing an identifier check does and looking for
         * the file does not.  So the file is the test, exactly as the note
         * above says it should be. */
        filesystem_t *fs = vfs_probe(d);
        if (!fs) continue;

        /* Mount it somewhere temporary to look, because "is our kernel on it"
         * is a question about files, not about partition tables. */
        vfs_mkdir("/boot");
        if (vfs_mount("/boot", fs) < 0) {
            if (fs->unmount) fs->unmount(fs);
            continue;
        }

        vstat_t st;
        if (vfs_stat("/boot/KESTREL/KERNEL.ELF", &st) == 0) {
            if (which) *which = d;
            block_note_boot_disk(d->name);

            /* It was probed before its own disk was known, so the guard will
             * have marked it read-only along with everything else.  Now that
             * the answer is in, this is the one volume that guard was never
             * about. */
            if (block_writes_allowed(d) && fs->readonly) {
                fs->readonly = false;
                /* Said out loud, because the guard has already announced the
                 * opposite about this volume a line or two above - it had to
                 * decide before the answer existed. */
                kinfo("vfs", "%s is the volume this system booted from, so it "
                             "is writable after all", d->name);
            }
            return fs;
        }

        /* vfs_unmount calls the filesystem's own unmount, so calling it
         * again here frees everything a second time.  That is where the
         * "double free" errors on the first real machine came from: this
         * runs once per partition that is not the boot volume, and there
         * were four of them. */
        vfs_unmount("/boot");
    }
    return NULL;
}

/* Start writing the log to the boot volume as early as there is one.
 *
 * The reason this is separate from mounting the installed system is entirely
 * about where a first boot goes wrong.  Bringing up USB, sound, the network
 * and the wireless all happen after storage, and on a machine nobody has
 * booted this on, any of them is a plausible place to stop dead - which is
 * precisely the case where the log matters and precisely the case where a log
 * that only starts afterwards contains nothing.
 *
 * So this runs the moment the disks are known, and everything from that point
 * on is on the stick by the time it happens rather than after.
 */
bool block_start_boot_log(void) {
    blockdev_t *boot = NULL;
    if (!find_boot_volume(&boot)) return false;
    kinfo("block", "logging this boot onto the volume it came from (%s)",
          boot->name);
    start_boot_log();
    return true;
}

/* Say what filesystem each volume holds.
 *
 * Only two volumes are ever mounted by name - the one booted from and the one
 * the system was installed onto - so every other volume in the machine went
 * unmentioned, whether it held something readable or nothing at all.  On a
 * machine where the expected volume is not found, that left no way to tell a
 * disk this kernel cannot read from a disk it never looked at, which are very
 * different problems.
 *
 * Each volume is probed and then let go again.  Probing is reads only, and the
 * filesystem is unmounted immediately, so this says what is there without
 * taking anything or changing anything. */
static void survey_volumes(void) {
    for (blockdev_t *d = devices; d; d = d->next) {
        filesystem_t *fs = vfs_probe(d);
        if (!fs) {
            kinfo("block", "%s: no filesystem this kernel understands", d->name);
            continue;
        }

        u64 total = fs->total_bytes ? fs->total_bytes(fs) : 0;

        /* And how many things are in its root.
         *
         * Reading the root is what separates a volume whose header parsed from
         * one that can actually be used: the header is a few dozen bytes near
         * the front, and everything hard about a filesystem is between there
         * and a list of names. */
        int entries = -1;
        if (fs->root && fs->root->ops && fs->root->ops->readdir) {
            entries = 0;
            for (u32 i = 0; i < 4096; i++) {
                dirent_k e;
                if (fs->root->ops->readdir(fs->root, i, &e) < 0) break;
                entries++;
            }
        }

        if (entries >= 0)
            kinfo("block", "%s: %s%s, %llu MiB, %d entr%s in its root",
                  d->name, fs->name, fs->readonly ? " (read only)" : "",
                  (unsigned long long)(total / (1024 * 1024)),
                  entries, entries == 1 ? "y" : "ies");
        else
            kinfo("block", "%s: %s%s, %llu MiB", d->name, fs->name,
                  fs->readonly ? " (read only)" : "",
                  (unsigned long long)(total / (1024 * 1024)));

        if (fs->unmount) fs->unmount(fs);
    }
}

/* Put the log on the volume this system was booted from.
 *
 * Used whenever the installed data partition cannot take it.  A stick
 * booted onto unfamiliar hardware is exactly the case where the log is the
 * only account of what happened, so "the preferred place did not work" must
 * not end with no log at all - it ends here.
 */
static void fall_back_to_boot_volume(const char *why) {
    if (klog_persist_active()) return;

    /* The boot volume was already found and LEFT MOUNTED at /boot when the boot
     * disk was identified earlier in block_mount_system_volumes.  Re-probing for
     * it here calls find_boot_volume again, which tries to mount /boot a second
     * time - and that fails because it is already mounted, making a perfectly
     * writable FAT boot volume look unwritable and losing the whole log.  So if
     * the boot disk is already known, write straight onto the mounted /boot
     * rather than hunting for it again. */
    if (boot_disk_name[0]) {
        kinfo("block", "%s, so the log goes onto the volume this was booted "
                       "from (%s) instead", why, boot_disk_name);
        start_boot_log();
        return;
    }

    blockdev_t *boot = NULL;
    if (!find_boot_volume(&boot)) {
        kwarn("block", "%s, and the volume this was booted from is not "
                       "writable either; this boot will leave no record",
              why);
        return;
    }

    kinfo("block", "%s, so the log goes onto the volume this was booted "
                   "from (%s) instead", why, boot->name);
    start_boot_log();
}

void block_mount_system_volumes(void) {
    /* /dev entries for everything, so the shell and installer can see them. */
    for (blockdev_t *d = devices; d; d = d->next) {
        /* devfs registration happens in devfs.c via block_devfs_publish */
        (void)d;
    }

    /* Which disk this system came from, before anything is mounted for use.
     *
     * Order matters here in a way that is easy to miss.  Volumes are mounted
     * read-only unless they are on the boot disk, and "is it on the boot
     * disk" cannot be answered until the boot disk has been found - so
     * anything mounted before this point is mounted read-only whatever it is,
     * including the data partition sitting on the same stick.  That produces
     * a log with nowhere to go, which is exactly the symptom this guard must
     * not cause. */
    if (!boot_disk_name[0]) {
        blockdev_t *boot = NULL;
        filesystem_t *bfs = find_boot_volume(&boot);
        if (bfs) {
            /* find_boot_volume leaves it mounted at /boot, which is where it
             * belongs; nothing to undo. */
            (void)bfs;
        } else {
            kwarn("block", "the disk this system booted from could not be "
                           "identified, so every volume stays read-only - "
                           "add 'writeanywhere' to the command line to "
                           "override that");
        }
    }

    survey_volumes();

    blockdev_t *data = find_data_partition();
    if (!data) {
        /* Nothing installed.  Fall back to the stick this was booted from, so
         * that a bare-metal boot still leaves a record of what happened - read
         * afterwards on any machine that takes a USB stick. */
        if (klog_persist_active()) return;   /* already started, earlier */

        blockdev_t *boot = NULL;
        filesystem_t *bfs = find_boot_volume(&boot);
        if (!bfs) {
            kinfo("block", "no persistent data partition and the volume this "
                           "was booted from is not writable; running entirely "
                           "from RAM, and nothing said here will outlive the "
                           "power");
            return;
        }

        kinfo("block", "no installed system, so the log goes back onto the "
                       "volume this was booted from (%s)", boot->name);
        start_boot_log();
        return;
    }

    filesystem_t *fs = vfs_probe(data);
    if (!fs) {
        kerr("block", "%s holds no filesystem this kernel understands", data->name);
        fall_back_to_boot_volume("its data partition holds no filesystem "
                                 "this kernel can write");
        return;
    }

    vfs_mkdir("/data");
    if (vfs_mount("/data", fs) < 0) {
        kerr("block", "cannot mount %s at /data", data->name);
        if (fs->unmount) fs->unmount(fs);
        fall_back_to_boot_volume("its data partition would not mount");
        return;
    }

    char guid[40];
    guid_format(data->part_guid, guid, sizeof guid);
    kinfo("block", "mounted %s (%s) at /data as %s", data->name, guid, fs->name);

    /* If that volume is NTFS, prove the writer works on it before anything
     * depends on it.
     *
     * This runs HERE and nowhere else because here is the one place the answer
     * to "may this be written to" is already known: it is this system's own
     * data partition, found by its own type GUID, mounted writable.  A test
     * that went looking for any NTFS volume would eventually find somebody's
     * Windows disk and write into it.
     *
     * The distinction is worth the paragraph: NTFS write support is thousands
     * of lines that had never created a file on a mounted volume, because the
     * machines this is tested on boot from FAT and any NTFS disk attached to
     * them is held read-only for not being the boot disk.  On a stick whose
     * data volume is NTFS, this is the code the log goes through. */
    if (!fs->readonly && !strcmp(fs->name, "ntfs")) {
        int ntfs_live_selftest(const char *dir);
        if (ntfs_live_selftest("/data"))
            kerr("block", "%s is NTFS and this kernel cannot write to it "
                          "correctly; the log and anything else kept here "
                          "would be damaged", data->name);
    }

    /* Turn on persistent event logging now that there is somewhere to put it.
     *
     * `nodatalog` on the command line skips this, which is how the fallback
     * below is tested.  Without it that path could only be reached by a
     * filesystem that refuses to create files - which is exactly the machine
     * this matters on, an NTFS data volume, and exactly the machine no test
     * runs against.  A flag is a poor substitute for the real thing and a much
     * better one than never exercising it at all. */
    if (cmdline_has("nodatalog")) {
        kinfo("log", "asked not to use the data volume for the log");
    } else if (klog_persist_active()) {
        /* Already going somewhere, and somewhere better.
         *
         * The boot volume was picked up long before this point - as soon as
         * the disk this system came from was identified - and the log has been
         * accumulating there since. Moving it to the data volume now is a
         * downgrade for the one thing a log is for.
         *
         * A log is read after the machine is off and the stick has been
         * carried to another computer. The boot volume is FAT, which every
         * operating system mounts without being asked. The data volume
         * carries this system's OWN partition type, and Windows will not so
         * much as give it a drive letter - so a log written there can only be
         * read back by the system that wrote it, which is the one situation
         * where it is least needed.
         *
         * It is also the volume the loader has already written BOOT.LOG to, so
         * it is known-writable on this exact hardware before the kernel tries
         * anything at all. */
        kinfo("log", "the log stays on the volume this booted from, which any "
                     "machine can read - the data volume carries this system's "
                     "own type and other systems will not mount it");
    } else if (boot_disk_name[0]) {
        /* The boot volume was identified above and is left mounted at /boot.
         * It is FAT - reliable, known-writable, and readable by any OS - so the
         * log belongs there rather than on the data volume, whose NTFS this
         * kernel writes only unreliably (a Windows-formatted volume's log opens
         * and then stops taking lines partway through).  Write straight onto
         * the already-mounted /boot; do NOT re-probe for it (that remounts
         * /boot and fails).  This is the FAT KERNEL.LOG the read-back reads. */
        start_boot_log();
    } else {
        vfs_mkdir("/data/logs");
        klog_persist_enable("/data/logs/events.log");
    }

    /* And if that did not take - a read-only mount, a full volume, a
     * filesystem this kernel reads but cannot write - the boot volume is
     * still there.  Leaving logging off because the preferred destination
     * failed is how a boot that most needed explaining produced no record
     * at all, and the desktop said "log on disk: NO" without saying why. */
    if (!klog_persist_active()) {
        kwarn("log", "%s mounted but will not take the log; falling back "
                     "to the volume this was booted from", data->name);
        fall_back_to_boot_volume("its data partition would not take the log");
        return;
    }

    char stamp[32];
    time_format(stamp, sizeof stamp, time_unix_seconds());

    /* Where it ACTUALLY went, asked rather than assumed.
     *
     * This line named /data/logs/events.log whatever the truth was, which it
     * got away with for as long as that was the only destination.  It is not
     * any more, and a line that says where the log is has to be right about
     * it - somebody reads that line precisely when they are trying to find
     * the log. */
    kinfo("log", "persistent event log opened at %s (%s UTC)",
          klog_persist_path(), stamp);
}
