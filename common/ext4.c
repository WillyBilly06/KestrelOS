/* ext4.c - reading ext2, ext3 and ext4.
 *
 * ---------------------------------------------------------------------------
 * TWO WAYS OF FINDING A FILE'S BLOCKS, AND WHY BOTH ARE HERE
 *
 * ext2 and ext3 keep fifteen block numbers in the inode: twelve blocks of the
 * file directly, then a block full of pointers, then a block of pointers to
 * blocks of pointers, then one more level of that.  It is simple and it costs
 * a read per level for anything past the first forty-eight kilobytes.
 *
 * ext4 replaced it with a small B-tree of extents - each one saying "this
 * range of the file lives at this range of the disk" - which for an
 * unfragmented file is one entry for the whole thing.
 *
 * A flag in the inode says which.  Both are read here because an ext2 or ext3
 * volume is not a museum piece: it is what a stick formatted by an older
 * machine, or deliberately for compatibility, still is.
 *
 * ---------------------------------------------------------------------------
 * THE ONE THAT LOOKS LIKE A BUG AND IS NOT
 *
 * An extent's length field can be greater than 32768, and that does not mean
 * a very long extent.  It means the extent is allocated but has never been
 * written, and its real length is the field minus 32768.  Reading such a range
 * gives zeroes rather than whatever the disk held before.  A reader that takes
 * the field at face value asks for an extent of impossible size; one that
 * masks the bit off but returns the disk's contents leaks whatever was there.
 */
#include "ext4.h"

/* ------------------------------------------------------------ little-endian */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void copy_bytes(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    while (n--) *d++ = *s++;
}
static void zero_bytes(void *dst, size_t n) {
    uint8_t *d = dst; while (n--) *d++ = 0;
}
static void say(ext4_volume_t *v, const char *msg) {
    size_t i = 0;
    while (msg[i] && i < sizeof v->error - 1) { v->error[i] = msg[i]; i++; }
    v->error[i] = 0;
}

/* ------------------------------------------------------------- the format */

#define SUPER_OFFSET          1024
#define SB_INODES_COUNT       0x000
#define SB_BLOCKS_COUNT       0x004
#define SB_FIRST_DATA_BLOCK   0x014
#define SB_LOG_BLOCK_SIZE     0x018
#define SB_BLOCKS_PER_GROUP   0x020
#define SB_INODES_PER_GROUP   0x028
#define SB_MAGIC              0x038
#define SB_INODE_SIZE         0x058
#define SB_FEATURE_INCOMPAT   0x060
#define SB_DESC_SIZE          0x0FE
#define SB_BLOCKS_COUNT_HI    0x150

#define EXT2_MAGIC            0xEF53
#define INCOMPAT_64BIT        0x0080

#define GD_INODE_TABLE        0x008
#define GD_INODE_TABLE_HI     0x028

#define INODE_MODE            0x000
#define INODE_SIZE_LO         0x004
#define INODE_FLAGS           0x020
#define INODE_BLOCK           0x028
#define INODE_SIZE_HI         0x06C

#define EXTENTS_FL            0x00080000u

#define EXTENT_MAGIC          0xF30A
#define EH_MAGIC              0x00
#define EH_ENTRIES            0x02
#define EH_DEPTH              0x06
#define EH_BYTES              12

#define EE_BLOCK              0x00
#define EE_LEN                0x04
#define EE_START_HI           0x06
#define EE_START              0x08

#define EI_BLOCK              0x00
#define EI_LEAF               0x04
#define EI_LEAF_HI            0x08

#define DIR_INODE             0x00
#define DIR_REC_LEN           0x04
#define DIR_NAME_LEN          0x06
#define DIR_FILE_TYPE         0x07
#define DIR_NAME              0x08

#define FT_DIR                2

#define ROOT_INODE            2
#define MODE_DIRECTORY        0x4000

#define UNWRITTEN_LEN         32768

/* --------------------------------------------------------------- the disk */

static bool read_block(ext4_volume_t *v, uint64_t block, void *buf) {
    if (!block) { zero_bytes(buf, v->block_size); return true; }  /* a hole */
    uint32_t per = v->block_size / 512;
    return v->read(v->ctx, block * per, per, buf);
}

