/* btrfs.h - reading Btrfs.
 *
 * The default filesystem on Fedora and openSUSE, and a common choice
 * elsewhere.  With FAT, NTFS, exFAT, ext4 and XFS beside it, every ordinary
 * way of formatting a disk is readable.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS ONE IS DIFFERENT
 *
 * Every other filesystem here has addresses that mean something directly: a
 * cluster number, a block number, something that multiplies out to a place on
 * the disk.  Btrfs has two address spaces.  Everything inside it refers to
 * *logical* addresses, and the map from those to actual disk offsets is itself
 * stored in the filesystem - in the chunk tree, which is at a logical address.
 *
 * So there is a bootstrap problem, and the superblock solves it: it carries a
 * small array of chunk mappings, enough to find the chunk tree and no more.
 * Read those first, use them to reach the chunk tree, read the rest of the map
 * from there, and only then is any other address meaningful.  A reader that
 * treats a logical address as a disk offset gets a plausible number and reads
 * the wrong part of the disk.
 *
 * Everything else - inodes, directory entries, file contents - lives as items
 * in B-trees keyed by (objectid, type, offset).
 *
 * Offsets and constants are from btrfs-progs' own btrfs_tree.h, confirmed by
 * compiling it and printing offsetof.
 */
#ifndef KESTREL_BTRFS_H
#define KESTREL_BTRFS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef bool (*btrfs_read_fn)(void *ctx, uint64_t lba, uint32_t count, void *buf);

#define BTRFS_MAX_CHUNKS 32

typedef struct {
    uint64_t logical;
    uint64_t length;
    uint64_t physical;
} btrfs_chunk_map_t;

typedef struct {
    void         *ctx;
    btrfs_read_fn read;

    uint32_t sector_size;
    uint32_t node_size;
    uint64_t total_bytes;
    uint64_t root_tree;          /* logical */
    uint64_t chunk_tree;         /* logical */
    uint64_t fs_tree;            /* logical, once found */

    btrfs_chunk_map_t chunks[BTRFS_MAX_CHUNKS];
    int chunk_count;

    bool mounted;
    char error[96];
} btrfs_volume_t;

typedef struct {
    uint64_t objectid;
    uint64_t size;
    uint32_t mode;
    bool     directory;
} btrfs_file_t;

bool btrfs_mount(btrfs_volume_t *v, void *ctx, btrfs_read_fn read);
bool btrfs_lookup(btrfs_volume_t *v, const char *path, btrfs_file_t *out);
long btrfs_read_file(btrfs_volume_t *v, const btrfs_file_t *f,
                     uint64_t offset, void *buf, uint32_t len);
bool btrfs_readdir(btrfs_volume_t *v, const btrfs_file_t *dir, uint32_t index,
                   char *name_out, size_t name_cap, btrfs_file_t *entry_out);

/* Where a logical address actually is.  Public because it is the one thing
 * about this format that has no counterpart in the others, and the piece a
 * test can check on its own. */
bool btrfs_resolve(const btrfs_volume_t *v, uint64_t logical,
                   uint64_t *physical);

int btrfs_selftest(void);

#endif /* KESTREL_BTRFS_H */
