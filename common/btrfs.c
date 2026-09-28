/* btrfs.c - reading Btrfs.
 *
 * See btrfs.h for the shape of the problem: two address spaces, and the map
 * between them stored inside the filesystem it describes.
 */
#include "btrfs.h"

/* ------------------------------------------------------------ little-endian */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void copy_bytes(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    while (n--) *d++ = *s++;
}
static void zero_bytes(void *dst, size_t n) {
    uint8_t *d = dst; while (n--) *d++ = 0;
}
static void say(btrfs_volume_t *v, const char *msg) {
    size_t i = 0;
    while (msg[i] && i < sizeof v->error - 1) { v->error[i] = msg[i]; i++; }
    v->error[i] = 0;
}

/* ------------------------------------------------------------- the format */

#define SUPER_OFFSET        65536
#define SUPER_MAGIC         0x4D5F53665248425FULL   /* "_BHRfS_M" */

#define SB_MAGIC            0x040
#define SB_ROOT             0x050
#define SB_CHUNK_ROOT       0x058
#define SB_TOTAL_BYTES      0x070
#define SB_SECTORSIZE       0x090
#define SB_NODESIZE         0x094
#define SB_SYS_ARRAY_SIZE   0x0A0
#define SB_SYS_ARRAY        0x32B

#define HDR_BYTES           101
#define HDR_NRITEMS         0x060
#define HDR_LEVEL           0x064

#define KEY_BYTES           17
#define KEY_OBJECTID        0x00
#define KEY_TYPE            0x08
#define KEY_OFFSET          0x09

#define ITEM_BYTES          25
#define ITEM_OFFSET         0x11
#define ITEM_SIZE           0x15

#define KEYPTR_BYTES        33
#define KEYPTR_BLOCK        0x11

#define CHUNK_LENGTH        0x00
#define CHUNK_NUM_STRIPES   0x2C
#define CHUNK_STRIPE        0x30
#define STRIPE_BYTES        32
#define STRIPE_OFFSET       0x08

#define ROOT_ITEM_BYTENR    0x0B0

#define INODE_SIZE_OFF      0x010
#define INODE_MODE_OFF      0x034

#define DIR_LOCATION        0x00
#define DIR_NAME_LEN        0x1B
#define DIR_TYPE            0x1D
#define DIR_NAME            0x1E

#define FE_RAM_BYTES        0x08
#define FE_COMPRESSION      0x10
#define FE_TYPE             0x14
#define FE_DISK_BYTENR      0x15
#define FE_OFFSET           0x25
#define FE_NUM_BYTES        0x2D
#define FE_INLINE_DATA      0x15

#define TYPE_INODE_ITEM     1
#define TYPE_DIR_INDEX      96
#define TYPE_EXTENT_DATA    108
#define TYPE_ROOT_ITEM      132
#define TYPE_CHUNK_ITEM     228

#define FS_TREE_OBJECTID    5
#define FIRST_FREE_OBJECTID 256

#define EXTENT_INLINE       0
#define EXTENT_REG          1

#define MODE_DIRECTORY      0x4000

/* ------------------------------------------------- logical to physical */

bool btrfs_resolve(const btrfs_volume_t *v, uint64_t logical,
                   uint64_t *physical) {
    for (int i = 0; i < v->chunk_count; i++) {
        const btrfs_chunk_map_t *c = &v->chunks[i];
        if (logical >= c->logical && logical < c->logical + c->length) {
            if (physical) *physical = c->physical + (logical - c->logical);
            return true;
        }
    }
    return false;
}

static bool add_chunk(btrfs_volume_t *v, uint64_t logical, const uint8_t *chunk) {
    uint32_t stripes = rd16(chunk + CHUNK_NUM_STRIPES);
    if (stripes != 1) {
        /* More than one stripe means the data is spread or mirrored across
         * devices, and this reads one device.  Refusing is the only honest
         * answer: taking the first stripe would return a quarter of the file
         * and three quarters of something else. */
        say(v, "this volume spans more than one device or is mirrored");
        return false;
    }
    if (v->chunk_count >= BTRFS_MAX_CHUNKS) {
        say(v, "this volume is in more pieces than this driver maps");
        return false;
    }

    btrfs_chunk_map_t *m = &v->chunks[v->chunk_count++];
    m->logical  = logical;
    m->length   = rd64(chunk + CHUNK_LENGTH);
    m->physical = rd64(chunk + CHUNK_STRIPE + STRIPE_OFFSET);
    return true;
}

