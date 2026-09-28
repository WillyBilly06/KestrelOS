/* xfs.c - reading XFS.
 *
 * See xfs.h for why this file has its own byte accessors and does not share
 * the ones next door: XFS is big-endian and every other filesystem here is
 * not, and a reader that borrows the wrong accessor reads plausible wrong
 * numbers rather than failing.
 */
#include "xfs.h"

/* --------------------------------------------------------------- big-endian
 *
 * Most significant byte first, which is the opposite of everything else here.
 */
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | p[3];
}
static uint64_t rd64(const uint8_t *p) {
    return ((uint64_t)rd32(p) << 32) | rd32(p + 4);
}

static void copy_bytes(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    while (n--) *d++ = *s++;
}
static void zero_bytes(void *dst, size_t n) {
    uint8_t *d = dst; while (n--) *d++ = 0;
}
static void say(xfs_volume_t *v, const char *msg) {
    size_t i = 0;
    while (msg[i] && i < sizeof v->error - 1) { v->error[i] = msg[i]; i++; }
    v->error[i] = 0;
}

/* ------------------------------------------------------------- the format */

#define SB_MAGIC              0x58465342u      /* "XFSB" */
#define SB_BLOCKSIZE          0x004
#define SB_DBLOCKS            0x008
#define SB_ROOTINO            0x038
#define SB_AGBLOCKS           0x054
#define SB_AGCOUNT            0x058
#define SB_VERSIONNUM         0x064
#define SB_INODESIZE          0x068
#define SB_BLOCKLOG           0x078
#define SB_INODELOG           0x07A
#define SB_INOPBLOG           0x07B
#define SB_AGBLKLOG           0x07C
#define SB_FEATURES2          0x0C8
#define SB_FEATURES_INCOMPAT  0x0D8

#define SB_VERSION_NUMBITS    0x000F
#define SB_VERSION_5          5
#define SB_VERSION2_FTYPE     0x00000200u
#define SB_FEAT_INCOMPAT_FTYPE 0x00000001u

#define DINODE_MAGIC          0x494E            /* "IN" */
#define DI_MAGIC              0x00
#define DI_MODE               0x02
#define DI_VERSION            0x04
#define DI_FORMAT             0x05
#define DI_SIZE               0x38
#define DI_NEXTENTS           0x4C
#define DI_FORKOFF            0x52

#define DINODE_V2_SIZE        100
#define DINODE_V3_SIZE        176

#define FMT_LOCAL             1
#define FMT_EXTENTS           2
#define FMT_BTREE             3

#define MODE_DIRECTORY        0x4000

#define EXTENT_BYTES          16

/* ------------------------------------------------------------- extents
 *
 * One extent is sixteen bytes holding four fields packed across the whole 128
 * bits, most significant first:
 *
 *     bit 127        was this range ever written to
 *     bits 126..73   where in the file it starts, in blocks   (54 bits)
 *     bits 72..21    where on the disk it starts, in blocks   (52 bits)
 *     bits 20..0     how many blocks                          (21 bits)
 *
 * None of those boundaries is on a byte, which is why this is done with shifts
 * over two 64-bit halves rather than by reading fields.
 */
void xfs_unpack_extent(const uint8_t *raw, uint64_t *file_block,
                       uint64_t *disk_block, uint32_t *count, bool *written) {
    uint64_t hi = rd64(raw);
    uint64_t lo = rd64(raw + 8);

    if (written) *written = (hi >> 63) == 0;      /* the flag means UNwritten */
    if (file_block) *file_block = (hi >> 9) & 0x3FFFFFFFFFFFFFull;   /* 54 bits */

    /* The disk block straddles the halves: nine bits at the bottom of the
     * first word and forty-three at the top of the second. */
    if (disk_block)
        *disk_block = ((hi & 0x1FFull) << 43) | (lo >> 21);

    if (count) *count = (uint32_t)(lo & 0x1FFFFFull);                /* 21 bits */
}

/* ---------------------------------------------------------------- mounting */

