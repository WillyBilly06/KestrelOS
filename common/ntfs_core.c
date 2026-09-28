/* ntfs_core.c - reading NTFS.  See ntfs_core.h for why this is shared.
 *
 * NTFS keeps everything, including its own bookkeeping, in files.  The master
 * file table is a file; the volume bitmap is a file; a directory is a file
 * whose contents are a sorted index.  So almost all of this is one operation
 * applied repeatedly: find a record, walk its attributes, follow the one you
 * want.  The three things that have to be right before any of that works are
 * fixups, run lists, and the resident/non-resident split, and they are what
 * most of the length below is.
 */
#include "ntfs_core.h"
#include "ntfs_internal.h"

/* A place for a caller to watch this work.
 *
 * The kernel and the loader both leave it null and pay nothing.  The host test
 * sets it, which is what makes a misread run list something that can be
 * watched rather than inferred. */
void (*ntfs_trace)(const char *what, unsigned long long a, unsigned long long b);
#define TRACE(w, a, b) do { if (ntfs_trace) ntfs_trace((w), (unsigned long long)(a), (unsigned long long)(b)); } while (0)

/* ------------------------------------------------------------ small helpers */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static uint32_t mft_runs_capacity;

void ntfs_copy_bytes(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    while (n--) *d++ = *s++;
}
void ntfs_zero_bytes(void *dst, size_t n) {
    uint8_t *d = dst; while (n--) *d++ = 0;
}

void ntfs_say(ntfs_volume_t *v, const char *msg) {
    size_t i = 0;
    while (msg[i] && i < sizeof v->error - 1) { v->error[i] = msg[i]; i++; }
    v->error[i] = 0;
}

/* ------------------------------------------------------------------ fixups
 *
 * Every multi-sector structure NTFS writes - a file record, an index block -
 * has the last two bytes of each of its sectors replaced with a sequence
 * number, and the bytes they displaced kept in an array at the front.  It is
 * how a torn write is detected: if the sequence number is not the same in
 * every sector, the structure was only partly written.
 *
 * Undoing that has to happen before anything else reads the structure, and
 * forgetting it does not fail loudly - it silently corrupts two bytes every
 * 512, which is the kind of damage that looks like a parsing bug anywhere
 * except where it actually is.
 */
bool ntfs_apply_fixups(uint8_t *buf, uint32_t bytes, uint32_t sector_size) {
    uint16_t usa_off = rd16(buf + 4);
    uint16_t usa_count = rd16(buf + 6);

    /* The array has to start after the two fields that describe it, and has to
     * fit inside the structure.  Nothing stronger than that: a file record and
     * an index block have different header lengths - 0x2A and 0x28 - and a
     * bound written for one silently rejects every one of the other, which is
     * a directory that lists as empty rather than an error anybody can see. */
    if (!usa_count || usa_off < 8) return false;
    if ((uint32_t)usa_off + (uint32_t)usa_count * 2 > bytes) return false;

    /* One entry for the check value, then one per sector. */
    if ((uint32_t)(usa_count - 1) * sector_size != bytes) return false;

    const uint8_t *usa = buf + usa_off;
    uint16_t check = rd16(usa);

    for (uint32_t i = 1; i < usa_count; i++) {
        uint8_t *tail = buf + i * sector_size - 2;
        if (rd16(tail) != check) return false;     /* torn, or not this record */
        tail[0] = usa[i * 2];
        tail[1] = usa[i * 2 + 1];
    }
    return true;
}

/* ---------------------------------------------------------------- run lists
 *
 * A non-resident attribute's data is described as a chain of runs, each a
 * length in clusters and an offset from the previous run's start.  Both are
 * stored in as few bytes as they need, with a header byte saying how many each
 * took - and the offset is signed, because a file's later parts can sit before
 * its earlier ones on the disk.
 *
 * A run with no offset field at all is a hole: a sparse region that reads as
 * zeroes and occupies nothing.
 */
