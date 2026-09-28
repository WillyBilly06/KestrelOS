/* ramfs.c - an in-memory filesystem.
 *
 * Used for the root, which is populated from the initrd the loader left in
 * memory.  Files start out pointing straight at the initrd image (no copy) and
 * are only copied into the heap when something writes to them, which keeps the
 * boot footprint down for the common read-only case.
 */
#include "kernel.h"
#include "vfs.h"
#include "mm.h"
#include "klog.h"
#include "time.h"
#include "../include/kestrel/kar.h"

typedef struct ramfs_node {
    char   name[VFS_NAME_MAX + 1];
    u32    type;
    u32    mode;
    u64    mtime;

    u8    *data;            /* file contents          */
    size_t size;
    size_t capacity;        /* 0 when data is borrowed from the initrd */

    struct ramfs_node *parent;
    struct ramfs_node *children;
    struct ramfs_node *sibling;

    vnode_t *vn;            /* cached vnode, created on demand */
} ramfs_node_t;

typedef struct {
    ramfs_node_t *root;
    size_t        bytes_used;
    int           node_count;
} ramfs_t;

static const vnode_ops_t ramfs_vops;

/* ------------------------------------------------------------------------- */
/* tree helpers                                                              */
/* ------------------------------------------------------------------------- */

static ramfs_node_t *node_new(const char *name, u32 type, ramfs_node_t *parent) {
    ramfs_node_t *n = kzalloc(sizeof *n);
    if (!n) return NULL;
    strlcpy(n->name, name, sizeof n->name);
    n->type = type;
    n->mode = (type == VN_DIR) ? 0755 : 0644;
    n->mtime = time_unix_seconds();
    n->parent = parent;
    if (parent) { n->sibling = parent->children; parent->children = n; }
    return n;
}

static ramfs_node_t *node_find(ramfs_node_t *dir, const char *name) {
    for (ramfs_node_t *c = dir->children; c; c = c->sibling)
        if (!strcmp(c->name, name)) return c;
    return NULL;
}

static void node_free(ramfs_t *fs, ramfs_node_t *n) {
    while (n->children) {
        ramfs_node_t *c = n->children;
        n->children = c->sibling;
        node_free(fs, c);
    }
    if (n->capacity && n->data) { kfree(n->data); fs->bytes_used -= n->capacity; }
    if (n->vn) n->vn->priv = NULL;
    fs->node_count--;
    kfree(n);
}

static vnode_t *node_vnode(filesystem_t *fs, ramfs_node_t *n) {
    if (n->vn) { n->vn->size = n->size; return vnode_ref(n->vn); }

    vnode_t *vn = kzalloc(sizeof *vn);
    if (!vn) return NULL;
    vn->type = n->type;
    vn->size = n->size;
    vn->priv = n;
    vn->fs   = fs;
    vn->ops  = &ramfs_vops;
    vn->refs = 1;
    n->vn = vn;
    return vn;
}

/* ------------------------------------------------------------------------- */
/* vnode operations                                                          */
/* ------------------------------------------------------------------------- */

static ssize_t_k ramfs_read(vnode_t *vn, void *buf, size_t len, u64 off) {
    ramfs_node_t *n = vn->priv;
    if (!n) return -E_NOENT;
    if (n->type != VN_FILE) return -E_ISDIR;
    if (off >= n->size) return 0;

    size_t avail = n->size - (size_t)off;
    if (len > avail) len = avail;
    memcpy(buf, n->data + off, len);
    return (ssize_t_k)len;
}

/* Grow the backing store, copying out of the initrd on the first write. */
static int ensure_capacity(ramfs_node_t *n, size_t need) {
    if (n->capacity >= need && n->data) return 0;

    size_t cap = n->capacity ? n->capacity : 64;
    while (cap < need) cap *= 2;

    u8 *fresh = kmalloc(cap);
    if (!fresh) return -E_NOMEM;
    if (n->data && n->size) memcpy(fresh, n->data, n->size);
    memset(fresh + n->size, 0, cap - n->size);

    if (n->capacity && n->data) kfree(n->data);
    n->data = fresh;
    n->capacity = cap;
    return 0;
}

static ssize_t_k ramfs_write(vnode_t *vn, const void *buf, size_t len, u64 off) {
    ramfs_node_t *n = vn->priv;
    if (!n) return -E_NOENT;
    if (n->type != VN_FILE) return -E_ISDIR;
    if (!len) return 0;
    if (off + len > (64ULL << 20)) return -E_NOSPC;   /* a sane per-file ceiling */

    int r = ensure_capacity(n, (size_t)off + len);
    if (r < 0) return r;

    /* Writing past the end leaves a zero-filled hole. */
    if (off > n->size) memset(n->data + n->size, 0, (size_t)off - n->size);

    memcpy(n->data + off, buf, len);
    if (off + len > n->size) n->size = (size_t)(off + len);
    n->mtime = time_unix_seconds();
    vn->size = n->size;
    return (ssize_t_k)len;
}