bool xfs_mount(xfs_volume_t *v, void *ctx, xfs_read_fn read) {
    zero_bytes(v, sizeof *v);
    v->ctx = ctx;
    v->read = read;

    static uint8_t sb[512];
    if (!read(ctx, 0, 1, sb)) { say(v, "the first sector could not be read"); return false; }

    if (rd32(sb) != SB_MAGIC) { say(v, "this volume is not XFS"); return false; }

    v->block_size = rd32(sb + SB_BLOCKSIZE);
    v->block_log  = sb[SB_BLOCKLOG];
    if (v->block_size != (1u << v->block_log) ||
        v->block_size < 512 || v->block_size > 65536) {
        say(v, "the block size is not one this driver reads");
        return false;
    }

    v->total_blocks        = rd64(sb + SB_DBLOCKS);
    v->root_inode          = rd64(sb + SB_ROOTINO);
    v->ag_blocks           = rd32(sb + SB_AGBLOCKS);
    v->ag_count            = rd32(sb + SB_AGCOUNT);
    v->inode_size          = rd16(sb + SB_INODESIZE);
    v->inode_per_block_log = sb[SB_INOPBLOG];
    v->ag_block_log        = sb[SB_AGBLKLOG];

    uint16_t version = rd16(sb + SB_VERSIONNUM);
    v->version_5 = (version & SB_VERSION_NUMBITS) == SB_VERSION_5;

    /* Whether a directory entry carries a type byte, which changes the size of
     * every entry.  Two different feature words say so depending on the
     * layout, and reading the wrong one for the layout in hand gets every name
     * after the first one wrong. */
    if (v->version_5)
        v->has_file_type = (rd32(sb + SB_FEATURES_INCOMPAT) &
                            SB_FEAT_INCOMPAT_FTYPE) != 0;
    else
        v->has_file_type = (rd32(sb + SB_FEATURES2) & SB_VERSION2_FTYPE) != 0;

    if (!v->ag_blocks || !v->ag_count || v->inode_size < 256) {
        say(v, "the superblock contradicts itself");
        return false;
    }

    v->mounted = true;
    return true;
}

/* --------------------------------------------------------------- the disk */

static bool read_block(xfs_volume_t *v, uint64_t block, void *buf) {
    uint32_t per = v->block_size / 512;
    return v->read(v->ctx, block * per, per, buf);
}

/* ---------------------------------------------------------------- inodes
 *
 * An inode number is three fields in one, and the widths come from the
 * superblock rather than being fixed.
 */
static bool read_inode(xfs_volume_t *v, uint64_t ino, xfs_file_t *out) {
    uint64_t agno   = ino >> (v->ag_block_log + v->inode_per_block_log);
    uint64_t agbno  = (ino >> v->inode_per_block_log) &
                      ((1ull << v->ag_block_log) - 1);
    uint64_t offset = ino & ((1ull << v->inode_per_block_log) - 1);

    if (agno >= v->ag_count) return false;

    uint64_t block = agno * v->ag_blocks + agbno;
    uint64_t at = block * v->block_size + offset * v->inode_size;

    static uint8_t buf[65536];
    if (v->block_size > sizeof buf) return false;
    if (!read_block(v, at / v->block_size, buf)) return false;

    uint32_t into = (uint32_t)(at % v->block_size);
    if (into + v->inode_size > v->block_size) return false;
    const uint8_t *ip = buf + into;

    if (rd16(ip + DI_MAGIC) != DINODE_MAGIC) return false;

    zero_bytes(out, sizeof *out);
    out->inode = ino;
    out->mode = rd16(ip + DI_MODE);
    out->format = ip[DI_FORMAT];
    out->size = rd64(ip + DI_SIZE);
    out->extent_count = rd32(ip + DI_NEXTENTS);
    out->directory = (out->mode & 0xF000) == MODE_DIRECTORY;

    /* The data fork begins after the header, whose length depends on which
     * version of the inode this is.  An attribute fork, when there is one,
     * starts at forkoff eight-byte words in and bounds the data fork. */
    uint32_t header = (ip[DI_VERSION] >= 3) ? DINODE_V3_SIZE : DINODE_V2_SIZE;
    uint32_t fork_max = v->inode_size - header;
    if (ip[DI_FORKOFF]) {
        uint32_t bounded = (uint32_t)ip[DI_FORKOFF] * 8;
        if (bounded < fork_max) fork_max = bounded;
    }
    if (fork_max > sizeof out->fork) fork_max = sizeof out->fork;

    copy_bytes(out->fork, ip + header, fork_max);
    out->fork_bytes = fork_max;
    return true;
}