void ntfs_runs_begin(ntfs_run_walker_t *w, const uint8_t *runs, uint32_t bytes) {
    w->p = runs;
    w->end = runs + bytes;
    w->lcn = 0;
    w->vcn = 0;
}

/* Returns false at the end of the list.  A hole reports lcn_out as -1. */
bool ntfs_runs_next(ntfs_run_walker_t *w, int64_t *lcn_out, uint64_t *count_out) {
    if (w->p >= w->end || !*w->p) return false;

    uint8_t header = *w->p++;
    uint8_t len_size = header & 0x0F;
    uint8_t off_size = (header >> 4) & 0x0F;

    if (!len_size || len_size > 8 || off_size > 8) return false;
    if (w->p + len_size + off_size > w->end) return false;

    uint64_t count = 0;
    for (int i = len_size - 1; i >= 0; i--) count = (count << 8) | w->p[i];
    w->p += len_size;

    if (off_size == 0) {
        *lcn_out = -1;                             /* sparse */
    } else {
        /* Sign-extend from however many bytes it was stored in. */
        int64_t delta = 0;
        for (int i = off_size - 1; i >= 0; i--) delta = (delta << 8) | w->p[i];
        if (w->p[off_size - 1] & 0x80) {
            int64_t sign = (int64_t)-1 << (off_size * 8);
            delta |= sign;
        }
        w->p += off_size;
        w->lcn += delta;
        *lcn_out = w->lcn;
    }

    *count_out = count;
    w->vcn += count;
    return true;
}

/* ------------------------------------------------------------- reading data */

bool ntfs_write_clusters(ntfs_volume_t *v, uint64_t lcn, uint32_t count,
                           const void *buf) {
    if (!v->write) return false;
    uint64_t lba = lcn * v->sectors_per_cluster;
    uint32_t sectors = count * v->sectors_per_cluster;
    return v->write(v->ctx, lba, sectors, buf);
}

bool ntfs_read_clusters(ntfs_volume_t *v, uint64_t lcn, uint32_t count,
                          void *buf) {
    uint64_t lba = lcn * v->sectors_per_cluster;
    uint32_t sectors = count * v->sectors_per_cluster;
    return v->read(v->ctx, lba, sectors, buf);
}

/* Read from a run list as though it were a flat file. */
long ntfs_read_runs(ntfs_volume_t *v, const uint8_t *runs, uint32_t runs_bytes,
                      uint64_t size, uint64_t offset, uint8_t *out,
                      uint32_t len) {
    if (offset >= size) return 0;
    if (offset + len > size) len = (uint32_t)(size - offset);

    static uint8_t cluster[65536];
    if (v->bytes_per_cluster > sizeof cluster) return -1;

    uint32_t done = 0;
    ntfs_run_walker_t w;
    ntfs_runs_begin(&w, runs, runs_bytes);

    uint64_t position = 0;                 /* bytes covered so far */
    int64_t  lcn;
    uint64_t count;

    while (done < len && ntfs_runs_next(&w, &lcn, &count)) {
        uint64_t run_bytes = count * v->bytes_per_cluster;

        if (offset + done >= position + run_bytes) {
            position += run_bytes;
            continue;                      /* entirely before what is wanted */
        }

        uint64_t into_run = (offset + done) - position;
        uint64_t cluster_index = into_run / v->bytes_per_cluster;
        uint32_t into_cluster = (uint32_t)(into_run % v->bytes_per_cluster);

        while (done < len && cluster_index < count) {
            uint32_t take = v->bytes_per_cluster - into_cluster;
            if (take > len - done) take = len - done;

            if (lcn < 0) {
                ntfs_zero_bytes(out + done, take);      /* a hole reads as zeroes */
            } else {
                if (!ntfs_read_clusters(v, (uint64_t)lcn + cluster_index, 1, cluster))
                    return -1;
                ntfs_copy_bytes(out + done, cluster + into_cluster, take);
            }

            done += take;
            into_cluster = 0;
            cluster_index++;
        }
        position += run_bytes;
    }
    return (long)done;
}

