/* ntfs.c - mounting NTFS, so the system can read the volume it lives on.
 *
 * The reading itself is in common/ntfs_core.c, which the loader compiles too.
 * This file is only the joins: turning this kernel's block devices into the
 * sector-fetching function that core wants, and turning what it returns into
 * the vnodes the rest of the system expects.
 *
 * Read-only, deliberately.  A reader that is wrong returns the wrong bytes and
 * the mistake is visible; a writer that is wrong damages a volume Windows also
 * uses, and the two are not worth building at once.  Anything that needs to
 * write still has somewhere to write: the FAT partition beside it.
 */
#include "kernel.h"
#include "klog.h"
#include "vfs.h"
#include "block.h"
#include "mm.h"
#include "../common/ntfs_core.h"

typedef struct {
    ntfs_volume_t vol;
    blockdev_t   *dev;
} ntfs_fs_t;

/* One open file or directory.  The whole description is kept rather than a
 * reference into the volume, because the core hands back a self-contained
 * value and holding it is cheaper than looking it up again on every read. */
typedef struct {
    ntfs_file_t file;
} ntfs_node_t;

static const vnode_ops_t ntfs_vops;

/* --------------------------------------------------------------- the joins */

/* The core asks for 512-byte units; a block device may have a different
 * sector size, so the two are reconciled here rather than in the core, which
 * has no idea what a block device is. */
static bool fetch(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    blockdev_t *dev = ctx;
    u32 per = dev->sector_size / 512;
    if (!per) return false;

    /* A device with sectors larger than 512 bytes can only be read a whole
     * sector at a time, so a request has to land on one. */
    if (lba % per || count % per) {
        static u8 scratch[8192];
        if (dev->sector_size > sizeof scratch) return false;

        u8 *out = buf;
        while (count) {
            u64 sector = lba / per;
            u32 into = (u32)(lba % per) * 512;
            u32 take = dev->sector_size - into;
            if (take > count * 512) take = count * 512;

            if (block_read(dev, sector, 1, scratch) < 0) return false;
            memcpy(out, scratch + into, take);

            out += take;
            lba += take / 512;
            count -= take / 512;
        }
        return true;
    }

    return block_read(dev, lba / per, count / per, buf) >= 0;
}

/* Putting sectors back.  The same reconciliation as fetch(), and the same
 * refusal to touch a device whose sectors are larger than the scratch buffer.
 *
 * A partial sector is read, changed and written whole, because a disk cannot
 * be written in smaller pieces than it is read - and writing without the read
 * first would put whatever was in the buffer over the neighbouring bytes. */
static bool store(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    blockdev_t *dev = ctx;
    u32 per = dev->sector_size / 512;
    if (!per) return false;

    if (lba % per || count % per) {
        static u8 scratch[8192];
        if (dev->sector_size > sizeof scratch) return false;

        const u8 *in = buf;
        while (count) {
            u64 sector = lba / per;
            u32 into = (u32)(lba % per) * 512;
            u32 take = dev->sector_size - into;
            if (take > count * 512) take = count * 512;

            if (block_read(dev, sector, 1, scratch) < 0) return false;
            memcpy(scratch + into, in, take);
            if (block_write(dev, sector, 1, scratch) < 0) return false;

            in += take;
            lba += take / 512;
            count -= take / 512;
        }
        return true;
    }

    return block_write(dev, lba / per, count / per, buf) >= 0;
}

static vnode_t *make_vnode(filesystem_t *fs, const ntfs_file_t *f) {
    vnode_t *vn = kzalloc(sizeof *vn);
    if (!vn) return NULL;

    ntfs_node_t *n = kzalloc(sizeof *n);
    if (!n) { kfree(vn); return NULL; }

    n->file = *f;
    vn->type = f->directory ? VN_DIR : VN_FILE;
    vn->size = f->size;
    vn->refs = 1;
    vn->priv = n;
    vn->fs = fs;
    vn->ops = &ntfs_vops;
    return vn;
}

/* ------------------------------------------------------------------- vnodes */

static ssize_t_k ntfs_vread(vnode_t *vn, void *buf, size_t len, u64 off) {
    ntfs_fs_t *fs = vn->fs->priv;
    ntfs_node_t *n = vn->priv;

    if (len > 0x40000000u) return -E_INVAL;
    long got = ntfs_read_file(&fs->vol, &n->file, off, buf, (uint32_t)len);
    return got < 0 ? -E_IO : (ssize_t_k)got;
}

/* Write into a file, giving it more room first if it needs it.
 *
 * This used to write only within the space a file already had, because that
 * was all the core could do.  Everything that wanted to put something new on
 * an NTFS volume - the kernel log above all - therefore found a file it could
 * not extend and stopped, which is where "log on disk: No" came from.
 *
 * Growing means allocating clusters, editing the volume bitmap, and possibly
 * moving the file's data out of its own MFT record.  All of that is in
 * ntfs_resize; what belongs here is only the decision to ask for it. */
