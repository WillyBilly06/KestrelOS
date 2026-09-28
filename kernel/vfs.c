/* vfs.c - path resolution, the mount table and the open-file layer.
 *
 * Paths are always absolute by the time they reach here; the process layer
 * expands anything relative against its working directory first.  Resolution
 * walks components one at a time, switching to a mounted filesystem's root
 * whenever it crosses a mount point.
 */
#include "kernel.h"
#include "vfs.h"
#include "mm.h"
#include "klog.h"
#include "block.h"
#include "proc.h"
#include "spinlock.h"
#include "smp.h"

/* ------------------------------------------------------------------------- */
/* one at a time                                                             */
/* ------------------------------------------------------------------------- */

/* The filesystem drivers are not reentrant and were never written to be.
 * Every one of them - FAT, NTFS, exFAT, ext4 - works out of static scratch
 * buffers, one per purpose, reused by whoever is inside the driver.  That is
 * a reasonable design for a kernel with one thread in its filesystems and a
 * silent corruption for one with two.
 *
 * This kernel now has two: the boot path, and the thread that flushes the log.
 * The symptom was a file that had just been created coming back "not found"
 * from a lookup a second later - because the lookup's record buffer had been
 * overwritten underneath it by the other thread's read.
 *
 * So one lock, taken at the entry points and released at the exits.  Not one
 * lock per filesystem: a copy between two volumes is inside two drivers at
 * once, and two locks taken in whichever order the caller happened to pick is
 * the classic way to build a deadlock.
 *
 * It is a plain flag rather than a proper mutex because this kernel has no
 * mutexes yet.  Interrupts are not disabled - a filesystem operation reads
 * disks and takes milliseconds, and holding interrupts off for that would
 * lose timer ticks and keyboard input - so the flag is tested and set with
 * interrupts off, and the waiting is done by yielding.
 */
static volatile u32 fs_busy;

static void fs_enter(void) {
    /* ATOMIC across cores.  This was a plain `if (!fs_busy) fs_busy = true`
     * guarded by irq_save() - but irq_save only masks THIS processor's
     * interrupts, not the other cores.  On an SMP machine two processors both
     * passed the check and both set the flag, so both ran inside a filesystem
     * driver at once, and the FAT driver's file-static scratch buffers are not
     * reentrant - the second write corrupted the first.  That is precisely what
     * stopped the on-disk log the instant a second thread (the GPU bring-up on
     * its own kernel thread) touched a disk while the boot thread was still
     * writing: the truncation was deterministic at that concurrency point, on
     * real hardware AND under QEMU -smp.  An atomic exchange is a real lock
     * across every core. */
    while (__atomic_exchange_n(&fs_busy, 1u, __ATOMIC_ACQUIRE)) {
        /* Someone else is inside a driver.  A filesystem op reads disks and
         * takes milliseconds, so yield rather than spin. */
        sched_yield();
    }
}

static void fs_leave(void) {
    __atomic_store_n(&fs_busy, 0u, __ATOMIC_RELEASE);
}


#define MAX_MOUNTS 16
#define MAX_FILES  256

typedef struct {
    char          path[VFS_PATH_MAX];
    filesystem_t *fs;
    bool          used;
} mount_t;

static mount_t mounts[MAX_MOUNTS];
static file_t  files[MAX_FILES];
static spinlock_t file_pool_lock;
static bool files_lock(void) {
    bool irq = irq_save();
    while (__atomic_exchange_n(&file_pool_lock.held, 1u, __ATOMIC_ACQUIRE)) {
        smp_tlb_poll();
        __asm__ volatile("pause");
    }
    return irq;
}
static void files_unlock(bool irq) { spin_unlock_irqrestore(&file_pool_lock, irq); }

file_t *vfs_file_ref(file_t *f) {
    if (!f) return NULL;
    bool irq = files_lock();
    if (!f->in_use || !f->refs || f->refs == 0xffffffffu) {
        files_unlock(irq);
        return NULL;
    }
    f->refs++;
    files_unlock(irq);
    return f;
}

typedef struct { char name[16]; fs_probe_fn probe; } fs_driver_t;
static fs_driver_t drivers[8];
static int         driver_count;

