#ifndef KESTREL_VFS_H
#define KESTREL_VFS_H

#include "kernel.h"

typedef s64 ssize_t_k;

/* Node types. */
enum { VN_NONE = 0, VN_FILE = 1, VN_DIR = 2, VN_CHR = 3, VN_BLK = 4 };

/* Open flags, matching the values the user-mode libc passes. */
#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_ACCMODE  0x0003
#define O_CREAT    0x0040
#define O_TRUNC    0x0200
#define O_APPEND   0x0400
#define O_DIRECTORY 0x1000

/* Errors, returned negated from every entry point. */
#define E_PERM      1
#define E_NOENT     2
#define E_INTR      4
#define E_IO        5
#define E_BADF      9
#define E_NOMEM    12
#define E_ACCES    13
#define E_FAULT    14
#define E_BUSY     16
#define E_EXIST    17
#define E_XDEV     18
#define E_NODEV    19
#define E_NOTDIR   20
#define E_ISDIR    21
#define E_INVAL    22
#define E_MFILE    24
#define E_NOSPC    28
#define E_SPIPE    29
#define E_ROFS     30
#define E_NAMETOOLONG 36
#define E_NOSYS    38
#define E_NOTEMPTY 39
#define E_AGAIN    11
#define E_TIMEDOUT 110

#define VFS_NAME_MAX 255
#define VFS_PATH_MAX 1024

typedef struct vnode vnode_t;
typedef struct filesystem filesystem_t;
typedef struct blockdev blockdev_t;

typedef struct {
    char name[VFS_NAME_MAX + 1];
    u32  type;
    u64  size;
} dirent_k;

typedef struct {
    u32 type;
    u64 size;
    u64 mtime;      /* unix seconds, 0 when the filesystem has no timestamps */
    u32 mode;
} vstat_t;

/* Fixed, bounded user payload: callbacks receive kernel memory, never a user
 * pointer. Unknown commands must fail; a NULL optional argument stays NULL. */
typedef struct {
    u32 in_bytes, out_bytes;
    bool required;
} vfs_ioctl_shape_t;

typedef struct vnode_ops {
    ssize_t_k (*read)(vnode_t *, void *buf, size_t len, u64 off);
    ssize_t_k (*write)(vnode_t *, const void *buf, size_t len, u64 off);
    int  (*truncate)(vnode_t *, u64 size);
    int  (*readdir)(vnode_t *, u32 index, dirent_k *out);
    int  (*lookup)(vnode_t *, const char *name, vnode_t **out);
    int  (*create)(vnode_t *, const char *name, u32 type, vnode_t **out);
    int  (*unlink)(vnode_t *, const char *name);
    int  (*rename)(vnode_t *dir, const char *from, const char *to);
    int  (*stat)(vnode_t *, vstat_t *);
    int  (*ioctl)(vnode_t *, u32 cmd, void *arg);
    int  (*sync)(vnode_t *);
    void (*release)(vnode_t *);      /* last reference dropped */
    int  (*ioctl_shape)(vnode_t *, u32 cmd, vfs_ioctl_shape_t *);
} vnode_ops_t;

struct vnode {
    u32                type;
    u64                size;
    u32                refs;
    void              *priv;         /* filesystem private data */
    filesystem_t      *fs;
    const vnode_ops_t *ops;
};

struct filesystem {
    char          name[16];
    blockdev_t   *dev;
    vnode_t      *root;
    void         *priv;
    bool          readonly;

    /* Whether two callers may be inside this driver at once.
     *
     * False for the disk filesystems, which are built out of static scratch
     * buffers - one per purpose, reused by whoever is inside - and would
     * quietly corrupt each other.  True for devfs and ramfs, which have no
     * such buffers and whose reads legitimately wait until somebody types
     * something.  Holding a lock across that is a stopped machine, which is
     * exactly what happened the first time this was tried. */
    bool          reentrant;
    int         (*unmount)(filesystem_t *);
    int         (*sync)(filesystem_t *);
    u64         (*free_bytes)(filesystem_t *);
    u64         (*total_bytes)(filesystem_t *);
};