/* -------------------------------------------------------------- the disk */

static bool read_logical(btrfs_volume_t *v, uint64_t logical, uint32_t bytes,
                         void *buf) {
    uint64_t physical = 0;
    if (!btrfs_resolve(v, logical, &physical)) return false;
    if (physical % 512) return false;
    return v->read(v->ctx, physical / 512, (bytes + 511) / 512, buf);
}

/* -------------------------------------------------------------- the trees
 *
 * Every tree is the same shape, so one walk serves all of them: a node with a
 * level above zero holds pointers to more nodes, and a node at level zero
 * holds items.  This visits every item in a tree and hands each to a
 * function - which is more work than a keyed search would be, and is what a
 * reader can be sure is right.
 */
typedef bool (*item_fn)(void *ctx, uint64_t objectid, uint8_t type,
                        uint64_t offset, const uint8_t *data, uint32_t size);

static bool walk_tree(btrfs_volume_t *v, uint64_t logical, int depth,
                      item_fn fn, void *ctx) {
    if (depth > 8) return true;                  /* deeper than this is wrong */

    static uint8_t node[4][65536];
    if (v->node_size > sizeof node[0]) return false;
    if (depth >= 4) return true;

    uint8_t *n = node[depth];
    if (!read_logical(v, logical, v->node_size, n)) return false;

    uint32_t items = rd32(n + HDR_NRITEMS);
    uint8_t level = n[HDR_LEVEL];

    if (level == 0) {
        for (uint32_t i = 0; i < items; i++) {
            const uint8_t *it = n + HDR_BYTES + (size_t)i * ITEM_BYTES;
            if (HDR_BYTES + (size_t)(i + 1) * ITEM_BYTES > v->node_size) break;

            uint64_t objectid = rd64(it + KEY_OBJECTID);
            uint8_t type = it[KEY_TYPE];
            uint64_t off = rd64(it + KEY_OFFSET);
            uint32_t data_at = rd32(it + ITEM_OFFSET);
            uint32_t size = rd32(it + ITEM_SIZE);

            if ((uint64_t)HDR_BYTES + data_at + size > v->node_size) continue;

            /* Returning false from the callback stops the walk - that is how
             * a search says it has found what it wanted. */
            if (!fn(ctx, objectid, type, off, n + HDR_BYTES + data_at, size))
                return true;
        }
        return true;
    }

    for (uint32_t i = 0; i < items; i++) {
        const uint8_t *kp = n + HDR_BYTES + (size_t)i * KEYPTR_BYTES;
        if (HDR_BYTES + (size_t)(i + 1) * KEYPTR_BYTES > v->node_size) break;
        uint64_t child = rd64(kp + KEYPTR_BLOCK);
        if (!walk_tree(v, child, depth + 1, fn, ctx)) return false;
    }
    return true;
}

/* ---------------------------------------------------------------- mounting */

typedef struct {
    btrfs_volume_t *v;
    bool ok;
} chunk_ctx_t;

static bool take_chunk(void *ctx, uint64_t objectid, uint8_t type,
                       uint64_t offset, const uint8_t *data, uint32_t size) {
    (void)objectid; (void)size;
    chunk_ctx_t *c = ctx;
    if (type != TYPE_CHUNK_ITEM) return true;

    /* Already known from the superblock's own array; the two overlap. */
    uint64_t where = 0;
    if (btrfs_resolve(c->v, offset, &where)) return true;

    if (!add_chunk(c->v, offset, data)) c->ok = false;
    return true;
}

typedef struct {
    uint64_t fs_tree;
    bool found;
} root_ctx_t;

static bool take_root(void *ctx, uint64_t objectid, uint8_t type,
                      uint64_t offset, const uint8_t *data, uint32_t size) {
    (void)offset;
    root_ctx_t *r = ctx;
    if (type != TYPE_ROOT_ITEM || objectid != FS_TREE_OBJECTID) return true;
    if (size < ROOT_ITEM_BYTENR + 8) return true;
    r->fs_tree = rd64(data + ROOT_ITEM_BYTENR);
    r->found = true;
    return false;                                /* stop; this is the one */
}