const char *vfs_strerror(int err) {
    if (err < 0) err = -err;
    switch (err) {
    case 0:            return "success";
    case E_PERM:       return "operation not permitted";
    case E_NOENT:      return "no such file or directory";
    case E_IO:         return "input/output error";
    case E_BADF:       return "bad file descriptor";
    case E_NOMEM:      return "out of memory";
    case E_ACCES:      return "permission denied";
    case E_BUSY:       return "device or resource busy";
    case E_EXIST:      return "file exists";
    case E_XDEV:       return "cross-device link";
    case E_NODEV:      return "no such device";
    case E_NOTDIR:     return "not a directory";
    case E_ISDIR:      return "is a directory";
    case E_INVAL:      return "invalid argument";
    case E_MFILE:      return "too many open files";
    case E_NOSPC:      return "no space left on device";
    case E_SPIPE:      return "illegal seek";
    case E_ROFS:       return "read-only file system";
    case E_NAMETOOLONG:return "file name too long";
    case E_NOSYS:      return "function not implemented";
    case E_NOTEMPTY:   return "directory not empty";
    case E_AGAIN:      return "resource temporarily unavailable";
    default:           return "unknown error";
    }
}

void vfs_init(void) {
    memset(mounts, 0, sizeof mounts);
    memset(files, 0, sizeof files);
    kinfo("vfs", "virtual filesystem ready");
}

void vfs_register_fs(const char *name, fs_probe_fn probe) {
    if (driver_count >= (int)ARRAY_LEN(drivers)) { kwarn("vfs", "filesystem table full; %s not registered", name); return; }
    strlcpy(drivers[driver_count].name, name, sizeof drivers[0].name);
    drivers[driver_count].probe = probe;
    driver_count++;
    kinfo("vfs", "filesystem driver registered: %s", name);
}

filesystem_t *vfs_probe(blockdev_t *dev) {
    for (int i = 0; i < driver_count; i++) {
        filesystem_t *fs = drivers[i].probe(dev);
        if (!fs) continue;

        /* A volume that is not on the disk this system booted from is
         * readable and nothing more.  Applied here, at the one place every
         * filesystem passes through, rather than at each mount - there is
         * more than one of those and the forgotten one is the one that
         * writes to somebody's Windows installation. */
        if (!block_writes_allowed(dev)) {
            fs->readonly = true;
            if (!block_described(dev))
                kinfo("vfs", "%s holds %s, mounted read-only: it is not on the "
                             "disk this system booted from",
                      dev && dev->name[0] ? dev->name : "a device", fs->name);
        }
        block_mark_described(dev);
        return fs;
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* reference counting                                                        */
/* ------------------------------------------------------------------------- */

vnode_t *vnode_ref(vnode_t *vn) {
    if (!vn) return NULL;
    u32 refs = __atomic_load_n(&vn->refs, __ATOMIC_RELAXED);
    do {
        if (!refs || refs == 0xffffffffu) panic("vfs: invalid vnode retain");
    } while (!__atomic_compare_exchange_n(&vn->refs, &refs, refs + 1u, false,
                                          __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    return vn;
}

void vnode_unref(vnode_t *vn) {
    if (!vn) return;
    u32 refs = __atomic_fetch_sub(&vn->refs, 1u, __ATOMIC_ACQ_REL);
    if (!refs) panic("vfs: vnode refcount underflow");
    if (refs == 1u && vn->ops->release) vn->ops->release(vn);
}

/* ------------------------------------------------------------------------- */
/* mounts                                                                    */
/* ------------------------------------------------------------------------- */

/* Normalise into `out`: absolute, no "." or "..", no trailing slash. */
static int normalise(const char *in, char *out, size_t cap) {
    if (!in || in[0] != '/') return -E_INVAL;

    /* The components, as pointers into the input rather than copies of it.
     * Sixty-four names of two hundred and fifty-six bytes each is sixteen
     * kilobytes of stack, on a path that every file operation goes through and
     * that has the whole filesystem and disk stack underneath it - which is
     * most of a kernel stack gone before any work starts. */
    const char *start[64];
    size_t      length[64];
    int depth = 0;

    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *from = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - from);
        if (len > VFS_NAME_MAX) return -E_NAMETOOLONG;

        if (len == 1 && from[0] == '.') continue;
        if (len == 2 && from[0] == '.' && from[1] == '.') {
            if (depth) depth--;
            continue;
        }
        if (depth >= 64) return -E_NAMETOOLONG;
        start[depth] = from;
        length[depth] = len;
        depth++;
    }

    if (depth == 0) {
        if (cap < 2) return -E_NAMETOOLONG;
        out[0] = '/';
        out[1] = 0;
        return 0;
    }

    /* Built separately, then copied, because a caller is allowed to hand in
     * the same buffer for both and writing as we go would overwrite the
     * components still to be read. */
    char built[VFS_PATH_MAX];
    size_t n = 0;
    for (int i = 0; i < depth; i++) {
        if (n + length[i] + 2 > sizeof built) return -E_NAMETOOLONG;
        built[n++] = '/';
        memcpy(built + n, start[i], length[i]);
        n += length[i];
    }
    built[n] = 0;

    if (n + 1 > cap) return -E_NAMETOOLONG;
    memcpy(out, built, n + 1);
    return 0;
}

/* The mount whose path is the longest prefix of `path`. */
static mount_t *find_mount(const char *path, size_t *prefix_len) {
    mount_t *best = NULL;
    size_t best_len = 0;

    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].used) continue;
        size_t len = strlen(mounts[i].path);
        if (len == 1) {                     /* "/" matches everything */
            if (!best) { best = &mounts[i]; best_len = 0; }
            continue;
        }
        if (strncmp(path, mounts[i].path, len)) continue;
        if (path[len] != 0 && path[len] != '/') continue;
        if (len > best_len) { best = &mounts[i]; best_len = len; }
    }
    if (prefix_len) *prefix_len = best_len;
    return best;
}

