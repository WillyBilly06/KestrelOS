/* ntfs_internal.h - what the reader knows, shared with the writer.
 *
 * ntfs_core.c understands the on-disk format: where a record lives, how a run
 * list is walked, how the per-sector fixups work.  The writer needs all of
 * that and needs it to mean exactly the same thing, because a writer that
 * disagrees with the reader by one byte produces a volume that this system
 * reads back happily and Windows does not.
 *
 * So none of it is reimplemented next door.  It is declared here and used.
 * This header is not for callers of the filesystem - it is the seam between
 * two files that together are one driver.
 */
#ifndef KESTREL_NTFS_INTERNAL_H
#define KESTREL_NTFS_INTERNAL_H

#include "ntfs_core.h"

/* --------------------------------------------------------- little-endian
 *
 * Everything in NTFS is little-endian and very little of it is aligned - an
 * attribute begins wherever the previous one ended, rounded to eight, and a
 * name inside it wherever the header says.  Reading a byte at a time is not a
 * performance question here; it is the only thing that is always correct.
 */
static inline uint16_t ntfs_rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t ntfs_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t ntfs_rd64(const uint8_t *p) {
    return (uint64_t)ntfs_rd32(p) | ((uint64_t)ntfs_rd32(p + 4) << 32);
}
static inline void ntfs_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static inline void ntfs_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void ntfs_wr64(uint8_t *p, uint64_t v) {
    ntfs_wr32(p, (uint32_t)v);
    ntfs_wr32(p + 4, (uint32_t)(v >> 32));
}

/* --------------------------------------------------------------- run lists */

typedef struct {
    const uint8_t *p, *end;
    int64_t        lcn;            /* running, because offsets are relative */
    uint64_t       vcn;
} ntfs_run_walker_t;

void ntfs_runs_begin(ntfs_run_walker_t *w, const uint8_t *runs, uint32_t bytes);
bool ntfs_runs_next(ntfs_run_walker_t *w, int64_t *lcn_out, uint64_t *count_out);

/* ------------------------------------------------------------ the format */

/* Attribute types, by the numbers NTFS gives them. */
#define NTFS_ATTR_STANDARD_INFO 0x10
#define NTFS_ATTR_ATTRIBUTE_LIST 0x20
#define NTFS_ATTR_FILE_NAME     0x30
#define NTFS_ATTR_OBJECT_ID     0x40
#define NTFS_ATTR_SECURITY_DESC 0x50
#define NTFS_ATTR_VOLUME_NAME   0x60
#define NTFS_ATTR_VOLUME_INFO   0x70
#define NTFS_ATTR_DATA          0x80
#define NTFS_ATTR_INDEX_ROOT    0x90
#define NTFS_ATTR_INDEX_ALLOC   0xA0
#define NTFS_ATTR_BITMAP        0xB0
#define NTFS_ATTR_END           0xFFFFFFFF

/* The records NTFS reserves for itself.  Numbers below 24 are the volume's
 * own bookkeeping and are never handed out to a file. */
#define NTFS_MFT_RECORD       0
#define NTFS_ROOT_RECORD      5
#define NTFS_BITMAP_RECORD    6
#define NTFS_UPCASE_RECORD   10
#define NTFS_FIRST_FREE_RECORD 24

/* File attribute bits, the ones this driver sets. */
#define NTFS_FA_READONLY     0x0001
#define NTFS_FA_HIDDEN       0x0002
#define NTFS_FA_SYSTEM       0x0004
#define NTFS_FA_ARCHIVE      0x0020
#define NTFS_FA_DIRECTORY    0x10000000  /* in $FILE_NAME, not on disk in $SI */

/* Record header flags. */
#define NTFS_RECORD_IN_USE    0x0001
#define NTFS_RECORD_DIRECTORY 0x0002

/* Index entry flags. */
#define NTFS_INDEX_HAS_SUBNODE 0x0001
#define NTFS_INDEX_LAST        0x0002

/* -------------------------------------------------- shared with the writer */

void ntfs_copy_bytes(void *dst, const void *src, size_t n);
void ntfs_zero_bytes(void *dst, size_t n);
void ntfs_say(ntfs_volume_t *v, const char *msg);

bool ntfs_apply_fixups(uint8_t *buf, uint32_t bytes, uint32_t sector_size);

bool ntfs_read_clusters(ntfs_volume_t *v, uint64_t lcn, uint32_t count, void *buf);
bool ntfs_write_clusters(ntfs_volume_t *v, uint64_t lcn, uint32_t count,
                         const void *buf);

long ntfs_read_runs(ntfs_volume_t *v, const uint8_t *runs, uint32_t runs_bytes,
                    uint64_t size, uint64_t offset, uint8_t *out, uint32_t len);
long ntfs_write_runs(ntfs_volume_t *v, const uint8_t *runs, uint32_t runs_bytes,
                     uint64_t size, uint64_t offset, const uint8_t *in,
                     uint32_t len);

bool ntfs_read_mft_record(ntfs_volume_t *v, uint64_t number, uint8_t *out);

/* Replace the cached copy of $MFT's own run list, after the MFT has been
 * made bigger.  Without this every newly added record is unreachable. */
bool ntfs_remember_mft_runs(ntfs_volume_t *v, const uint8_t *runs,
                            uint32_t len);

const uint8_t *ntfs_find_attribute(const uint8_t *rec, uint32_t rec_bytes,
                                   uint32_t type, const uint16_t *name,
                                   uint8_t name_len);

#endif /* KESTREL_NTFS_INTERNAL_H */