/* ---------------------------------------------------------------- mounting */

bool ext4_mount(ext4_volume_t *v, void *ctx, ext4_read_fn read) {
    zero_bytes(v, sizeof *v);
    v->ctx = ctx;
    v->read = read;

    static uint8_t sb[1024];
    if (!read(ctx, SUPER_OFFSET / 512, 1024 / 512, sb)) {
        say(v, "the superblock could not be read");
        return false;
    }

    if (rd16(sb + SB_MAGIC) != EXT2_MAGIC) {
        say(v, "this volume is not ext2, ext3 or ext4");
        return false;
    }

    uint32_t log = rd32(sb + SB_LOG_BLOCK_SIZE);
    if (log > 6) { say(v, "the block size is not one this driver reads"); return false; }
    v->block_size = 1024u << log;

    v->first_data_block = rd32(sb + SB_FIRST_DATA_BLOCK);
    v->blocks_per_group = rd32(sb + SB_BLOCKS_PER_GROUP);
    v->inodes_per_group = rd32(sb + SB_INODES_PER_GROUP);
    v->inode_size       = rd16(sb + SB_INODE_SIZE);
    if (!v->inode_size) v->inode_size = 128;      /* the original, unrecorded */

    v->block_count = rd32(sb + SB_BLOCKS_COUNT);

    uint32_t incompat = rd32(sb + SB_FEATURE_INCOMPAT);
    v->sixty_four_bit = (incompat & INCOMPAT_64BIT) != 0;
    if (v->sixty_four_bit) {
        v->block_count |= (uint64_t)rd32(sb + SB_BLOCKS_COUNT_HI) << 32;
        v->desc_size = rd16(sb + SB_DESC_SIZE);
        if (v->desc_size < 32) v->desc_size = 64;
    } else {
        v->desc_size = 32;
    }

    if (!v->blocks_per_group || !v->inodes_per_group || v->inode_size < 128) {
        say(v, "the superblock contradicts itself");
        return false;
    }

    v->group_count = (uint32_t)((v->block_count - v->first_data_block +
                                v->blocks_per_group - 1) / v->blocks_per_group);
    v->mounted = true;
    return true;
}

/* ------------------------------------------------------------- the inodes */

static bool read_inode(ext4_volume_t *v, uint32_t number, uint8_t *out) {
    if (number < 1) return false;

    uint32_t group = (number - 1) / v->inodes_per_group;
    uint32_t index = (number - 1) % v->inodes_per_group;
    if (group >= v->group_count) return false;

    /* The group descriptors follow the superblock.  With a 1 KiB block the
     * superblock occupies block 1 and they start at block 2; with anything
     * larger the superblock shares block 0 and they start at block 1. */
    uint64_t desc_block = v->first_data_block + 1;
    uint64_t byte = (uint64_t)group * v->desc_size;

    static uint8_t block[65536];
    if (v->block_size > sizeof block) return false;

    if (!read_block(v, desc_block + byte / v->block_size, block)) return false;
    const uint8_t *gd = block + (byte % v->block_size);

    uint64_t table = rd32(gd + GD_INODE_TABLE);
    if (v->sixty_four_bit && v->desc_size >= 64)
        table |= (uint64_t)rd32(gd + GD_INODE_TABLE_HI) << 32;

    uint64_t at = table * v->block_size + (uint64_t)index * v->inode_size;
    if (!read_block(v, at / v->block_size, block)) return false;

    uint32_t into = (uint32_t)(at % v->block_size);
    if (into + 128 > v->block_size) return false;
    copy_bytes(out, block + into, 128);
    return true;
}

static void take_inode(ext4_file_t *f, uint32_t number, const uint8_t *inode) {
    zero_bytes(f, sizeof *f);
    f->inode = number;
    f->mode = rd16(inode + INODE_MODE);
    f->directory = (f->mode & 0xF000) == MODE_DIRECTORY;

    f->size = rd32(inode + INODE_SIZE_LO);
    /* The high half is a size for a file and something else entirely for a
     * directory, so it is only taken for files. */
    if (!f->directory)
        f->size |= (uint64_t)rd32(inode + INODE_SIZE_HI) << 32;

    f->extents = (rd32(inode + INODE_FLAGS) & EXTENTS_FL) != 0;
    for (int i = 0; i < 15; i++)
        f->block[i] = rd32(inode + INODE_BLOCK + i * 4);
}