int vfs_mount(const char *path, filesystem_t *fs) {
    char norm[VFS_PATH_MAX];
    int r = normalise(path, norm, sizeof norm);
    if (r < 0) return r;
    if (!fs || !fs->root) return -E_INVAL;

    for (int i = 0; i < MAX_MOUNTS; i++)
        if (mounts[i].used && !strcmp(mounts[i].path, norm)) return -E_BUSY;

    /* Everything except the root must be mounted onto an existing directory. */
    if (strcmp(norm, "/")) {
        vnode_t *vn = NULL;
        r = vfs_resolve(norm, &vn);
        if (r < 0) return r;
        u32 type = vn->type;
        vnode_unref(vn);
        if (type != VN_DIR) return -E_NOTDIR;
    }

    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].used) continue;
        strlcpy(mounts[i].path, norm, sizeof mounts[i].path);
        mounts[i].fs = fs;
        mounts[i].used = true;
        kinfo("vfs", "mounted %s at %s%s", fs->name, norm, fs->readonly ? " (read-only)" : "");
        return 0;
    }
    return -E_MFILE;
}

int vfs_unmount(const char *path) {
    char norm[VFS_PATH_MAX];
    int r = normalise(path, norm, sizeof norm);
    if (r < 0) return r;

    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].used || strcmp(mounts[i].path, norm)) continue;

        /* Refuse while any file on this filesystem is still open. */
        for (int f = 0; f < MAX_FILES; f++)
            if (files[f].in_use && files[f].vn && files[f].vn->fs == mounts[i].fs) return -E_BUSY;

        filesystem_t *fs = mounts[i].fs;
        if (fs->sync) fs->sync(fs);
        mounts[i].used = false;
        if (fs->unmount) fs->unmount(fs);
        kinfo("vfs", "unmounted %s", norm);
        return 0;
    }
    return -E_INVAL;
}

filesystem_t *vfs_mounted_at(const char *path) {
    char norm[VFS_PATH_MAX];
    if (normalise(path, norm, sizeof norm) < 0) return NULL;
    for (int i = 0; i < MAX_MOUNTS; i++)
        if (mounts[i].used && !strcmp(mounts[i].path, norm)) return mounts[i].fs;
    return NULL;
}

int vfs_list_mounts(char *buf, size_t cap) {
    size_t n = 0;
    int count = 0;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].used) continue;
        filesystem_t *fs = mounts[i].fs;
        u64 total = fs->total_bytes ? fs->total_bytes(fs) : 0;
        u64 free = fs->free_bytes ? fs->free_bytes(fs) : 0;
        int w = snprintf(buf + n, cap - n, "%-12s %-8s %6lu MiB total %6lu MiB free%s\n",
                         mounts[i].path, fs->name, total / (1024 * 1024), free / (1024 * 1024),
                         fs->readonly ? " (ro)" : "");
        if (w < 0 || (size_t)w >= cap - n) break;
        n += (size_t)w;
        count++;
    }
    if (cap) buf[n] = 0;
    return count;
}