/* Put bytes back into a run list, which is the mirror of ntfs_read_runs.
 *
 * Two things it will not do, both deliberate.  It will not write to a hole:
 * a sparse region has no clusters behind it, so writing there means allocating
 * some, which is the bookkeeping this stays away from.  And a write that does
 * not start and end on a cluster boundary is done as a read, a modify and a
 * write, because a disk cannot be written in smaller pieces than it is read -
 * and skipping the read would put whatever was in the buffer over the
 * neighbouring bytes of the file.
 */
long ntfs_write_runs(ntfs_volume_t *v, const uint8_t *runs, uint32_t runs_bytes,
                       uint64_t size, uint64_t offset, const uint8_t *in,
                       uint32_t len) {
    if (!v->write) return -1;
    if (offset >= size) return 0;
    if (offset + len > size) len = (uint32_t)(size - offset);

    static uint8_t cluster[65536];
    if (v->bytes_per_cluster > sizeof cluster) return -1;

    uint32_t done = 0;
    ntfs_run_walker_t w;
    ntfs_runs_begin(&w, runs, runs_bytes);

    uint64_t position = 0;
    int64_t  lcn;
    uint64_t count;

    while (done < len && ntfs_runs_next(&w, &lcn, &count)) {
        uint64_t run_bytes = count * v->bytes_per_cluster;

        if (offset + done >= position + run_bytes) {
            position += run_bytes;
            continue;
        }

        uint64_t into_run = (offset + done) - position;
        uint64_t cluster_index = into_run / v->bytes_per_cluster;
        uint32_t into_cluster = (uint32_t)(into_run % v->bytes_per_cluster);

        while (done < len && cluster_index < count) {
            uint32_t take = v->bytes_per_cluster - into_cluster;
            if (take > len - done) take = len - done;

            if (lcn < 0) return (long)done;   /* a hole; stop rather than guess */

            uint64_t at = (uint64_t)lcn + cluster_index;

            if (take == v->bytes_per_cluster) {
                if (!ntfs_write_clusters(v, at, 1, in + done)) return -1;
            } else {
                /* Part of a cluster: read it, change the part being written,
                 * and put the whole thing back. */
                if (!ntfs_read_clusters(v, at, 1, cluster)) return -1;
                ntfs_copy_bytes(cluster + into_cluster, in + done, take);
                if (!ntfs_write_clusters(v, at, 1, cluster)) return -1;
            }

            done += take;
            into_cluster = 0;
            cluster_index++;
        }
        position += run_bytes;
    }
    return (long)done;
}

/* Where the MFT itself lives, when that changes.
 *
 * The run list of $MFT is copied out at mount time and every record lookup
 * goes through the copy.  That is fine until the MFT grows - and then the copy
 * describes the file as it used to be, so the records that were just added are
 * outside it and every read of one fails.  Silently, because a run list that
 * ends early is not an error, it is just a shorter file.
 *
 * So growing the MFT has to say so here.  This is the only thing that may
 * replace the copy, and it lives beside the code that made it.
 */
bool ntfs_remember_mft_runs(ntfs_volume_t *v, const uint8_t *runs,
                            uint32_t len) {
    if (!v->mft_runs || !mft_runs_capacity) return false;
    if (len > mft_runs_capacity) return false;
    ntfs_copy_bytes(v->mft_runs, runs, len);
    v->mft_runs_bytes = len;
    return true;
}

/* ------------------------------------------------------------- MFT records */

#define ATTR_STANDARD_INFO 0x10
#define ATTR_FILE_NAME     0x30
#define ATTR_DATA          0x80
#define ATTR_INDEX_ROOT    0x90
#define ATTR_INDEX_ALLOC   0xA0
#define ATTR_END           0xFFFFFFFF

#define FLAG_COMPRESSED    0x0001
#define FLAG_ENCRYPTED     0x4000
#define FLAG_SPARSE        0x8000