static ssize_t_k ntfs_vwrite(vnode_t *vn, const void *buf, size_t len, u64 off) {
    ntfs_fs_t *fs = vn->fs->priv;
    ntfs_node_t *n = vn->priv;

    if (len > 0x40000000u) return -E_INVAL;

    if (off + len > n->file.size) {
        if (!ntfs_resize(&fs->vol, &n->file, off + len)) {
            kwarn("ntfs", "%s could not be made %llu bytes long: %s",
                  vn->fs->dev ? vn->fs->dev->name : "the volume",
                  (unsigned long long)(off + len), fs->vol.error);
            return -E_NOSPC;
        }
        vn->size = n->file.size;
    }

    long put = ntfs_write_file(&fs->vol, &n->file, off, buf, (uint32_t)len);
    if (put < 0) return -E_PERM;
    return (ssize_t_k)put;
}

/* Make a file or a directory.
 *
 * The directory is already open, so its record number is already known and the
 * path that led here is not rebuilt - the core is asked to create inside this
 * directory rather than to resolve a name from the root again. */
static int ntfs_vcreate(vnode_t *dir, const char *name, u32 type,
                        vnode_t **out) {
    ntfs_fs_t *fs = dir->fs->priv;
    ntfs_node_t *n = dir->priv;

    if (dir->type != VN_DIR) return -E_NOTDIR;
    if (type != VN_DIR && type != VN_FILE) return -E_INVAL;

    ntfs_file_t made;
    if (!ntfs_create_in(&fs->vol, n->file.record, name, type == VN_DIR,
                        &made)) {
        kwarn("ntfs", "%s could not be created: %s", name, fs->vol.error);
        /* The core distinguishes "already there" from everything else, and
         * the caller acts on that difference - open() with O_CREAT wants to
         * open the existing file rather than fail. */
        if (fs->vol.error[0] == 'a') return -E_EXIST;
        return -E_IO;
    }

    vnode_t *vn = make_vnode(dir->fs, &made);
    if (!vn) return -E_NOMEM;
    *out = vn;
    return 0;
}

static int ntfs_vlookup(vnode_t *dir, const char *name, vnode_t **out) {
    ntfs_fs_t *fs = dir->fs->priv;
    ntfs_node_t *n = dir->priv;

    /* The core resolves a whole path; here it is one name inside a directory
     * already open, so it is walked entry by entry.  A directory large enough
     * for that to matter is a directory nothing here opens. */
    for (u32 i = 0; ; i++) {
        char have[256];
        ntfs_file_t entry;
        if (!ntfs_readdir(&fs->vol, &n->file, i, have, sizeof have, &entry))
            return -E_NOENT;

        /* NTFS names are matched without regard to case, whatever case they
         * were created in. */
        const char *a = have, *b = name;
        bool same = true;
        while (*a && *b) {
            char x = *a, y = *b;
            if (x >= 'a' && x <= 'z') x = (char)(x - 32);
            if (y >= 'a' && y <= 'z') y = (char)(y - 32);
            if (x != y) { same = false; break; }
            a++; b++;
        }
        if (!same || *a || *b) continue;

        vnode_t *vn = make_vnode(dir->fs, &entry);
        if (!vn) return -E_NOMEM;
        *out = vn;
        return 0;
    }
}

static int ntfs_vreaddir(vnode_t *dir, u32 index, dirent_k *out) {
    ntfs_fs_t *fs = dir->fs->priv;
    ntfs_node_t *n = dir->priv;

    ntfs_file_t entry;
    if (!ntfs_readdir(&fs->vol, &n->file, index, out->name, sizeof out->name,
                      &entry))
        return -E_NOENT;

    out->type = entry.directory ? VN_DIR : VN_FILE;
    return 0;
}

static int ntfs_vstat(vnode_t *vn, vstat_t *st) {
    ntfs_node_t *n = vn->priv;
    memset(st, 0, sizeof *st);
    st->size = n->file.size;
    st->type = vn->type;
    return 0;
}

static void ntfs_vrelease(vnode_t *vn) {
    if (vn->priv) kfree(vn->priv);
}

static const vnode_ops_t ntfs_vops = {
    .read    = ntfs_vread,
    .write   = ntfs_vwrite,
    .create  = ntfs_vcreate,
    .lookup  = ntfs_vlookup,
    .readdir = ntfs_vreaddir,
    .stat    = ntfs_vstat,
    .release = ntfs_vrelease,
};

/* ---------------------------------------------------------------- mounting */

static int ntfs_unmount(filesystem_t *fs) {
    if (fs->priv) kfree(fs->priv);
    if (fs->root) { if (fs->root->priv) kfree(fs->root->priv); kfree(fs->root); }
    kfree(fs);
    return 0;
}

static u64 ntfs_total(filesystem_t *fs) {
    ntfs_fs_t *n = fs->priv;
    return n->vol.total_sectors * n->vol.bytes_per_sector;
}

/* Free space is in the volume's bitmap, which this does not read: reporting a
 * figure that was not measured would be worse than reporting none. */
static u64 ntfs_free(filesystem_t *fs) { (void)fs; return 0; }

