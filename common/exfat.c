/* exfat.c - reading exFAT.
 *
 * A large USB stick or an SD card is formatted this way by default, because
 * FAT32 cannot hold a file over four gigabytes and NTFS is more filesystem
 * than a camera needs.  A system that reads FAT and NTFS and not this one
 * still cannot read most of the removable media it will be handed.
 *
 * ---------------------------------------------------------------------------
 * THE THREE THINGS THAT MAKE IT NOT FAT
 *
 * It looks like FAT and is not, in three ways that each cost a bug if missed:
 *
 *   A file takes three entries, not one.  A primary entry with the attributes,
 *   a stream entry with the size and first cluster, and one or more name
 *   entries carrying fifteen characters each.  Reading the first and stopping
 *   gets a file with no name and no size.
 *
 *   A file may opt out of the allocation table.  When its clusters happen to
 *   be contiguous a flag says so and the table holds nothing for it - so a
 *   reader that always follows the chain reads whatever the table's unused
 *   entries contain, which is usually zero, which looks like a file that ends
 *   immediately.
 *
 *   The size is stored twice.  `valid_size` is how much has been written and
 *   `size` is how much is allocated; a file created and then extended without
 *   being written has the second larger than the first, and the bytes between
 *   them are not the file's.
 *
 * Every offset below was taken from the exfatprogs headers and confirmed by
 * compiling them and printing offsetof - not counted by hand.
 */
#include "exfat.h"

/* ------------------------------------------------------------ little-endian */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void copy_bytes(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    while (n--) *d++ = *s++;
}
static void zero_bytes(void *dst, size_t n) {
    uint8_t *d = dst; while (n--) *d++ = 0;
}
static void say(exfat_volume_t *v, const char *msg) {
    size_t i = 0;
    while (msg[i] && i < sizeof v->error - 1) { v->error[i] = msg[i]; i++; }
    v->error[i] = 0;
}

/* --------------------------------------------------------- the boot sector */

#define BSX_VOL_LENGTH      0x48
#define BSX_FAT_OFFSET      0x50
#define BSX_FAT_LENGTH      0x54
#define BSX_CLU_OFFSET      0x58
#define BSX_CLU_COUNT       0x5C
#define BSX_ROOT_CLUSTER    0x60
#define BSX_FS_VERSION      0x68
#define BSX_SECT_SIZE_BITS  0x6C
#define BSX_SECT_PER_CLU    0x6D
#define BSX_NUM_FATS        0x6E

/* ------------------------------------------------------- directory entries */

#define ENTRY_BYTES     32
#define ENTRY_END       0x00
#define ENTRY_IN_USE    0x80
#define ENTRY_BITMAP    0x81
#define ENTRY_UPCASE    0x82
#define ENTRY_VOLUME    0x83
#define ENTRY_FILE      0x85
#define ENTRY_STREAM    0xC0
#define ENTRY_NAME      0xC1

#define FILE_NUM_EXT    0x01
#define FILE_CHECKSUM   0x02
#define FILE_ATTR       0x04

#define STREAM_FLAGS    0x01
#define STREAM_NAME_LEN 0x03
#define STREAM_HASH     0x04
#define STREAM_VALID    0x08
#define STREAM_START    0x14
#define STREAM_SIZE     0x18

#define NAME_CHARS      0x02      /* fifteen of them, per entry */

#define FLAG_CONTIGUOUS 0x02      /* the allocation table holds nothing */

#define ATTR_DIRECTORY  0x0010

#define CLUSTER_FIRST   2
#define CLUSTER_EOF     0xFFFFFFFFu
#define CLUSTER_BAD     0xFFFFFFF7u

#define MAX_NAME_CHARS  255

/* ------------------------------------------------------------- checksums
 *
 * Three of them, all rotate-and-add, all subtly different - which is exactly
 * the sort of thing to take from the source rather than remember.
 */

