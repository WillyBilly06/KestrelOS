/* ntfs_write.c - creating files on NTFS.
 *
 * The reader next door has been able to change a file's bytes for a while, and
 * that sounded close to writing.  It was not.  Every path in this system that
 * wanted to put something new on the data volume - the kernel log, a saved
 * setting, anything the desktop keeps - needed a file that did not exist yet,
 * and the answer was always no.  On a stick whose data partition is NTFS that
 * turned into "log on disk: No" and a volume that stayed empty, which from the
 * outside looked like logging being broken rather than like this.
 *
 * ---------------------------------------------------------------------------
 * WHAT CREATING A FILE ACTUALLY IS
 *
 * On FAT it is one directory entry and a chain of clusters.  On NTFS it is
 * four separate structures that must agree with each other:
 *
 *   1. a bit in $Bitmap for every cluster the data occupies,
 *   2. a bit in $MFT's own bitmap for the record the file gets,
 *   3. that record - a FILE record holding the file's attributes,
 *   4. an entry in the parent directory's index, which is a B-tree sorted by
 *      the name, upcased through the volume's own $UpCase table.
 *
 * Disagreement between any two of them is corruption, and Windows will mount
 * the volume anyway and behave strangely rather than refuse.
 *
 * ---------------------------------------------------------------------------
 * WHY THE ORDER OF THE WRITES IS THE SAFETY
 *
 * NTFS is journalled and this driver does not write the journal.  That is a
 * real limitation and it is not hidden: it means a power cut in the middle of
 * a create cannot be replayed.  What can be done - and is done here - is to
 * order the writes so that every possible interruption leaves the volume
 * consistent, just with something wasted:
 *
 *   clusters marked used  ->  worst case: space nothing owns.  chkdsk frees it.
 *   the record written    ->  worst case: a record no directory points to.
 *   the record's bit set  ->  worst case: the same, and chkdsk moves it to
 *                             found.000 rather than losing it.
 *   the index entry       <-  THE COMMIT.  Until this, the file does not exist;
 *                             after it, every structure it needs is already on
 *                             disk.
 *
 * The reverse order - index entry first - is the one that produces a directory
 * pointing at a record that was never written, which is the failure that makes
 * a volume unmountable.  So the order here is not a detail; it is the whole
 * substitute for a journal, and it is why this is safe to point at a disk that
 * Windows also has to mount.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS REFUSES TO DO
 *
 * A refusal is a volume that still works.  A guess is a volume that does not.
 * So this stops, with a message, rather than improvise, when:
 *
 *   - a directory's index would have to be split to take another entry,
 *   - an MFT record has no room left for another attribute,
 *   - the MFT is full (growing the MFT itself is not implemented),
 *   - a file would need more than a bounded number of extents,
 *   - the name is not one this can collate confidently.
 *
 * Every one of those is a thing that can be added later.  None of them can be
 * approximated now.
 * ---------------------------------------------------------------------------
 */
#include "ntfs_core.h"
#include "ntfs_internal.h"

/* The clock, if the caller has one.  The kernel sets this; the loader and the
 * host test do not, and get a fixed plausible time rather than zero - a zero
 * timestamp is a thing Windows shows as an error rather than as a date. */
uint64_t (*ntfs_now)(void);

/* 1st January 2020, in the units NTFS keeps time in: 100-nanosecond ticks
 * since 1601.  Only used when nothing better is available. */
#define NTFS_FALLBACK_TIME 132223104000000000ull

static uint64_t now_ticks(void) {
    return ntfs_now ? ntfs_now() : NTFS_FALLBACK_TIME;
}

/* An overlap-safe move.  There is no libc here and the tail of a record is
 * routinely shifted over itself. */
static void move_bytes(uint8_t *dst, const uint8_t *src, uint32_t n) {
    if (dst == src || !n) return;
    if (dst < src) {
        while (n--) *dst++ = *src++;
    } else {
        dst += n; src += n;
        while (n--) *--dst = *--src;
    }
}

static uint32_t align8(uint32_t n) { return (n + 7u) & ~7u; }

/* =========================================================== writing records
 *
 * The fixups, in the direction the reader does not go.
 *
 * Every sector of a record has its last two bytes replaced by one shared check
 * value, and the bytes that belonged there are kept in an array in the header.
 * The reader puts them back.  A writer has to take them out again - and has to
 * change the check value each time, because the entire point is that a record
 * whose sectors do not all carry the current value was torn by a power cut
 * partway through being written.
 *
 * Getting this wrong does not fail here.  It fails later, in the reader, on
 * two bytes in every 512 - which looks exactly like a parsing bug and is not.
 */
static bool embed_fixups(uint8_t *buf, uint32_t bytes, uint32_t sector_size) {
    uint16_t usa_off = ntfs_rd16(buf + 4);
    uint16_t usa_count = ntfs_rd16(buf + 6);

    if (!usa_count || usa_off < 8) return false;
    if ((uint32_t)usa_off + (uint32_t)usa_count * 2 > bytes) return false;
    if ((uint32_t)(usa_count - 1) * sector_size != bytes) return false;

    uint8_t *usa = buf + usa_off;

    /* A new check value.  Zero and 0xFFFF are both avoided: the first is what
     * unwritten disk looks like and the second is what a failing one does, so
     * neither can be told apart from a record that was never written. */
    uint16_t check = (uint16_t)(ntfs_rd16(usa) + 1);
    if (check == 0 || check == 0xFFFF) check = 1;
    ntfs_wr16(usa, check);

    for (uint32_t i = 1; i < usa_count; i++) {
        uint8_t *tail = buf + i * sector_size - 2;
        usa[i * 2]     = tail[0];
        usa[i * 2 + 1] = tail[1];
        ntfs_wr16(tail, check);
    }
    return true;
}

/* Put one MFT record back.  The buffer is modified - the fixups go in - so it
 * cannot be reused as a parsed record afterwards without re-reading. */
static bool write_mft_record(ntfs_volume_t *v, uint64_t number, uint8_t *rec) {
    if (!v->write) { ntfs_say(v, "volume opened for reading"); return false; }
    if (!embed_fixups(rec, v->record_bytes, v->bytes_per_sector)) {
        ntfs_say(v, "record header does not describe its own fixups");
        return false;
    }
    long put = ntfs_write_runs(v, v->mft_runs, v->mft_runs_bytes,
                               (uint64_t)-1, number * v->record_bytes,
                               rec, v->record_bytes);
    return put == (long)v->record_bytes;
}

/* ============================================================ the $UpCase map
 *
 * NTFS sorts directory entries by the name with every character upcased, and
 * the mapping it upcases through is stored on the volume rather than assumed -
 * $UpCase, record 10, one 16-bit entry per code unit.
 *
 * This matters more than it sounds.  The index is a B-tree and Windows finds a
 * name by binary search through it.  An entry inserted at a position that
 * disagrees with the volume's own ordering is not found by that search - the
 * file is on the disk, its record is intact, and it is invisible.  Guessing
 * "A-Z, and probably the rest" is exactly the kind of nearly-right that
 * produces that.
 *
 * So the real table is loaded.  128 KB, once, on the first write.  If it
 * cannot be loaded, names outside ASCII are refused rather than sorted by
 * guesswork.
 */
#define UPCASE_ENTRIES 65536
static uint16_t upcase[UPCASE_ENTRIES];
static bool upcase_loaded;

static bool load_upcase(ntfs_volume_t *v) {
    if (upcase_loaded) return true;

    ntfs_file_t f;
    static uint8_t rec[4096];
    if (v->record_bytes > sizeof rec) return false;
    if (!ntfs_read_mft_record(v, NTFS_UPCASE_RECORD, rec)) return false;

    const uint8_t *a = ntfs_find_attribute(rec, v->record_bytes,
                                           NTFS_ATTR_DATA, NULL, 0);
    if (!a || !a[8]) return false;            /* $UpCase is never resident */

    ntfs_zero_bytes(&f, sizeof f);
    f.size = ntfs_rd64(a + 0x30);
    uint16_t runs_at = ntfs_rd16(a + 0x20);
    uint32_t a_len = ntfs_rd32(a + 4);
    uint32_t runs_len = a_len > runs_at ? a_len - runs_at : 0;
    if (runs_len > sizeof f.runs) runs_len = sizeof f.runs;
    ntfs_copy_bytes(f.runs, a + runs_at, runs_len);
    f.runs_bytes = runs_len;

    if (f.size < sizeof upcase) return false;

    /* In pieces, because a single 128 KB read is larger than the scratch
     * buffers anything else here uses. */
    uint8_t *out = (uint8_t *)upcase;
    for (uint32_t off = 0; off < sizeof upcase; off += 4096) {
        long got = ntfs_read_runs(v, f.runs, f.runs_bytes, f.size, off,
                                  out + off, 4096);
        if (got != 4096) return false;
    }

    /* The table is stored little-endian; on a little-endian machine that is
     * already what it is, but say so rather than rely on it. */
    for (uint32_t i = 0; i < UPCASE_ENTRIES; i++)
        upcase[i] = ntfs_rd16((const uint8_t *)upcase + i * 2);

    /* A table that does not upcase 'a' is not the table. */
    if (upcase['a'] != 'A' || upcase['z'] != 'Z' || upcase['A'] != 'A')
        return false;

    upcase_loaded = true;
    return true;
}

static uint16_t up(uint16_t c) {
    if (upcase_loaded) return upcase[c];
    if (c >= 'a' && c <= 'z') return (uint16_t)(c - 'a' + 'A');
    return c;
}

/* NTFS's own ordering: upcased code unit by code unit, then the shorter name
 * first.  Not a byte comparison and not case-insensitive-ASCII; this is the
 * rule the volume was sorted with and the one it must stay sorted with. */
