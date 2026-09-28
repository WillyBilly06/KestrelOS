/* ntfs_core.h - reading NTFS, for the loader and the kernel both.
 *
 * The layout this system boots from is the one Windows and Linux both use, and
 * for the same reason: firmware can only be relied on to read FAT, so a small
 * FAT partition carries the loader and nothing else, and everything that
 * matters - the kernel, the initial filesystem, vendor firmware, the logs -
 * lives on the real filesystem behind it.  Windows keeps System32 there;
 * Linux keeps everything outside /boot/efi there.
 *
 * That means the same volume has to be readable from two very different
 * places.  The loader runs under the firmware, before there is a kernel, and
 * has to find and read the kernel.  The kernel runs after the firmware is gone
 * and reaches its disks through its own drivers.  Neither can use the other's
 * way of reading a sector, and a second implementation of NTFS would be a
 * second set of bugs - so the format is understood in one place and the two
 * callers differ only in the function that fetches a sector.
 *
 * What this reads and what it does not:
 *
 *   It reads.  There is no writing here at all.  A reader that is wrong
 *   returns the wrong bytes; a writer that is wrong destroys a volume that
 *   Windows also uses, and the two are not worth building at the same time.
 *
 *   It does not decompress or decrypt.  NTFS can store a file's data
 *   compressed or encrypted, and a file stored either way is reported as
 *   unreadable rather than returned as rubbish.  Nothing this system puts on
 *   a volume is stored either way unless somebody asks Windows to.
 */
#ifndef KESTREL_NTFS_CORE_H
#define KESTREL_NTFS_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* How a caller fetches sectors.  512-byte units, because that is what both
 * the firmware's block protocol and this system's block layer speak. */
typedef bool (*ntfs_read_fn)(void *ctx, uint64_t lba, uint32_t count, void *buf);

/* And, optionally, a way to put sectors back.  A caller that passes nothing
 * here gets a volume that cannot be written to at all, which is what the
 * loader wants: it has no business changing a filesystem. */
typedef bool (*ntfs_write_fn)(void *ctx, uint64_t lba, uint32_t count,
                              const void *buf);

typedef struct {
    void         *ctx;
    ntfs_read_fn  read;
    ntfs_write_fn write;          /* NULL on a volume opened for reading   */

    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_cluster;
    uint32_t record_bytes;        /* one MFT record, usually 1024        */
    uint64_t mft_lcn;
    uint64_t total_sectors;

    /* Where the MFT itself lives, read out of its own first record so that a
     * fragmented MFT can be followed.  Without this only the first run of the
     * MFT is reachable, which is enough for a small volume and silently wrong
     * on a large one. */
    uint8_t *mft_runs;            /* the run list, copied out             */
    uint32_t mft_runs_bytes;

    bool     mounted;
    char     error[96];
} ntfs_volume_t;

/* One file or directory, once found. */
typedef struct {
    uint64_t record;              /* its MFT record number                */
    uint64_t size;                /* bytes of $DATA                       */
    bool     directory;
    bool     resident;            /* small files live inside the record   */
    bool     unreadable;          /* compressed or encrypted              */

    /* Where its data is: either inside the record, or a run list. */
    uint8_t  resident_data[1024];
    uint32_t resident_bytes;
    uint8_t  runs[512];
    uint32_t runs_bytes;
} ntfs_file_t;

/* Bring a volume up from its first sector.  `read` is called immediately. */
bool ntfs_mount(ntfs_volume_t *v, void *ctx, ntfs_read_fn read);

/* The same, with a way to write.  Kept separate so that opening a volume for
 * reading is the shorter call and the default. */
bool ntfs_mount_rw(ntfs_volume_t *v, void *ctx, ntfs_read_fn read,
                   ntfs_write_fn write);

/* Find a file by path, with either separator: "/KESTREL/KERNEL.ELF" and
 * "\\KESTREL\\KERNEL.ELF" are the same file, because the loader and the kernel
 * write paths in different conventions and neither should have to care. */
bool ntfs_lookup(ntfs_volume_t *v, const char *path, ntfs_file_t *out);

/* Read from a file.  Returns bytes read, or -1. */
long ntfs_read_file(ntfs_volume_t *v, const ntfs_file_t *f,
                    uint64_t offset, void *buf, uint32_t len);

/* Write into a file, within the space it already has.
 *
 * This deliberately cannot grow a file, create one, or change any of the
 * volume's own bookkeeping.  It writes data clusters that are already
 * allocated to the file it was given, and nothing else - so the worst it can
 * do when it is wrong is put the wrong bytes in one file, rather than damage a
 * filesystem that Windows also has to be able to mount.
 *
 * Growing a file means allocating clusters, which means editing the volume
 * bitmap and the file's own record, which means being correct about the
 * journal as well.  That is a much larger and much more dangerous piece of
 * work, and it is not this one.
 *
 * Returns bytes written, or -1.  A write that would run past the end of the
 * file writes what fits and reports that.
 */
long ntfs_write_file(ntfs_volume_t *v, const ntfs_file_t *f,
                     uint64_t offset, const void *buf, uint32_t len);

/* Walk a directory.  `index` counts from zero; returns false when the
 * directory is exhausted.  Names come back in UTF-8. */
bool ntfs_readdir(ntfs_volume_t *v, const ntfs_file_t *dir, uint32_t index,
                  char *name_out, size_t name_cap, ntfs_file_t *entry_out);


/* ------------------------------------------------------------------ writing
 *
 * Creating files, which is a different and much larger thing than changing
 * one - see ntfs_write.c for what has to agree with what, and for why the
 * order these happen in is the whole safety story on a filesystem whose
 * journal this driver does not write.
 */

/* Where the clock comes from, when the caller has one.  NTFS keeps time in
 * 100-nanosecond ticks since 1601.  Left null, files get a fixed plausible
 * date rather than 1601 itself, which Windows displays as an error. */
extern uint64_t (*ntfs_now)(void);

/* Whether this volume can be written to at all. */
bool ntfs_writable(ntfs_volume_t *v);

/* Make a file or a directory.  The parent must exist; the name must not. */
bool ntfs_create(ntfs_volume_t *v, const char *path, bool directory,
                 ntfs_file_t *out);

/* The same, in a directory already open.  A caller that is holding the
 * directory - which the kernel always is - should use this: rebuilding a path
 * so that it can be parsed back into the directory it started from is a way to
 * get a different answer than the one it already had. */
bool ntfs_create_in(ntfs_volume_t *v, uint64_t parent_record, const char *leaf,
                    bool directory, ntfs_file_t *out);

/* Give a file the space for `bytes`, allocating clusters and moving it out of
 * its own record if it has outgrown living inside one.  After this,
 * ntfs_write_file can put data anywhere below `bytes`. */
bool ntfs_resize(ntfs_volume_t *v, ntfs_file_t *f, uint64_t bytes);

/* Check the parts of the writer that are pure arithmetic - run list encoding,
 * name ordering, the fixups - against values worked out by hand.  Returns the
 * number of failures. */
int ntfs_write_selftest(void);

#endif /* KESTREL_NTFS_CORE_H */