bool ntfs_read_mft_record(ntfs_volume_t *v, uint64_t number, uint8_t *out) {
    uint64_t offset = number * v->record_bytes;

    /* The MFT is itself a file, so its own run list is followed rather than
     * assuming it is one contiguous stretch.  On a volume that has been in use
     * for a while it is not. */
    long got = ntfs_read_runs(v, v->mft_runs, v->mft_runs_bytes,
                         (uint64_t)-1, offset, out, v->record_bytes);
    if (got != (long)v->record_bytes) return false;

    if (out[0] != 'F' || out[1] != 'I' || out[2] != 'L' || out[3] != 'E')
        return false;
    if (!ntfs_apply_fixups(out, v->record_bytes, v->bytes_per_sector)) return false;
    if (!(rd16(out + 0x16) & 1)) return false;      /* not in use */
    return true;
}

/* Find one attribute in a record.  `name` is matched when given. */
const uint8_t *ntfs_find_attribute(const uint8_t *rec, uint32_t rec_bytes,
                                     uint32_t type, const uint16_t *name,
                                     uint8_t name_len) {
    uint32_t off = rd16(rec + 0x14);

    while (off + 8 <= rec_bytes) {
        const uint8_t *a = rec + off;
        uint32_t a_type = rd32(a);
        if (a_type == ATTR_END) break;

        uint32_t a_len = rd32(a + 4);
        if (!a_len || off + a_len > rec_bytes) break;

        if (a_type == type) {
            uint8_t this_name_len = a[9];
            if (!name || !name_len) {
                if (!this_name_len) return a;
            } else if (this_name_len == name_len) {
                const uint8_t *n = a + rd16(a + 10);
                bool same = true;
                for (uint8_t i = 0; i < name_len; i++)
                    if (rd16(n + i * 2) != name[i]) { same = false; break; }
                if (same) return a;
            }
        }
        off += a_len;
    }
    return NULL;
}

/* Pull an attribute's content into a file description. */
static void take_attribute(ntfs_file_t *f, const uint8_t *a) {
    uint16_t flags = rd16(a + 12);
    if (flags & (FLAG_COMPRESSED | FLAG_ENCRYPTED)) {
        f->unreadable = true;
        return;
    }

    if (!a[8]) {                                    /* resident */
        uint32_t len = rd32(a + 0x10);
        uint16_t at = rd16(a + 0x14);
        if (len > sizeof f->resident_data) len = sizeof f->resident_data;
        ntfs_copy_bytes(f->resident_data, a + at, len);
        f->resident_bytes = len;
        f->resident = true;
        f->size = len;
        return;
    }

    f->resident = false;
    f->size = rd64(a + 0x30);                       /* real size */

    uint16_t runs_at = rd16(a + 0x20);
    uint32_t a_len = rd32(a + 4);
    uint32_t runs_len = a_len > runs_at ? a_len - runs_at : 0;
    if (runs_len > sizeof f->runs) runs_len = sizeof f->runs;
    ntfs_copy_bytes(f->runs, a + runs_at, runs_len);
    f->runs_bytes = runs_len;
}

/* ----------------------------------------------------------------- mounting */

bool ntfs_mount_rw(ntfs_volume_t *v, void *ctx, ntfs_read_fn read,
                   ntfs_write_fn write) {
    if (!ntfs_mount(v, ctx, read)) return false;
    v->write = write;
    return true;
}