/* ------------------------------------------------------------------------- */
/* resolution                                                                */
/* ------------------------------------------------------------------------- */

/* Walk `path`, returning the vnode with a reference taken.
 *
 * When `parent` is non-NULL the walk stops one component short, the final name
 * is copied into `last`, and the directory holding it is returned - which is
 * what create, unlink and rename need.  Mount points are handled by starting
 * the walk at the deepest mount whose path prefixes this one, so a crossing
 * never has to be detected mid-walk. */
static int walk(const char *path, vnode_t **out, vnode_t **parent, char *last, size_t last_cap) {
    char norm[VFS_PATH_MAX];
    int r = normalise(path, norm, sizeof norm);
    if (r < 0) return r;

    if (parent) {
        const char *slash = strrchr(norm, '/');
        if (!slash || !slash[1]) return -E_INVAL;      /* "/" has no parent entry */
        strlcpy(last, slash + 1, last_cap);
        /* Trim the final component and re-resolve the directory part, which
         * may itself be a mount point. */
        size_t dirlen = (size_t)(slash - norm);
        if (dirlen == 0) { norm[1] = 0; }
        else norm[dirlen] = 0;
    }

    size_t prefix = 0;
    mount_t *m = find_mount(norm, &prefix);
    if (!m) return -E_NOENT;

    vnode_t *cur = vnode_ref(m->fs->root);
    const char *p = norm + prefix;

    for (;;) {
        while (*p == '/') p++;
        if (!*p) break;

        const char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);
        if (len > VFS_NAME_MAX) { vnode_unref(cur); return -E_NAMETOOLONG; }

        if (cur->type != VN_DIR) { vnode_unref(cur); return -E_NOTDIR; }
        if (!cur->ops->lookup)   { vnode_unref(cur); return -E_NOSYS; }

        char name[VFS_NAME_MAX + 1];
        memcpy(name, start, len);
        name[len] = 0;

        vnode_t *child = NULL;
        r = cur->ops->lookup(cur, name, &child);
        vnode_unref(cur);
        if (r < 0) return r;
        cur = child;
    }

    if (parent) *parent = cur;
    else *out = cur;
    return 0;
}

int vfs_resolve(const char *path, vnode_t **out) { return walk(path, out, NULL, NULL, 0); }

/* The directory part of a normalised path, for comparing two paths' parents. */
static int parent_path(const char *path, char *out, size_t cap) {
    char norm[VFS_PATH_MAX];
    int r = normalise(path, norm, sizeof norm);
    if (r < 0) return r;
    char *slash = strrchr(norm, '/');
    if (!slash || !slash[1]) return -E_INVAL;
    if (slash == norm) { strlcpy(out, "/", cap); return 0; }
    *slash = 0;
    strlcpy(out, norm, cap);
    return 0;
}

static int locked_vfs_stat(const char *path, vstat_t *st) {
    vnode_t *vn = NULL;
    int r = vfs_resolve(path, &vn);
    if (r < 0) return r;

    memset(st, 0, sizeof *st);
    st->type = vn->type;
    st->size = vn->size;
    if (vn->ops->stat) r = vn->ops->stat(vn, st);
    vnode_unref(vn);
    return r;
}

static int locked_vfs_mkdir(const char *path) {
    vnode_t *parent = NULL;
    char name[VFS_NAME_MAX + 1];
    int r = walk(path, NULL, &parent, name, sizeof name);
    if (r < 0) return r;

    if (parent->type != VN_DIR) { vnode_unref(parent); return -E_NOTDIR; }
    if (parent->fs->readonly)   { vnode_unref(parent); return -E_ROFS; }

    vnode_t *existing = NULL;
    if (parent->ops->lookup && parent->ops->lookup(parent, name, &existing) == 0) {
        vnode_unref(existing);
        vnode_unref(parent);
        return -E_EXIST;
    }

    if (!parent->ops->create) { vnode_unref(parent); return -E_NOSYS; }
    vnode_t *made = NULL;
    r = parent->ops->create(parent, name, VN_DIR, &made);
    vnode_unref(parent);
    if (r == 0 && made) vnode_unref(made);
    return r;
}

