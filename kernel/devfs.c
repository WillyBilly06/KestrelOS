/* devfs.c - the /dev filesystem.
 *
 * A flat directory of character and block devices.  Drivers register through
 * devfs_register; block devices are published automatically so the installer
 * and the shell can address raw disks by path.
 */
#include "kernel.h"
#include "vfs.h"
#include "block.h"
#include "mm.h"
#include "klog.h"
#include "input.h"

#define MAX_DEVICES 48

typedef struct {
    char              name[VFS_NAME_MAX + 1];
    u32               type;
    const devfs_ops_t *ops;
    void             *ctx;
    bool              used;
    vnode_t          *vn;
} devfs_entry_t;

static devfs_entry_t entries[MAX_DEVICES];
static filesystem_t *devfs;
static vnode_t       devfs_root;

static const vnode_ops_t dev_file_vops;
static const vnode_ops_t dev_root_vops;

/* ------------------------------------------------------------------------- */
/* device node operations                                                    */
/* ------------------------------------------------------------------------- */

static ssize_t_k dev_read(vnode_t *vn, void *buf, size_t len, u64 off) {
    devfs_entry_t *e = vn->priv;
    if (!e || !e->used || !e->ops->read) return -E_NOSYS;
    return e->ops->read(e->ctx, buf, len, off);
}

static ssize_t_k dev_write(vnode_t *vn, const void *buf, size_t len, u64 off) {
    devfs_entry_t *e = vn->priv;
    if (!e || !e->used || !e->ops->write) return -E_NOSYS;
    return e->ops->write(e->ctx, buf, len, off);
}

static int dev_ioctl(vnode_t *vn, u32 cmd, void *arg) {
    devfs_entry_t *e = vn->priv;
    if (!e || !e->used || !e->ops->ioctl) return -E_NOSYS;
    return e->ops->ioctl(e->ctx, cmd, arg);
}

static int dev_ioctl_shape(vnode_t *vn, u32 cmd, vfs_ioctl_shape_t *shape) {
    devfs_entry_t *e = vn->priv;
    if (!e || !e->used || !e->ops->ioctl_shape) return -E_NOSYS;
    return e->ops->ioctl_shape(e->ctx, cmd, shape);
}

static int dev_stat(vnode_t *vn, vstat_t *st) {
    devfs_entry_t *e = vn->priv;
    st->type = vn->type;
    st->size = (e && e->ops->size) ? e->ops->size(e->ctx) : 0;
    st->mode = 0666;
    st->mtime = 0;
    return 0;
}

static void dev_release(vnode_t *vn) {
    devfs_entry_t *e = vn->priv;
    if (e) e->vn = NULL;
    kfree(vn);
}

static const vnode_ops_t dev_file_vops = {
    .read    = dev_read,
    .write   = dev_write,
    .ioctl   = dev_ioctl,
    .ioctl_shape = dev_ioctl_shape,
    .stat    = dev_stat,
    .release = dev_release,
};

/* ------------------------------------------------------------------------- */
/* root directory                                                            */
/* ------------------------------------------------------------------------- */

static int devfs_readdir(vnode_t *vn, u32 index, dirent_k *out) {
    (void)vn;
    u32 seen = 0;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!entries[i].used) continue;
        if (seen++ != index) continue;
        strlcpy(out->name, entries[i].name, sizeof out->name);
        out->type = entries[i].type;
        out->size = entries[i].ops->size ? entries[i].ops->size(entries[i].ctx) : 0;
        return 0;
    }
    return -E_NOENT;
}

static int devfs_lookup(vnode_t *vn, const char *name, vnode_t **out) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!entries[i].used || strcmp(entries[i].name, name)) continue;

        if (entries[i].vn) { *out = vnode_ref(entries[i].vn); return 0; }

        vnode_t *n = kzalloc(sizeof *n);
        if (!n) return -E_NOMEM;
        n->type = entries[i].type;
        n->size = entries[i].ops->size ? entries[i].ops->size(entries[i].ctx) : 0;
        n->priv = &entries[i];
        n->fs   = vn->fs;
        n->ops  = &dev_file_vops;
        n->refs = 1;
        entries[i].vn = n;
        *out = n;
        return 0;
    }
    return -E_NOENT;
}

static int devfs_root_stat(vnode_t *vn, vstat_t *st) {
    (void)vn;
    st->type = VN_DIR;
    st->size = 0;
    st->mode = 0755;
    st->mtime = 0;
    return 0;
}

static const vnode_ops_t dev_root_vops = {
    .readdir = devfs_readdir,
    .lookup  = devfs_lookup,
    .stat    = devfs_root_stat,
};

/* ------------------------------------------------------------------------- */
/* registration                                                              */
/* ------------------------------------------------------------------------- */

int devfs_register(const char *name, u32 type, const devfs_ops_t *ops, void *ctx) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (entries[i].used && !strcmp(entries[i].name, name)) return -E_EXIST;
    }
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (entries[i].used) continue;
        strlcpy(entries[i].name, name, sizeof entries[i].name);
        entries[i].type = type;
        entries[i].ops  = ops;
        entries[i].ctx  = ctx;
        entries[i].vn   = NULL;
        entries[i].used = true;
        return 0;
    }
    kwarn("devfs", "device table full; %s not registered", name);
    return -E_NOSPC;
}