bool ntfs_mount(ntfs_volume_t *v, void *ctx, ntfs_read_fn read) {
    ntfs_zero_bytes(v, sizeof *v);
    v->ctx = ctx;
    v->read = read;

    static uint8_t boot[512];
    if (!read(ctx, 0, 1, boot)) { ntfs_say(v, "the first sector could not be read"); return false; }

    if (boot[3] != 'N' || boot[4] != 'T' || boot[5] != 'F' || boot[6] != 'S') {
        ntfs_say(v, "this volume is not NTFS");
        return false;
    }

    v->bytes_per_sector = rd16(boot + 0x0B);
    uint8_t spc = boot[0x0D];
    /* Stored as a power of two when it will not fit in a byte, which is how a
     * volume formatted with very large clusters describes itself. */
    v->sectors_per_cluster = (spc > 0x80) ? (1u << (256 - spc)) : spc;
    v->total_sectors = rd64(boot + 0x28);
    v->mft_lcn = rd64(boot + 0x30);

    int8_t per_record = (int8_t)boot[0x40];
    v->record_bytes = (per_record < 0) ? (1u << (uint8_t)(-per_record))
                                       : (uint32_t)per_record * v->sectors_per_cluster
                                         * v->bytes_per_sector;

    if (!v->bytes_per_sector || !v->sectors_per_cluster ||
        v->bytes_per_sector > 4096 || v->record_bytes < 512 ||
        v->record_bytes > 4096) {
        ntfs_say(v, "the boot sector does not describe a volume this can read");
        return false;
    }
    v->bytes_per_cluster = v->bytes_per_sector * v->sectors_per_cluster;

    /* The MFT's own record, read directly from where the boot sector says the
     * MFT starts - the one read that cannot go through the MFT, because it is
     * what makes the MFT reachable. */
    static uint8_t rec[4096];
    if (!ntfs_read_clusters(v, v->mft_lcn, (v->record_bytes + v->bytes_per_cluster - 1)
                                      / v->bytes_per_cluster, rec)) {
        ntfs_say(v, "the master file table could not be read");
        return false;
    }
    if (rec[0] != 'F' || rec[1] != 'I' || rec[2] != 'L' || rec[3] != 'E' ||
        !ntfs_apply_fixups(rec, v->record_bytes, v->bytes_per_sector)) {
        ntfs_say(v, "the master file table's first record is damaged");
        return false;
    }

    const uint8_t *data = ntfs_find_attribute(rec, v->record_bytes, ATTR_DATA, NULL, 0);
    if (!data || !data[8]) {
        ntfs_say(v, "the master file table has no data run list");
        return false;
    }

    static uint8_t mft_runs[1024];
    mft_runs_capacity = sizeof mft_runs;
    uint16_t runs_at = rd16(data + 0x20);
    uint32_t a_len = rd32(data + 4);
    uint32_t runs_len = a_len > runs_at ? a_len - runs_at : 0;
    if (runs_len > sizeof mft_runs) runs_len = sizeof mft_runs;
    ntfs_copy_bytes(mft_runs, data + runs_at, runs_len);
    v->mft_runs = mft_runs;
    v->mft_runs_bytes = runs_len;
    mft_runs_capacity = sizeof mft_runs;

    v->mounted = true;
    return true;
}

/* ------------------------------------------------------------- directories */

/* The name NTFS gives a directory's index: "$I30", in UTF-16. */
static const uint16_t INDEX_NAME[4] = { '$', 'I', '3', '0' };

/* One entry in a directory index. */
static bool entry_name(const uint8_t *e, char *out, size_t cap, uint64_t *ref) {
    uint16_t key_len = rd16(e + 0x0A);
    if (key_len < 0x42) return false;

    const uint8_t *fn = e + 0x10;
    uint8_t name_len = fn[0x40];
    uint8_t space = fn[0x41];

    /* Namespace 2 is the short "8.3" name that exists alongside the real one.
     * Returning both would list every file twice. */
    if (space == 2) return false;

    const uint8_t *name = fn + 0x42;
    size_t o = 0;
    for (uint8_t i = 0; i < name_len && o + 4 < cap; i++) {
        uint16_t c = rd16(name + i * 2);
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
    *ref = rd64(e) & 0x0000FFFFFFFFFFFFull;        /* the low 48 bits          */
    return o > 0;
}

static bool same_name(const char *a, const char *b) {
    /* NTFS names are case-insensitive as they are used, whatever case they
     * were created in, so a path typed in either case finds the file. */
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return false;
        a++; b++;
    }
    return !*a && !*b;
}