/* Where a block of the file lives on the disk, from the extent list. */
static uint64_t block_of(xfs_volume_t *v, const xfs_file_t *f, uint64_t want) {
    (void)v;
    if (f->format != FMT_EXTENTS) return 0;

    uint32_t have = f->fork_bytes / EXTENT_BYTES;
    uint32_t count = f->extent_count < have ? f->extent_count : have;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t first, disk; uint32_t n; bool written;
        xfs_unpack_extent(f->fork + i * EXTENT_BYTES, &first, &disk, &n, &written);
        if (want >= first && want < first + n) {
            /* A range that was allocated and never written reads as zeroes;
             * what is on the disk there belongs to whoever had it before. */
            if (!written) return 0;
            return disk + (want - first);
        }
    }
    return 0;
}

long xfs_read_file(xfs_volume_t *v, const xfs_file_t *f,
                   uint64_t offset, void *buf, uint32_t len) {
    if (!v->mounted || !f) return -1;
    if (offset >= f->size) return 0;
    if (offset + len > f->size) len = (uint32_t)(f->size - offset);

    /* A small file lives inside its own inode and there is nothing to look
     * up - the fork is the file. */
    if (f->format == FMT_LOCAL) {
        if (offset + len > f->fork_bytes) {
            if (offset >= f->fork_bytes) return 0;
            len = f->fork_bytes - (uint32_t)offset;
        }
        copy_bytes(buf, f->fork + offset, len);
        return (long)len;
    }

    if (f->format == FMT_BTREE) return -1;    /* refused; see xfs.h */

    static uint8_t block[65536];
    if (v->block_size > sizeof block) return -1;

    uint8_t *out = buf;
    uint32_t done = 0;

    while (done < len) {
        uint64_t at = offset + done;
        uint64_t disk = block_of(v, f, at / v->block_size);

        uint32_t into = (uint32_t)(at % v->block_size);
        uint32_t take = v->block_size - into;
        if (take > len - done) take = len - done;

        if (!disk) {
            zero_bytes(out + done, take);
        } else {
            if (!read_block(v, disk, block)) return -1;
            copy_bytes(out + done, block + into, take);
        }
        done += take;
    }
    return (long)done;
}

/* ------------------------------------------------------------ directories
 *
 * A small directory is stored inside its own inode, as a count, the parent's
 * number, and then one entry per name.  The awkward parts are that the inode
 * numbers in it are four bytes or eight depending on a flag in the header, and
 * that each entry may or may not carry a type byte depending on a feature bit
 * in the superblock - so the stride from one entry to the next is not fixed
 * and cannot be assumed.
 */
static bool shortform_entry(xfs_volume_t *v, const xfs_file_t *dir,
                            uint32_t index, char *name_out, size_t name_cap,
                            uint64_t *ino_out) {
    if (dir->fork_bytes < 6) return false;

    uint8_t count = dir->fork[0];
    uint8_t i8count = dir->fork[1];
    uint32_t ino_bytes = i8count ? 8 : 4;
    uint32_t at = 2 + ino_bytes;             /* past the count and the parent */

    if (index >= count) return false;

    for (uint32_t i = 0; i < count; i++) {
        if (at + 3 > dir->fork_bytes) return false;

        uint8_t namelen = dir->fork[at];
        uint32_t name_at = at + 3;           /* namelen, then a two-byte offset */
        uint32_t after = name_at + namelen;
        if (v->has_file_type) after += 1;

        if (after + ino_bytes > dir->fork_bytes) return false;

        if (i == index) {
            size_t o = 0;
            while (o < namelen && o + 1 < name_cap) {
                name_out[o] = (char)dir->fork[name_at + o];
                o++;
            }
            name_out[o] = 0;

            *ino_out = ino_bytes == 8 ? rd64(dir->fork + after)
                                      : rd32(dir->fork + after);
            return true;
        }
        at = after + ino_bytes;
    }
    return false;
}