bool btrfs_mount(btrfs_volume_t *v, void *ctx, btrfs_read_fn read) {
    zero_bytes(v, sizeof *v);
    v->ctx = ctx;
    v->read = read;

    static uint8_t sb[4096];
    if (!read(ctx, SUPER_OFFSET / 512, sizeof sb / 512, sb)) {
        say(v, "the superblock could not be read");
        return false;
    }
    if (rd64(sb + SB_MAGIC) != SUPER_MAGIC) {
        say(v, "this volume is not Btrfs");
        return false;
    }

    v->sector_size = rd32(sb + SB_SECTORSIZE);
    v->node_size   = rd32(sb + SB_NODESIZE);
    v->total_bytes = rd64(sb + SB_TOTAL_BYTES);
    v->root_tree   = rd64(sb + SB_ROOT);
    v->chunk_tree  = rd64(sb + SB_CHUNK_ROOT);

    if (!v->node_size || v->node_size > 65536 || !v->sector_size) {
        say(v, "the superblock contradicts itself");
        return false;
    }

    /* The bootstrap: enough of the map to find the map.  Pairs of a key and a
     * chunk, packed one after another, for as many bytes as the superblock
     * says. */
    uint32_t array_bytes = rd32(sb + SB_SYS_ARRAY_SIZE);
    if (array_bytes > sizeof sb - SB_SYS_ARRAY) {
        say(v, "the superblock's chunk array runs past the end of it");
        return false;
    }

    uint32_t at = 0;
    while (at + KEY_BYTES + CHUNK_STRIPE + STRIPE_BYTES <= array_bytes) {
        const uint8_t *key = sb + SB_SYS_ARRAY + at;
        const uint8_t *chunk = key + KEY_BYTES;

        if (key[KEY_TYPE] != TYPE_CHUNK_ITEM) break;
        uint64_t logical = rd64(key + KEY_OFFSET);
        uint32_t stripes = rd16(chunk + CHUNK_NUM_STRIPES);
        if (!stripes) break;

        if (!add_chunk(v, logical, chunk)) return false;
        at += KEY_BYTES + CHUNK_STRIPE + stripes * STRIPE_BYTES;
    }

    if (!v->chunk_count) {
        say(v, "the superblock carries no chunk map, so nothing can be found");
        return false;
    }

    /* Now the rest of the map, from the chunk tree the bootstrap just made
     * reachable.  A volume small enough for the bootstrap to cover entirely
     * adds nothing here, which is not an error. */
    {
        chunk_ctx_t c = { v, true };
        walk_tree(v, v->chunk_tree, 0, take_chunk, &c);
        if (!c.ok) return false;
    }

    /* And the tree the files are actually in. */
    {
        root_ctx_t r = { 0, false };
        walk_tree(v, v->root_tree, 0, take_root, &r);
        if (!r.found) {
            say(v, "the root tree holds no filesystem tree");
            return false;
        }
        v->fs_tree = r.fs_tree;
    }

    v->mounted = true;
    return true;
}

/* ----------------------------------------------------------------- inodes */

typedef struct {
    uint64_t want;
    btrfs_file_t *out;
    bool found;
} inode_ctx_t;

static bool take_inode(void *ctx, uint64_t objectid, uint8_t type,
                       uint64_t offset, const uint8_t *data, uint32_t size) {
    (void)offset;
    inode_ctx_t *c = ctx;
    if (type != TYPE_INODE_ITEM || objectid != c->want) return true;
    if (size < INODE_MODE_OFF + 4) return true;

    zero_bytes(c->out, sizeof *c->out);
    c->out->objectid = objectid;
    c->out->size = rd64(data + INODE_SIZE_OFF);
    c->out->mode = rd32(data + INODE_MODE_OFF);
    c->out->directory = (c->out->mode & 0xF000) == MODE_DIRECTORY;
    c->found = true;
    return false;
}

static bool read_inode(btrfs_volume_t *v, uint64_t objectid, btrfs_file_t *out) {
    inode_ctx_t c = { objectid, out, false };
    walk_tree(v, v->fs_tree, 0, take_inode, &c);
    return c.found;
}

/* ------------------------------------------------------------ directories */