static int locked_vfs_unlink(const char *path) {
    vnode_t *parent = NULL;
    char name[VFS_NAME_MAX + 1];
    int r = walk(path, NULL, &parent, name, sizeof name);
    if (r < 0) return r;

    if (parent->fs->readonly) { vnode_unref(parent); return -E_ROFS; }
    if (!parent->ops->unlink) { vnode_unref(parent); return -E_NOSYS; }
    r = parent->ops->unlink(parent, name);
    vnode_unref(parent);
    return r;
}

/* Renames are within a single directory.  Anything else - a different
 * directory or a different filesystem - is reported as -E_XDEV so callers can
 * fall back to copy-then-delete. */
int vfs_rename(const char *from, const char *to) {
    char dir_from[VFS_PATH_MAX], dir_to[VFS_PATH_MAX];
    int r = parent_path(from, dir_from, sizeof dir_from);
    if (r < 0) return r;
    r = parent_path(to, dir_to, sizeof dir_to);
    if (r < 0) return r;
    if (strcmp(dir_from, dir_to)) return -E_XDEV;

    vnode_t *dir = NULL;
    char na[VFS_NAME_MAX + 1], nb[VFS_NAME_MAX + 1];
    r = walk(from, NULL, &dir, na, sizeof na);
    if (r < 0) return r;
    {
        const char *slash = strrchr(to, '/');
        strlcpy(nb, slash ? slash + 1 : to, sizeof nb);
    }

    if (dir->fs->readonly) { vnode_unref(dir); return -E_ROFS; }
    if (!dir->ops->rename) { vnode_unref(dir); return -E_NOSYS; }

    r = dir->ops->rename(dir, na, nb);
    vnode_unref(dir);
    return r;
}

int vfs_sync(void) {
    int worst = 0;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].used || !mounts[i].fs->sync) continue;
        int r = mounts[i].fs->sync(mounts[i].fs);
        if (r < 0) worst = r;
    }
    return worst;
}

/* ------------------------------------------------------------------------- */
/* open files                                                                */
/* ------------------------------------------------------------------------- */

static int locked_vfs_open(const char *path, u32 flags, file_t **out) {
    vnode_t *vn = NULL;
    int r = vfs_resolve(path, &vn);

    if (r == -E_NOENT && (flags & O_CREAT)) {
        vnode_t *parent = NULL;
        char name[VFS_NAME_MAX + 1];
        r = walk(path, NULL, &parent, name, sizeof name);
        if (r < 0) return r;
        if (parent->fs->readonly) { vnode_unref(parent); return -E_ROFS; }
        if (!parent->ops->create) { vnode_unref(parent); return -E_NOSYS; }
        r = parent->ops->create(parent, name, VN_FILE, &vn);
        vnode_unref(parent);
        if (r < 0) return r;
    } else if (r < 0) {
        return r;
    }

    if ((flags & O_DIRECTORY) && vn->type != VN_DIR) { vnode_unref(vn); return -E_NOTDIR; }
    if (vn->type == VN_DIR && (flags & O_ACCMODE) != O_RDONLY) { vnode_unref(vn); return -E_ISDIR; }
    if ((flags & O_ACCMODE) != O_RDONLY && vn->fs && vn->fs->readonly) { vnode_unref(vn); return -E_ROFS; }

    if ((flags & O_TRUNC) && vn->type == VN_FILE && (flags & O_ACCMODE) != O_RDONLY) {
        if (vn->ops->truncate) {
            r = vn->ops->truncate(vn, 0);
            if (r < 0) { vnode_unref(vn); return r; }
        }
    }

    r = vfs_open_vnode(vn, flags, out);
    if (r < 0) vnode_unref(vn);
    return r;
}

/* Wrap a vnode that already exists in an open file description.  Pipes have
 * no path, so they cannot go through vfs_open. */
int vfs_open_vnode(vnode_t *vn, u32 flags, file_t **out) {
    if (!vn || !out) return -E_INVAL;
    *out = NULL;
    bool irq = files_lock();
    for (int i = 0; i < MAX_FILES; i++) {
        if (files[i].in_use) continue;
        files[i].in_use = true;
        files[i].vn = vn;
        files[i].flags = flags;
        files[i].pos = (flags & O_APPEND) ? vn->size : 0;
        files[i].refs = 1;
        files[i].io_busy = 0;
        *out = &files[i];
        files_unlock(irq);
        return 0;
    }
    files_unlock(irq);
    kerr("vfs", "open file table is full");
    return -E_MFILE;
}