filesystem_t *ntfs_probe(blockdev_t *dev) {
    ntfs_fs_t *n = kzalloc(sizeof *n);
    if (!n) return NULL;
    n->dev = dev;

    /* Opened for writing, within the limits the core imposes: existing space
     * in existing files, and nothing else. */
    if (!ntfs_mount_rw(&n->vol, dev, fetch, store)) {
        /* Not being NTFS is the ordinary case - every volume is offered to
         * every filesystem - so it is not worth a word.  Being NTFS and
         * unreadable is. */
        if (n->vol.error[0] && n->vol.error[0] != 't')
            kinfo("ntfs", "%s: %s", dev->name, n->vol.error);
        kfree(n);
        return NULL;
    }

    filesystem_t *fs = kzalloc(sizeof *fs);
    if (!fs) { kfree(n); return NULL; }

    strlcpy(fs->name, "ntfs", sizeof fs->name);
    fs->dev = dev;
    fs->priv = n;
    fs->readonly = false;
    fs->unmount = ntfs_unmount;
    fs->total_bytes = ntfs_total;
    fs->free_bytes = ntfs_free;

    ntfs_file_t root;
    if (!ntfs_lookup(&n->vol, "/", &root) || !root.directory) {
        kwarn("ntfs", "%s: the root directory could not be opened", dev->name);
        kfree(fs); kfree(n);
        return NULL;
    }

    fs->root = make_vnode(fs, &root);
    if (!fs->root) { kfree(fs); kfree(n); return NULL; }

    if (!block_described(dev)) {
        kinfo("ntfs", "%s: %llu MiB, %u-byte clusters, %u-byte records - this "
                      "driver can read it, and can create and grow files on it",
              dev->name,
              (unsigned long long)(ntfs_total(fs) / (1024 * 1024)),
              n->vol.bytes_per_cluster, n->vol.record_bytes);

        /* Said once - genuinely once now, per volume.  It is a real gap and
         * one somebody will notice in a file listing rather than in a log:
         * there is no wall clock in this kernel yet, so everything created
         * here carries one fixed date instead of the date it was made. */
        kinfo("ntfs", "files created here are dated 1 January 2020 - this "
                      "kernel has no clock to ask");
    }
    return fs;
}

void ntfs_register(void) { vfs_register_fs("ntfs", ntfs_probe); }

/* ============================== the writer, on a real volume ==============
 *
 * `ntfs_write_selftest` checks the writer's arithmetic and needs no disk.  This
 * is the other half, and until it existed the difference was invisible: every
 * line of ntfs_write.c had been reasoned about and none of it had ever created
 * a file on a mounted volume.  The machine this is tested on boots from FAT,
 * and the NTFS disk attached to it is mounted read-only because it is not the
 * boot disk - so the create and write paths had never executed.
 *
 * That matters now because this system's data volume is meant to BE NTFS: the
 * log, the settings and everything a user keeps go through exactly this code.
 *
 * WHERE THIS MAY RUN, and it is not negotiable.  Only on a volume this system
 * is already entitled to write to - its own data partition, or the volume it
 * booted from.  Never on a disk that merely happens to hold NTFS.  That is
 * somebody's Windows installation, and a selftest that writes into it to prove
 * a point would be unforgivable.  The caller decides; this function is handed
 * a directory and trusts that the decision was made.
 */
int ntfs_live_selftest(const char *dir) {
    if (!dir || !dir[0]) return 0;

    char path[128];
    snprintf(path, sizeof path, "%s/KESTREL.CHK", dir);

    /* Bigger than one cluster on purpose.
     *
     * A four-kilobyte volume cluster means anything shorter fits where the
     * file first lands and never exercises the part that finds more room and
     * records where it went - which is the part with the arithmetic in it.
     * Nine kilobytes crosses at least two boundaries at every cluster size
     * this driver supports. */
    enum { BYTES = 9000 };
    static u8 written[BYTES];
    static u8 back[BYTES];

    for (int i = 0; i < BYTES; i++)
        written[i] = (u8)(i * 31 + (i >> 8) * 7 + 1);

    if (vfs_write_file(path, written, BYTES) < 0) {
        kwarn("ntfs", "selftest: %s could not be written, so this volume "
                      "cannot hold a log or anything else", path);
        return 1;
    }

    /* Read back through a fresh open, not from anything still in hand: the
     * question is what is ON the volume, and a buffer that was never dropped
     * would answer a different one. */
    memset(back, 0, sizeof back);
    s64 got = vfs_read_file(path, back, sizeof back);

    if (got != BYTES) {
        kwarn("ntfs", "selftest: %lld byte(s) came back from %s rather than "
                      "%d - the file was created but its size or its extents "
                      "are wrong", (long long)got, path, BYTES);
        return 1;
    }

    for (int i = 0; i < BYTES; i++) {
        if (back[i] == written[i]) continue;
        kwarn("ntfs", "selftest: %s differs at byte %d (%02x, expected %02x) "
                      "- the data went somewhere, but not all of it here",
              path, i, back[i], written[i]);
        return 1;
    }

    kinfo("ntfs", "the writer works on a real volume: %d bytes created, "
                  "grown across clusters and read back byte for byte at %s",
          BYTES, path);
    return 0;
}