typedef struct {
    uint64_t dir;
    uint32_t want;
    uint32_t seen;
    char    *name;
    size_t   name_cap;
    uint64_t child;
    bool     found;

    const char *match;          /* when looking for one name rather than an index */
} dir_ctx_t;

static bool same_name(const char *a, const char *b, uint32_t alen) {
    for (uint32_t i = 0; i < alen; i++) {
        if (!b[i] || a[i] != b[i]) return false;
    }
    return b[alen] == 0;
}

static bool take_dir_entry(void *ctx, uint64_t objectid, uint8_t type,
                           uint64_t offset, const uint8_t *data, uint32_t size) {
    (void)offset;
    dir_ctx_t *c = ctx;
    if (type != TYPE_DIR_INDEX || objectid != c->dir) return true;
    if (size < DIR_NAME) return true;

    uint16_t namelen = rd16(data + DIR_NAME_LEN);
    if (DIR_NAME + namelen > size) return true;

    const char *name = (const char *)data + DIR_NAME;
    uint64_t child = rd64(data + DIR_LOCATION + KEY_OBJECTID);

    if (c->match) {
        if (!same_name(name, c->match, namelen)) return true;
    } else {
        if (c->seen++ != c->want) return true;
    }

    if (c->name) {
        size_t o = 0;
        while (o < namelen && o + 1 < c->name_cap) { c->name[o] = name[o]; o++; }
        c->name[o] = 0;
    }
    c->child = child;
    c->found = true;
    return false;
}

bool btrfs_readdir(btrfs_volume_t *v, const btrfs_file_t *dir, uint32_t index,
                   char *name_out, size_t name_cap, btrfs_file_t *entry_out) {
    if (!v->mounted || !dir || !dir->directory) return false;

    dir_ctx_t c;
    zero_bytes(&c, sizeof c);
    c.dir = dir->objectid;
    c.want = index;
    c.name = name_out;
    c.name_cap = name_cap;

    walk_tree(v, v->fs_tree, 0, take_dir_entry, &c);
    if (!c.found) return false;
    if (entry_out && !read_inode(v, c.child, entry_out)) return false;
    return true;
}

bool btrfs_lookup(btrfs_volume_t *v, const char *path, btrfs_file_t *out) {
    if (!v->mounted || !path) return false;

    btrfs_file_t here;
    if (!read_inode(v, FIRST_FREE_OBJECTID, &here)) {
        say(v, "the filesystem tree has no root directory");
        return false;
    }

    while (*path == '/' || *path == '\\') path++;
    if (!*path) { if (out) *out = here; return true; }

    while (*path) {
        char part[256];
        size_t n = 0;
        while (path[n] && path[n] != '/' && path[n] != '\\' &&
               n + 1 < sizeof part) { part[n] = path[n]; n++; }
        part[n] = 0;
        path += n;
        while (*path == '/' || *path == '\\') path++;

        if (!here.directory) return false;

        dir_ctx_t c;
        zero_bytes(&c, sizeof c);
        c.dir = here.objectid;
        c.match = part;

        walk_tree(v, v->fs_tree, 0, take_dir_entry, &c);
        if (!c.found) return false;
        if (!read_inode(v, c.child, &here)) return false;
    }

    if (out) *out = here;
    return true;
}

/* ------------------------------------------------------------------ files */

typedef struct {
    btrfs_volume_t *v;
    uint64_t objectid;
    uint64_t want;              /* the byte being asked for */
    uint64_t logical;           /* where it is, 0 for a hole */
    const uint8_t *inline_at;
    uint32_t inline_bytes;
    uint64_t inline_start;
    bool found;
    bool refused;
} extent_ctx_t;

