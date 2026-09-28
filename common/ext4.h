/* ext4.h - reading ext2, ext3 and ext4.
 *
 * This is what a Linux system's disk is, and what a great many USB sticks
 * handed between Linux machines are.  A system that reads FAT, exFAT and NTFS
 * and not this one can read everything Windows writes and nothing Linux does.
 *
 * The three names are one format with two ways of finding a file's blocks:
 * the older indirect chain, where a block holds pointers to more blocks, and
 * ext4's extent tree, where a small B-tree maps ranges of the file onto ranges
 * of the disk.  A flag in the inode says which, and both are read here -
 * ext2 and ext3 volumes are still common and the difference is one branch.
 *
 * Every offset was taken from e2fsprogs' own ext2_fs.h and ext3_extents.h and
 * confirmed by compiling them and printing offsetof.
 */
#ifndef KESTREL_EXT4_H
#define KESTREL_EXT4_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef bool (*ext4_read_fn)(void *ctx, uint64_t lba, uint32_t count, void *buf);

typedef struct {
    void        *ctx;
    ext4_read_fn read;

    uint32_t block_size;
    uint32_t first_data_block;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t group_count;
    uint32_t desc_size;          /* 32 on ext2/3, 64 when the volume is 64-bit */
    uint64_t block_count;
    bool     sixty_four_bit;

    bool     mounted;
    char     error[96];
} ext4_volume_t;

typedef struct {
    uint32_t inode;
    uint64_t size;
    uint16_t mode;
    bool     directory;
    bool     extents;            /* an extent tree rather than an indirect chain */
    uint32_t block[15];          /* the inode's own block pointers, or its root */
} ext4_file_t;

/* Bring a volume up from its superblock. */
bool ext4_mount(ext4_volume_t *v, void *ctx, ext4_read_fn read);

/* Find a file by path.  Either separator; a leading one is optional. */
bool ext4_lookup(ext4_volume_t *v, const char *path, ext4_file_t *out);

/* Read from a file.  Returns bytes read, or -1. */
long ext4_read_file(ext4_volume_t *v, const ext4_file_t *f,
                    uint64_t offset, void *buf, uint32_t len);

/* Walk a directory.  `index` counts from zero; false when exhausted. */
bool ext4_readdir(ext4_volume_t *v, const ext4_file_t *dir, uint32_t index,
                  char *name_out, size_t name_cap, ext4_file_t *entry_out);

#endif /* KESTREL_EXT4_H */