static int collate(const uint8_t *a, uint8_t alen,
                   const uint8_t *b, uint8_t blen) {
    uint8_t n = alen < blen ? alen : blen;
    for (uint8_t i = 0; i < n; i++) {
        uint16_t ca = up(ntfs_rd16(a + i * 2));
        uint16_t cb = up(ntfs_rd16(b + i * 2));
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (alen == blen) return 0;
    return alen < blen ? -1 : 1;
}

/* ============================================================== the bitmaps
 *
 * Both allocators below are the same shape: a file whose bytes are bits, one
 * per thing.  $Bitmap covers clusters; $MFT's own $BITMAP attribute covers
 * records.
 */
typedef struct {
    uint8_t  runs[512];
    uint32_t runs_bytes;
    uint64_t size;                /* bytes of the bitmap                    */
    bool     resident;
    uint8_t  data[512];           /* when resident, which small volumes are */
} bitmap_t;

static bool bitmap_of_attribute(ntfs_volume_t *v, const uint8_t *a, bitmap_t *b) {
    ntfs_zero_bytes(b, sizeof *b);
    if (!a[8]) {
        uint32_t len = ntfs_rd32(a + 0x10);
        if (len > sizeof b->data) return false;
        ntfs_copy_bytes(b->data, a + ntfs_rd16(a + 0x14), len);
        b->size = len;
        b->resident = true;
        return true;
    }
    b->size = ntfs_rd64(a + 0x30);
    uint16_t runs_at = ntfs_rd16(a + 0x20);
    uint32_t a_len = ntfs_rd32(a + 4);
    uint32_t runs_len = a_len > runs_at ? a_len - runs_at : 0;
    if (runs_len > sizeof b->runs) return false;
    ntfs_copy_bytes(b->runs, a + runs_at, runs_len);
    b->runs_bytes = runs_len;
    return true;
}

/* Find `want` consecutive clear bits at or above `floor`, set them, and say
 * where.
 *
 * First fit from a hint, which is what NTFS itself does and is the reason a
 * freshly written volume's files end up next to each other rather than
 * scattered.  Returns how many were actually taken, which may be fewer than
 * asked for - the caller builds a second run rather than failing.
 *
 * The floor and the hint are different things and both are needed.  The hint
 * is where to look first and the search wraps past it when that part is full;
 * the floor is a bit the search may never return no matter how full the rest
 * is.  Collapsing the two - treating the hint as the bottom - looks correct
 * until the volume fills enough for the wrap to happen, and then hands out
 * one of the two dozen MFT records NTFS keeps for its own metadata.
 */
static uint64_t bitmap_take(ntfs_volume_t *v, bitmap_t *b, uint64_t limit,
                            uint64_t floor, uint64_t hint, uint64_t want,
                            uint64_t *at_out) {
    static uint8_t chunk[4096];

    uint64_t bits = b->size * 8;
    if (bits > limit) bits = limit;
    if (floor >= bits) return 0;
    if (hint < floor || hint >= bits) hint = floor;

    /* Two passes: from the hint to the end, then from the floor back up to the
     * hint, so a volume that is full at the end still finds room at the front
     * without ever dropping below the floor. */
    for (int pass = 0; pass < 2; pass++) {
        uint64_t from = pass == 0 ? hint : floor;
        uint64_t to   = pass == 0 ? bits : hint;
        if (from >= to) continue;

        uint64_t run_start = 0, run_len = 0;

        for (uint64_t bit = from; bit < to; ) {
            uint64_t byte = bit / 8;
            uint64_t base = byte & ~(uint64_t)(sizeof chunk - 1);
            uint32_t span = sizeof chunk;
            if (base + span > b->size) span = (uint32_t)(b->size - base);
            if (!span) break;

            if (b->resident) {
                if (base + span > sizeof b->data) return 0;
                ntfs_copy_bytes(chunk, b->data + base, span);
            } else {
                long got = ntfs_read_runs(v, b->runs, b->runs_bytes, b->size,
                                          base, chunk, span);
                if (got != (long)span) return 0;
            }

            uint64_t chunk_end_bit = (base + span) * 8;
            if (chunk_end_bit > to) chunk_end_bit = to;

            while (bit < chunk_end_bit) {
                uint32_t index = (uint32_t)(bit / 8 - base);
                if (chunk[index] & (1u << (bit & 7))) {
                    run_len = 0;                     /* taken; start over */
                } else {
                    if (!run_len) run_start = bit;
                    run_len++;
                    if (run_len >= want) goto found;
                }
                bit++;
            }

            /* A run may continue into the next chunk, so run_len is kept. */
        }
        continue;

    found:
        {
            uint64_t took = run_len;
            /* Set every bit of the run, chunk by chunk, reading each byte
             * back first so the bits belonging to other files survive. */
            for (uint64_t bit = run_start; bit < run_start + took; ) {
                uint64_t byte = bit / 8;
                uint64_t base = byte & ~(uint64_t)(sizeof chunk - 1);
                uint32_t span = sizeof chunk;
                if (base + span > b->size) span = (uint32_t)(b->size - base);

                if (b->resident) {
                    ntfs_copy_bytes(chunk, b->data + base, span);
                } else {
                    long got = ntfs_read_runs(v, b->runs, b->runs_bytes,
                                              b->size, base, chunk, span);
                    if (got != (long)span) return 0;
                }

                uint64_t chunk_end_bit = (base + span) * 8;
                uint64_t stop = run_start + took;
                if (chunk_end_bit < stop) stop = chunk_end_bit;

                uint64_t first = bit;
                while (bit < stop) {
                    chunk[(uint32_t)(bit / 8 - base)] |= (uint8_t)(1u << (bit & 7));
                    bit++;
                }

                /* Only the bytes actually touched are written back. */
                uint32_t lo = (uint32_t)(first / 8 - base);
                uint32_t hi = (uint32_t)((bit - 1) / 8 - base) + 1;
                if (b->resident) {
                    ntfs_copy_bytes(b->data + base + lo, chunk + lo, hi - lo);
                    /* The caller writes a resident bitmap back with its
                     * record; nothing to do here. */
                } else {
                    long put = ntfs_write_runs(v, b->runs, b->runs_bytes,
                                               b->size, base + lo,
                                               chunk + lo, hi - lo);
                    if (put != (long)(hi - lo)) return 0;
                }
            }
            *at_out = run_start;
            return took;
        }
    }
    return 0;
}

/* Give bits back.  Used only to undo a partial create, which is why it is
 * allowed to fail quietly - the alternative to a leaked cluster is a caller
 * that cannot report the real error underneath. */
static void bitmap_give_back(ntfs_volume_t *v, bitmap_t *b, uint64_t at,
                             uint64_t count) {
    static uint8_t chunk[4096];
    if (b->resident) return;

    for (uint64_t bit = at; bit < at + count; ) {
        uint64_t base = (bit / 8) & ~(uint64_t)(sizeof chunk - 1);
        uint32_t span = sizeof chunk;
        if (base + span > b->size) span = (uint32_t)(b->size - base);
        if (!span) return;
        if (ntfs_read_runs(v, b->runs, b->runs_bytes, b->size, base,
                           chunk, span) != (long)span) return;

        uint64_t chunk_end_bit = (base + span) * 8;
        uint64_t stop = at + count;
        if (chunk_end_bit < stop) stop = chunk_end_bit;

        uint64_t first = bit;
        while (bit < stop) {
            chunk[(uint32_t)(bit / 8 - base)] &= (uint8_t)~(1u << (bit & 7));
            bit++;
        }
        uint32_t lo = (uint32_t)(first / 8 - base);
        uint32_t hi = (uint32_t)((bit - 1) / 8 - base) + 1;
        ntfs_write_runs(v, b->runs, b->runs_bytes, b->size, base + lo,
                        chunk + lo, hi - lo);
    }
}

/* ------------------------------------------------------------ cluster space */

static bool cluster_bitmap(ntfs_volume_t *v, bitmap_t *b) {
    static uint8_t rec[4096];
    if (v->record_bytes > sizeof rec) return false;
    if (!ntfs_read_mft_record(v, NTFS_BITMAP_RECORD, rec)) {
        ntfs_say(v, "$Bitmap is not readable");
        return false;
    }
    const uint8_t *a = ntfs_find_attribute(rec, v->record_bytes,
                                           NTFS_ATTR_DATA, NULL, 0);
    if (!a || !bitmap_of_attribute(v, a, b)) {
        ntfs_say(v, "$Bitmap has no data this driver understands");
        return false;
    }
    return true;
}

/* ================================================================= extents
 *
 * A run list, taken apart so it can be added to and put back together.  The
 * bound is deliberate: a file that needs more pieces than this is one whose
 * run list might not fit in its record either, and refusing is better than
 * finding that out halfway through writing the record.
 */
#define MAX_EXTENTS 32

typedef struct {
    uint64_t lcn, count;
} extent_t;

static bool extents_of(const uint8_t *runs, uint32_t runs_bytes,
                       extent_t *out, uint32_t *n_out) {
    ntfs_run_walker_t w;
    ntfs_runs_begin(&w, runs, runs_bytes);
    uint32_t n = 0;
    int64_t lcn; uint64_t count;
    while (ntfs_runs_next(&w, &lcn, &count)) {
        if (lcn < 0) return false;            /* sparse; not written here */
        if (n >= MAX_EXTENTS) return false;
        out[n].lcn = (uint64_t)lcn;
        out[n].count = count;
        n++;
    }
    *n_out = n;
    return true;
}

/* How many bytes a signed value needs, smallest first - the encoding stores
 * each field in as few bytes as it fits in, and "fits" means the top bit of
 * the last byte is the sign. */
static uint8_t signed_width(int64_t value) {
    for (uint8_t n = 1; n < 8; n++) {
        int64_t lo = -((int64_t)1 << (n * 8 - 1));
        int64_t hi =  ((int64_t)1 << (n * 8 - 1)) - 1;
        if (value >= lo && value <= hi) return n;
    }
    return 8;
}

static uint8_t unsigned_width(uint64_t value) {
    for (uint8_t n = 1; n < 8; n++)
        if (value < ((uint64_t)1 << (n * 8))) return n;
    return 8;
}

static uint32_t extents_encode(const extent_t *e, uint32_t n,
                               uint8_t *out, uint32_t cap) {
    uint32_t at = 0;
    int64_t previous = 0;

    for (uint32_t i = 0; i < n; i++) {
        int64_t delta = (int64_t)e[i].lcn - previous;
        uint8_t lw = unsigned_width(e[i].count);
        uint8_t ow = signed_width(delta);
        if (at + 1 + lw + ow + 1 > cap) return 0;

        out[at++] = (uint8_t)(lw | (ow << 4));
        for (uint8_t b = 0; b < lw; b++) out[at++] = (uint8_t)(e[i].count >> (b * 8));
        for (uint8_t b = 0; b < ow; b++) out[at++] = (uint8_t)((uint64_t)delta >> (b * 8));
        previous = (int64_t)e[i].lcn;
    }
    out[at++] = 0;                              /* the terminator */
    return at;
}

/* ============================================================== attributes */

static uint8_t *find_attr_mut(uint8_t *rec, uint32_t rec_bytes, uint32_t type,
                              const uint16_t *name, uint8_t name_len) {
    return (uint8_t *)ntfs_find_attribute(rec, rec_bytes, type, name, name_len);
}

/* Make room inside a record by pushing everything after `a` further along.
 * `extra` must already be a multiple of eight, because every attribute after
 * this one starts where the previous ended and they are all eight-aligned. */
static bool make_room_after(ntfs_volume_t *v, uint8_t *rec, uint8_t *a,
                            uint32_t extra) {
    uint32_t used = ntfs_rd32(rec + 0x18);
    if (used + extra > v->record_bytes) {
        ntfs_say(v, "no room in the record to enlarge an attribute");
        return false;
    }
    uint32_t a_len = ntfs_rd32(a + 4);
    uint8_t *after = a + a_len;
    uint32_t tail = (uint32_t)((rec + used) - after);
    move_bytes(after + extra, after, tail);
    ntfs_zero_bytes(after, extra);
    ntfs_wr32(rec + 0x18, used + extra);
    return true;
}

/* And the reverse, for an attribute that shrinks. */
static void take_room_after(ntfs_volume_t *v, uint8_t *rec, uint8_t *a,
                            uint32_t less) {
    uint32_t used = ntfs_rd32(rec + 0x18);
    uint32_t a_len = ntfs_rd32(a + 4);
    uint8_t *after = a + a_len;
    uint32_t tail = (uint32_t)((rec + used) - after);
    move_bytes(after - less, after, tail);
    ntfs_zero_bytes(rec + used - less, less);
    ntfs_wr32(rec + 0x18, used - less);
}

/* Append a resident attribute.  Attributes are kept in ascending order of
 * type, which Windows relies on, so this inserts rather than appends when the
 * record already holds a higher-numbered one. */
static uint8_t *add_resident_attr(ntfs_volume_t *v, uint8_t *rec, uint32_t type,
                                  const void *value, uint32_t value_len,
                                  uint8_t indexed) {
    uint32_t header = 0x18;
    uint32_t total = align8(header + value_len);

    /* The insertion point: the first attribute whose type is greater. */
    uint32_t off = ntfs_rd16(rec + 0x14);
    while (off + 4 <= v->record_bytes) {
        uint32_t t = ntfs_rd32(rec + off);
        if (t == NTFS_ATTR_END || t > type) break;
        uint32_t len = ntfs_rd32(rec + off + 4);
        if (!len || off + len > v->record_bytes) return NULL;
        off += len;
    }

    uint32_t used = ntfs_rd32(rec + 0x18);
    if (used + total > v->record_bytes) {
        ntfs_say(v, "no room in the record for one more attribute");
        return NULL;
    }

    uint8_t *a = rec + off;
    move_bytes(a + total, a, used - off);
    ntfs_zero_bytes(a, total);
    ntfs_wr32(rec + 0x18, used + total);

    ntfs_wr32(a + 0x00, type);
    ntfs_wr32(a + 0x04, total);
    a[0x08] = 0;                                  /* resident */
    a[0x09] = 0;                                  /* no name */
    ntfs_wr16(a + 0x0A, 0);
    ntfs_wr16(a + 0x0C, 0);
    ntfs_wr16(a + 0x0E, ntfs_rd16(rec + 0x28));   /* instance */
    ntfs_wr16(rec + 0x28, (uint16_t)(ntfs_rd16(rec + 0x28) + 1));
    ntfs_wr32(a + 0x10, value_len);
    ntfs_wr16(a + 0x14, (uint16_t)header);
    a[0x16] = indexed;
    a[0x17] = 0;
    if (value) ntfs_copy_bytes(a + header, value, value_len);
    return a;
}

/* The same, for one that carries a name - $INDEX_ROOT is always "$I30". */
static uint8_t *add_named_resident_attr(ntfs_volume_t *v, uint8_t *rec,
                                        uint32_t type, const uint16_t *name,
                                        uint8_t name_len, const void *value,
                                        uint32_t value_len) {
    uint32_t name_at = 0x18;
    uint32_t value_at = align8(name_at + (uint32_t)name_len * 2);
    uint32_t total = align8(value_at + value_len);

    uint32_t off = ntfs_rd16(rec + 0x14);
    while (off + 4 <= v->record_bytes) {
        uint32_t t = ntfs_rd32(rec + off);
        if (t == NTFS_ATTR_END || t > type) break;
        uint32_t len = ntfs_rd32(rec + off + 4);
        if (!len || off + len > v->record_bytes) return NULL;
        off += len;
    }

    uint32_t used = ntfs_rd32(rec + 0x18);
    if (used + total > v->record_bytes) {
        ntfs_say(v, "no room in the record for one more named attribute");
        return NULL;
    }

    uint8_t *a = rec + off;
    move_bytes(a + total, a, used - off);
    ntfs_zero_bytes(a, total);
    ntfs_wr32(rec + 0x18, used + total);

    ntfs_wr32(a + 0x00, type);
    ntfs_wr32(a + 0x04, total);
    a[0x08] = 0;
    a[0x09] = name_len;
    ntfs_wr16(a + 0x0A, (uint16_t)name_at);
    ntfs_wr16(a + 0x0C, 0);
    ntfs_wr16(a + 0x0E, ntfs_rd16(rec + 0x28));
    ntfs_wr16(rec + 0x28, (uint16_t)(ntfs_rd16(rec + 0x28) + 1));
    ntfs_wr32(a + 0x10, value_len);
    ntfs_wr16(a + 0x14, (uint16_t)value_at);
    a[0x16] = 0;
    a[0x17] = 0;
    for (uint8_t i = 0; i < name_len; i++)
        ntfs_wr16(a + name_at + i * 2, name[i]);
    if (value) ntfs_copy_bytes(a + value_at, value, value_len);
    return a;
}

/* A named attribute whose data lives in clusters rather than in the record.
 * $INDEX_ALLOCATION is the only one this driver adds. */
static uint8_t *add_named_nonresident_attr(ntfs_volume_t *v, uint8_t *rec,
                                           uint32_t type, const uint16_t *name,
                                           uint8_t name_len,
                                           const uint8_t *runs,
                                           uint32_t runs_len,
                                           uint64_t allocated, uint64_t size) {
    uint32_t name_at = 0x40;                     /* after the long header */
    uint32_t runs_at = align8(name_at + (uint32_t)name_len * 2);
    uint32_t total = align8(runs_at + runs_len);

    uint32_t off = ntfs_rd16(rec + 0x14);
    while (off + 4 <= v->record_bytes) {
        uint32_t t = ntfs_rd32(rec + off);
        if (t == NTFS_ATTR_END || t > type) break;
        uint32_t len = ntfs_rd32(rec + off + 4);
        if (!len || off + len > v->record_bytes) return NULL;
        off += len;
    }

    uint32_t used = ntfs_rd32(rec + 0x18);
    if (used + total > v->record_bytes) {
        ntfs_say(v, "the record has no room for the index allocation");
        return NULL;
    }

    uint8_t *a = rec + off;
    move_bytes(a + total, a, used - off);
    ntfs_zero_bytes(a, total);
    ntfs_wr32(rec + 0x18, used + total);

    ntfs_wr32(a + 0x00, type);
    ntfs_wr32(a + 0x04, total);
    a[0x08] = 1;                                  /* non-resident */
    a[0x09] = name_len;
    ntfs_wr16(a + 0x0A, (uint16_t)name_at);
    ntfs_wr16(a + 0x0C, 0);
    ntfs_wr16(a + 0x0E, ntfs_rd16(rec + 0x28));
    ntfs_wr16(rec + 0x28, (uint16_t)(ntfs_rd16(rec + 0x28) + 1));
    ntfs_wr64(a + 0x10, 0);                       /* lowest vcn */
    ntfs_wr64(a + 0x18, allocated / v->bytes_per_cluster - 1);
    ntfs_wr16(a + 0x20, (uint16_t)runs_at);
    ntfs_wr16(a + 0x22, 0);
    ntfs_wr64(a + 0x28, allocated);
    ntfs_wr64(a + 0x30, size);
    ntfs_wr64(a + 0x38, size);
    for (uint8_t i = 0; i < name_len; i++)
        ntfs_wr16(a + name_at + i * 2, name[i]);
    ntfs_copy_bytes(a + runs_at, runs, runs_len);
    return a;
}

/* ============================================================ names in UTF-16
 *
 * Names arrive as UTF-8 and are stored as UTF-16.  Only the range this driver
 * can be sure it collates correctly is accepted - see up() above for why a
 * name sorted wrongly is a file that Windows cannot find.
 */
#define MAX_NAME 255

static bool to_utf16(const char *name, uint16_t *out, uint8_t *len_out) {
    uint32_t n = 0;
    const uint8_t *p = (const uint8_t *)name;

    while (*p) {
        uint32_t c;
        if (*p < 0x80) {
            c = *p++;
        } else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            c = (uint32_t)(*p & 0x1F) << 6 | (p[1] & 0x3F);
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 &&
                   (p[2] & 0xC0) == 0x80) {
            c = (uint32_t)(*p & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 |
                (p[2] & 0x3F);
            p += 3;
        } else {
            return false;                 /* four-byte forms need surrogates */
        }
        if (c > 0xFFFF || (c >= 0xD800 && c <= 0xDFFF)) return false;
        if (n >= MAX_NAME) return false;
        out[n++] = (uint16_t)c;
    }
    if (!n) return false;

    /* The characters Windows will not accept in a name.  Refused here rather
     * than written, because a volume carrying one of these is a volume some
     * Windows tools cannot repair. */
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = out[i];
        if (c < 0x20) return false;
        if (c == '"' || c == '*' || c == '/' || c == ':' || c == '<' ||
            c == '>' || c == '?' || c == '\\' || c == '|') return false;
    }

    *len_out = (uint8_t)n;
    return true;
}