static int ramfs_truncate(vnode_t *vn, u64 size) {
    ramfs_node_t *n = vn->priv;
    if (!n) return -E_NOENT;
    if (n->type != VN_FILE) return -E_ISDIR;

    if (size > n->size) {
        int r = ensure_capacity(n, (size_t)size);
        if (r < 0) return r;
        memset(n->data + n->size, 0, (size_t)size - n->size);
    } else if (!n->capacity && n->data) {
        /* Shrinking a borrowed file still needs a private copy so the initrd
         * image is never modified. */
        int r = ensure_capacity(n, (size_t)size ? (size_t)size : 1);
        if (r < 0) return r;
    }
    n->size = (size_t)size;
    n->mtime = time_unix_seconds();
    vn->size = n->size;
    return 0;
}

static int ramfs_readdir(vnode_t *vn, u32 index, dirent_k *out) {
    ramfs_node_t *n = vn->priv;
    if (!n) return -E_NOENT;
    if (n->type != VN_DIR) return -E_NOTDIR;

    /* The list is built by prepending, so walk it backwards to keep the order
     * stable and roughly insertion-ordered. */
    u32 count = 0;
    for (ramfs_node_t *c = n->children; c; c = c->sibling) count++;
    if (index >= count) return -E_NOENT;

    u32 want = count - 1 - index;
    ramfs_node_t *c = n->children;
    for (u32 i = 0; i < want && c; i++) c = c->sibling;
    if (!c) return -E_NOENT;

    strlcpy(out->name, c->name, sizeof out->name);
    out->type = c->type;
    out->size = c->size;
    return 0;
}

static int ramfs_lookup(vnode_t *vn, const char *name, vnode_t **out) {
    ramfs_node_t *n = vn->priv;
    if (!n) return -E_NOENT;
    if (n->type != VN_DIR) return -E_NOTDIR;

    ramfs_node_t *c = node_find(n, name);
    if (!c) return -E_NOENT;
    vnode_t *cv = node_vnode(vn->fs, c);
    if (!cv) return -E_NOMEM;
    *out = cv;
    return 0;
}

static int ramfs_node_create(vnode_t *vn, const char *name, u32 type, vnode_t **out) {
    ramfs_node_t *n = vn->priv;
    ramfs_t *fs = vn->fs->priv;
    if (!n) return -E_NOENT;
    if (n->type != VN_DIR) return -E_NOTDIR;
    if (node_find(n, name)) return -E_EXIST;
    if (strlen(name) > VFS_NAME_MAX) return -E_NAMETOOLONG;

    ramfs_node_t *c = node_new(name, type, n);
    if (!c) return -E_NOMEM;
    fs->node_count++;

    if (out) {
        vnode_t *cv = node_vnode(vn->fs, c);
        if (!cv) return -E_NOMEM;
        *out = cv;
    }
    return 0;
}

static int ramfs_unlink(vnode_t *vn, const char *name) {
    ramfs_node_t *n = vn->priv;
    ramfs_t *fs = vn->fs->priv;
    if (!n) return -E_NOENT;

    ramfs_node_t *c = node_find(n, name);
    if (!c) return -E_NOENT;
    if (c->type == VN_DIR && c->children) return -E_NOTEMPTY;
    if (c->vn && c->vn->refs > 0) {
        /* Someone still holds it open; detach so the name goes away but the
         * memory survives until the last reference is dropped. */
        c->vn->priv = NULL;
    }

    ramfs_node_t **link = &n->children;
    while (*link && *link != c) link = &(*link)->sibling;
    if (*link) *link = c->sibling;
    node_free(fs, c);
    return 0;
}

static int ramfs_rename(vnode_t *vn, const char *from, const char *to) {
    ramfs_node_t *n = vn->priv;
    if (!n) return -E_NOENT;

    ramfs_node_t *c = node_find(n, from);
    if (!c) return -E_NOENT;
    if (node_find(n, to)) return -E_EXIST;
    strlcpy(c->name, to, sizeof c->name);
    return 0;
}

static int ramfs_stat(vnode_t *vn, vstat_t *st) {
    ramfs_node_t *n = vn->priv;
    if (!n) return -E_NOENT;
    st->type = n->type;
    st->size = n->size;
    st->mtime = n->mtime;
    st->mode = n->mode;
    return 0;
}

static void ramfs_release(vnode_t *vn) {
    ramfs_node_t *n = vn->priv;
    if (n) n->vn = NULL;
    kfree(vn);
}

static const vnode_ops_t ramfs_vops = {
    .read     = ramfs_read,
    .write    = ramfs_write,
    .truncate = ramfs_truncate,
    .readdir  = ramfs_readdir,
    .lookup   = ramfs_lookup,
    .create   = ramfs_node_create,
    .unlink   = ramfs_unlink,
    .rename   = ramfs_rename,
    .stat     = ramfs_stat,
    .release  = ramfs_release,
};

/* ------------------------------------------------------------------------- */
/* filesystem                                                                */
/* ------------------------------------------------------------------------- */

static u64 ramfs_total(filesystem_t *fs) { (void)fs; return pmm_total_bytes(); }
static u64 ramfs_free(filesystem_t *fs)  { (void)fs; return pmm_free_bytes(); }

