/* btrfsfs.c - Btrfs, as a filesystem this system can mount.
 *
 * The format is understood in common/btrfs.c; this turns it into something
 * the VFS can mount, open and list.  Same arrangement as the other four.
 *
 * Read-only, and narrower still than the others: a volume spread or mirrored
 * across more than one device is refused rather than half-read, and a
 * compressed file is refused rather than returned as the bytes that happen to
 * be on the disk.  Both are said in a sentence rather than left to look like
 * an empty directory or a corrupt file.
 */
#include "kernel.h"
#include "klog.h"
#include "vfs.h"
#include "block.h"
#include "mm.h"
#include "../common/btrfs.h"

typedef struct {
    btrfs_volume_t vol;
    blockdev_t    *dev;
} btrfs_fs_t;

typedef struct {
    btrfs_file_t file;
} btrfs_node_t;

static const vnode_ops_t btrfs_vops;

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

static vnode_t *make_vnode(filesystem_t *fs, const btrfs_file_t *f) {
    vnode_t *vn = kzalloc(sizeof *vn);
    if (!vn) return NULL;

    btrfs_node_t *n = kzalloc(sizeof *n);
    if (!n) { kfree(vn); return NULL; }

    n->file = *f;
    vn->type = f->directory ? VN_DIR : VN_FILE;
    vn->size = f->size;
    vn->refs = 1;
    vn->priv = n;
    vn->fs = fs;
    vn->ops = &btrfs_vops;
    return vn;
}

static ssize_t_k btrfs_vread(vnode_t *vn, void *buf, size_t len, u64 off) {
    btrfs_fs_t *fs = vn->fs->priv;
    btrfs_node_t *n = vn->priv;

    if (len > 0x40000000u) return -E_INVAL;
    long got = btrfs_read_file(&fs->vol, &n->file, off, buf, (uint32_t)len);
    return got < 0 ? -E_IO : (ssize_t_k)got;
}

static int btrfs_vlookup(vnode_t *dir, const char *name, vnode_t **out) {
    btrfs_fs_t *fs = dir->fs->priv;
    btrfs_node_t *n = dir->priv;

    for (u32 i = 0; ; i++) {
        char have[256];
        btrfs_file_t entry;
        if (!btrfs_readdir(&fs->vol, &n->file, i, have, sizeof have, &entry))
            return -E_NOENT;

        /* Case sensitive: a Unix filesystem, where "Makefile" and "makefile"
         * are two different files and folding them opens the wrong one. */
        const char *a = have, *b = name;
        bool same = true;
        while (*a && *b) { if (*a != *b) { same = false; break; } a++; b++; }
        if (!same || *a || *b) continue;

        vnode_t *vn = make_vnode(dir->fs, &entry);
        if (!vn) return -E_NOMEM;
        *out = vn;
        return 0;
    }
}

static int btrfs_vreaddir(vnode_t *dir, u32 index, dirent_k *out) {
    btrfs_fs_t *fs = dir->fs->priv;
    btrfs_node_t *n = dir->priv;

    btrfs_file_t entry;
    if (!btrfs_readdir(&fs->vol, &n->file, index, out->name, sizeof out->name,
                       &entry))
        return -E_NOENT;

    out->type = entry.directory ? VN_DIR : VN_FILE;
    return 0;
}

static int btrfs_vstat(vnode_t *vn, vstat_t *st) {
    btrfs_node_t *n = vn->priv;
    memset(st, 0, sizeof *st);
    st->size = n->file.size;
    st->type = vn->type;
    return 0;
}

static void btrfs_vrelease(vnode_t *vn) {
    if (vn->priv) kfree(vn->priv);
}

static const vnode_ops_t btrfs_vops = {
    .read    = btrfs_vread,
    .lookup  = btrfs_vlookup,
    .readdir = btrfs_vreaddir,
    .stat    = btrfs_vstat,
    .release = btrfs_vrelease,
};

static int btrfs_unmount(filesystem_t *fs) {
    if (fs->priv) kfree(fs->priv);
    if (fs->root) { if (fs->root->priv) kfree(fs->root->priv); kfree(fs->root); }
    kfree(fs);
    return 0;
}

static u64 btrfs_total(filesystem_t *fs) {
    btrfs_fs_t *n = fs->priv;
    return n->vol.total_bytes;
}

static u64 btrfs_free(filesystem_t *fs) { (void)fs; return 0; }

filesystem_t *btrfs_probe(blockdev_t *dev) {
    btrfs_fs_t *n = kzalloc(sizeof *n);
    if (!n) return NULL;
    n->dev = dev;

    if (!btrfs_mount(&n->vol, dev, fetch)) {
        /* Not being exFAT is the ordinary case - every volume is offered to
         * every filesystem - so it is not worth a word.  Being exFAT and
         * unreadable is. */
        if (n->vol.error[0] && n->vol.error[0] != 't')
            kinfo("btrfs", "%s: %s", dev->name, n->vol.error);
        kfree(n);
        return NULL;
    }

    filesystem_t *fs = kzalloc(sizeof *fs);
    if (!fs) { kfree(n); return NULL; }

    strlcpy(fs->name, "btrfs", sizeof fs->name);
    fs->dev = dev;
    fs->priv = n;
    fs->readonly = true;
    fs->unmount = btrfs_unmount;
    fs->total_bytes = btrfs_total;
    fs->free_bytes = btrfs_free;

    btrfs_file_t root;
    if (!btrfs_lookup(&n->vol, "/", &root) || !root.directory) {
        kwarn("btrfs", "%s: the root directory could not be opened", dev->name);
        kfree(fs); kfree(n);
        return NULL;
    }

    fs->root = make_vnode(fs, &root);
    if (!fs->root) { kfree(fs); kfree(n); return NULL; }

    kinfo("btrfs", "%s: %llu MiB, %u-byte nodes, %d chunk(s) mapped - "
                   "readable, and not written to (see btrfsfs.c)",
          dev->name, (unsigned long long)(btrfs_total(fs) / (1024 * 1024)),
          n->vol.node_size, n->vol.chunk_count);
    return fs;
}

void btrfs_register(void) { vfs_register_fs("btrfs", btrfs_probe); }