/* Whether a name is already a legal 8.3 name in upper case.  When it is, the
 * file needs only one $FILE_NAME, in the namespace that means "this is both
 * the long name and the short one"; when it is not, it gets a long name alone,
 * which is what Windows itself does with 8.3 generation turned off. */
static bool is_dos_name(const uint16_t *name, uint8_t len) {
    uint8_t dot = 0xFF;
    for (uint8_t i = 0; i < len; i++) {
        uint16_t c = name[i];
        if (c == '.') {
            if (dot != 0xFF || i == 0) return false;
            dot = i;
            continue;
        }
        bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-' || c == '$' || c == '~';
        if (!ok) return false;
    }
    uint8_t stem = dot == 0xFF ? len : dot;
    uint8_t ext = dot == 0xFF ? 0 : (uint8_t)(len - dot - 1);
    return stem >= 1 && stem <= 8 && ext <= 3;
}

/* ------------------------------------------------------- the mirror and $MFT
 *
 * The first four records of the MFT are kept twice: once in $MFT and once in
 * $MFTMirr, which exists so that a volume whose MFT start is damaged can still
 * be repaired.  chkdsk compares them, so anything that changes one of those
 * four has to change both - a driver that writes only $MFT leaves a volume
 * Windows reports as damaged even though every structure in it is correct.
 *
 * Nothing else in this file touches records 0 to 3 except the two places that
 * have to: growing the MFT, and a volume small enough that $MFT's bitmap lives
 * inside record 0.
 */
#define NTFS_MIRRORED_RECORDS 4

static bool write_mirrored_record(ntfs_volume_t *v, uint64_t number,
                                  uint8_t *rec) {
    if (!write_mft_record(v, number, rec)) return false;
    if (number >= NTFS_MIRRORED_RECORDS) return true;

    /* The buffer now has its fixups in it, which is exactly what the mirror
     * should contain - the same bytes, in the same form. */
    static uint8_t mirror[4096];
    if (v->record_bytes > sizeof mirror) return true;
    if (!ntfs_read_mft_record(v, 1, mirror)) return true;   /* $MFTMirr */

    const uint8_t *a = ntfs_find_attribute(mirror, v->record_bytes,
                                           NTFS_ATTR_DATA, NULL, 0);
    if (!a || !a[8]) return true;

    uint16_t runs_at = ntfs_rd16(a + 0x20);
    uint32_t a_len = ntfs_rd32(a + 4);
    uint32_t runs_len = a_len > runs_at ? a_len - runs_at : 0;
    uint64_t size = ntfs_rd64(a + 0x30);

    long put = ntfs_write_runs(v, a + runs_at, runs_len, size,
                               number * v->record_bytes, rec, v->record_bytes);
    if (put != (long)v->record_bytes)
        ntfs_say(v, "the mirror of the volume's first records could not be "
                    "updated");
    return true;
}

/* Make the MFT bigger.
 *
 * The MFT is a file like any other and runs out of records like any other
 * file runs out of space.  Until this existed, a volume whose MFT was full
 * simply refused every further create - which on a fresh volume is a few
 * hundred files, enough to look like it works and not enough to hold a
 * system.
 *
 * Growing it is: more clusters for its data, empty records written into them
 * so that nothing reads uninitialised disk as a file, and its own bitmap
 * stretched to cover them.  The order matters here as everywhere - the
 * records are formatted before the bitmap admits they exist, so an
 * interruption leaves records nothing will hand out rather than handles onto
 * whatever those clusters held before.
 */
static bool grow_mft(ntfs_volume_t *v, uint64_t want_records) {
    static uint8_t rec[4096];
    static uint8_t blank[4096];
    if (v->record_bytes > sizeof rec) {
        ntfs_say(v, "this volume's MFT records are too large for this driver");
        return false;
    }
    if (!ntfs_read_mft_record(v, NTFS_MFT_RECORD, rec)) {
        ntfs_say(v, "$MFT's own record is not readable");
        return false;
    }

    uint8_t *data = find_attr_mut(rec, v->record_bytes, NTFS_ATTR_DATA, NULL, 0);
    uint8_t *bits = find_attr_mut(rec, v->record_bytes, NTFS_ATTR_BITMAP, NULL, 0);
    if (!data || !data[8] || !bits) {
        ntfs_say(v, "$MFT is not in a shape this driver can grow");
        return false;
    }

    uint64_t allocated = ntfs_rd64(data + 0x28);
    uint64_t have = allocated / v->record_bytes;
    if (want_records <= have) return true;

    /* Grow by a useful amount rather than by one record: each growth costs a
     * rewrite of $MFT's run list, and a run list that gains an extent per
     * file is one that stops fitting in the record. */
    uint64_t step = have / 8;
    if (step < 64) step = 64;
    uint64_t target = have + step;
    if (target < want_records) target = want_records;

    uint64_t cluster = v->bytes_per_cluster;
    uint64_t need_bytes = target * v->record_bytes;
    uint64_t need_clusters = (need_bytes + cluster - 1) / cluster;
    uint64_t have_clusters = allocated / cluster;

    uint16_t runs_at = ntfs_rd16(data + 0x20);
    uint32_t a_len = ntfs_rd32(data + 4);
    uint32_t runs_len = a_len > runs_at ? a_len - runs_at : 0;

    extent_t extents[MAX_EXTENTS];
    uint32_t extent_count = 0;
    if (!extents_of(data + runs_at, runs_len, extents, &extent_count)) {
        ntfs_say(v, "$MFT is in more pieces than this driver can rewrite");
        return false;
    }

    bitmap_t vol;
    if (!cluster_bitmap(v, &vol)) return false;
    uint64_t total = v->total_sectors / v->sectors_per_cluster;

    uint64_t need = need_clusters - have_clusters;
    uint64_t hint = extent_count ? extents[extent_count - 1].lcn +
                                   extents[extent_count - 1].count : v->mft_lcn;
    uint64_t first_new_cluster = have_clusters;

    while (need) {
        uint64_t at = 0;
        uint64_t took = bitmap_take(v, &vol, total, 0, hint, need, &at);
        if (!took) { ntfs_say(v, "no room on the volume to grow $MFT"); return false; }

        if (extent_count && extents[extent_count - 1].lcn +
                            extents[extent_count - 1].count == at) {
            extents[extent_count - 1].count += took;
        } else {
            if (extent_count >= MAX_EXTENTS) {
                ntfs_say(v, "$MFT would be in too many pieces");
                return false;
            }
            extents[extent_count].lcn = at;
            extents[extent_count].count = took;
            extent_count++;
        }
        need -= took;
        hint = at + took;
    }

    uint8_t runs[512];
    uint32_t new_runs_len = extents_encode(extents, extent_count, runs, sizeof runs);
    if (!new_runs_len) { ntfs_say(v, "$MFT's run list does not fit"); return false; }

    uint32_t new_total = align8(runs_at + new_runs_len);
    uint32_t old_total = ntfs_rd32(data + 4);
    if (new_total > old_total) {
        if (!make_room_after(v, rec, data, new_total - old_total)) {
            ntfs_say(v, "$MFT's record has no room for a longer run list");
            return false;
        }
    } else if (new_total < old_total) {
        take_room_after(v, rec, data, old_total - new_total);
    }
    ntfs_wr32(data + 0x04, new_total);
    ntfs_wr64(data + 0x18, need_clusters - 1);
    ntfs_wr64(data + 0x28, need_clusters * cluster);
    ntfs_wr64(data + 0x30, need_clusters * cluster);
    ntfs_wr64(data + 0x38, need_clusters * cluster);
    ntfs_copy_bytes(data + runs_at, runs, new_runs_len);

    /* The bitmap is a higher-numbered attribute than the data, so it sits
     * after it in the record and has just been moved by that resize.  The
     * pointer taken before it is now aimed at whatever followed. */
    bits = find_attr_mut(rec, v->record_bytes, NTFS_ATTR_BITMAP, NULL, 0);
    if (!bits) {
        ntfs_say(v, "$MFT's bitmap vanished while its data was being grown");
        return false;
    }

    /* Empty records into the new space, before anything can be handed out. */
    ntfs_zero_bytes(blank, v->record_bytes);
    uint16_t usa_count = (uint16_t)(v->record_bytes / v->bytes_per_sector + 1);
    uint32_t attrs_at = align8(0x30 + (uint32_t)usa_count * 2);
    blank[0] = 'F'; blank[1] = 'I'; blank[2] = 'L'; blank[3] = 'E';
    ntfs_wr16(blank + 0x04, 0x30);
    ntfs_wr16(blank + 0x06, usa_count);
    ntfs_wr16(blank + 0x10, 1);                    /* sequence */
    ntfs_wr16(blank + 0x14, (uint16_t)attrs_at);
    ntfs_wr16(blank + 0x16, 0);                    /* not in use */
    ntfs_wr32(blank + 0x18, attrs_at + 8);
    ntfs_wr32(blank + 0x1C, v->record_bytes);
    ntfs_wr32(blank + attrs_at, NTFS_ATTR_END);

    uint64_t first_new_record = first_new_cluster * cluster / v->record_bytes;
    for (uint64_t n = first_new_record; n < need_clusters * cluster / v->record_bytes; n++) {
        static uint8_t one[4096];
        ntfs_copy_bytes(one, blank, v->record_bytes);
        ntfs_wr32(one + 0x2C, (uint32_t)n);
        if (!embed_fixups(one, v->record_bytes, v->bytes_per_sector)) {
            ntfs_say(v, "a blank MFT record's fixup array is wrong");
            return false;
        }
        if (ntfs_write_runs(v, runs, new_runs_len, need_clusters * cluster,
                            n * v->record_bytes, one, v->record_bytes)
            != (long)v->record_bytes) {
            ntfs_say(v, "a new MFT record could not be written");
            return false;
        }
    }

    /* And the bitmap that says which of them are in use.  It only needs to be
     * long enough to hold a bit each; NTFS rounds it to eight bytes. */
    uint64_t records_now = need_clusters * cluster / v->record_bytes;
    uint64_t bitmap_bytes = (records_now + 7) / 8;
    bitmap_bytes = (bitmap_bytes + 7) & ~7ull;

    if (!bits[8]) {
        uint32_t was = ntfs_rd32(bits + 0x10);
        if (bitmap_bytes > was) {
            uint32_t extra = align8((uint32_t)bitmap_bytes) - align8(was);
            if (extra && !make_room_after(v, rec, bits, extra)) {
                ntfs_say(v, "$MFT's record has no room for a longer bitmap");
                return false;
            }
            ntfs_wr32(bits + 0x04, ntfs_rd32(bits + 0x04) + extra);
            ntfs_wr32(bits + 0x10, (uint32_t)bitmap_bytes);
            ntfs_zero_bytes(bits + ntfs_rd16(bits + 0x14) + was,
                            (uint32_t)bitmap_bytes - was);
        }
    } else {
        uint64_t was = ntfs_rd64(bits + 0x30);
        uint64_t room = ntfs_rd64(bits + 0x28);

        if (bitmap_bytes > room) {
            /* The bitmap has run out of the clusters it was given.  It is an
             * ordinary non-resident attribute, so it is extended the same way
             * everything else is - and it has to be, because it is one bit per
             * MFT record and the MFT is what just got bigger. */
            uint16_t br = ntfs_rd16(bits + 0x20);
            uint32_t bl = ntfs_rd32(bits + 4);
            uint32_t brl = bl > br ? bl - br : 0;

            extent_t be[MAX_EXTENTS];
            uint32_t bn = 0;
            if (!extents_of(bits + br, brl, be, &bn)) {
                ntfs_say(v, "$MFT's bitmap is in more pieces than this driver "
                            "can rewrite");
                return false;
            }

            uint64_t want_clusters = (bitmap_bytes + cluster - 1) / cluster;
            uint64_t have_bitmap = room / cluster;

            bitmap_t vol2;
            if (!cluster_bitmap(v, &vol2)) return false;
            uint64_t total2 = v->total_sectors / v->sectors_per_cluster;

            uint64_t want_more = want_clusters - have_bitmap;
            uint64_t bhint = bn ? be[bn - 1].lcn + be[bn - 1].count : v->mft_lcn;
            while (want_more) {
                uint64_t bat = 0;
                uint64_t got = bitmap_take(v, &vol2, total2, 0, bhint,
                                           want_more, &bat);
                if (!got) {
                    ntfs_say(v, "no room on the volume to extend $MFT's bitmap");
                    return false;
                }
                if (bn && be[bn - 1].lcn + be[bn - 1].count == bat) {
                    be[bn - 1].count += got;
                } else {
                    if (bn >= MAX_EXTENTS) {
                        ntfs_say(v, "$MFT's bitmap would be in too many pieces");
                        return false;
                    }
                    be[bn].lcn = bat; be[bn].count = got; bn++;
                }
                want_more -= got;
                bhint = bat + got;
            }

            uint8_t bruns[512];
            uint32_t brn = extents_encode(be, bn, bruns, sizeof bruns);
            if (!brn) {
                ntfs_say(v, "$MFT's bitmap run list does not fit");
                return false;
            }
            uint32_t bnew = align8(br + brn);
            uint32_t bold = ntfs_rd32(bits + 4);
            if (bnew > bold) {
                if (!make_room_after(v, rec, bits, bnew - bold)) {
                    ntfs_say(v, "no room in $MFT's record for a longer bitmap "
                                "run list");
                    return false;
                }
            } else if (bnew < bold) {
                take_room_after(v, rec, bits, bold - bnew);
            }
            ntfs_wr32(bits + 0x04, bnew);
            ntfs_wr64(bits + 0x18, want_clusters - 1);
            ntfs_wr64(bits + 0x28, want_clusters * cluster);
            ntfs_copy_bytes(bits + br, bruns, brn);
            room = want_clusters * cluster;
        }

        if (bitmap_bytes > was) {
            ntfs_wr64(bits + 0x30, bitmap_bytes);
            ntfs_wr64(bits + 0x38, bitmap_bytes);
            /* The bytes between the old end and the new one are whatever was
             * on the disk; every bit of them has to read as free. */
            static uint8_t zeros[512];
            ntfs_zero_bytes(zeros, sizeof zeros);
            uint16_t br = ntfs_rd16(bits + 0x20);
            uint32_t bl = ntfs_rd32(bits + 4);
            uint32_t brl = bl > br ? bl - br : 0;
            for (uint64_t off = was; off < bitmap_bytes; ) {
                uint32_t span = (uint32_t)(bitmap_bytes - off);
                if (span > sizeof zeros) span = sizeof zeros;
                ntfs_write_runs(v, bits + br, brl, bitmap_bytes, off, zeros, span);
                off += span;
            }
        }
    }

    if (!write_mirrored_record(v, NTFS_MFT_RECORD, rec)) return false;

    /* And the copy every record lookup goes through.  Until this line the MFT
     * is bigger on disk and unchanged as far as this driver is concerned, so
     * each record just added reads as being past the end of the file. */
    if (!ntfs_remember_mft_runs(v, runs, new_runs_len)) {
        ntfs_say(v, "$MFT grew but its new run list could not be cached, so "
                    "the records it gained are unreachable");
        return false;
    }
    return true;
}