/* Walk one index block's entries, calling back for each. */
typedef bool (*entry_fn)(void *ctx, const uint8_t *entry);

static void walk_entries(const uint8_t *first, const uint8_t *end, entry_fn fn,
                         void *ctx) {
    const uint8_t *e = first;
    while (e + 0x10 <= end) {
        uint16_t len = rd16(e + 0x08);
        uint16_t flags = rd16(e + 0x0C);
        if (!len || e + len > end) break;

        if (!(flags & 2)) {                        /* not the end marker */
            if (!fn(ctx, e)) return;
        }
        if (flags & 2) break;
        e += len;
    }
}

typedef struct {
    const char   *want;         /* NULL when listing                     */
    uint32_t      want_index;   /* which one, when listing               */
    uint32_t      seen;
    bool          found;
    uint64_t      ref;
    char         *name_out;
    size_t        name_cap;
} search_t;

static bool consider(void *ctx, const uint8_t *e) {
    search_t *s = ctx;
    char name[512];
    uint64_t ref;
    if (!entry_name(e, name, sizeof name, &ref)) return true;

    if (s->want) {
        if (same_name(name, s->want)) { s->found = true; s->ref = ref; return false; }
        return true;
    }

    if (s->seen == s->want_index) {
        s->found = true;
        s->ref = ref;
        if (s->name_out) {
            size_t i = 0;
            while (name[i] && i + 1 < s->name_cap) { s->name_out[i] = name[i]; i++; }
            s->name_out[i] = 0;
        }
        return false;
    }
    s->seen++;
    return true;
}

/* Search a directory's index, both the part inside its record and the blocks
 * outside it.  A directory small enough to fit in its record has no blocks. */
static bool search_directory(ntfs_volume_t *v, uint64_t dir_record,
                             search_t *s) {
    static uint8_t rec[4096];
    bool got_record = ntfs_read_mft_record(v, dir_record, rec);
    TRACE("dir_record", dir_record, got_record);
    if (!got_record) return false;

    const uint8_t *root = ntfs_find_attribute(rec, v->record_bytes, ATTR_INDEX_ROOT,
                                         INDEX_NAME, 4);
    TRACE("index_root", root ? 1 : 0, root ? root[8] : 0);
    if (!root || root[8]) return false;            /* always resident */

    const uint8_t *value = root + rd16(root + 0x14);
    uint32_t value_len = rd32(root + 0x10);

    /* The index header sits after a 16-byte preamble; entries begin at the
     * offset it gives, counted from the header itself. */
    const uint8_t *hdr = value + 0x10;
    uint32_t first = rd32(hdr);
    uint32_t used = rd32(hdr + 4);
    if (first < 0x10 || used > value_len) return false;

    walk_entries(hdr + first, hdr + used, consider, s);
    if (s->found) return true;

    /* And the blocks, when the directory outgrew its record. */
    const uint8_t *alloc = ntfs_find_attribute(rec, v->record_bytes, ATTR_INDEX_ALLOC,
                                          INDEX_NAME, 4);
    TRACE("index_alloc", alloc ? 1 : 0, alloc ? alloc[8] : 0);
    if (!alloc || !alloc[8]) return false;

    ntfs_file_t index;
    ntfs_zero_bytes(&index, sizeof index);
    take_attribute(&index, alloc);
    if (index.unreadable) return false;

    uint32_t block_bytes = rd32(value + 0x08);
    TRACE("block_bytes", block_bytes, index.size);
    if (block_bytes < 512 || block_bytes > 65536) return false;

    static uint8_t block[65536];
    for (uint64_t at = 0; at + block_bytes <= index.size; at += block_bytes) {
        long got = ntfs_read_runs(v, index.runs, index.runs_bytes, index.size,
                             at, block, block_bytes);
        TRACE("indx_read", at, got);
        if (got != (long)block_bytes) break;

        if (block[0] != 'I' || block[1] != 'N' || block[2] != 'D' || block[3] != 'X')
            continue;
        if (!ntfs_apply_fixups(block, block_bytes, v->bytes_per_sector)) {
            TRACE("indx_fixup_failed", at, 0);
            continue;
        }

        const uint8_t *hdr2 = block + 0x18;
        uint32_t first2 = rd32(hdr2);
        uint32_t used2 = rd32(hdr2 + 4);
        if (first2 < 0x10 || 0x18 + used2 > block_bytes) continue;

        TRACE("indx_walk", first2, used2);
        walk_entries(hdr2 + first2, hdr2 + used2, consider, s);
        if (s->found) return true;
    }
    return false;
}