bool xfs_readdir(xfs_volume_t *v, const xfs_file_t *dir, uint32_t index,
                 char *name_out, size_t name_cap, xfs_file_t *entry_out) {
    if (!v->mounted || !dir || !dir->directory) return false;

    if (dir->format != FMT_LOCAL) {
        /* A directory large enough to have left the inode is stored as data
         * blocks with their own headers, and past that as a b-tree.  Neither
         * is read here yet, and saying so is better than returning the first
         * few names and stopping without a word. */
        say(v, "this directory is too large for what this driver reads");
        return false;
    }

    char name[256];
    uint64_t ino = 0;
    if (!shortform_entry(v, dir, index, name, sizeof name, &ino)) return false;

    if (name_out) {
        size_t o = 0;
        while (name[o] && o + 1 < name_cap) { name_out[o] = name[o]; o++; }
        name_out[o] = 0;
    }
    if (entry_out && !read_inode(v, ino, entry_out)) return false;
    return true;
}

static bool same_name(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return !*a && !*b;
}

bool xfs_lookup(xfs_volume_t *v, const char *path, xfs_file_t *out) {
    if (!v->mounted || !path) return false;

    xfs_file_t here;
    if (!read_inode(v, v->root_inode, &here)) {
        say(v, "the root inode is unreadable");
        return false;
    }
    if (!here.directory) { say(v, "the root inode is not a directory"); return false; }

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

        bool found = false;
        for (uint32_t i = 0; ; i++) {
            char name[256];
            xfs_file_t e;
            if (!xfs_readdir(v, &here, i, name, sizeof name, &e)) break;
            if (!same_name(name, part)) continue;
            here = e;
            found = true;
            break;
        }
        if (!found) return false;
    }

    if (out) *out = here;
    return true;
}

/* ------------------------------------------------------------------- tests
 *
 * The extent record, because it is the one piece of this format that is pure
 * arithmetic and the one most likely to be wrong: four fields packed across
 * 128 bits with not one of them starting on a byte.
 */
int xfs_selftest(void) {
    int failures = 0;

    /* Built by hand from the field positions: file block 1, disk block 2,
     * three blocks, written.
     *
     *   hi = (0 << 63) | (1 << 9) | (2 >> 43)      = 0x0000000000000200
     *   lo = ((2 & 0x7FFFFFFFFFF) << 21) | 3       = 0x0000000000400003
     */
    {
        uint8_t raw[16] = {
            0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,
            0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x03,
        };
        uint64_t fb = 0, db = 0; uint32_t n = 0; bool written = false;
        xfs_unpack_extent(raw, &fb, &db, &n, &written);
        if (fb != 1 || db != 2 || n != 3 || !written) failures++;
    }

    /* The unwritten flag is the top bit, and setting it must not disturb any
     * of the three numbers beside it. */
    {
        uint8_t raw[16] = {
            0x80,0x00,0x00,0x00,0x00,0x00,0x02,0x00,
            0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x03,
        };
        uint64_t fb = 0, db = 0; uint32_t n = 0; bool written = true;
        xfs_unpack_extent(raw, &fb, &db, &n, &written);
        if (fb != 1 || db != 2 || n != 3 || written) failures++;
    }

    /* A disk block that straddles the two halves - nine bits in the first word
     * and forty-three in the second.  A reader that takes it from one word
     * only gets small numbers right and large ones wrong, which is a bug that
     * hides until a volume is bigger than a few gigabytes. */
    {
        uint64_t want_disk = 0x1234567890ull;
        uint64_t hi = (0ull << 63) | (7ull << 9) | (want_disk >> 43);
        uint64_t lo = ((want_disk & 0x7FFFFFFFFFFull) << 21) | 5;

        uint8_t raw[16];
        for (int i = 0; i < 8; i++) raw[i]     = (uint8_t)(hi >> (56 - i * 8));
        for (int i = 0; i < 8; i++) raw[8 + i] = (uint8_t)(lo >> (56 - i * 8));

        uint64_t fb = 0, db = 0; uint32_t n = 0; bool written = false;
        xfs_unpack_extent(raw, &fb, &db, &n, &written);
        if (fb != 7 || db != want_disk || n != 5) failures++;
    }

    return failures;
}