/* ======================================================= allocating a record
 *
 * The MFT's own bitmap, one bit per record.  Records below 24 are the volume's
 * and are never handed out.
 *
 * This does not grow the MFT.  A volume whose MFT is full is refused - which
 * on any volume this system writes to is a very long way off, and is a clear
 * message rather than a silent misallocation into space the MFT does not own.
 */
static bool allocate_record(ntfs_volume_t *v, uint64_t *number_out) {
    static uint8_t rec[4096];
    if (v->record_bytes > sizeof rec) {
        ntfs_say(v, "this volume's MFT records are too large for this driver");
        return false;
    }
    if (!ntfs_read_mft_record(v, NTFS_MFT_RECORD, rec)) {
        ntfs_say(v, "$MFT's own record is not readable");
        return false;
    }

    const uint8_t *data = ntfs_find_attribute(rec, v->record_bytes,
                                              NTFS_ATTR_DATA, NULL, 0);
    const uint8_t *bits = ntfs_find_attribute(rec, v->record_bytes,
                                              NTFS_ATTR_BITMAP, NULL, 0);
    if (!data || !bits || !data[8]) {
        ntfs_say(v, "$MFT has no bitmap this driver understands");
        return false;
    }
    uint64_t records = ntfs_rd64(data + 0x28) / v->record_bytes;

    /* Only records the MFT already has space for.  Its allocated size, not its
     * data size: NTFS keeps the MFT larger than it is using precisely so that
     * a new record needs no allocation. */
    bitmap_t b;
    if (!bitmap_of_attribute(v, bits, &b)) {
        ntfs_say(v, "$MFT's bitmap is not in a form this driver reads");
        return false;
    }

    uint64_t at = 0;
    if (bitmap_take(v, &b, records, NTFS_FIRST_FREE_RECORD,
                    NTFS_FIRST_FREE_RECORD, 1, &at) != 1) {
        /* Full is not the end of it: the MFT is a file and can be made
         * bigger.  Once only - if there is still no room after growing, the
         * volume itself is out of space and saying so is the honest answer. */
        if (!grow_mft(v, records + 1)) return false;
        if (!ntfs_read_mft_record(v, NTFS_MFT_RECORD, rec)) {
            ntfs_say(v, "$MFT is not readable after being grown");
            return false;
        }
        data = ntfs_find_attribute(rec, v->record_bytes, NTFS_ATTR_DATA, NULL, 0);
        bits = ntfs_find_attribute(rec, v->record_bytes, NTFS_ATTR_BITMAP, NULL, 0);
        if (!data || !bits) {
            ntfs_say(v, "$MFT lost an attribute while being grown");
            return false;
        }
        records = ntfs_rd64(data + 0x28) / v->record_bytes;
        if (!bitmap_of_attribute(v, bits, &b)) {
            ntfs_say(v, "$MFT's bitmap is unreadable after being grown");
            return false;
        }
        if (bitmap_take(v, &b, records, NTFS_FIRST_FREE_RECORD,
                        NTFS_FIRST_FREE_RECORD, 1, &at) != 1) {
            ntfs_say(v, "the volume has no room for another file");
            return false;
        }
    }
    if (at < NTFS_FIRST_FREE_RECORD) {
        ntfs_say(v, "the MFT bitmap offered a reserved record");
        return false;
    }

    /* A resident MFT bitmap lives inside record 0 and has to go back with it.
     * Small volumes are the only ones that have one. */
    if (b.resident) {
        uint8_t *m = find_attr_mut(rec, v->record_bytes, NTFS_ATTR_BITMAP,
                                   NULL, 0);
        if (!m) {
            ntfs_say(v, "$MFT's bitmap vanished between two reads");
            return false;
        }
        uint32_t len = ntfs_rd32(m + 0x10);
        ntfs_copy_bytes(m + ntfs_rd16(m + 0x14), b.data, len);
        if (!write_mirrored_record(v, NTFS_MFT_RECORD, rec)) return false;
    }

    *number_out = at;
    return true;
}

/* Give a record back, when the create that took it could not be finished.
 *
 * The order is the reverse of taking it, and for the same reason: the record
 * is marked not-in-use first, so that an interruption between the two leaves a
 * free record whose bit is still set - one wasted slot - rather than a record
 * the bitmap says is free while its own header says it is in use, which is the
 * inconsistency chkdsk has to repair.
 */
static void release_record(ntfs_volume_t *v, uint64_t number) {
    static uint8_t rec[4096];
    if (v->record_bytes > sizeof rec) return;

    if (ntfs_read_mft_record(v, number, rec)) {
        ntfs_wr16(rec + 0x16, (uint16_t)(ntfs_rd16(rec + 0x16) &
                                         ~(uint16_t)NTFS_RECORD_IN_USE));
        /* The sequence number moves on now, which is what makes any reference
         * left pointing at this record recognisable as stale. */
        uint16_t s = (uint16_t)(ntfs_rd16(rec + 0x10) + 1);
        ntfs_wr16(rec + 0x10, s ? s : 1);
        write_mft_record(v, number, rec);
    }

    static uint8_t mft[4096];
    if (!ntfs_read_mft_record(v, NTFS_MFT_RECORD, mft)) return;
    const uint8_t *bits = ntfs_find_attribute(mft, v->record_bytes,
                                              NTFS_ATTR_BITMAP, NULL, 0);
    if (!bits) return;
    bitmap_t b;
    if (!bitmap_of_attribute(v, bits, &b)) return;
    if (b.resident) return;            /* would need record 0 written back */
    bitmap_give_back(v, &b, number, 1);
}

/* ========================================================== the FILE record */

/* The sequence number a record keeps when it is handed out again, and the
 * fixup counter that goes with it.
 *
 * The obvious guess is that allocating a record bumps its sequence number.  It
 * does not: NTFS bumps it when a record is FREED, so by the time it is handed
 * out the number already differs from the one any stale reference is carrying,
 * and bumping it again here would just skip a value.  This follows the
 * reference implementation and keeps what is there.
 *
 * The fixup counter is kept for a different reason.  It is what tells a torn
 * record from an intact one, and starting it over at 1 in a record that has
 * been used before means a half-written sector left from the record's previous
 * life can carry the value the new one is about to use.
 */
static void reuse_identity(ntfs_volume_t *v, uint64_t number,
                           uint16_t *sequence_out, uint16_t *usn_out) {
    static uint8_t old[4096];
    *sequence_out = 1;
    *usn_out = 0;
    if (v->record_bytes > sizeof old) return;

    long got = ntfs_read_runs(v, v->mft_runs, v->mft_runs_bytes, (uint64_t)-1,
                              number * v->record_bytes, old, v->record_bytes);
    if (got != (long)v->record_bytes) return;
    if (old[0] != 'F' || old[1] != 'I' || old[2] != 'L' || old[3] != 'E')
        return;

    uint16_t s = ntfs_rd16(old + 0x10);
    if (s) *sequence_out = s;

    uint16_t usa_off = ntfs_rd16(old + 4);
    if (usa_off >= 8 && usa_off + 2 <= v->record_bytes) {
        uint16_t usn = ntfs_rd16(old + usa_off);
        if (usn && usn != 0xFFFF) *usn_out = usn;
    }
}

typedef struct {
    uint64_t parent;              /* the parent's full reference             */
    uint16_t name[MAX_NAME];
    uint8_t  name_len;
    bool     directory;
    uint32_t attributes;
    uint32_t security_id;
    uint64_t time;
} new_file_t;

/* $STANDARD_INFORMATION, in the 72-byte form every NTFS since 3.0 uses. */
static void build_standard_info(uint8_t *out, const new_file_t *n) {
    ntfs_zero_bytes(out, 72);
    ntfs_wr64(out + 0x00, n->time);
    ntfs_wr64(out + 0x08, n->time);
    ntfs_wr64(out + 0x10, n->time);
    ntfs_wr64(out + 0x18, n->time);
    ntfs_wr32(out + 0x20, n->attributes);
    ntfs_wr32(out + 0x34, n->security_id);
}

/* $FILE_NAME.  The same bytes go in the record and in the parent's index -
 * they are one structure stored twice, and chkdsk compares them. */
static uint32_t build_file_name(uint8_t *out, const new_file_t *n,
                                uint64_t allocated, uint64_t size) {
    uint32_t len = 0x42 + (uint32_t)n->name_len * 2;
    ntfs_zero_bytes(out, len);
    ntfs_wr64(out + 0x00, n->parent);
    ntfs_wr64(out + 0x08, n->time);
    ntfs_wr64(out + 0x10, n->time);
    ntfs_wr64(out + 0x18, n->time);
    ntfs_wr64(out + 0x20, n->time);
    ntfs_wr64(out + 0x28, allocated);
    ntfs_wr64(out + 0x30, size);
    ntfs_wr32(out + 0x38, n->attributes);
    out[0x40] = n->name_len;
    out[0x41] = is_dos_name(n->name, n->name_len) ? 3 : 1;
    for (uint8_t i = 0; i < n->name_len; i++)
        ntfs_wr16(out + 0x42 + i * 2, n->name[i]);
    return len;
}

/* An empty directory index: a header and nothing but the end marker. */
static uint32_t build_index_root(ntfs_volume_t *v, uint8_t *out) {
    uint32_t block = 4096;
    if (block < v->bytes_per_cluster) block = v->bytes_per_cluster;

    ntfs_zero_bytes(out, 0x30);
    ntfs_wr32(out + 0x00, NTFS_ATTR_FILE_NAME);   /* indexed attribute type */
    ntfs_wr32(out + 0x04, 1);                     /* collate by file name   */
    ntfs_wr32(out + 0x08, block);
    out[0x0C] = (uint8_t)(block >= v->bytes_per_cluster
                          ? block / v->bytes_per_cluster
                          : 0);

    /* The header, whose offsets are relative to itself. */
    ntfs_wr32(out + 0x10, 0x10);                  /* entries start here     */
    ntfs_wr32(out + 0x14, 0x20);                  /* used                   */
    ntfs_wr32(out + 0x18, 0x20);                  /* allocated              */
    out[0x1C] = 0;                                /* small: no allocation   */

    /* The end marker, which is an entry with no key. */
    ntfs_wr16(out + 0x20 + 0x08, 0x10);           /* length                 */
    ntfs_wr16(out + 0x20 + 0x0A, 0);              /* key length             */
    ntfs_wr16(out + 0x20 + 0x0C, NTFS_INDEX_LAST);
    return 0x30;
}

/* ============================================================ the index tree
 *
 * Inserting into a directory means finding the leaf the name belongs in and
 * putting an entry there in sorted order.  The tree is walked the way Windows
 * walks it, because a name inserted into the wrong node is a name its binary
 * search will never reach.
 */
static const uint16_t I30[4] = { '$', 'I', '3', '0' };

typedef struct {
    ntfs_volume_t *v;
    uint8_t  parent_rec[4096];
    uint64_t parent_number;

    /* The $INDEX_ALLOCATION attribute's runs, when the directory has one. */
    bool     has_allocation;
    uint8_t  alloc_runs[512];
    uint32_t alloc_runs_bytes;
    uint64_t alloc_size;

    uint32_t block_bytes;
    uint32_t vcn_shift;           /* position = vcn << this                 */
} index_t;