static int ramfs_unmount_fs(filesystem_t *fs) {
    ramfs_t *r = fs->priv;
    if (r) { node_free(r, r->root); kfree(r); }
    kfree(fs->root);
    kfree(fs);
    return 0;
}

filesystem_t *ramfs_create(void) {
    filesystem_t *fs = kzalloc(sizeof *fs);
    ramfs_t *r = kzalloc(sizeof *r);
    if (!fs || !r) { kfree(fs); kfree(r); return NULL; }

    strlcpy(fs->name, "ramfs", sizeof fs->name);
    /* No static scratch buffers, and its reads wait for a person -
     * see the note beside the lock in vfs.c. */
    fs->reentrant = true;
    fs->priv = r;
    fs->total_bytes = ramfs_total;
    fs->free_bytes = ramfs_free;
    fs->unmount = ramfs_unmount_fs;

    r->root = node_new("", VN_DIR, NULL);
    if (!r->root) { kfree(fs); kfree(r); return NULL; }
    r->node_count = 1;

    fs->root = node_vnode(fs, r->root);
    if (!fs->root) { kfree(r->root); kfree(fs); kfree(r); return NULL; }
    return fs;
}

/* ------------------------------------------------------------------------- */
/* loading the initrd                                                        */
/* ------------------------------------------------------------------------- */

/* Create every directory along `path`, returning the final directory node. */
static ramfs_node_t *make_dirs(ramfs_t *fs, const char *path, bool include_last) {
    ramfs_node_t *cur = fs->root;
    const char *p = path;

    for (;;) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);

        bool last = true;
        for (const char *q = p; *q; q++) if (*q != '/') { last = false; break; }
        if (last && !include_last) break;

        char name[VFS_NAME_MAX + 1];
        if (len > VFS_NAME_MAX) return NULL;
        memcpy(name, start, len);
        name[len] = 0;

        ramfs_node_t *next = node_find(cur, name);
        if (!next) {
            next = node_new(name, VN_DIR, cur);
            if (!next) return NULL;
            fs->node_count++;
        } else if (next->type != VN_DIR) {
            return NULL;
        }
        cur = next;
    }
    return cur;
}

int ramfs_load_kar(filesystem_t *fs, const void *image, size_t len) {
    ramfs_t *r = fs->priv;
    const u8 *base = image;

    if (len < sizeof(kar_header)) { kerr("ramfs", "initrd is too small (%zu bytes)", len); return -E_INVAL; }
    const kar_header *h = image;
    if (h->magic != KAR_MAGIC) { kerr("ramfs", "initrd magic %#x is wrong", h->magic); return -E_INVAL; }
    if (h->total_size > len) {
        kerr("ramfs", "initrd claims %lu bytes but only %zu were loaded", h->total_size, len);
        return -E_INVAL;
    }
    if ((u64)h->entry_offset + (u64)h->entry_count * KAR_ENTRY_SIZE > len) {
        kerr("ramfs", "initrd entry table runs past the end of the image");
        return -E_INVAL;
    }

    int loaded = 0;
    for (u32 i = 0; i < h->entry_count; i++) {
        const kar_entry *e = (const kar_entry *)(base + h->entry_offset + (u64)i * KAR_ENTRY_SIZE);

        char name[KAR_NAME_MAX + 1];
        memcpy(name, e->name, KAR_NAME_MAX);
        name[KAR_NAME_MAX] = 0;
        if (!name[0] || name[0] != '/') { kwarn("ramfs", "initrd entry %u has a bad name; skipped", i); continue; }

        if (e->type == KAR_DIR) {
            if (!make_dirs(r, name, true)) { kwarn("ramfs", "cannot create directory %s", name); continue; }
            loaded++;
            continue;
        }
        if (e->type != KAR_FILE) { kwarn("ramfs", "initrd entry %s has unknown type %u", name, e->type); continue; }

        if (e->offset + e->size > len) {
            kerr("ramfs", "initrd entry %s runs past the end of the image", name);
            continue;
        }

        ramfs_node_t *dir = make_dirs(r, name, false);
        if (!dir) { kwarn("ramfs", "cannot create the parent directory of %s", name); continue; }

        const char *leaf = strrchr(name, '/');
        leaf = leaf ? leaf + 1 : name;
        if (!*leaf) continue;

        ramfs_node_t *f = node_find(dir, leaf);
        if (f) { kwarn("ramfs", "duplicate entry %s in the initrd", name); continue; }

        f = node_new(leaf, VN_FILE, dir);
        if (!f) { kerr("ramfs", "out of memory loading %s", name); return -E_NOMEM; }
        r->node_count++;

        /* Point straight at the loaded image; a write copies it first. */
        f->data = (u8 *)base + e->offset;
        f->size = (size_t)e->size;
        f->capacity = 0;
        f->mode = e->mode ? e->mode : 0644;
        loaded++;
    }

    kinfo("ramfs", "loaded %d entries from a %zu KiB initrd", loaded, len / 1024);
    return loaded;
}