/* Open file description. */
typedef struct {
    vnode_t *vn;
    u64      pos;
    u32      flags;
    bool     in_use;
    u32      refs;          /* file-pool lock: descriptors + in-flight users */
    u32      io_busy;       /* shared regular-file position; never held for CHR */
} file_t;

void vfs_init(void);

/* Mount table.  `path` must already exist as a directory. */
int  vfs_mount(const char *path, filesystem_t *fs);
int  vfs_unmount(const char *path);
filesystem_t *vfs_mounted_at(const char *path);
int  vfs_list_mounts(char *buf, size_t cap);

/* Path operations.  All return 0 or a negative error. */
int  vfs_resolve(const char *path, vnode_t **out);
int  vfs_stat(const char *path, vstat_t *st);
int  vfs_mkdir(const char *path);
int  vfs_unlink(const char *path);
int  vfs_rename(const char *from, const char *to);
int  vfs_sync(void);

/* Handle operations. */
int       vfs_open(const char *path, u32 flags, file_t **out);
int       vfs_open_vnode(vnode_t *vn, u32 flags, file_t **out);
int       pipe_create(file_t **read_end, file_t **write_end);
void      vfs_close(file_t *f);
/* Requires an existing retained reference (or the descriptor-table lock).
 * Returns another reference; NULL for a retiring/invalid description. */
file_t   *vfs_file_ref(file_t *f);
ssize_t_k vfs_read(file_t *f, void *buf, size_t len);
ssize_t_k vfs_write(file_t *f, const void *buf, size_t len);
/* iov is an immutable KERNEL snapshot. Payload addresses belong to the current
 * pml4; caller retains the file and process through all waits. */
typedef struct { u64 base, len; } vfs_iovec_t;
#define VFS_IOV_MAX 1024u
ssize_t_k vfs_user_iov(file_t *f, u64 pml4, const vfs_iovec_t *iov, size_t count, bool write);
ssize_t_k vfs_user_io(file_t *f, u64 pml4, u64 address, size_t len, bool write);
s64       vfs_seek(file_t *f, s64 off, int whence);
int       vfs_readdir(file_t *f, u32 index, dirent_k *out);
int       vfs_ioctl(file_t *f, u32 cmd, void *arg);
int       vfs_user_ioctl(file_t *f, u64 pml4, u32 cmd, u64 address);
int       vfs_truncate(file_t *f, u64 size);

/* Convenience wrappers used by the kernel itself. */
int  vfs_append(const char *path, const void *data, size_t len);
int  vfs_write_file(const char *path, const void *data, size_t len);
s64  vfs_read_file(const char *path, void *buf, size_t cap);

/* Reference counting. */
vnode_t *vnode_ref(vnode_t *vn);
void     vnode_unref(vnode_t *vn);

/* Filesystem drivers register a probe so vfs_mount_auto can pick one. */
typedef filesystem_t *(*fs_probe_fn)(blockdev_t *dev);
void vfs_register_fs(const char *name, fs_probe_fn probe);
filesystem_t *vfs_probe(blockdev_t *dev);

/* ram filesystem, used for the root populated from the initrd */
filesystem_t *ramfs_create(void);
int  ramfs_load_kar(filesystem_t *fs, const void *image, size_t len);

/* device filesystem, mounted at /dev */
filesystem_t *devfs_create(void);
typedef struct devfs_ops {
    ssize_t_k (*read)(void *ctx, void *buf, size_t len, u64 off);
    ssize_t_k (*write)(void *ctx, const void *buf, size_t len, u64 off);
    int       (*ioctl)(void *ctx, u32 cmd, void *arg);
    u64       (*size)(void *ctx);
    int       (*ioctl_shape)(void *ctx, u32 cmd, vfs_ioctl_shape_t *);
} devfs_ops_t;
int devfs_register(const char *name, u32 type, const devfs_ops_t *ops, void *ctx);
void devfs_unregister(const char *name);

const char *vfs_strerror(int err);

#endif