static bool index_open(ntfs_volume_t *v, uint64_t dir_record, index_t *ix) {
    ntfs_zero_bytes(ix, sizeof *ix);
    ix->v = v;
    ix->parent_number = dir_record;
    if (v->record_bytes > sizeof ix->parent_rec) {
        ntfs_say(v, "this volume's MFT records are larger than this driver "
                    "can hold");
        return false;
    }
    if (!ntfs_read_mft_record(v, dir_record, ix->parent_rec)) {
        ntfs_say(v, "the directory's record is not readable");
        return false;
    }

    const uint8_t *root = ntfs_find_attribute(ix->parent_rec, v->record_bytes,
                                              NTFS_ATTR_INDEX_ROOT, I30, 4);
    if (!root || root[8]) {
        ntfs_say(v, "the directory has no index this driver understands");
        return false;
    }
    const uint8_t *value = root + ntfs_rd16(root + 0x14);
    ix->block_bytes = ntfs_rd32(value + 0x08);
    if (!ix->block_bytes || ix->block_bytes > 65536) {
        ntfs_say(v, "the directory's index blocks are an unusable size");
        return false;
    }

    /* Index blocks are addressed in clusters when they are at least a cluster
     * long, and in 512-byte units when they are smaller.  Getting this the
     * wrong way round reads the wrong block, which looks like a corrupt
     * directory rather than like an addressing mistake. */
    uint32_t unit = ix->block_bytes >= v->bytes_per_cluster
                    ? v->bytes_per_cluster : 512;
    ix->vcn_shift = 0;
    while ((1u << ix->vcn_shift) < unit) ix->vcn_shift++;

    const uint8_t *alloc = ntfs_find_attribute(ix->parent_rec, v->record_bytes,
                                               NTFS_ATTR_INDEX_ALLOC, I30, 4);
    if (alloc && alloc[8]) {
        ix->has_allocation = true;
        ix->alloc_size = ntfs_rd64(alloc + 0x30);
        uint16_t runs_at = ntfs_rd16(alloc + 0x20);
        uint32_t a_len = ntfs_rd32(alloc + 4);
        uint32_t runs_len = a_len > runs_at ? a_len - runs_at : 0;
        if (runs_len > sizeof ix->alloc_runs) {
            ntfs_say(v, "the directory's index is in too many pieces");
            return false;
        }
        ntfs_copy_bytes(ix->alloc_runs, alloc + runs_at, runs_len);
        ix->alloc_runs_bytes = runs_len;
    }
    return true;
}

static bool index_read_block(index_t *ix, uint64_t vcn, uint8_t *out) {
    uint64_t at = vcn << ix->vcn_shift;
    long got = ntfs_read_runs(ix->v, ix->alloc_runs, ix->alloc_runs_bytes,
                              ix->alloc_size, at, out, ix->block_bytes);
    if (got != (long)ix->block_bytes) return false;
    if (out[0] != 'I' || out[1] != 'N' || out[2] != 'D' || out[3] != 'X')
        return false;
    return ntfs_apply_fixups(out, ix->block_bytes, ix->v->bytes_per_sector);
}

static bool index_write_block(index_t *ix, uint64_t vcn, uint8_t *block) {
    if (!embed_fixups(block, ix->block_bytes, ix->v->bytes_per_sector)) {
        ntfs_say(ix->v, "an index block's fixup array is wrong");
        return false;
    }
    uint64_t at = vcn << ix->vcn_shift;
    long put = ntfs_write_runs(ix->v, ix->alloc_runs, ix->alloc_runs_bytes,
                               ix->alloc_size, at, block, ix->block_bytes);
    if (put != (long)ix->block_bytes) {
        ntfs_say(ix->v, "an index block could not be written back");
        return false;
    }
    return true;
}

/* ============================================================ the index tree
 *
 * A directory's index is a B-tree whose root lives inside the directory's own
 * MFT record and whose other nodes are blocks on the disk.  Everything below
 * exists to keep that tree in the exact shape Windows expects, because Windows
 * finds a name by binary search through it - an entry in the wrong node is a
 * file that is on the disk, intact, and invisible.
 *
 * The sizes involved are why the tree has to be a real one.  The root is
 * bounded by the MFT record it sits in, which is a thousand bytes, so it holds
 * about six names.  A block is four thousand and holds about thirty five.
 * Neither is a directory.  What makes a directory possible is that a full node
 * splits and hands the entry between its halves to its parent, and that when
 * the root itself is full the whole tree gains a level.
 *
 * Both of those are here.  Together they mean the only limit on how many files
 * a directory holds is the disk.
 */

#define MAX_DEPTH 16

/* Where the descent went: one of these per level, from the root down. */
typedef struct {
    bool     is_root;
    uint64_t vcn;                 /* the block, when it is not the root      */
    uint32_t pos;                 /* the entry we went through, or insert at */
} step_t;

/* ------------------------------------------------------------ another block
 *
 * A directory's index blocks are numbered, and which numbers are in use is
 * kept in a $BITMAP attribute beside them - the same arrangement as $MFT and
 * its records, one level down.  Finding a free one may mean giving the
 * allocation more clusters, and may mean making the bitmap longer.
 */
static bool index_block_alloc(index_t *ix, uint64_t *vcn_out) {
    ntfs_volume_t *v = ix->v;
    uint8_t *rec = ix->parent_rec;

    uint8_t *bm = find_attr_mut(rec, v->record_bytes, NTFS_ATTR_BITMAP, I30, 4);
    if (!bm || bm[8]) {
        ntfs_say(v, "the directory's index bitmap is not where this driver "
                    "looks for it");
        return false;
    }
    uint32_t bm_len = ntfs_rd32(bm + 0x10);
    uint8_t *bits = bm + ntfs_rd16(bm + 0x14);

    uint32_t index = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < bm_len * 8; i++) {
        if (!(bits[i / 8] & (1u << (i & 7)))) { index = i; break; }
    }

    if (index == 0xFFFFFFFFu) {
        /* Every bit taken, so the bitmap grows.  Eight bytes at a time, which
         * is the unit NTFS keeps it in. */
        uint32_t was = bm_len, now = bm_len + 8;
        uint32_t name_bytes = (uint32_t)4 * 2;
        uint32_t extra = align8(align8(0x18 + name_bytes) + now) -
                         align8(align8(0x18 + name_bytes) + was);
        if (extra) {
            if (!make_room_after(v, rec, bm, extra)) {
                ntfs_say(v, "no room in the directory's record for a longer "
                            "index bitmap");
                return false;
            }
            ntfs_wr32(bm + 0x04, ntfs_rd32(bm + 0x04) + extra);
        }
        bits = bm + ntfs_rd16(bm + 0x14);
        ntfs_zero_bytes(bits + was, now - was);
        ntfs_wr32(bm + 0x10, now);
        index = was * 8;
    }

    uint64_t position = (uint64_t)index * ix->block_bytes;

    uint8_t *ia = find_attr_mut(rec, v->record_bytes, NTFS_ATTR_INDEX_ALLOC,
                                I30, 4);
    if (!ia || !ia[8]) {
        ntfs_say(v, "the directory has no index allocation to extend");
        return false;
    }

    uint64_t allocated = ntfs_rd64(ia + 0x28);
    if (position + ix->block_bytes > allocated) {
        uint16_t runs_at = ntfs_rd16(ia + 0x20);
        uint32_t a_len = ntfs_rd32(ia + 4);
        uint32_t runs_len = a_len > runs_at ? a_len - runs_at : 0;

        extent_t extents[MAX_EXTENTS];
        uint32_t extent_count = 0;
        if (!extents_of(ia + runs_at, runs_len, extents, &extent_count)) {
            ntfs_say(v, "the directory's index is in more pieces than this "
                        "driver can rewrite");
            return false;
        }

        uint64_t cluster = v->bytes_per_cluster;
        uint64_t want = (position + ix->block_bytes + cluster - 1) / cluster;
        uint64_t have = allocated / cluster;

        bitmap_t vol;
        if (!cluster_bitmap(v, &vol)) return false;
        uint64_t total = v->total_sectors / v->sectors_per_cluster;

        uint64_t need = want - have;
        uint64_t hint = extent_count ? extents[extent_count - 1].lcn +
                                       extents[extent_count - 1].count
                                     : v->mft_lcn;
        while (need) {
            uint64_t at = 0;
            uint64_t took = bitmap_take(v, &vol, total, 0, hint, need, &at);
            if (!took) {
                ntfs_say(v, "no room on the volume for another index block");
                return false;
            }
            if (extent_count && extents[extent_count - 1].lcn +
                                extents[extent_count - 1].count == at) {
                extents[extent_count - 1].count += took;
            } else {
                if (extent_count >= MAX_EXTENTS) {
                    ntfs_say(v, "the directory's index would be in too many "
                                "pieces");
                    return false;
                }
                extents[extent_count].lcn = at;
                extents[extent_count].count = took;
                extent_count++;
            }
            need -= took;
            hint = at + took;
        }

        uint8_t runs[512];
        uint32_t new_runs_len = extents_encode(extents, extent_count, runs,
                                               sizeof runs);
        if (!new_runs_len) {
            ntfs_say(v, "the directory's index run list does not fit");
            return false;
        }
        uint32_t new_total = align8(runs_at + new_runs_len);
        uint32_t old_total = ntfs_rd32(ia + 4);
        if (new_total > old_total) {
            if (!make_room_after(v, rec, ia, new_total - old_total)) {
                ntfs_say(v, "no room in the directory's record for a longer "
                            "index run list");
                return false;
            }
        } else if (new_total < old_total) {
            take_room_after(v, rec, ia, old_total - new_total);
        }
        ntfs_wr32(ia + 0x04, new_total);
        ntfs_wr64(ia + 0x18, want - 1);
        ntfs_wr64(ia + 0x28, want * cluster);
        ntfs_wr64(ia + 0x30, want * cluster);
        ntfs_wr64(ia + 0x38, want * cluster);
        ntfs_copy_bytes(ia + runs_at, runs, new_runs_len);

        /* The bitmap sits after the allocation in the record, so it has just
         * moved. */
        bm = find_attr_mut(rec, v->record_bytes, NTFS_ATTR_BITMAP, I30, 4);
        if (!bm) return false;
        bits = bm + ntfs_rd16(bm + 0x14);
    }

    bits[index / 8] |= (uint8_t)(1u << (index & 7));

    if (!write_mft_record(v, ix->parent_number, rec)) return false;
    if (!index_open(v, ix->parent_number, ix)) return false;

    *vcn_out = position >> ix->vcn_shift;
    return true;
}

/* An empty index block, ready to take entries.  Returns where they start. */
static uint32_t index_block_init(index_t *ix, uint8_t *out, uint64_t vcn,
                                 uint8_t node_flags) {
    ntfs_volume_t *v = ix->v;
    uint16_t usa_count = (uint16_t)(ix->block_bytes / v->bytes_per_sector + 1);
    uint32_t first = align8(0x10 + (uint32_t)usa_count * 2);

    ntfs_zero_bytes(out, ix->block_bytes);
    out[0] = 'I'; out[1] = 'N'; out[2] = 'D'; out[3] = 'X';
    ntfs_wr16(out + 0x04, 0x28);       /* the array follows the block header */
    ntfs_wr16(out + 0x06, usa_count);
    ntfs_wr16(out + 0x28, 1);          /* first fixup value */
    ntfs_wr64(out + 0x08, 0);
    ntfs_wr64(out + 0x10, vcn);

    uint8_t *h = out + 0x18;
    ntfs_wr32(h + 0x00, first);
    ntfs_wr32(h + 0x04, first);
    ntfs_wr32(h + 0x08, ix->block_bytes - 0x18);
    h[0x0C] = node_flags;
    return first;
}

/* ------------------------------------------------------------- the root
 *
 * The root is a resident attribute, so making it longer means making room in
 * the MFT record and moving every attribute after it.  Returns false when the
 * record cannot take it - which is not an error, it is the signal that the
 * tree needs another level.
 */
static bool root_reserve(index_t *ix, uint32_t index_len) {
    ntfs_volume_t *v = ix->v;
    uint8_t *root = find_attr_mut(ix->parent_rec, v->record_bytes,
                                  NTFS_ATTR_INDEX_ROOT, I30, 4);
    if (!root) return false;

    uint32_t value_at = ntfs_rd16(root + 0x14);
    uint32_t new_value_len = 0x10 + index_len;
    uint32_t new_attr_len = align8(value_at + new_value_len);
    uint32_t old_attr_len = ntfs_rd32(root + 4);

    if (new_attr_len > old_attr_len) {
        if (!make_room_after(v, ix->parent_rec, root,
                             new_attr_len - old_attr_len))
            return false;
        ntfs_wr32(root + 0x04, new_attr_len);
    }
    ntfs_wr32(root + 0x10, new_value_len);
    return true;
}

/* Whether a node could take `bytes` more, without changing anything. */
static bool node_has_room(index_t *ix, const step_t *st, uint32_t bytes,
                          uint8_t *scratch) {
    ntfs_volume_t *v = ix->v;

    if (st->is_root) {
        uint8_t *root = find_attr_mut(ix->parent_rec, v->record_bytes,
                                      NTFS_ATTR_INDEX_ROOT, I30, 4);
        if (!root) return false;
        uint32_t value_at = ntfs_rd16(root + 0x14);
        uint8_t *h = root + value_at + 0x10;
        uint32_t want = align8(value_at + 0x10 + ntfs_rd32(h + 0x04) + bytes);
        uint32_t have = ntfs_rd32(root + 4);
        uint32_t grow = want > have ? want - have : 0;
        return ntfs_rd32(ix->parent_rec + 0x18) + grow <= v->record_bytes;
    }

    if (!index_read_block(ix, st->vcn, scratch)) return false;
    uint8_t *h = scratch + 0x18;
    return ntfs_rd32(h + 0x04) + bytes <= ntfs_rd32(h + 0x08);
}

/* ---------------------------------------------------------------- deepening
 *
 * Move everything the root holds into a block of its own, and leave the root
 * with a single entry pointing at it.  The tree gains a level and the root is
 * empty again.
 *
 * The first time this happens the directory has no blocks at all, so the
 * allocation and its bitmap are created here.  Every time after that it is
 * one more block like the others.  Both cases have to leave the root
 * followable: a root that says it has a child and does not is a directory
 * that reads as empty.
 */