void devfs_unregister(const char *name) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!entries[i].used || strcmp(entries[i].name, name)) continue;
        if (entries[i].vn) entries[i].vn->priv = NULL;
        entries[i].used = false;
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* built-in devices                                                          */
/* ------------------------------------------------------------------------- */

static ssize_t_k null_read(void *ctx, void *buf, size_t len, u64 off) {
    (void)ctx; (void)buf; (void)len; (void)off; return 0;
}
static ssize_t_k null_write(void *ctx, const void *buf, size_t len, u64 off) {
    (void)ctx; (void)buf; (void)off; return (ssize_t_k)len;
}
static const devfs_ops_t null_ops = { .read = null_read, .write = null_write };

static ssize_t_k zero_read(void *ctx, void *buf, size_t len, u64 off) {
    (void)ctx; (void)off;
    memset(buf, 0, len);
    return (ssize_t_k)len;
}
static const devfs_ops_t zero_ops = { .read = zero_read, .write = null_write };

/* /dev/kmsg exposes the event ring to userland, one entry per read. */
#include "klog.h"

static ssize_t_k kmsg_read(void *ctx, void *buf, size_t len, u64 off) {
    (void)ctx;
    /* `off` doubles as the sequence number the reader wants next. */
    klog_entry e;
    int n = klog_read(off ? off : 1, &e, 1);
    if (n <= 0) return 0;

    char line[256];
    int w = snprintf(line, sizeof line, "%lu %u %lu %s %s\n",
                     e.seq, e.level, e.time_ms, e.subsys, e.msg);
    if (w < 0) return 0;
    if ((size_t)w > len) w = (int)len;
    memcpy(buf, line, (size_t)w);
    return w;
}
static const devfs_ops_t kmsg_ops = { .read = kmsg_read };

/* /dev/initrd exposes the archive the loader placed in memory.  The installer
 * copies it straight onto the target's EFI partition, which is the only way to
 * reproduce the running system when booting from a medium the kernel cannot
 * read back - an El Torito CD, for instance. */
static ssize_t_k initrd_read(void *ctx, void *buf, size_t len, u64 off) {
    (void)ctx;
    if (!g_boot.initrd_base || !g_boot.initrd_size) return -E_NODEV;
    if (off >= g_boot.initrd_size) return 0;
    u64 avail = g_boot.initrd_size - off;
    if (len > avail) len = (size_t)avail;
    memcpy(buf, (const u8 *)phys_to_virt(g_boot.initrd_base) + off, len);
    return (ssize_t_k)len;
}

static u64 initrd_size(void *ctx) { (void)ctx; return g_boot.initrd_size; }

static const devfs_ops_t initrd_ops = { .read = initrd_read, .size = initrd_size };

/* Block devices, addressable as whole disks or partitions. */
static ssize_t_k blk_read(void *ctx, void *buf, size_t len, u64 off) {
    blockdev_t *d = ctx;
    if (off >= d->sector_count * d->sector_size) return 0;
    u64 avail = d->sector_count * d->sector_size - off;
    if (len > avail) len = (size_t)avail;
    int r = block_read_bytes(d, off, buf, len);
    return r < 0 ? r : (ssize_t_k)len;
}

static ssize_t_k blk_write(void *ctx, const void *buf, size_t len, u64 off) {
    blockdev_t *d = ctx;
    if (d->readonly) return -E_ROFS;
    if (off >= d->sector_count * d->sector_size) return -E_NOSPC;
    u64 avail = d->sector_count * d->sector_size - off;
    if (len > avail) len = (size_t)avail;
    int r = block_write_bytes(d, off, buf, len);
    return r < 0 ? r : (ssize_t_k)len;
}

static u64 blk_size(void *ctx) {
    blockdev_t *d = ctx;
    return d->sector_count * d->sector_size;
}

static const devfs_ops_t blk_ops = { .read = blk_read, .write = blk_write, .size = blk_size };

void devfs_publish_block_devices(void) {
    for (blockdev_t *d = block_first(); d; d = d->next)
        devfs_register(d->name, VN_BLK, &blk_ops, d);
}

filesystem_t *devfs_create(void) {
    if (devfs) return devfs;

    devfs = kzalloc(sizeof *devfs);
    if (!devfs) return NULL;
    strlcpy(devfs->name, "devfs", sizeof devfs->name);

    /* No static scratch buffers, and a read from /dev/console waits until
     * somebody types.  Holding the filesystem lock across that stops the
     * machine - see the note beside the lock in vfs.c. */
    devfs->reentrant = true;

    devfs_root.type = VN_DIR;
    devfs_root.refs = 1;
    devfs_root.fs   = devfs;
    devfs_root.ops  = &dev_root_vops;
    devfs->root = &devfs_root;

    devfs_register("null", VN_CHR, &null_ops, NULL);
    devfs_register("zero", VN_CHR, &zero_ops, NULL);
    devfs_register("kmsg", VN_CHR, &kmsg_ops, NULL);
    if (g_boot.initrd_size) devfs_register("initrd", VN_BLK, &initrd_ops, NULL);

    kinfo("devfs", "device filesystem ready");
    return devfs;
}
