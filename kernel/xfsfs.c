/* xfsfs.c - XFS, as a filesystem this system can mount.
 *
 * The format is understood in common/xfs.c; this turns it into something the
 * VFS can mount, open and list.  Same arrangement as the other three.
 *
 * Read-only, and more narrowly than the others: directories large enough to
 * have left their inode are not read yet, and neither is a file whose extents
 * have grown into a b-tree.  Both are refused with a sentence rather than
 * returning the first few entries and stopping quietly, which is the failure
 * that would be mistaken for an empty directory.
 */
#include "kernel.h"
#include "klog.h"
#include "vfs.h"
#include "block.h"
#include "mm.h"
#include "../common/xfs.h"

typedef struct {
    xfs_volume_t vol;
    blockdev_t    *dev;
} xfs_fs_t;

typedef struct {
    xfs_file_t file;
} xfs_node_t;

static const vnode_ops_t xfs_vops;

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

static vnode_t *make_vnode(filesystem_t *fs, const xfs_file_t *f) {
    vnode_t *vn = kzalloc(sizeof *vn);
    if (!vn) return NULL;

    xfs_node_t *n = kzalloc(sizeof *n);
    if (!n) { kfree(vn); return NULL; }

    n->file = *f;
    vn->type = f->directory ? VN_DIR : VN_FILE;
    vn->size = f->size;
    vn->refs = 1;
    vn->priv = n;
    vn->fs = fs;
    vn->ops = &xfs_vops;
    return vn;
}

static ssize_t_k xfs_vread(vnode_t *vn, void *buf, size_t len, u64 off) {
    xfs_fs_t *fs = vn->fs->priv;
    xfs_node_t *n = vn->priv;

    if (len > 0x40000000u) return -E_INVAL;
    long got = xfs_read_file(&fs->vol, &n->file, off, buf, (uint32_t)len);
    return got < 0 ? -E_IO : (ssize_t_k)got;
}

static int xfs_vlookup(vnode_t *dir, const char *name, vnode_t **out) {
    xfs_fs_t *fs = dir->fs->priv;
    xfs_node_t *n = dir->priv;

    for (u32 i = 0; ; i++) {
        char have[256];
        xfs_file_t entry;
        if (!xfs_readdir(&fs->vol, &n->file, i, have, sizeof have, &entry))
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

static int xfs_vreaddir(vnode_t *dir, u32 index, dirent_k *out) {
    xfs_fs_t *fs = dir->fs->priv;
    xfs_node_t *n = dir->priv;

    xfs_file_t entry;
    if (!xfs_readdir(&fs->vol, &n->file, index, out->name, sizeof out->name,
                       &entry))
        return -E_NOENT;

    out->type = entry.directory ? VN_DIR : VN_FILE;
    return 0;
}

static int xfs_vstat(vnode_t *vn, vstat_t *st) {
    xfs_node_t *n = vn->priv;
    memset(st, 0, sizeof *st);
    st->size = n->file.size;
    st->type = vn->type;
    return 0;
}

static void xfs_vrelease(vnode_t *vn) {
    if (vn->priv) kfree(vn->priv);
}

static const vnode_ops_t xfs_vops = {
    .read    = xfs_vread,
    .lookup  = xfs_vlookup,
    .readdir = xfs_vreaddir,
    .stat    = xfs_vstat,
    .release = xfs_vrelease,
};

static int xfs_unmount(filesystem_t *fs) {
    if (fs->priv) kfree(fs->priv);
    if (fs->root) { if (fs->root->priv) kfree(fs->root->priv); kfree(fs->root); }
    kfree(fs);
    return 0;
}

static u64 xfs_total(filesystem_t *fs) {
    xfs_fs_t *n = fs->priv;
    return n->vol.total_blocks * n->vol.block_size;
}

static u64 xfs_free(filesystem_t *fs) { (void)fs; return 0; }

filesystem_t *xfs_probe(blockdev_t *dev) {
    xfs_fs_t *n = kzalloc(sizeof *n);
    if (!n) return NULL;
    n->dev = dev;

    if (!xfs_mount(&n->vol, dev, fetch)) {
        /* Not being exFAT is the ordinary case - every volume is offered to
         * every filesystem - so it is not worth a word.  Being exFAT and
         * unreadable is. */
        if (n->vol.error[0] && n->vol.error[0] != 't')
            kinfo("xfs", "%s: %s", dev->name, n->vol.error);
        kfree(n);
        return NULL;
    }

    filesystem_t *fs = kzalloc(sizeof *fs);
    if (!fs) { kfree(n); return NULL; }

    strlcpy(fs->name, "xfs", sizeof fs->name);
    fs->dev = dev;
    fs->priv = n;
    fs->readonly = true;
    fs->unmount = xfs_unmount;
    fs->total_bytes = xfs_total;
    fs->free_bytes = xfs_free;

    xfs_file_t root;
    if (!xfs_lookup(&n->vol, "/", &root) || !root.directory) {
        kwarn("xfs", "%s: the root directory could not be opened", dev->name);
        kfree(fs); kfree(n);
        return NULL;
    }

    fs->root = make_vnode(fs, &root);
    if (!fs->root) { kfree(fs); kfree(n); return NULL; }

    kinfo("xfs", "%s: %llu MiB, %u-byte blocks, %u group(s) - readable, and "
                 "not written to (see xfsfs.c)",
          dev->name, (unsigned long long)(xfs_total(fs) / (1024 * 1024)),
          n->vol.block_size, n->vol.ag_count);
    return fs;
}

void xfs_register(void) { vfs_register_fs("xfs", xfs_probe); }