void vfs_close(file_t *f) {
    if (!f) return;
    bool irq = files_lock();
    if (!f->in_use || !f->refs) panic("vfs: invalid file reference release");
    if (--f->refs) { files_unlock(irq); return; }
    /* Keep the pool slot reserved while callbacks run, but hold no metadata
     * spinlock across a filesystem wait or vnode release. No new retain can
     * revive this zero-reference description. */
    files_unlock(irq);

    /* Closing reaches a driver twice - once to flush and once to release the
     * vnode - so it belongs inside the lock with everything else that does. */
    /* Even reentrant device/RAM filesystems cache vnodes in their namespace.
     * Final release must serialize with lookup, which uses fs_enter(). Unlike
     * a device read, final close does not wait for user input. */
    fs_enter();
    if (f->vn && f->vn->ops->sync) f->vn->ops->sync(f->vn);
    vnode_unref(f->vn);
    fs_leave();

    irq = files_lock();
    f->vn = NULL;
    f->in_use = false;
    files_unlock(irq);
}

/* A retained description cannot be recycled while waiting here. Stream
 * devices deliberately bypass this mutex: stdin/stdout can share a description
 * and a blocked console read must not prevent a write or control operation. */
static bool file_position_enter(file_t *f) {
    if (!f || !f->in_use || !f->vn || f->vn->type == VN_CHR) return false;
    while (__atomic_exchange_n(&f->io_busy, 1u, __ATOMIC_ACQUIRE)) sched_yield();
    return true;
}
static void file_position_leave(file_t *f) { __atomic_store_n(&f->io_busy, 0u, __ATOMIC_RELEASE); }

static ssize_t_k locked_vfs_read(file_t *f, void *buf, size_t len) {
    if (!f || !f->in_use || !f->vn) return -E_BADF;
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -E_BADF;
    if (!f->vn->ops->read) return -E_NOSYS;

    ssize_t_k n = f->vn->ops->read(f->vn, buf, len, f->pos);
    if (n > 0 && f->vn->type != VN_CHR) f->pos += (u64)n;
    return n;
}

static ssize_t_k locked_vfs_write(file_t *f, const void *buf, size_t len) {
    if (!f || !f->in_use || !f->vn) return -E_BADF;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -E_BADF;
    if (!f->vn->ops->write) return -E_NOSYS;

    if (f->vn->type != VN_CHR && (f->flags & O_APPEND)) f->pos = f->vn->size;
    ssize_t_k n = f->vn->ops->write(f->vn, buf, len, f->pos);
    if (n > 0 && f->vn->type != VN_CHR) f->pos += (u64)n;
    return n;
}

static s64 locked_vfs_seek(file_t *f, s64 off, int whence) {
    if (!f || !f->in_use || !f->vn) return -E_BADF;
    if (f->vn->type == VN_CHR) return -E_SPIPE;

    s64 base;
    switch (whence) {
    case 0: base = 0; break;
    case 1: base = (s64)f->pos; break;
    case 2: base = (s64)f->vn->size; break;
    default: return -E_INVAL;
    }
    if (base < 0 || (off > 0 && base > (s64)(~0ull >> 1) - off) ||
        (off < 0 && (u64)(0ull - (u64)off) > (u64)base)) return -E_INVAL;
    s64 target = base + off;
    if (target < 0) return -E_INVAL;
    f->pos = (u64)target;
    return target;
}

static int locked_vfs_readdir(file_t *f, u32 index, dirent_k *out) {
    if (!f || !f->in_use || !f->vn) return -E_BADF;
    if (f->vn->type != VN_DIR) return -E_NOTDIR;
    if (!f->vn->ops->readdir) return -E_NOSYS;
    return f->vn->ops->readdir(f->vn, index, out);
}

int vfs_ioctl(file_t *f, u32 cmd, void *arg) {
    if (!f || !f->in_use || !f->vn) return -E_BADF;
    if (!f->vn->ops->ioctl) return -E_NOSYS;
    return f->vn->ops->ioctl(f->vn, cmd, arg);
}

static int locked_vfs_truncate(file_t *f, u64 size) {
    if (!f || !f->in_use || !f->vn) return -E_BADF;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -E_BADF;
    if (!f->vn->ops->truncate) return -E_NOSYS;
    return f->vn->ops->truncate(f->vn, size);
}

