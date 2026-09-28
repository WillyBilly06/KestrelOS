/* exfat.h - reading exFAT, for the loader and the kernel both.
 *
 * exFAT is what a large USB stick or an SD card arrives formatted as, because
 * FAT32 cannot hold a file over four gigabytes and NTFS is more filesystem
 * than a camera wants.  A system that reads FAT and NTFS and not this one
 * still cannot read most of the removable media it will meet.
 *
 * It is a much simpler format than NTFS: a boot sector, a single allocation
 * table, and directories made of fixed 32-byte entries.  The awkward parts are
 * that a single file needs three entries in a row to describe it, that a file
 * can opt out of the allocation table entirely when its clusters happen to be
 * contiguous, and that the name is spread fifteen characters at a time across
 * however many entries it takes.
 *
 * Every offset here was taken from the exfatprogs headers and checked by
 * compiling them - not counted by hand.  See ntfs_core.h for the same
 * arrangement and the same reasons.
 */
#ifndef KESTREL_EXFAT_H
#define KESTREL_EXFAT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef bool (*exfat_read_fn)(void *ctx, uint64_t lba, uint32_t count, void *buf);
typedef bool (*exfat_write_fn)(void *ctx, uint64_t lba, uint32_t count,
                               const void *buf);

typedef struct {
    void          *ctx;
    exfat_read_fn  read;
    exfat_write_fn write;          /* NULL when opened for reading */

    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_cluster;

    uint64_t volume_sectors;
    uint32_t fat_sector;           /* where the allocation table starts   */
    uint32_t fat_sectors;
    uint32_t cluster_sector;       /* where cluster 2 begins              */
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint8_t  fat_count;

    bool     mounted;
    char     error[96];
} exfat_volume_t;

typedef struct {
    uint32_t first_cluster;
    uint64_t size;
    bool     directory;
    bool     contiguous;           /* no chain to follow; clusters run on  */
} exfat_file_t;

/* Bring a volume up from its first sector. */
bool exfat_mount(exfat_volume_t *v, void *ctx, exfat_read_fn read);

/* Find a file by path, with either separator. */
bool exfat_lookup(exfat_volume_t *v, const char *path, exfat_file_t *out);

/* Read from a file.  Returns bytes read, or -1. */
long exfat_read_file(exfat_volume_t *v, const exfat_file_t *f,
                     uint64_t offset, void *buf, uint32_t len);

/* Walk a directory.  `index` counts from zero; false when exhausted. */
bool exfat_readdir(exfat_volume_t *v, const exfat_file_t *dir, uint32_t index,
                   char *name_out, size_t name_cap, exfat_file_t *entry_out);

/* The checks that need no disk: the two checksums the format defines, and the
 * name hash a directory is searched by.  Returns the number of failures. */
int exfat_selftest(void);

/* The name hash exFAT stores in every stream entry, and the checksum that
 * covers a file's set of entries.  Public because the tests check them and
 * because writing will need both. */
uint16_t exfat_name_hash(const uint16_t *name, uint8_t length);
uint16_t exfat_entry_checksum(const uint8_t *entries, int count);
uint32_t exfat_boot_checksum(const uint8_t *sectors, uint32_t bytes);

#endif /* KESTREL_EXFAT_H */