uint32_t exfat_boot_checksum(const uint8_t *sectors, uint32_t bytes) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < bytes; i++) {
        /* Three bytes of the first sector are skipped: the volume flags and
         * the percentage in use, both of which change while the volume is
         * mounted and would otherwise invalidate the checksum. */
        if (i == 106 || i == 107 || i == 112) continue;
        sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + sectors[i];
    }
    return sum;
}

uint16_t exfat_entry_checksum(const uint8_t *entries, int count) {
    uint16_t sum = 0;
    for (int e = 0; e < count; e++) {
        const uint8_t *b = entries + e * ENTRY_BYTES;
        /* The first entry's own checksum field is skipped; the others are
         * covered whole. */
        int skip_from = (e == 0) ? 4 : 2;
        sum = (uint16_t)((sum << 15) | (sum >> 1));
        sum = (uint16_t)(sum + b[0]);
        sum = (uint16_t)((sum << 15) | (sum >> 1));
        sum = (uint16_t)(sum + b[1]);
        for (int i = skip_from; i < ENTRY_BYTES; i++) {
            sum = (uint16_t)((sum << 15) | (sum >> 1));
            sum = (uint16_t)(sum + b[i]);
        }
    }
    return sum;
}

/* The upcase used for the hash.
 *
 * The volume carries its own table and this does not read it yet, so only the
 * range where every table agrees is upcased - which is enough to find any name
 * made of ASCII, and honest about the rest: a name outside it simply does not
 * match, rather than matching the wrong file. */
static uint16_t upcase(uint16_t c) {
    if (c >= 'a' && c <= 'z') return (uint16_t)(c - 'a' + 'A');
    return c;
}

uint16_t exfat_name_hash(const uint16_t *name, uint8_t length) {
    uint16_t sum = 0;
    for (uint8_t i = 0; i < length; i++) {
        uint16_t c = upcase(name[i]);
        sum = (uint16_t)((sum << 15) | (sum >> 1));
        sum = (uint16_t)(sum + (c & 0xFF));
        sum = (uint16_t)((sum << 15) | (sum >> 1));
        sum = (uint16_t)(sum + (c >> 8));
    }
    return sum;
}

/* ---------------------------------------------------------------- mounting */

bool exfat_mount(exfat_volume_t *v, void *ctx, exfat_read_fn read) {
    zero_bytes(v, sizeof *v);
    v->ctx = ctx;
    v->read = read;

    static uint8_t boot[512];
    if (!read(ctx, 0, 1, boot)) { say(v, "the first sector could not be read"); return false; }

    /* "EXFAT   " and nothing else.  The trailing spaces are part of it. */
    static const char want[8] = { 'E','X','F','A','T',' ',' ',' ' };
    for (int i = 0; i < 8; i++)
        if (boot[3 + i] != (uint8_t)want[i]) {
            say(v, "this volume is not exFAT");
            return false;
        }

    uint8_t sect_bits = boot[BSX_SECT_SIZE_BITS];
    uint8_t clus_bits = boot[BSX_SECT_PER_CLU];
    if (sect_bits < 9 || sect_bits > 12) {
        say(v, "the sector size is not one this driver reads");
        return false;
    }
    if (clus_bits > 25 - sect_bits) {
        say(v, "the cluster size is not one this driver reads");
        return false;
    }

    v->bytes_per_sector    = 1u << sect_bits;
    v->sectors_per_cluster = 1u << clus_bits;
    v->bytes_per_cluster   = v->bytes_per_sector * v->sectors_per_cluster;

    v->volume_sectors = rd64(boot + BSX_VOL_LENGTH);
    v->fat_sector     = rd32(boot + BSX_FAT_OFFSET);
    v->fat_sectors    = rd32(boot + BSX_FAT_LENGTH);
    v->cluster_sector = rd32(boot + BSX_CLU_OFFSET);
    v->cluster_count  = rd32(boot + BSX_CLU_COUNT);
    v->root_cluster   = rd32(boot + BSX_ROOT_CLUSTER);
    v->fat_count      = boot[BSX_NUM_FATS];

    if (boot[BSX_FS_VERSION + 1] != 1) {
        say(v, "this volume is a version of exFAT this driver does not know");
        return false;
    }
    if (!v->fat_sectors || !v->cluster_count ||
        v->root_cluster < CLUSTER_FIRST ||
        v->root_cluster >= CLUSTER_FIRST + v->cluster_count) {
        say(v, "the boot sector contradicts itself");
        return false;
    }

    v->mounted = true;
    return true;
}