/* ---------------------------------------------------------------- lookup */

static void describe(ntfs_volume_t *v, uint64_t record, ntfs_file_t *out) {
    ntfs_zero_bytes(out, sizeof *out);
    out->record = record;

    static uint8_t rec[4096];
    if (!ntfs_read_mft_record(v, record, rec)) { out->unreadable = true; return; }

    out->directory = (rd16(rec + 0x16) & 2) != 0;

    if (out->directory) return;

    const uint8_t *data = ntfs_find_attribute(rec, v->record_bytes, ATTR_DATA, NULL, 0);
    if (!data) { out->size = 0; return; }
    take_attribute(out, data);
}

#define ROOT_RECORD 5                              /* "." is always record 5 */

bool ntfs_lookup(ntfs_volume_t *v, const char *path, ntfs_file_t *out) {
    if (!v->mounted) return false;

    uint64_t at = ROOT_RECORD;

    while (*path == '/' || *path == '\\') path++;

    while (*path) {
        char part[256];
        size_t n = 0;
        while (path[n] && path[n] != '/' && path[n] != '\\' &&
               n + 1 < sizeof part) {
            part[n] = path[n];
            n++;
        }
        part[n] = 0;
        path += n;
        while (*path == '/' || *path == '\\') path++;

        if (!n) break;

        search_t s;
        ntfs_zero_bytes(&s, sizeof s);
        s.want = part;
        if (!search_directory(v, at, &s) || !s.found) return false;
        at = s.ref;
    }

    describe(v, at, out);
    return !out->unreadable || out->directory;
}

long ntfs_read_file(ntfs_volume_t *v, const ntfs_file_t *f, uint64_t offset,
                    void *buf, uint32_t len) {
    if (!v->mounted || f->directory || f->unreadable) return -1;

    if (f->resident) {
        if (offset >= f->resident_bytes) return 0;
        uint32_t take = f->resident_bytes - (uint32_t)offset;
        if (take > len) take = len;
        ntfs_copy_bytes(buf, f->resident_data + offset, take);
        return (long)take;
    }
    return ntfs_read_runs(v, f->runs, f->runs_bytes, f->size, offset, buf, len);
}

long ntfs_write_file(ntfs_volume_t *v, const ntfs_file_t *f, uint64_t offset,
                     const void *buf, uint32_t len) {
    if (!v->mounted || !v->write) return -1;
    if (f->directory || f->unreadable) return -1;

    /* A file small enough to live inside its own record is not written here.
     * Changing it means writing to the master file table, which is the volume's
     * own bookkeeping - exactly what this stays out of.  The caller is told no
     * rather than being given a partial write it did not ask for. */
    if (f->resident) return -1;

    return ntfs_write_runs(v, f->runs, f->runs_bytes, f->size, offset, buf, len);
}

bool ntfs_readdir(ntfs_volume_t *v, const ntfs_file_t *dir, uint32_t index,
                  char *name_out, size_t name_cap, ntfs_file_t *entry_out) {
    if (!v->mounted || !dir->directory) return false;

    search_t s;
    ntfs_zero_bytes(&s, sizeof s);
    s.want_index = index;
    s.name_out = name_out;
    s.name_cap = name_cap;

    if (!search_directory(v, dir->record, &s) || !s.found) return false;
    if (entry_out) describe(v, s.ref, entry_out);
    return true;
}