static bool deepen_index(index_t *ix) {
    ntfs_volume_t *v = ix->v;
    static uint8_t block[65536];
    static uint8_t saved[65536];

    uint8_t *rec = ix->parent_rec;
    uint8_t *root = find_attr_mut(rec, v->record_bytes,
                                  NTFS_ATTR_INDEX_ROOT, I30, 4);
    if (!root) { ntfs_say(v, "the directory has lost its index root"); return false; }

    uint32_t value_at = ntfs_rd16(root + 0x14);
    uint8_t *value = root + value_at;
    uint8_t *h = value + 0x10;

    uint32_t block_bytes = ntfs_rd32(value + 0x08);
    if (!block_bytes || block_bytes > sizeof block) {
        ntfs_say(v, "the directory's index block size is unusable");
        return false;
    }

    uint32_t first = ntfs_rd32(h + 0x00);
    uint32_t used = ntfs_rd32(h + 0x04);
    if (used < first) {
        ntfs_say(v, "the directory's index header contradicts itself");
        return false;
    }
    uint32_t entries_len = used - first;
    uint8_t node_flags = h[0x0C];      /* whether these entries have children */

    if (entries_len > sizeof saved) {
        ntfs_say(v, "the directory's index root is larger than this driver "
                    "can move");
        return false;
    }
    ntfs_copy_bytes(saved, h + first, entries_len);

    bool fresh = !ix->has_allocation;
    uint64_t new_vcn = 0;
    uint64_t lcn = 0, clusters = 0;

    if (fresh) {
        /* No blocks yet: take clusters directly, and add the two attributes
         * that describe them once the root has shrunk and made the room. */
        clusters = block_bytes / v->bytes_per_cluster;
        if (!clusters) clusters = 1;

        bitmap_t vol;
        if (!cluster_bitmap(v, &vol)) return false;
        uint64_t total = v->total_sectors / v->sectors_per_cluster;
        if (bitmap_take(v, &vol, total, 0, v->mft_lcn, clusters, &lcn) != clusters) {
            ntfs_say(v, "no room on the volume for an index block");
            return false;
        }
    } else {
        if (!index_block_alloc(ix, &new_vcn)) return false;
        /* The record was rewritten and re-read, so everything is re-found. */
        rec = ix->parent_rec;
        root = find_attr_mut(rec, v->record_bytes, NTFS_ATTR_INDEX_ROOT, I30, 4);
        if (!root) return false;
        value_at = ntfs_rd16(root + 0x14);
        h = root + value_at + 0x10;
    }

    /* The block that now holds what the root held. */
    uint16_t usa_count = (uint16_t)(block_bytes / v->bytes_per_sector + 1);
    uint32_t block_first = align8(0x10 + (uint32_t)usa_count * 2);
    if (0x18 + block_first + entries_len > block_bytes) {
        ntfs_say(v, "the directory's entries do not fit in one index block");
        return false;
    }

    ntfs_zero_bytes(block, block_bytes);
    block[0] = 'I'; block[1] = 'N'; block[2] = 'D'; block[3] = 'X';
    ntfs_wr16(block + 0x04, 0x28);
    ntfs_wr16(block + 0x06, usa_count);
    ntfs_wr16(block + 0x28, 1);
    ntfs_wr64(block + 0x08, 0);
    ntfs_wr64(block + 0x10, new_vcn);

    uint8_t *bh = block + 0x18;
    ntfs_wr32(bh + 0x00, block_first);
    ntfs_wr32(bh + 0x04, block_first + entries_len);
    ntfs_wr32(bh + 0x08, block_bytes - 0x18);
    bh[0x0C] = node_flags;             /* the entries keep their children */
    ntfs_copy_bytes(bh + block_first, saved, entries_len);

    if (fresh) {
        if (!embed_fixups(block, block_bytes, v->bytes_per_sector)) {
            ntfs_say(v, "the new index block's fixup array is wrong");
            return false;
        }
        if (!ntfs_write_clusters(v, lcn, (uint32_t)clusters, block)) {
            ntfs_say(v, "the new index block could not be written");
            return false;
        }
    } else {
        if (!index_write_block(ix, new_vcn, block)) return false;
    }

    /* ---- the root becomes a single pointer ------------------------------ */
    uint32_t node_len = 0x18;
    uint32_t new_index_len = first + node_len;
    uint32_t new_value_len = 0x10 + new_index_len;
    uint32_t new_attr_len = align8(value_at + new_value_len);
    uint32_t old_attr_len = ntfs_rd32(root + 4);

    uint8_t *e = h + first;
    ntfs_zero_bytes(e, node_len);
    ntfs_wr16(e + 0x08, (uint16_t)node_len);
    ntfs_wr16(e + 0x0A, 0);
    ntfs_wr16(e + 0x0C, NTFS_INDEX_LAST | NTFS_INDEX_HAS_SUBNODE);
    ntfs_wr64(e + node_len - 8, new_vcn);

    ntfs_wr32(h + 0x04, new_index_len);
    ntfs_wr32(h + 0x08, new_index_len);
    h[0x0C] = 1;                       /* large: the entries are in blocks */

    if (new_attr_len < old_attr_len)
        take_room_after(v, rec, root, old_attr_len - new_attr_len);
    ntfs_wr32(root + 0x04, new_attr_len);
    ntfs_wr32(root + 0x10, new_value_len);

    if (fresh) {
        uint8_t runs[32];
        extent_t one = { lcn, clusters };
        uint32_t runs_len = extents_encode(&one, 1, runs, sizeof runs);
        if (!runs_len) {
            ntfs_say(v, "the index block's run list does not fit");
            return false;
        }
        if (!add_named_nonresident_attr(v, rec, NTFS_ATTR_INDEX_ALLOC, I30, 4,
                                        runs, runs_len,
                                        clusters * v->bytes_per_cluster,
                                        block_bytes))
            return false;

        uint8_t bmp[8];
        ntfs_zero_bytes(bmp, sizeof bmp);
        bmp[0] = 1;                    /* block zero is in use */
        if (!find_attr_mut(rec, v->record_bytes, NTFS_ATTR_BITMAP, I30, 4)) {
            if (!add_named_resident_attr(v, rec, NTFS_ATTR_BITMAP, I30, 4,
                                         bmp, sizeof bmp))
                return false;
        }
    }

    if (!write_mft_record(v, ix->parent_number, rec)) return false;
    return index_open(v, ix->parent_number, ix);
}

/* Put an entry into a node at a known position.
 *
 * The order here is the reference implementation's: the length is raised
 * first and the move is measured from the new length, which comes to the same
 * as moving the old tail.  Room must already exist.
 */
static void node_insert_at(uint8_t *header, uint32_t at, const uint8_t *entry,
                           uint32_t entry_len) {
    uint32_t used = ntfs_rd32(header + 0x04);
    move_bytes(header + at + entry_len, header + at, used - at);
    ntfs_copy_bytes(header + at, entry, entry_len);
    ntfs_wr32(header + 0x04, used + entry_len);
}

/* ------------------------------------------------------------------ splitting */

enum { SPLIT_OK, SPLIT_PARENT_FULL, SPLIT_ERROR };

/* Split the block at stack[i] in two and hand the entry between the halves to
 * stack[i-1].
 *
 * The halves are arranged the way NTFS expects: the entries below the middle
 * one stay where they are, the entries above it move to a new block, and the
 * middle entry moves up into the parent as the separator between them.  The
 * parent's own entry, which used to point here, is left pointing at the new
 * block - it sorts after everything, and the new block holds the larger names.
 *
 * The promotion happens BEFORE the old block is cut down.  The middle entry is
 * the only copy of that name, so cutting first and promoting second means a
 * promotion that fails takes the name with it - and every other structure
 * stays perfectly consistent, so nothing reports it.  That is not a
 * hypothetical: it is what this did, and one file in a hundred quietly
 * vanished.
 */
static int split_node(index_t *ix, step_t *stack, int i) {
    ntfs_volume_t *v = ix->v;
    static uint8_t block[65536];
    static uint8_t fresh[65536];
    static uint8_t parent[65536];

    if (i < 1 || stack[i].is_root) return SPLIT_ERROR;
    if (ix->block_bytes > sizeof block) {
        ntfs_say(v, "the directory's index blocks are larger than this driver "
                    "can hold");
        return SPLIT_ERROR;
    }
    if (!index_read_block(ix, stack[i].vcn, block)) {
        ntfs_say(v, "an index block is not readable");
        return SPLIT_ERROR;
    }

    uint8_t *h = block + 0x18;
    uint32_t first = ntfs_rd32(h + 0x00);
    uint32_t used = ntfs_rd32(h + 0x04);

    uint32_t count = 0;
    for (uint32_t at = first; at < used; ) {
        uint8_t *e = h + at;
        if (ntfs_rd16(e + 0x0C) & NTFS_INDEX_LAST) break;
        uint16_t len = ntfs_rd16(e + 0x08);
        if (!len) { ntfs_say(v, "an index entry is zero bytes long"); return SPLIT_ERROR; }
        at += len;
        count++;
    }
    if (count < 2) {
        ntfs_say(v, "an index block too full for one more entry holds fewer "
                    "than two, so it cannot be split");
        return SPLIT_ERROR;
    }

    uint32_t median_at = first;
    for (uint32_t k = 0; k < count / 2; k++)
        median_at += ntfs_rd16(h + median_at + 0x08);
    uint8_t *median = h + median_at;
    uint16_t median_len = ntfs_rd16(median + 0x08);

    uint32_t tail_at = median_at + median_len;
    if (tail_at > used) {
        ntfs_say(v, "the index block's entries overrun it");
        return SPLIT_ERROR;
    }
    uint32_t tail_len = used - tail_at;

    static uint8_t keep[0x10 + 0x42 + MAX_NAME * 2 + 8];
    if ((uint32_t)median_len + 8 > sizeof keep) {
        ntfs_say(v, "an index entry is longer than this driver can move");
        return SPLIT_ERROR;
    }
    ntfs_copy_bytes(keep, median, median_len);
    bool had_child = (ntfs_rd16(keep + 0x0C) & NTFS_INDEX_HAS_SUBNODE) != 0;
    uint64_t median_child = had_child ? ntfs_rd64(keep + median_len - 8) : 0;

    uint16_t up_len = (uint16_t)align8(had_child ? median_len
                                                 : (uint32_t)median_len + 8);

    /* Asked before a single byte is written, because every answer after this
     * point leaves the directory half split. */
    if (!node_has_room(ix, &stack[i - 1], up_len, parent))
        return SPLIT_PARENT_FULL;

    /* ---- the new block, holding the upper half ------------------------- */
    uint64_t new_vcn = 0;
    if (!index_block_alloc(ix, &new_vcn)) return SPLIT_ERROR;

    uint32_t new_first = index_block_init(ix, fresh, new_vcn, h[0x0C]);
    if (new_first + tail_len > ix->block_bytes - 0x18) {
        ntfs_say(v, "the upper half of a split index block does not fit");
        return SPLIT_ERROR;
    }
    ntfs_copy_bytes(fresh + 0x18 + new_first, h + tail_at, tail_len);
    ntfs_wr32(fresh + 0x18 + 0x04, new_first + tail_len);
    if (!index_write_block(ix, new_vcn, fresh)) return SPLIT_ERROR;

    /* ---- the middle entry moves up ------------------------------------- */
    ntfs_zero_bytes(keep + median_len, up_len - median_len);
    ntfs_wr16(keep + 0x08, up_len);
    ntfs_wr16(keep + 0x0C, (uint16_t)(ntfs_rd16(keep + 0x0C) |
                                      NTFS_INDEX_HAS_SUBNODE));
    ntfs_wr64(keep + up_len - 8, stack[i].vcn);

    if (stack[i - 1].is_root) {
        uint8_t *root = find_attr_mut(ix->parent_rec, v->record_bytes,
                                      NTFS_ATTR_INDEX_ROOT, I30, 4);
        if (!root) { ntfs_say(v, "the directory lost its index root"); return SPLIT_ERROR; }
        uint32_t value_at = ntfs_rd16(root + 0x14);
        uint8_t *rh = root + value_at + 0x10;
        uint32_t r_used = ntfs_rd32(rh + 0x04);
        if (stack[i - 1].pos >= r_used) {
            ntfs_say(v, "the index root entry for this block has moved");
            return SPLIT_ERROR;
        }
        if (!root_reserve(ix, r_used + up_len)) return SPLIT_PARENT_FULL;

        uint8_t *at_entry = rh + stack[i - 1].pos;
        uint16_t at_len = ntfs_rd16(at_entry + 0x08);
        if (ntfs_rd16(at_entry + 0x0C) & NTFS_INDEX_HAS_SUBNODE)
            ntfs_wr64(at_entry + at_len - 8, new_vcn);

        node_insert_at(rh, stack[i - 1].pos, keep, up_len);
        ntfs_wr32(rh + 0x08, ntfs_rd32(rh + 0x04));
        if (!write_mft_record(v, ix->parent_number, ix->parent_rec))
            return SPLIT_ERROR;
    } else {
        if (!index_read_block(ix, stack[i - 1].vcn, parent)) {
            ntfs_say(v, "the parent index block is not readable");
            return SPLIT_ERROR;
        }
        uint8_t *ph = parent + 0x18;
        uint32_t p_used = ntfs_rd32(ph + 0x04);
        if (stack[i - 1].pos >= p_used ||
            p_used + up_len > ntfs_rd32(ph + 0x08)) {
            return SPLIT_PARENT_FULL;
        }
        uint8_t *at_entry = ph + stack[i - 1].pos;
        uint16_t at_len = ntfs_rd16(at_entry + 0x08);
        if (ntfs_rd16(at_entry + 0x0C) & NTFS_INDEX_HAS_SUBNODE)
            ntfs_wr64(at_entry + at_len - 8, new_vcn);

        node_insert_at(ph, stack[i - 1].pos, keep, up_len);
        if (!index_write_block(ix, stack[i - 1].vcn, parent))
            return SPLIT_ERROR;
    }

    /* ---- and only now does the old block give up its upper half --------- */
    {
        uint8_t end[24];
        ntfs_zero_bytes(end, sizeof end);
        uint16_t end_len = had_child ? 0x18 : 0x10;
        ntfs_wr16(end + 0x08, end_len);
        ntfs_wr16(end + 0x0A, 0);
        ntfs_wr16(end + 0x0C, (uint16_t)(NTFS_INDEX_LAST |
                  (had_child ? NTFS_INDEX_HAS_SUBNODE : 0)));
        if (had_child) ntfs_wr64(end + end_len - 8, median_child);

        ntfs_copy_bytes(h + median_at, end, end_len);
        ntfs_wr32(h + 0x04, median_at + end_len);
        if (!index_write_block(ix, stack[i].vcn, block)) return SPLIT_ERROR;
    }

    return index_open(v, ix->parent_number, ix) ? SPLIT_OK : SPLIT_ERROR;
}

/* ------------------------------------------------------------- inserting
 *
 * Descend to the leaf the name belongs in and put it there.  When that leaf is
 * full, make room - by splitting it, or by splitting whichever ancestor is in
 * the way, or by giving the whole tree another level - and then start the
 * descent again, because the shape it was measured against has changed.
 */
