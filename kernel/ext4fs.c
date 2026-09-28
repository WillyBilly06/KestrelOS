/* ext4fs.c - ext2, ext3 and ext4, as a filesystem this system can mount.
 *
 * The format is understood in common/ext4.c; this is the part that turns it
 * into something the VFS can mount, open and list.  Same arrangement as
 * ntfs.c and exfatfs.c, and for the same reason.
 *
 * Read-only, stated rather than implied.  Writing ext4 means block and inode
 * bitmaps, group descriptor accounting, the extent tree, and - on any volume
 * made this decade - a journal that has to be written before the data it
 * describes.  A volume this can read and not write is useful; one it writes
 * incorrectly is a Linux install that no longer boots.
 */
#include "kernel.h"
#include "klog.h"
#include "vfs.h"
#include "block.h"
#include "mm.h"
#include "../common/ext4.h"

typedef struct {
    ext4_volume_t vol;
    blockdev_t    *dev;
} ext4_fs_t;

typedef struct {
    ext4_file_t file;
} ext4_node_t;

static const vnode_ops_t ext4_vops;

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

static vnode_t *make_vnode(filesystem_t *fs, const ext4_file_t *f) {
    vnode_t *vn = kzalloc(sizeof *vn);
    if (!vn) return NULL;

    ext4_node_t *n = kzalloc(sizeof *n);
    if (!n) { kfree(vn); return NULL; }

    n->file = *f;
    vn->type = f->directory ? VN_DIR : VN_FILE;
    vn->size = f->size;
    vn->refs = 1;
    vn->priv = n;
    vn->fs = fs;
    vn->ops = &ext4_vops;
    return vn;
}

static ssize_t_k ext4_vread(vnode_t *vn, void *buf, size_t len, u64 off) {
    ext4_fs_t *fs = vn->fs->priv;
    ext4_node_t *n = vn->priv;

    if (len > 0x40000000u) return -E_INVAL;
    long got = ext4_read_file(&fs->vol, &n->file, off, buf, (uint32_t)len);
    return got < 0 ? -E_IO : (ssize_t_k)got;
}

static int ext4_vlookup(vnode_t *dir, const char *name, vnode_t **out) {
    ext4_fs_t *fs = dir->fs->priv;
    ext4_node_t *n = dir->priv;

    for (u32 i = 0; ; i++) {
        char have[256];
        ext4_file_t entry;
        if (!ext4_readdir(&fs->vol, &n->file, i, have, sizeof have, &entry))
            return -E_NOENT;

        /* Case sensitive, unlike every other filesystem here.  This is a
         * Unix filesystem and "Makefile" and "makefile" are two files on it;
         * folding them together would open the wrong one. */
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

static int ext4_vreaddir(vnode_t *dir, u32 index, dirent_k *out) {
    ext4_fs_t *fs = dir->fs->priv;
    ext4_node_t *n = dir->priv;

    ext4_file_t entry;
    if (!ext4_readdir(&fs->vol, &n->file, index, out->name, sizeof out->name,
                       &entry))
        return -E_NOENT;

    out->type = entry.directory ? VN_DIR : VN_FILE;
    return 0;
}

static int ext4_vstat(vnode_t *vn, vstat_t *st) {
    ext4_node_t *n = vn->priv;
    memset(st, 0, sizeof *st);
    st->size = n->file.size;
    st->type = vn->type;
    return 0;
}

static void ext4_vrelease(vnode_t *vn) {
    if (vn->priv) kfree(vn->priv);
}

static const vnode_ops_t ext4_vops = {
    .read    = ext4_vread,
    .lookup  = ext4_vlookup,
    .readdir = ext4_vreaddir,
    .stat    = ext4_vstat,
    .release = ext4_vrelease,
};

static int ext4_unmount(filesystem_t *fs) {
    if (fs->priv) kfree(fs->priv);
    if (fs->root) { if (fs->root->priv) kfree(fs->root->priv); kfree(fs->root); }
    kfree(fs);
    return 0;
}

static u64 ext4_total(filesystem_t *fs) {
    ext4_fs_t *n = fs->priv;
    return n->vol.block_count * n->vol.block_size;
}

static u64 ext4_free(filesystem_t *fs) { (void)fs; return 0; }

filesystem_t *ext4_probe(blockdev_t *dev) {
    ext4_fs_t *n = kzalloc(sizeof *n);
    if (!n) return NULL;
    n->dev = dev;

    if (!ext4_mount(&n->vol, dev, fetch)) {
        /* Not being exFAT is the ordinary case - every volume is offered to
         * every filesystem - so it is not worth a word.  Being exFAT and
         * unreadable is. */
        if (n->vol.error[0] && n->vol.error[0] != 't')
            kinfo("ext4", "%s: %s", dev->name, n->vol.error);
        kfree(n);
        return NULL;
    }

    filesystem_t *fs = kzalloc(sizeof *fs);
    if (!fs) { kfree(n); return NULL; }

    strlcpy(fs->name, "ext4", sizeof fs->name);
    fs->dev = dev;
    fs->priv = n;
    fs->readonly = true;
    fs->unmount = ext4_unmount;
    fs->total_bytes = ext4_total;
    fs->free_bytes = ext4_free;

    ext4_file_t root;
    if (!ext4_lookup(&n->vol, "/", &root) || !root.directory) {
        kwarn("ext4", "%s: the root directory could not be opened", dev->name);
        kfree(fs); kfree(n);
        return NULL;
    }

    fs->root = make_vnode(fs, &root);
    if (!fs->root) { kfree(fs); kfree(n); return NULL; }

    kinfo("ext4", "%s: %llu MiB, %u-byte blocks, %u-byte inodes - readable, "
                  "and not written to (see ext4fs.c)",
          dev->name, (unsigned long long)(ext4_total(fs) / (1024 * 1024)),
          n->vol.block_size, n->vol.inode_size);
    return fs;
}

void ext4_register(void) { vfs_register_fs("ext4", ext4_probe); }