/* ------------------------------------------------- finding a file's blocks */

/* Walk an extent tree to the disk block holding one block of the file.
 * Returns 0 for a hole or an unwritten range, which reads as zeroes. */
static uint64_t extent_lookup(ext4_volume_t *v, const uint8_t *node,
                              uint32_t want, int depth_guard) {
    if (depth_guard <= 0) return 0;
    if (rd16(node + EH_MAGIC) != EXTENT_MAGIC) return 0;

    uint16_t entries = rd16(node + EH_ENTRIES);
    uint16_t depth = rd16(node + EH_DEPTH);
    const uint8_t *e = node + EH_BYTES;

    if (depth == 0) {
        for (uint16_t i = 0; i < entries; i++, e += 12) {
            uint32_t first = rd32(e + EE_BLOCK);
            uint16_t len = rd16(e + EE_LEN);

            /* Over 32768 means allocated and never written - see the note at
             * the top.  Its real length is the field minus that, and what is
             * on the disk there is not the file's. */
            if (len > UNWRITTEN_LEN) return 0;

            if (want >= first && want < first + len) {
                uint64_t start = rd32(e + EE_START) |
                                 ((uint64_t)rd16(e + EE_START_HI) << 32);
                return start + (want - first);
            }
        }
        return 0;
    }

    /* An interior node: the last index whose first block is not past the one
     * being looked for. */
    uint64_t child = 0;
    for (uint16_t i = 0; i < entries; i++, e += 12) {
        if (rd32(e + EI_BLOCK) > want) break;
        child = rd32(e + EI_LEAF) | ((uint64_t)rd16(e + EI_LEAF_HI) << 32);
    }
    if (!child) return 0;

    static uint8_t deeper[4][65536];
    int level = 4 - depth_guard;
    if (level < 0 || level >= 4) return 0;
    if (v->block_size > sizeof deeper[0]) return 0;
    if (!read_block(v, child, deeper[level])) return 0;

    return extent_lookup(v, deeper[level], want, depth_guard - 1);
}

/* The older arrangement: twelve direct, then one, two and three levels of
 * indirection. */
static uint64_t indirect_lookup(ext4_volume_t *v, const ext4_file_t *f,
                                uint32_t want) {
    uint32_t per = v->block_size / 4;

    if (want < 12) return f->block[want];
    want -= 12;

    static uint8_t level[3][65536];
    if (v->block_size > sizeof level[0]) return 0;

    if (want < per) {
        if (!read_block(v, f->block[12], level[0])) return 0;
        return rd32(level[0] + want * 4);
    }
    want -= per;

    if (want < per * per) {
        if (!read_block(v, f->block[13], level[0])) return 0;
        uint32_t mid = rd32(level[0] + (want / per) * 4);
        if (!read_block(v, mid, level[1])) return 0;
        return rd32(level[1] + (want % per) * 4);
    }
    want -= per * per;

    if (want < per * per * per) {
        if (!read_block(v, f->block[14], level[0])) return 0;
        uint32_t a = rd32(level[0] + (want / (per * per)) * 4);
        if (!read_block(v, a, level[1])) return 0;
        uint32_t b = rd32(level[1] + ((want / per) % per) * 4);
        if (!read_block(v, b, level[2])) return 0;
        return rd32(level[2] + (want % per) * 4);
    }
    return 0;
}

static uint64_t block_of(ext4_volume_t *v, const ext4_file_t *f, uint32_t want) {
    if (f->extents)
        return extent_lookup(v, (const uint8_t *)f->block, want, 4);
    return indirect_lookup(v, f, want);
}