static bool index_insert(index_t *ix, const uint8_t *entry, uint32_t entry_len,
                         const uint8_t *key, uint8_t key_len) {
    ntfs_volume_t *v = ix->v;
    static uint8_t block[65536];
    static step_t stack[MAX_DEPTH];
    int settle = 0;

restart:
    if (++settle > 64) {
        ntfs_say(v, "the directory's index will not settle");
        return false;
    }

    uint8_t *root = find_attr_mut(ix->parent_rec, v->record_bytes,
                                  NTFS_ATTR_INDEX_ROOT, I30, 4);
    if (!root) {
        ntfs_say(v, "the directory has no index root to insert into");
        return false;
    }

    uint8_t *h = root + ntfs_rd16(root + 0x14) + 0x10;
    int depth = 0;
    stack[0].is_root = true;
    stack[0].vcn = 0;
    bool in_block = false;

    for (;;) {
        uint32_t used = ntfs_rd32(h + 0x04);
        uint32_t at = ntfs_rd32(h + 0x00);
        uint8_t *found = NULL;

        while (at < used) {
            uint8_t *e = h + at;
            uint16_t flags = ntfs_rd16(e + 0x0C);
            uint16_t len = ntfs_rd16(e + 0x08);
            if (!len) {
                ntfs_say(v, "an index entry claims to be zero bytes long");
                return false;
            }
            if (flags & NTFS_INDEX_LAST) { found = e; break; }

            const uint8_t *e_name = e + 0x10 + 0x42;
            uint8_t e_name_len = e[0x10 + 0x40];
            int order = collate(key, key_len, e_name, e_name_len);
            if (order == 0) {
                ntfs_say(v, "a file of that name is already there");
                return false;
            }
            if (order < 0) { found = e; break; }
            at += len;
        }
        if (!found) {
            ntfs_say(v, "an index node has no end marker");
            return false;
        }
        stack[depth].pos = (uint32_t)(found - h);

        uint16_t found_flags = ntfs_rd16(found + 0x0C);
        if (found_flags & NTFS_INDEX_HAS_SUBNODE) {
            uint16_t len = ntfs_rd16(found + 0x08);
            if (len < 8) {
                ntfs_say(v, "an index entry says it has a child but is too "
                            "short to hold its number");
                return false;
            }
            uint64_t vcn = ntfs_rd64(found + len - 8);
            if (!ix->has_allocation) {
                ntfs_say(v, "the index names a child it has no room for");
                return false;
            }
            if (depth + 1 >= MAX_DEPTH) {
                ntfs_say(v, "the directory's index is deeper than this driver "
                            "follows");
                return false;
            }
            if (ix->block_bytes > sizeof block) {
                ntfs_say(v, "the directory's index blocks are larger than "
                            "this driver can hold");
                return false;
            }
            if (!index_read_block(ix, vcn, block)) {
                ntfs_say(v, "an index block is not readable");
                return false;
            }
            depth++;
            stack[depth].is_root = false;
            stack[depth].vcn = vcn;
            h = block + 0x18;
            in_block = true;
            continue;
        }

        /* A leaf.  `stack[depth].pos` is where the entry goes. */
        if (in_block) {
            if (used + entry_len <= ntfs_rd32(h + 0x08)) {
                node_insert_at(h, stack[depth].pos, entry, entry_len);
                return index_write_block(ix, stack[depth].vcn, block);
            }
        } else {
            if (root_reserve(ix, used + entry_len)) {
                node_insert_at(h, stack[depth].pos, entry, entry_len);
                ntfs_wr32(h + 0x08, ntfs_rd32(h + 0x04));
                return write_mft_record(v, ix->parent_number, ix->parent_rec);
            }
        }

        /* No room.  Split from the leaf upwards: if a node's parent cannot
         * take the promoted entry, that parent is the one that has to be
         * split first.  When even the root cannot, the tree gains a level. */
        for (int i = depth; i >= 1; i--) {
            int r = split_node(ix, stack, i);
            if (r == SPLIT_OK) goto restart;
            if (r == SPLIT_ERROR) return false;
            /* SPLIT_PARENT_FULL: try again one level up. */
        }
        if (!deepen_index(ix)) return false;
        goto restart;
    }
}

/* ================================================================= creating */

/* Split a path into the directory holding it and the last component. */
static const char *last_component(const char *path, char *dir, uint32_t dir_cap) {
    const char *leaf = path;
    uint32_t split = 0;
    for (uint32_t i = 0; path[i]; i++)
        if (path[i] == '/' || path[i] == '\\') { split = i; leaf = path + i + 1; }

    if (!split) { dir[0] = '/'; dir[1] = 0; return leaf; }
    if (split >= dir_cap) return NULL;
    for (uint32_t i = 0; i < split; i++) dir[i] = path[i];
    dir[split] = 0;
    return leaf;
}

bool ntfs_create(ntfs_volume_t *v, const char *path, bool directory,
                 ntfs_file_t *out) {
    char dir_path[512];
    const char *leaf = last_component(path, dir_path, sizeof dir_path);
    if (!leaf) { ntfs_say(v, "path is too long"); return false; }

    ntfs_file_t parent;
    if (!ntfs_lookup(v, dir_path, &parent) || !parent.directory) {
        ntfs_say(v, "the directory to create in is not there");
        return false;
    }
    return ntfs_create_in(v, parent.record, leaf, directory, out);
}

bool ntfs_create_in(ntfs_volume_t *v, uint64_t parent_record, const char *leaf,
                    bool directory, ntfs_file_t *out) {
    if (!v->mounted) { ntfs_say(v, "not mounted"); return false; }
    if (!v->write) { ntfs_say(v, "volume opened for reading"); return false; }

    /* Static, not automatic: see the note above about stack size.  Between
     * them these are eight kilobytes, and a kernel stack is not. */
    static new_file_t n;
    ntfs_zero_bytes(&n, sizeof n);
    if (!to_utf16(leaf, n.name, &n.name_len)) {
        ntfs_say(v, "that name cannot be stored on NTFS");
        return false;
    }

    /* A name this cannot collate is a name that would be invisible to
     * Windows.  Loading the volume's table is what makes any name safe; when
     * that fails, only the range the fallback is certainly right about is. */
    if (!load_upcase(v)) {
        for (uint8_t i = 0; i < n.name_len; i++) {
            if (n.name[i] > 0x7F) {
                ntfs_say(v, "$UpCase is unreadable, so only ASCII names can "
                            "be placed in the right order");
                return false;
            }
        }
    }

    /* The parent's identity - the reference stored in $FILE_NAME carries the
     * sequence number, not just the record. */
    static ntfs_file_t parent;
    ntfs_zero_bytes(&parent, sizeof parent);
    parent.record = parent_record;
    parent.directory = true;

    /* A name that is already there is caught by the index insert at the end,
     * which finds it while descending to the place the new entry would go.
     *
     * The obvious thing - check first, then create - was tried and removed.
     * Checking means walking the directory, walking a directory means reading
     * every one of its index blocks, and doing that before every create turns
     * filling a directory from linear into quadratic: two thousand files took
     * longer than the whole rest of the test suite.  The descent already has
     * to visit the exact place a duplicate would be, so it costs nothing
     * there and everything here.
     *
     * The price is that a duplicate is refused after a record has been taken
     * rather than before.  release_record gives it straight back.
     */

    static uint8_t parent_rec[4096];
    if (v->record_bytes > sizeof parent_rec) {
        ntfs_say(v, "this volume's MFT records are too large for this driver");
        return false;
    }
    if (!ntfs_read_mft_record(v, parent_record, parent_rec)) {
        ntfs_say(v, "the directory's record is not readable");
        return false;
    }
    uint16_t parent_seq = ntfs_rd16(parent_rec + 0x10);
    n.parent = parent_record | ((uint64_t)parent_seq << 48);

    /* Security is inherited from the directory, which is both what Windows
     * does and the only way to get a valid $Secure reference without writing
     * $Secure - a file with no security id is one Windows reports as damaged. */
    const uint8_t *psi = ntfs_find_attribute(parent_rec, v->record_bytes,
                                             NTFS_ATTR_STANDARD_INFO, NULL, 0);
    if (psi && !psi[8] && ntfs_rd32(psi + 0x10) >= 0x38)
        n.security_id = ntfs_rd32(psi + ntfs_rd16(psi + 0x14) + 0x34);

    n.directory = directory;
    n.attributes = directory ? 0x10000000u : NTFS_FA_ARCHIVE;
    n.time = now_ticks();

    /* ---- 1: a record ---------------------------------------------------- */
    uint64_t number = 0;
    if (!allocate_record(v, &number)) return false;
    uint16_t sequence = 1, usn = 0;
    reuse_identity(v, number, &sequence, &usn);

    /* ---- 2: build it ---------------------------------------------------- */
    static uint8_t rec[4096];
    ntfs_zero_bytes(rec, v->record_bytes);

    uint16_t usa_count = (uint16_t)(v->record_bytes / v->bytes_per_sector + 1);
    uint32_t attrs_at = align8(0x30 + (uint32_t)usa_count * 2);

    rec[0] = 'F'; rec[1] = 'I'; rec[2] = 'L'; rec[3] = 'E';
    ntfs_wr16(rec + 0x04, 0x30);                      /* fixup array         */
    ntfs_wr16(rec + 0x06, usa_count);
    ntfs_wr16(rec + 0x30, usn);                       /* carried on, not reset */
    ntfs_wr64(rec + 0x08, 0);                         /* no journal entry    */
    ntfs_wr16(rec + 0x10, sequence);
    ntfs_wr16(rec + 0x12, 1);                         /* one name -> one link */
    ntfs_wr16(rec + 0x14, (uint16_t)attrs_at);
    ntfs_wr16(rec + 0x16, (uint16_t)(NTFS_RECORD_IN_USE |
                                     (directory ? NTFS_RECORD_DIRECTORY : 0)));
    ntfs_wr32(rec + 0x18, attrs_at + 8);              /* header + end marker */
    ntfs_wr32(rec + 0x1C, v->record_bytes);
    ntfs_wr64(rec + 0x20, 0);                         /* not an extension    */
    ntfs_wr16(rec + 0x28, 0);                         /* next instance       */
    ntfs_wr32(rec + 0x2C, (uint32_t)number);
    ntfs_wr32(rec + attrs_at, NTFS_ATTR_END);
    ntfs_wr32(rec + attrs_at + 4, 0);

    uint8_t si[72];
    build_standard_info(si, &n);
    if (!add_resident_attr(v, rec, NTFS_ATTR_STANDARD_INFO, si, sizeof si, 0))
        { release_record(v, number); return false; }

    static uint8_t fn[0x42 + MAX_NAME * 2];
    uint32_t fn_len = build_file_name(fn, &n, 0, 0);
    if (!add_resident_attr(v, rec, NTFS_ATTR_FILE_NAME, fn, fn_len, 1))
        { release_record(v, number); return false; }

    if (directory) {
        uint8_t root[0x30];
        uint32_t root_len = build_index_root(v, root);
        if (!add_named_resident_attr(v, rec, NTFS_ATTR_INDEX_ROOT, I30, 4,
                                     root, root_len))
            { release_record(v, number); return false; }
    } else {
        /* An empty file: $DATA with nothing in it, which is resident and
         * costs no clusters at all. */
        if (!add_resident_attr(v, rec, NTFS_ATTR_DATA, NULL, 0, 0))
            { release_record(v, number); return false; }
    }

    if (!write_mft_record(v, number, rec))
        { release_record(v, number); return false; }

    /* ---- 3: the commit -------------------------------------------------- */
    static uint8_t entry[0x10 + 0x42 + MAX_NAME * 2 + 8];
    uint32_t key_len = fn_len;
    uint32_t entry_len = align8(0x10 + key_len);
    ntfs_zero_bytes(entry, entry_len);
    ntfs_wr64(entry + 0x00, number | ((uint64_t)sequence << 48));
    ntfs_wr16(entry + 0x08, (uint16_t)entry_len);
    ntfs_wr16(entry + 0x0A, (uint16_t)key_len);
    ntfs_wr16(entry + 0x0C, 0);
    ntfs_copy_bytes(entry + 0x10, fn, fn_len);

    static index_t ix;
    if (!index_open(v, parent_record, &ix)) { release_record(v, number); return false; }
    if (!index_insert(&ix, entry, entry_len, fn + 0x42, n.name_len)) {
        /* Nothing points at the record, so it can simply be given back.  Note
         * which half of the create this is: everything before the index entry
         * is undoable, and the index entry itself is the point after which the
         * file exists.  That is why it is last. */
        release_record(v, number);
        return false;
    }

    if (out) {
        ntfs_zero_bytes(out, sizeof *out);
        out->record = number;
        out->directory = directory;
        out->resident = true;
        out->size = 0;
    }
    return true;
}

/* --------------------------------------------------------------- growing */

/* Give a file the space for `bytes`, converting it away from living inside its
 * own record if it has outgrown that.
 *
 * The index entry is updated too.  It carries its own copy of the file's size,
 * and Windows shows that copy in a directory listing - so a file grown without
 * it still reads correctly through its record and lists as zero bytes, which
 * is a discrepancy chkdsk reports and a user sees first.
 */