/* ------------------------------------------------------------- the clusters */

static uint64_t cluster_lba(exfat_volume_t *v, uint32_t cluster) {
    return (uint64_t)v->cluster_sector +
           (uint64_t)(cluster - CLUSTER_FIRST) * v->sectors_per_cluster;
}

static bool read_cluster(exfat_volume_t *v, uint32_t cluster, void *buf) {
    if (cluster < CLUSTER_FIRST ||
        cluster >= CLUSTER_FIRST + v->cluster_count) return false;
    return v->read(v->ctx, cluster_lba(v, cluster), v->sectors_per_cluster, buf);
}

/* The next cluster, out of the allocation table. */
static uint32_t next_cluster(exfat_volume_t *v, uint32_t cluster) {
    static uint8_t sector[4096];
    if (v->bytes_per_sector > sizeof sector) return CLUSTER_EOF;

    uint32_t per_sector = v->bytes_per_sector / 4;
    uint64_t lba = (uint64_t)v->fat_sector + cluster / per_sector;
    if (!v->read(v->ctx, lba, 1, sector)) return CLUSTER_EOF;
    return rd32(sector + (cluster % per_sector) * 4);
}

/* Walk to the cluster holding a given offset.  Contiguous files skip the
 * table entirely - which is the point of the flag, and the trap for a reader
 * that follows the chain regardless. */
static uint32_t cluster_at(exfat_volume_t *v, const exfat_file_t *f,
                           uint64_t offset) {
    uint64_t index = offset / v->bytes_per_cluster;

    if (f->contiguous) {
        uint64_t c = (uint64_t)f->first_cluster + index;
        return c >= CLUSTER_FIRST + (uint64_t)v->cluster_count
               ? CLUSTER_EOF : (uint32_t)c;
    }

    uint32_t c = f->first_cluster;
    while (index--) {
        if (c < CLUSTER_FIRST || c >= CLUSTER_BAD) return CLUSTER_EOF;
        c = next_cluster(v, c);
    }
    return c;
}

long exfat_read_file(exfat_volume_t *v, const exfat_file_t *f,
                     uint64_t offset, void *buf, uint32_t len) {
    if (!v->mounted || !f) return -1;
    if (offset >= f->size) return 0;
    if (offset + len > f->size) len = (uint32_t)(f->size - offset);

    static uint8_t cluster[65536];
    if (v->bytes_per_cluster > sizeof cluster) return -1;

    uint8_t *out = buf;
    uint32_t done = 0;

    while (done < len) {
        uint32_t c = cluster_at(v, f, offset + done);
        if (c < CLUSTER_FIRST || c >= CLUSTER_BAD) break;
        if (!read_cluster(v, c, cluster)) return -1;

        uint32_t into = (uint32_t)((offset + done) % v->bytes_per_cluster);
        uint32_t take = v->bytes_per_cluster - into;
        if (take > len - done) take = len - done;

        copy_bytes(out + done, cluster + into, take);
        done += take;
    }
    return (long)done;
}

/* ------------------------------------------------------------- directories
 *
 * A directory is a file whose bytes are 32-byte entries.  One file is a run of
 * them: the primary, the stream, and enough name entries to spell it.
 */

typedef struct {
    exfat_volume_t *v;
    const exfat_file_t *dir;
    uint64_t at;                   /* byte offset within the directory */
} walker_t;