s64 vfs_seek(file_t *f, s64 off, int whence) {
    bool position = file_position_enter(f);
    bool guard = f && f->vn && f->vn->fs && !f->vn->fs->reentrant;
    if (guard) fs_enter();
    s64 result = locked_vfs_seek(f, off, whence);
    if (guard) fs_leave();
    if (position) file_position_leave(f);
    return result;
}

int vfs_truncate(file_t *f, u64 size) {
    bool position = file_position_enter(f);
    bool guard = f && f->vn && f->vn->fs && !f->vn->fs->reentrant;
    if (guard) fs_enter();
    int result = locked_vfs_truncate(f, size);
    if (guard) fs_leave();
    if (position) file_position_leave(f);
    return result;
}

/* ------------------------------------------------------------------------- */
/* whole-file helpers                                                        */
/* ------------------------------------------------------------------------- */

int vfs_append(const char *path, const void *data, size_t len) {
    file_t *f = NULL;
    int r = vfs_open(path, O_WRONLY | O_CREAT | O_APPEND, &f);
    if (r < 0) return r;
    ssize_t_k n = vfs_write(f, data, len);
    vfs_close(f);
    if (n < 0) return (int)n;
    return (size_t)n == len ? 0 : -E_IO;
}

int vfs_write_file(const char *path, const void *data, size_t len) {
    file_t *f = NULL;
    int r = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, &f);
    if (r < 0) return r;
    ssize_t_k n = vfs_write(f, data, len);
    vfs_close(f);
    if (n < 0) return (int)n;
    return (size_t)n == len ? 0 : -E_IO;
}

s64 vfs_read_file(const char *path, void *buf, size_t cap) {
    file_t *f = NULL;
    int r = vfs_open(path, O_RDONLY, &f);
    if (r < 0) return r;
    ssize_t_k n = vfs_read(f, buf, cap);
    vfs_close(f);
    return n;
}

/* ------------------------------------------------------------------------- */
/* the locked entry points                                                   */
/* ------------------------------------------------------------------------- */
/*
 * Every one of these is the same three lines: take the lock, do the work,
 * let go.  They sit here, below the functions they call, so that the order
 * of the file says what depends on what.
 *
 * The composites above - vfs_append, vfs_write_file, vfs_read_file - are
 * built out of these and deliberately do not take the lock themselves.  It
 * is not recursive, and taking it twice on one thread would not race, it
 * would stop.
 */
int vfs_stat(const char *path, vstat_t *st) {
    fs_enter();
    int r = locked_vfs_stat(path, st);
    fs_leave();
    return r;
}

int vfs_mkdir(const char *path) {
    fs_enter();
    int r = locked_vfs_mkdir(path);
    fs_leave();
    return r;
}

int vfs_unlink(const char *path) {
    fs_enter();
    int r = locked_vfs_unlink(path);
    fs_leave();
    return r;
}

int vfs_open(const char *path, u32 flags, file_t **out) {
    fs_enter();
    int r = locked_vfs_open(path, flags, out);
    fs_leave();
    return r;
}

ssize_t_k vfs_read(file_t *f, void *buf, size_t len) {
    /* A read from a tty waits for a person and must not be holding
     * anything while it does. */
    bool position = file_position_enter(f);
    bool guard = f && f->vn && f->vn->fs && !f->vn->fs->reentrant;
    if (guard) fs_enter();
    ssize_t_k r = locked_vfs_read(f, buf, len);
    if (guard) fs_leave();
    if (position) file_position_leave(f);
    return r;
}

ssize_t_k vfs_write(file_t *f, const void *buf, size_t len) {
    /* A read from a tty waits for a person and must not be holding
     * anything while it does. */
    bool position = file_position_enter(f);
    bool guard = f && f->vn && f->vn->fs && !f->vn->fs->reentrant;
    if (guard) fs_enter();
    ssize_t_k r = locked_vfs_write(f, buf, len);
    if (guard) fs_leave();
    if (position) file_position_leave(f);
    return r;
}

int vfs_readdir(file_t *f, u32 index, dirent_k *out) {
    /* A read from a tty waits for a person and must not be holding
     * anything while it does. */
    bool guard = f && f->vn && f->vn->fs && !f->vn->fs->reentrant;
    if (guard) fs_enter();
    int r = locked_vfs_readdir(f, index, out);
    if (guard) fs_leave();
    return r;
}

#include "vfs_user_io.h"