long ext4_read_file(ext4_volume_t *v, const ext4_file_t *f,
                    uint64_t offset, void *buf, uint32_t len) {
    if (!v->mounted || !f) return -1;
    if (offset >= f->size) return 0;
    if (offset + len > f->size) len = (uint32_t)(f->size - offset);

    static uint8_t block[65536];
    if (v->block_size > sizeof block) return -1;

    uint8_t *out = buf;
    uint32_t done = 0;

    while (done < len) {
        uint64_t at = offset + done;
        uint64_t disk = block_of(v, f, (uint32_t)(at / v->block_size));

        uint32_t into = (uint32_t)(at % v->block_size);
        uint32_t take = v->block_size - into;
        if (take > len - done) take = len - done;

        if (!disk) {
            /* A hole, or a range allocated and never written.  Zeroes, not
             * whatever the disk happens to hold there. */
            zero_bytes(out + done, take);
        } else {
            if (!read_block(v, disk, block)) return -1;
            copy_bytes(out + done, block + into, take);
        }
        done += take;
    }
    return (long)done;
}

/* ------------------------------------------------------------ directories */

typedef struct {
    ext4_volume_t *v;
    const ext4_file_t *dir;
    uint64_t at;
} walker_t;

static bool next_entry(walker_t *w, char *name, size_t cap, uint32_t *inode_out,
                       bool *is_dir) {
    static uint8_t entry[264 + 8];

    while (w->at + 8 <= w->dir->size) {
        if (ext4_read_file(w->v, w->dir, w->at, entry, 8) != 8) return false;

        uint32_t inode = rd32(entry + DIR_INODE);
        uint16_t rec = rd16(entry + DIR_REC_LEN);
        uint8_t nlen = entry[DIR_NAME_LEN];
        uint8_t type = entry[DIR_FILE_TYPE];

        /* A record shorter than its own header, or one that does not advance,
         * would loop here forever. */
        if (rec < 8 || w->at + rec > w->dir->size) return false;

        uint64_t here = w->at;
        w->at += rec;

        if (!inode) continue;                     /* a deleted entry */
        if (nlen == 0) continue;

        if (nlen > 255) nlen = 255;
        if (ext4_read_file(w->v, w->dir, here + DIR_NAME, entry, nlen) != nlen)
            return false;

        size_t o = 0;
        while (o < nlen && o + 1 < cap) { name[o] = (char)entry[o]; o++; }
        name[o] = 0;

        if (inode_out) *inode_out = inode;
        if (is_dir) *is_dir = (type == FT_DIR);
        return true;
    }
    return false;
}

bool ext4_readdir(ext4_volume_t *v, const ext4_file_t *dir, uint32_t index,
                  char *name_out, size_t name_cap, ext4_file_t *entry_out) {
    if (!v->mounted || !dir || !dir->directory) return false;

    walker_t w = { v, dir, 0 };
    for (uint32_t i = 0; ; i++) {
        char name[256];
        uint32_t inode = 0;
        bool is_dir = false;
        if (!next_entry(&w, name, sizeof name, &inode, &is_dir)) return false;
        if (i != index) continue;

        if (name_out) {
            size_t o = 0;
            while (name[o] && o + 1 < name_cap) { name_out[o] = name[o]; o++; }
            name_out[o] = 0;
        }
        if (entry_out) {
            uint8_t raw[128];
            if (!read_inode(v, inode, raw)) return false;
            take_inode(entry_out, inode, raw);
        }
        return true;
    }
}

static bool same_name(const char *a, const char *b) {
    /* Case sensitive, unlike the others here - this is a Unix filesystem and
     * "Makefile" and "makefile" are two different files on it. */
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return !*a && !*b;
}

bool ext4_lookup(ext4_volume_t *v, const char *path, ext4_file_t *out) {
    if (!v->mounted || !path) return false;

    uint8_t raw[128];
    if (!read_inode(v, ROOT_INODE, raw)) { say(v, "the root inode is unreadable"); return false; }

    ext4_file_t here;
    take_inode(&here, ROOT_INODE, raw);
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

        walker_t w = { v, &here, 0 };
        bool found = false;
        for (;;) {
            char name[256];
            uint32_t inode = 0;
            bool is_dir = false;
            if (!next_entry(&w, name, sizeof name, &inode, &is_dir)) break;
            if (!same_name(name, part)) continue;

            if (!read_inode(v, inode, raw)) return false;
            take_inode(&here, inode, raw);
            found = true;
            break;
        }
        if (!found) return false;
    }

    if (out) *out = here;
    return true;
}