static bool walker_entry(walker_t *w, uint8_t *out) {
    if (w->at + ENTRY_BYTES > w->dir->size) return false;
    long got = exfat_read_file(w->v, w->dir, w->at, out, ENTRY_BYTES);
    if (got != ENTRY_BYTES) return false;
    w->at += ENTRY_BYTES;
    return true;
}

/* UTF-16 to UTF-8, for as much of a name as fits. */
static void name_out(const uint16_t *name, uint8_t chars, char *out, size_t cap) {
    size_t o = 0;
    for (uint8_t i = 0; i < chars && o + 4 < cap; i++) {
        uint16_t c = name[i];
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
}

/* The next file in a directory, however many entries it takes. */
static bool next_file(walker_t *w, char *name, size_t name_cap,
                      exfat_file_t *out) {
    uint8_t e[ENTRY_BYTES];

    while (walker_entry(w, e)) {
        if (e[0] == ENTRY_END) return false;          /* nothing follows */
        if (!(e[0] & ENTRY_IN_USE)) continue;         /* deleted */
        if (e[0] != ENTRY_FILE) continue;             /* bitmap, upcase, label */

        int extras = e[FILE_NUM_EXT];
        if (extras < 1) continue;                     /* no stream: unusable */

        uint16_t attr = rd16(e + FILE_ATTR);

        uint8_t stream[ENTRY_BYTES];
        if (!walker_entry(w, stream)) return false;
        if (stream[0] != ENTRY_STREAM) continue;

        uint8_t chars = stream[STREAM_NAME_LEN];
        if (chars > MAX_NAME_CHARS) chars = MAX_NAME_CHARS;

        static uint16_t wide[MAX_NAME_CHARS];
        uint8_t have = 0;

        for (int i = 1; i < extras; i++) {
            uint8_t n[ENTRY_BYTES];
            if (!walker_entry(w, n)) return false;
            if (n[0] != ENTRY_NAME) continue;
            for (int k = 0; k < 15 && have < chars; k++)
                wide[have++] = rd16(n + NAME_CHARS + k * 2);
        }

        if (name) name_out(wide, have, name, name_cap);
        if (out) {
            zero_bytes(out, sizeof *out);
            out->first_cluster = rd32(stream + STREAM_START);
            /* The written length, not the allocated one - see the note at the
             * top about the two sizes. */
            out->size = rd64(stream + STREAM_VALID);
            out->directory = (attr & ATTR_DIRECTORY) != 0;
            out->contiguous = (stream[STREAM_FLAGS] & FLAG_CONTIGUOUS) != 0;
        }
        return true;
    }
    return false;
}

bool exfat_readdir(exfat_volume_t *v, const exfat_file_t *dir, uint32_t index,
                   char *name_out_buf, size_t name_cap, exfat_file_t *entry_out) {
    if (!v->mounted || !dir || !dir->directory) return false;

    walker_t w = { v, dir, 0 };
    for (uint32_t i = 0; ; i++) {
        char name[512];
        exfat_file_t e;
        if (!next_file(&w, name, sizeof name, &e)) return false;
        if (i != index) continue;

        if (name_out_buf) {
            size_t o = 0;
            while (name[o] && o + 1 < name_cap) { name_out_buf[o] = name[o]; o++; }
            name_out_buf[o] = 0;
        }
        if (entry_out) *entry_out = e;
        return true;
    }
}

static bool same_name(const char *a, const char *b) {
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return false;
        a++; b++;
    }
    return !*a && !*b;
}