static bool take_extent(void *ctx, uint64_t objectid, uint8_t type,
                        uint64_t offset, const uint8_t *data, uint32_t size) {
    extent_ctx_t *c = ctx;
    if (type != TYPE_EXTENT_DATA || objectid != c->objectid) return true;
    if (size < FE_TYPE + 1) return true;

    if (data[FE_COMPRESSION] != 0) {
        /* Btrfs can store a file compressed.  Returning the bytes on the disk
         * would be returning something that is not the file. */
        c->refused = true;
        return false;
    }

    uint8_t kind = data[FE_TYPE];

    if (kind == EXTENT_INLINE) {
        uint64_t ram = rd64(data + FE_RAM_BYTES);
        if (c->want < offset || c->want >= offset + ram) return true;
        if (size <= FE_INLINE_DATA) return true;
        c->inline_at = data + FE_INLINE_DATA;
        c->inline_bytes = size - FE_INLINE_DATA;
        c->inline_start = offset;
        c->found = true;
        return false;
    }

    if (size < FE_NUM_BYTES + 8) return true;
    uint64_t num = rd64(data + FE_NUM_BYTES);
    if (c->want < offset || c->want >= offset + num) return true;

    uint64_t disk = rd64(data + FE_DISK_BYTENR);
    if (!disk) { c->logical = 0; c->found = true; return false; }  /* a hole */

    uint64_t into = rd64(data + FE_OFFSET);
    c->logical = disk + into + (c->want - offset);
    c->found = true;
    return false;
}

long btrfs_read_file(btrfs_volume_t *v, const btrfs_file_t *f,
                     uint64_t offset, void *buf, uint32_t len) {
    if (!v->mounted || !f) return -1;
    if (offset >= f->size) return 0;
    if (offset + len > f->size) len = (uint32_t)(f->size - offset);

    static uint8_t block[65536];
    uint8_t *out = buf;
    uint32_t done = 0;

    while (done < len) {
        extent_ctx_t c;
        zero_bytes(&c, sizeof c);
        c.v = v;
        c.objectid = f->objectid;
        c.want = offset + done;

        walk_tree(v, v->fs_tree, 0, take_extent, &c);

        if (c.refused) { say(v, "this file is stored compressed"); return -1; }

        if (!c.found) {
            /* No extent covers it: a sparse file's hole. */
            zero_bytes(out + done, len - done);
            return (long)len;
        }

        if (c.inline_at) {
            /* A small file kept inside the tree itself. */
            uint64_t into = c.want - c.inline_start;
            if (into >= c.inline_bytes) { zero_bytes(out + done, len - done); return (long)len; }
            uint32_t take = c.inline_bytes - (uint32_t)into;
            if (take > len - done) take = len - done;
            copy_bytes(out + done, c.inline_at + into, take);
            done += take;
            continue;
        }

        if (!c.logical) {
            uint32_t take = len - done;
            zero_bytes(out + done, take);
            done += take;
            continue;
        }

        /* One sector at a time: the extent says where the file's bytes are in
         * the logical space, and that still has to be mapped to the disk. */
        uint64_t aligned = c.logical & ~(uint64_t)(v->sector_size - 1);
        uint32_t into = (uint32_t)(c.logical - aligned);
        if (v->sector_size > sizeof block) return -1;
        if (!read_logical(v, aligned, v->sector_size, block)) return -1;

        uint32_t take = v->sector_size - into;
        if (take > len - done) take = len - done;
        copy_bytes(out + done, block + into, take);
        done += take;
    }
    return (long)done;
}

/* ------------------------------------------------------------------- tests */

int btrfs_selftest(void) {
    int failures = 0;

    /* The map from logical addresses to real ones, which is the thing about
     * this format with no counterpart in the others. */
    btrfs_volume_t v;
    zero_bytes(&v, sizeof v);
    v.chunk_count = 2;
    v.chunks[0].logical = 0x100000; v.chunks[0].length = 0x100000;
    v.chunks[0].physical = 0x400000;
    v.chunks[1].logical = 0x800000; v.chunks[1].length = 0x080000;
    v.chunks[1].physical = 0x900000;

    uint64_t where = 0;
    if (!btrfs_resolve(&v, 0x100000, &where) || where != 0x400000) failures++;
    if (!btrfs_resolve(&v, 0x1FFFFF, &where) || where != 0x4FFFFF) failures++;
    if (!btrfs_resolve(&v, 0x800010, &where) || where != 0x900010) failures++;

    /* Outside every chunk.  Answering with something plausible here is how a
     * reader ends up reading a part of the disk that belongs to nothing. */
    if (btrfs_resolve(&v, 0x0FFFFF, &where)) failures++;
    if (btrfs_resolve(&v, 0x200000, &where)) failures++;
    if (btrfs_resolve(&v, 0x880000, &where)) failures++;

    return failures;
}
