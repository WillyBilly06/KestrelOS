/* exfatfs.c - exFAT, as a filesystem this system can mount.
 *
 * The format itself is understood in common/exfat.c, which the loader could
 * also use; this is the part that turns it into something the VFS can mount,
 * open and list.  Same arrangement as ntfs.c next door, and for the same
 * reason: one implementation of the format, two callers that reach a disk
 * differently.
 *
 * Read-only, and that is stated rather than implied.  Writing exFAT means the
 * allocation bitmap, the table, and directory entries that come in sets of
 * three with a checksum over them - which is a piece of work of its own, and
 * one that damages a volume somebody else's camera also writes to when it is
 * wrong.  A volume this can read and not write is useful; a volume it writes
 * incorrectly is not.
 */
#include "kernel.h"
#include "klog.h"
#include "vfs.h"
#include "block.h"
#include "mm.h"
#include "../common/exfat.h"

typedef struct {
    exfat_volume_t vol;
    blockdev_t    *dev;
} exfat_fs_t;

typedef struct {
    exfat_file_t file;
} exfat_node_t;

static const vnode_ops_t exfat_vops;

/* The block layer speaks in the device's own sectors; the format speaks in
 * 512-byte units.  A device with larger sectors is read a sector at a time and
 * the piece that was asked for taken out of it. */
static bool fetch(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    blockdev_t *dev = ctx;
    if (!dev) return false;

    if (dev->sector_size == 512)
        return block_read(dev, lba, count, buf) >= 0;

    static u8 scratch[4096];
    if (dev->sector_size > sizeof scratch) return false;

    u32 per = dev->sector_size / 512;
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

static vnode_t *make_vnode(filesystem_t *fs, const exfat_file_t *f) {
    vnode_t *vn = kzalloc(sizeof *vn);
    if (!vn) return NULL;

    exfat_node_t *n = kzalloc(sizeof *n);
    if (!n) { kfree(vn); return NULL; }

    n->file = *f;
    vn->type = f->directory ? VN_DIR : VN_FILE;
    vn->size = f->size;
    vn->refs = 1;
    vn->priv = n;
    vn->fs = fs;
    vn->ops = &exfat_vops;
    return vn;
}

static ssize_t_k exfat_vread(vnode_t *vn, void *buf, size_t len, u64 off) {
    exfat_fs_t *fs = vn->fs->priv;
    exfat_node_t *n = vn->priv;

    if (len > 0x40000000u) return -E_INVAL;
    long got = exfat_read_file(&fs->vol, &n->file, off, buf, (uint32_t)len);
    return got < 0 ? -E_IO : (ssize_t_k)got;
}

static int exfat_vlookup(vnode_t *dir, const char *name, vnode_t **out) {
    exfat_fs_t *fs = dir->fs->priv;
    exfat_node_t *n = dir->priv;

    for (u32 i = 0; ; i++) {
        char have[256];
        exfat_file_t entry;
        if (!exfat_readdir(&fs->vol, &n->file, i, have, sizeof have, &entry))
            return -E_NOENT;

        /* exFAT names are matched without regard to case, like NTFS. */
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

static int exfat_vreaddir(vnode_t *dir, u32 index, dirent_k *out) {
    exfat_fs_t *fs = dir->fs->priv;
    exfat_node_t *n = dir->priv;

    exfat_file_t entry;
    if (!exfat_readdir(&fs->vol, &n->file, index, out->name, sizeof out->name,
                       &entry))
        return -E_NOENT;

    out->type = entry.directory ? VN_DIR : VN_FILE;
    return 0;
}

static int exfat_vstat(vnode_t *vn, vstat_t *st) {
    exfat_node_t *n = vn->priv;
    memset(st, 0, sizeof *st);
    st->size = n->file.size;
    st->type = vn->type;
    return 0;
}

static void exfat_vrelease(vnode_t *vn) {
    if (vn->priv) kfree(vn->priv);
}

static const vnode_ops_t exfat_vops = {
    .read    = exfat_vread,
    .lookup  = exfat_vlookup,
    .readdir = exfat_vreaddir,
    .stat    = exfat_vstat,
    .release = exfat_vrelease,
};

static int exfat_unmount(filesystem_t *fs) {
    if (fs->priv) kfree(fs->priv);
    if (fs->root) { if (fs->root->priv) kfree(fs->root->priv); kfree(fs->root); }
    kfree(fs);
    return 0;
}

static u64 exfat_total(filesystem_t *fs) {
    exfat_fs_t *n = fs->priv;
    return (u64)n->vol.cluster_count * n->vol.bytes_per_cluster;
}

static u64 exfat_free(filesystem_t *fs) { (void)fs; return 0; }

filesystem_t *exfat_probe(blockdev_t *dev) {
    exfat_fs_t *n = kzalloc(sizeof *n);
    if (!n) return NULL;
    n->dev = dev;

    if (!exfat_mount(&n->vol, dev, fetch)) {
        /* Not being exFAT is the ordinary case - every volume is offered to
         * every filesystem - so it is not worth a word.  Being exFAT and
         * unreadable is. */
        if (n->vol.error[0] && n->vol.error[0] != 't')
            kinfo("exfat", "%s: %s", dev->name, n->vol.error);
        kfree(n);
        return NULL;
    }

    filesystem_t *fs = kzalloc(sizeof *fs);
    if (!fs) { kfree(n); return NULL; }

    strlcpy(fs->name, "exfat", sizeof fs->name);
    fs->dev = dev;
    fs->priv = n;
    fs->readonly = true;
    fs->unmount = exfat_unmount;
    fs->total_bytes = exfat_total;
    fs->free_bytes = exfat_free;

    exfat_file_t root;
    if (!exfat_lookup(&n->vol, "/", &root) || !root.directory) {
        kwarn("exfat", "%s: the root directory could not be opened", dev->name);
        kfree(fs); kfree(n);
        return NULL;
    }

    fs->root = make_vnode(fs, &root);
    if (!fs->root) { kfree(fs); kfree(n); return NULL; }

    kinfo("exfat", "%s: %llu MiB, %u-byte clusters - readable, and not "
                   "written to (see exfatfs.c)",
          dev->name, (unsigned long long)(exfat_total(fs) / (1024 * 1024)),
          n->vol.bytes_per_cluster);
    return fs;
}

void exfat_register(void) { vfs_register_fs("exfat", exfat_probe); }