bool ntfs_resize(ntfs_volume_t *v, ntfs_file_t *f, uint64_t bytes) {
    if (!v->write) { ntfs_say(v, "volume opened for reading"); return false; }
    if (f->directory) { ntfs_say(v, "a directory has no data to resize"); return false; }

    static uint8_t rec[4096];
    if (v->record_bytes > sizeof rec) return false;
    if (!ntfs_read_mft_record(v, f->record, rec)) return false;

    uint8_t *a = find_attr_mut(rec, v->record_bytes, NTFS_ATTR_DATA, NULL, 0);
    if (!a) { ntfs_say(v, "the file has no data attribute"); return false; }

    uint64_t cluster = v->bytes_per_cluster;
    uint64_t want_clusters = (bytes + cluster - 1) / cluster;

    /* Keep whatever is already there.  A resident file's content is small by
     * definition, so it fits in a buffer; a non-resident one keeps its
     * clusters and only gains more. */
    static uint8_t keep[1024];
    uint32_t keep_len = 0;
    extent_t extents[MAX_EXTENTS];
    uint32_t extent_count = 0;
    uint64_t have_clusters = 0;

    if (!a[8]) {
        keep_len = ntfs_rd32(a + 0x10);
        if (keep_len > sizeof keep) return false;
        ntfs_copy_bytes(keep, a + ntfs_rd16(a + 0x14), keep_len);
        if (bytes <= keep_len) {
            /* Shrinking within the record: just say it is shorter. */
            ntfs_wr32(a + 0x10, (uint32_t)bytes);
            f->size = bytes;
            return write_mft_record(v, f->record, rec);
        }
    } else {
        uint16_t runs_at = ntfs_rd16(a + 0x20);
        uint32_t a_len = ntfs_rd32(a + 4);
        uint32_t runs_len = a_len > runs_at ? a_len - runs_at : 0;
        if (!extents_of(a + runs_at, runs_len, extents, &extent_count)) {
            ntfs_say(v, "the file is in more pieces than this driver handles");
            return false;
        }
        for (uint32_t i = 0; i < extent_count; i++)
            have_clusters += extents[i].count;

        if (want_clusters <= have_clusters) {
            /* Room already.  Only the sizes change. */
            ntfs_wr64(a + 0x30, bytes);
            if (ntfs_rd64(a + 0x38) < bytes) ntfs_wr64(a + 0x38, bytes);
            f->size = bytes;
            if (!write_mft_record(v, f->record, rec)) return false;
            goto update_index;
        }
    }

    /* More space is needed. */
    {
        bitmap_t bits;
        if (!cluster_bitmap(v, &bits)) return false;
        uint64_t total = v->total_sectors / v->sectors_per_cluster;

        uint64_t need = want_clusters - have_clusters;
        uint64_t hint = extent_count ? extents[extent_count - 1].lcn +
                                       extents[extent_count - 1].count
                                     : v->mft_lcn;

        while (need) {
            uint64_t at = 0;
            uint64_t took = bitmap_take(v, &bits, total, 0, hint, need, &at);
            if (!took) {
                ntfs_say(v, "the volume has no room left");
                return false;
            }

            /* A run that carries straight on from the previous one is the
             * same run, and encoding it as two wastes a record's worth of
             * space describing something contiguous. */
            if (extent_count && extents[extent_count - 1].lcn +
                                extents[extent_count - 1].count == at) {
                extents[extent_count - 1].count += took;
            } else {
                if (extent_count >= MAX_EXTENTS) {
                    ntfs_say(v, "the file would be in too many pieces");
                    return false;
                }
                extents[extent_count].lcn = at;
                extents[extent_count].count = took;
                extent_count++;
            }
            need -= took;
            hint = at + took;
        }

        uint8_t runs[512];
        uint32_t runs_len = extents_encode(extents, extent_count, runs,
                                           sizeof runs);
        if (!runs_len) {
            ntfs_say(v, "the file's run list does not fit");
            return false;
        }

        /* Rebuild the attribute as a non-resident one. */
        uint32_t runs_at = 0x40;
        uint32_t total_len = align8(runs_at + runs_len);
        uint32_t old_len = ntfs_rd32(a + 4);

        if (total_len > old_len) {
            if (!make_room_after(v, rec, a, total_len - old_len)) return false;
        } else if (total_len < old_len) {
            take_room_after(v, rec, a, old_len - total_len);
        }

        uint16_t instance = ntfs_rd16(a + 0x0E);
        ntfs_zero_bytes(a, total_len);
        ntfs_wr32(a + 0x00, NTFS_ATTR_DATA);
        ntfs_wr32(a + 0x04, total_len);
        a[0x08] = 1;                                  /* non-resident        */
        a[0x09] = 0;
        ntfs_wr16(a + 0x0A, 0);
        ntfs_wr16(a + 0x0C, 0);
        ntfs_wr16(a + 0x0E, instance);
        ntfs_wr64(a + 0x10, 0);                       /* lowest vcn          */
        ntfs_wr64(a + 0x18, want_clusters - 1);       /* highest vcn         */
        ntfs_wr16(a + 0x20, (uint16_t)runs_at);
        ntfs_wr16(a + 0x22, 0);                       /* not compressed      */
        ntfs_wr64(a + 0x28, want_clusters * cluster); /* allocated           */
        ntfs_wr64(a + 0x30, bytes);                   /* real size           */
        ntfs_wr64(a + 0x38, bytes);                   /* initialised         */
        ntfs_copy_bytes(a + runs_at, runs, runs_len);

        if (!write_mft_record(v, f->record, rec)) return false;

        /* Whatever had been living inside the record goes into the clusters
         * it now owns, so growing a small file does not lose its content. */
        if (keep_len) {
            if (ntfs_write_runs(v, runs, runs_len, bytes, 0, keep, keep_len)
                != (long)keep_len) {
                ntfs_say(v, "the file's old content could not be moved out");
                return false;
            }
        }

        f->resident = false;
        f->size = bytes;
        f->runs_bytes = runs_len > sizeof f->runs ? sizeof f->runs : runs_len;
        ntfs_copy_bytes(f->runs, runs, f->runs_bytes);
    }

update_index:
    /* The copy of the size the directory keeps.  Nothing moves, so this is a
     * read, two fields, and a write. */
    {
        static uint8_t fresh[4096];
        if (!ntfs_read_mft_record(v, f->record, fresh)) return true;
        const uint8_t *fn = ntfs_find_attribute(fresh, v->record_bytes,
                                                NTFS_ATTR_FILE_NAME, NULL, 0);
        const uint8_t *da = ntfs_find_attribute(fresh, v->record_bytes,
                                                NTFS_ATTR_DATA, NULL, 0);
        if (!fn || fn[8] || !da) return true;

        const uint8_t *value = fn + ntfs_rd16(fn + 0x14);
        uint64_t parent = ntfs_rd64(value) & 0xFFFFFFFFFFFFull;
        uint64_t allocated = da[8] ? ntfs_rd64(da + 0x28) : 0;

        static index_t ix;
        if (!index_open(v, parent, &ix)) return true;
        (void)ix;

        /* Walking the whole index to find one entry is not free, but it
         * happens once per size change and the alternative is a directory
         * listing that disagrees with the file. */
        /* Deliberately best-effort: a stale size in the index is a cosmetic
         * fault chkdsk repairs, and failing the whole write over it would
         * turn a cosmetic fault into a lost file. */
        static uint8_t block[65536];
        uint8_t *root = find_attr_mut(ix.parent_rec, v->record_bytes,
                                      NTFS_ATTR_INDEX_ROOT, I30, 4);
        if (!root) return true;
        uint8_t *h = root + ntfs_rd16(root + 0x14) + 0x10;

        for (int pass = 0; pass < 2; pass++) {
            uint32_t used = ntfs_rd32(h + 0x04);
            uint32_t at = ntfs_rd32(h + 0x00);
            while (at < used) {
                uint8_t *e = h + at;
                uint16_t len = ntfs_rd16(e + 0x08);
                uint16_t flags = ntfs_rd16(e + 0x0C);
                if (!len) break;
                if (flags & NTFS_INDEX_LAST) break;
                if ((ntfs_rd64(e) & 0xFFFFFFFFFFFFull) == f->record) {
                    ntfs_wr64(e + 0x10 + 0x28, allocated);
                    ntfs_wr64(e + 0x10 + 0x30, f->size);
                    if (pass == 0)
                        write_mft_record(v, ix.parent_number, ix.parent_rec);
                    return true;
                }
                at += len;
            }
            if (pass == 0 && ix.has_allocation) {
                /* One level down is as far as this looks; deeper trees keep
                 * the stale size until chkdsk corrects it. */
                if (ix.block_bytes <= sizeof block &&
                    index_read_block(&ix, 0, block)) {
                    h = block + 0x18;
                    continue;
                }
            }
            break;
        }
    }
    return true;
}

/* Whether this volume can be written to at all - the thing every caller that
 * used to assume "NTFS means read-only" should be asking instead. */
bool ntfs_writable(ntfs_volume_t *v) {
    return v && v->mounted && v->write != 0;
}

/* ================================================================== tests
 *
 * Only the parts that are arithmetic: run-list encoding, name ordering, the
 * fixups, and the 8.3 test.  These need no disk, so they run at boot and say
 * whether the pieces that turn a file into bytes are right - separately from
 * whether a real volume accepts the result, which only a real volume can say.
 *
 * The expected values below were worked out by hand from the format, not
 * recorded from a run, so this checks the code against the specification
 * rather than against itself.
 */
int ntfs_write_selftest(void) {
    int failures = 0;

    /* ---- run lists ---------------------------------------------------- */
    {
        /* One run of 8 clusters at 0x20.  Header 0x11: one length byte, one
         * offset byte.  Then 0x08, then 0x20, then the terminator. */
        extent_t one = { 0x20, 8 };
        uint8_t out[32];
        uint32_t n = extents_encode(&one, 1, out, sizeof out);
        if (n != 4 || out[0] != 0x11 || out[1] != 0x08 || out[2] != 0x20 ||
            out[3] != 0x00) {
            failures++;
        }

        /* And read back through the same walker the reader uses - which is
         * the property that actually matters, because these two disagreeing
         * is a file that writes to one place and reads from another. */
        ntfs_run_walker_t w;
        ntfs_runs_begin(&w, out, n);
        int64_t lcn = 0; uint64_t count = 0;
        if (!ntfs_runs_next(&w, &lcn, &count) || lcn != 0x20 || count != 8)
            failures++;
        if (ntfs_runs_next(&w, &lcn, &count)) failures++;   /* only one */
    }
    {
        /* Two runs, the second BEFORE the first on the disk - the case the
         * offsets being signed exists for, and the one a writer that stores
         * them unsigned gets wrong in a way no single-run test can see. */
        extent_t two[2] = { { 1000, 4 }, { 100, 2 } };
        uint8_t out[32];
        uint32_t n = extents_encode(two, 2, out, sizeof out);
        ntfs_run_walker_t w;
        ntfs_runs_begin(&w, out, n);
        int64_t lcn = 0; uint64_t count = 0;
        if (!ntfs_runs_next(&w, &lcn, &count) || lcn != 1000 || count != 4)
            failures++;
        if (!ntfs_runs_next(&w, &lcn, &count) || lcn != 100 || count != 2)
            failures++;
    }

    /* ---- the fixups --------------------------------------------------- */
    {
        /* A 1024-byte record over 512-byte sectors: two sectors, so three
         * entries in the array.  Take the fixups out and put them back, and
         * the bytes that were displaced have to come back unchanged. */
        static uint8_t rec[1024];
        for (uint32_t i = 0; i < sizeof rec; i++) rec[i] = (uint8_t)(i * 7 + 3);
        rec[0] = 'F'; rec[1] = 'I'; rec[2] = 'L'; rec[3] = 'E';
        ntfs_wr16(rec + 4, 0x30);
        ntfs_wr16(rec + 6, 3);
        ntfs_wr16(rec + 0x30, 0);

        uint8_t before[4];
        before[0] = rec[510]; before[1] = rec[511];
        before[2] = rec[1022]; before[3] = rec[1023];

        if (!embed_fixups(rec, sizeof rec, 512)) failures++;

        /* Both sectors must now end in the same new check value, and it must
         * not be the zero it started from. */
        uint16_t check = ntfs_rd16(rec + 0x30);
        if (check == 0 || ntfs_rd16(rec + 510) != check ||
            ntfs_rd16(rec + 1022) != check) failures++;

        if (!ntfs_apply_fixups(rec, sizeof rec, 512)) failures++;
        if (rec[510] != before[0] || rec[511] != before[1] ||
            rec[1022] != before[2] || rec[1023] != before[3]) failures++;

        /* A torn record - one sector written, one not - has to be refused
         * rather than read as though it were whole. */
        if (!embed_fixups(rec, sizeof rec, 512)) failures++;
        rec[1022] ^= 0xFF;
        if (ntfs_apply_fixups(rec, sizeof rec, 512)) failures++;
    }

    /* ---- name ordering ------------------------------------------------ */
    {
        /* Without $UpCase loaded this is the ASCII fallback, which is what
         * these check.  "a" and "A" are the same name to NTFS's ordering, and
         * a driver that sorts them apart puts files where Windows will not
         * look for them. */
        static const uint16_t a[] = { 'A', 'B', 'C' };
        static const uint16_t b[] = { 'a', 'b', 'c' };
        static const uint16_t c[] = { 'A', 'B' };
        static const uint16_t d[] = { 'A', 'B', 'D' };

        if (collate((const uint8_t *)a, 3, (const uint8_t *)b, 3) != 0)
            failures++;
        if (collate((const uint8_t *)c, 2, (const uint8_t *)a, 3) >= 0)
            failures++;                       /* shorter sorts first */
        if (collate((const uint8_t *)a, 3, (const uint8_t *)d, 3) >= 0)
            failures++;                       /* C before D */
        if (collate((const uint8_t *)d, 3, (const uint8_t *)a, 3) <= 0)
            failures++;                       /* and the other way round */
    }

    /* ---- names -------------------------------------------------------- */
    {
        uint16_t name[MAX_NAME];
        uint8_t len = 0;

        if (!to_utf16("KERNEL.LOG", name, &len) || len != 10) failures++;
        if (!is_dos_name(name, len)) failures++;

        if (!to_utf16("Program Files", name, &len) || len != 13) failures++;
        if (is_dos_name(name, len)) failures++;      /* space, lower case */

        /* The characters Windows will not have in a name. */
        if (to_utf16("a:b", name, &len)) failures++;
        if (to_utf16("a/b", name, &len)) failures++;
        if (to_utf16("", name, &len)) failures++;
    }

    /* ---- an empty index ----------------------------------------------- */
    {
        ntfs_volume_t fake;
        ntfs_zero_bytes(&fake, sizeof fake);
        fake.bytes_per_cluster = 4096;

        uint8_t root[0x30];
        uint32_t n = build_index_root(&fake, root);
        if (n != 0x30) failures++;

        /* The header says the entries begin at 0x10 from itself and run for
         * 0x20 bytes - which is the header plus the one end marker. */
        const uint8_t *h = root + 0x10;
        if (ntfs_rd32(h + 0x00) != 0x10) failures++;
        if (ntfs_rd32(h + 0x04) != 0x20) failures++;
        if (ntfs_rd32(h + 0x08) != 0x20) failures++;
        if (h[0x0C] != 0) failures++;                /* small: no allocation */

        /* And the one entry it holds is the end marker: no key, and the flag
         * that stops a walk. */
        const uint8_t *e = h + 0x10;
        if (ntfs_rd16(e + 0x08) != 0x10) failures++;
        if (ntfs_rd16(e + 0x0A) != 0) failures++;
        if (ntfs_rd16(e + 0x0C) != NTFS_INDEX_LAST) failures++;
    }

    return failures;
}