bool exfat_lookup(exfat_volume_t *v, const char *path, exfat_file_t *out) {
    if (!v->mounted || !path) return false;

    exfat_file_t here;
    zero_bytes(&here, sizeof here);
    here.first_cluster = v->root_cluster;
    here.directory = true;
    here.contiguous = false;
    /* The root has no entry describing it, so its length is whatever its
     * chain turns out to be. */
    {
        uint64_t bytes = 0;
        uint32_t c = v->root_cluster;
        for (int guard = 0; guard < 1 << 20; guard++) {
            if (c < CLUSTER_FIRST || c >= CLUSTER_BAD) break;
            bytes += v->bytes_per_cluster;
            c = next_cluster(v, c);
        }
        here.size = bytes;
    }

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
            char name[512];
            exfat_file_t e;
            if (!next_file(&w, name, sizeof name, &e)) break;
            if (!same_name(name, part)) continue;
            here = e;
            /* A directory's own length is not recorded either, so it is the
             * length of its chain. */
            if (here.directory && !here.size) {
                uint64_t bytes = 0;
                uint32_t c = here.first_cluster;
                for (int guard = 0; guard < 1 << 20; guard++) {
                    if (c < CLUSTER_FIRST || c >= CLUSTER_BAD) break;
                    bytes += v->bytes_per_cluster;
                    if (here.contiguous) c++; else c = next_cluster(v, c);
                }
                here.size = bytes;
            }
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
 * The three checksums, against values worked out from the algorithms rather
 * than recorded from a run.  These need no disk, and each of them is a thing
 * that fails silently: a wrong name hash finds no file, a wrong entry checksum
 * makes every file look corrupt, and a wrong boot checksum makes the volume
 * unmountable by anything that checks it.
 */
int exfat_selftest(void) {
    int failures = 0;

    /* The name hash rotates right one bit and adds, twice per character.
     * "A" is 0x0041: first byte 0x41, second 0x00.
     *   sum = 0, rotate -> 0, + 0x41 = 0x0041
     *   rotate right 1 -> 0x8020 wait: (0x41 << 15) | (0x41 >> 1) = 0x8020
     *   + 0x00 = 0x8020
     * so the hash of "A" is 0x8020. */
    {
        uint16_t a[1] = { 'A' };
        uint16_t got = exfat_name_hash(a, 1);
        if (got != 0x8020) failures++;
    }

    /* Lower case hashes the same as upper, or a name typed in the wrong case
     * would never be found. */
    {
        uint16_t lower[3] = { 'a', 'b', 'c' };
        uint16_t upper[3] = { 'A', 'B', 'C' };
        if (exfat_name_hash(lower, 3) != exfat_name_hash(upper, 3)) failures++;
    }

    /* Different names hash differently - a hash that collapsed everything to
     * zero would still pass the two checks above. */
    {
        uint16_t a[2] = { 'A', 'B' };
        uint16_t b[2] = { 'B', 'A' };
        if (exfat_name_hash(a, 2) == exfat_name_hash(b, 2)) failures++;
    }

    /* The entry checksum ignores the two bytes where it is itself stored, so
     * changing those must not change it - and changing anything else must. */
    {
        static uint8_t entries[ENTRY_BYTES * 2];
        for (int i = 0; i < ENTRY_BYTES * 2; i++) entries[i] = (uint8_t)(i * 7 + 1);
        entries[0] = ENTRY_FILE;
        entries[ENTRY_BYTES] = ENTRY_STREAM;

        uint16_t before = exfat_entry_checksum(entries, 2);
        entries[FILE_CHECKSUM] ^= 0xFF;
        entries[FILE_CHECKSUM + 1] ^= 0xFF;
        if (exfat_entry_checksum(entries, 2) != before) failures++;

        entries[FILE_ATTR] ^= 0xFF;
        if (exfat_entry_checksum(entries, 2) == before) failures++;
    }

    /* The boot checksum skips three bytes for the same kind of reason: they
     * change while the volume is mounted. */
    {
        static uint8_t boot[512];
        for (int i = 0; i < 512; i++) boot[i] = (uint8_t)(i);
        uint32_t before = exfat_boot_checksum(boot, 512);

        boot[106] ^= 0xFF; boot[107] ^= 0xFF; boot[112] ^= 0xFF;
        if (exfat_boot_checksum(boot, 512) != before) failures++;

        boot[105] ^= 0xFF;
        if (exfat_boot_checksum(boot, 512) == before) failures++;
    }

    return failures;
}
