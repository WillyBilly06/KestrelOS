/* xfs.h - reading XFS.
 *
 * The filesystem a Red Hat or CentOS or Rocky machine's disk is, and the one
 * most likely to be under a Linux server somebody hands you.  With ext4 and
 * exFAT beside it, the four common ways of formatting a disk are all readable.
 *
 * ---------------------------------------------------------------------------
 * IT IS BIG-ENDIAN, AND NOTHING ELSE HERE IS
 *
 * FAT, NTFS, exFAT and ext4 all store their numbers least significant byte
 * first.  XFS does the opposite, on every field, everywhere.  A reader written
 * by pattern-matching against the ones next door does not fail on the first
 * field it reads - it reads a plausible wrong number and follows it, which is
 * why this file has its own accessors and no shared ones.
 *
 * The other thing that is unlike the rest: an inode's number is not an index.
 * It packs which allocation group the inode is in, which block of that group,
 * and which inode within that block - so finding one is arithmetic on three
 * shift widths taken from the superblock, and getting any of them wrong reads
 * a different inode rather than failing.
 *
 * Offsets and constants are from xfsprogs' own xfs_format.h, confirmed by
 * compiling it and printing offsetof.
 */
#ifndef KESTREL_XFS_H
#define KESTREL_XFS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef bool (*xfs_read_fn)(void *ctx, uint64_t lba, uint32_t count, void *buf);

typedef struct {
    void       *ctx;
    xfs_read_fn read;

    uint32_t block_size;
    uint32_t ag_blocks;
    uint32_t ag_count;
    uint16_t inode_size;
    uint8_t  block_log;
    uint8_t  inode_per_block_log;
    uint8_t  ag_block_log;
    uint64_t root_inode;
    uint64_t total_blocks;

    bool     version_5;        /* the CRC-carrying layout                */
    bool     has_file_type;    /* directory entries carry a type byte    */

    bool     mounted;
    char     error[96];
} xfs_volume_t;

typedef struct {
    uint64_t inode;
    uint64_t size;
    uint16_t mode;
    uint8_t  format;           /* local, extents, or a b-tree            */
    bool     directory;
    uint8_t  fork[512];        /* the inode's data fork, copied out      */
    uint32_t fork_bytes;
    uint32_t extent_count;
} xfs_file_t;

bool xfs_mount(xfs_volume_t *v, void *ctx, xfs_read_fn read);
bool xfs_lookup(xfs_volume_t *v, const char *path, xfs_file_t *out);
long xfs_read_file(xfs_volume_t *v, const xfs_file_t *f,
                   uint64_t offset, void *buf, uint32_t len);
bool xfs_readdir(xfs_volume_t *v, const xfs_file_t *dir, uint32_t index,
                 char *name_out, size_t name_cap, xfs_file_t *entry_out);

/* The packed extent record, unpacked.  Public because it is the one piece of
 * this format that is pure arithmetic, and so the one piece a test can check
 * without a disk. */
void xfs_unpack_extent(const uint8_t *raw, uint64_t *file_block,
                       uint64_t *disk_block, uint32_t *count, bool *written);

int xfs_selftest(void);

#endif /* KESTREL_XFS_H */
