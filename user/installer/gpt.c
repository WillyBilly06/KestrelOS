/* gpt.c - reading and writing GUID partition tables.
 *
 * Both copies of the table are always written, and both headers carry correct
 * CRCs: a disk with only one valid copy is a disk that some firmware will
 * quietly "repair" in a way the user did not ask for.
 */
#include "disk.h"

const uint8_t GUID_ESP[16] = {
    0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11, 0xBA,0x4B, 0x00,0xA0,0xC9,0x3E,0xC9,0x3B
};
const uint8_t GUID_KESTREL[16] = {
    0x7E,0x1A,0x4C,0xB3, 0x62,0x9D, 0x47,0x4E, 0x9C,0x31, 0x5A,0x6F,0x2E,0x88,0xD1,0x40
};
const uint8_t GUID_MSDATA[16] = {
    0xA2,0xA0,0xD0,0xEB, 0xE5,0xB9, 0x33,0x44, 0x87,0xC0, 0x68,0xB6,0xB7,0x26,0x99,0xC7
};
const uint8_t GUID_MSRESERVED[16] = {
    0x16,0xE3,0xC9,0xE3, 0x5C,0x0B, 0xB8,0x4D, 0x81,0x7D, 0xF9,0x2D,0xF0,0x02,0x15,0xAE
};

/* ------------------------------------------------------------------- crc32 */

static uint32_t crc_table[256];
static bool crc_ready;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_ready = true;
}

uint32_t crc32_of(const void *data, size_t len) {
    if (!crc_ready) crc_init();
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* -------------------------------------------------------------------- GUIDs */

/* GPT stores the first three fields little-endian and the rest big-endian,
 * which is why the text form interleaves byte orders. */
void guid_text(const uint8_t g[16], char *out, size_t cap) {
    snprintf(out, cap, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
             g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

bool guid_is_zero(const uint8_t g[16]) {
    for (int i = 0; i < 16; i++) if (g[i]) return false;
    return true;
}

bool guid_same(const uint8_t a[16], const uint8_t b[16]) { return memcmp(a, b, 16) == 0; }

/* Not cryptographic, but each partition needs an identifier that will not
 * collide with another disk's: mix the clock, the uptime and a counter. */
void guid_generate(uint8_t out[16]) {
    static uint32_t counter;
    uint64_t seed = time_now();
    uint64_t spin = uptime_ms();
    counter++;

    uint32_t words[4];
    words[0] = (uint32_t)(seed ^ (spin << 11) ^ (counter * 2654435761u));
    words[1] = (uint32_t)((seed >> 32) ^ (spin * 40503u) ^ (counter << 17));
    words[2] = (uint32_t)crc32_of(&seed, sizeof seed) ^ (counter * 2246822519u);
    words[3] = (uint32_t)crc32_of(&spin, sizeof spin) ^ (counter * 3266489917u);
    memcpy(out, words, 16);

    out[6] = (uint8_t)((out[6] & 0x0F) | 0x40);      /* version 4 */
    out[8] = (uint8_t)((out[8] & 0x3F) | 0x80);      /* variant 1 */
}

/* ------------------------------------------------------------------- helpers */

static int read_sectors(int fd, uint64_t lba, uint32_t count, void *buf) {
    if (lseek(fd, (off_t)(lba * SECTOR_SIZE), SEEK_SET) < 0) return -1;
    size_t want = (size_t)count * SECTOR_SIZE;
    size_t done = 0;
    while (done < want) {
        ssize_t n = read(fd, (char *)buf + done, want - done);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static int write_sectors(int fd, uint64_t lba, uint32_t count, const void *buf) {
    if (lseek(fd, (off_t)(lba * SECTOR_SIZE), SEEK_SET) < 0) return -1;
    size_t want = (size_t)count * SECTOR_SIZE;
    size_t done = 0;
    while (done < want) {
        ssize_t n = write(fd, (const char *)buf + done, want - done);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* ---------------------------------------------------------------- open/read */

int gpt_open(gpt_disk_t *d, const char *name) {
    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "/dev/%s", name);

    d->fd = open(d->path, O_RDWR);
    if (d->fd < 0) return -1;

    kstat_t st;
    if (stat(d->path, &st) < 0) { close(d->fd); d->fd = -1; return -1; }
    d->sector_size = SECTOR_SIZE;
    d->sectors = st.size / SECTOR_SIZE;

    return gpt_reload(d);
}

void gpt_close(gpt_disk_t *d) {
    if (d->fd >= 0) close(d->fd);
    d->fd = -1;
}

int gpt_reload(gpt_disk_t *d) {
    for (int i = 0; i < GPT_ENTRIES; i++) d->parts[i].used = false;
    d->part_count = 0;
    d->has_gpt = false;

    uint8_t header[SECTOR_SIZE];
    if (read_sectors(d->fd, 1, 1, header) < 0) return -1;
    if (memcmp(header, "EFI PART", 8)) {
        /* No table yet; offer the whole disk. */
        d->first_usable = ALIGN_SECTORS;
        d->last_usable = d->sectors > (GPT_TABLE_SECTORS + 2)
                       ? d->sectors - GPT_TABLE_SECTORS - 2 : 0;
        return 0;
    }

    d->has_gpt = true;
    memcpy(d->disk_guid, header + 56, 16);
    d->first_usable = rd64(header + 40);
    d->last_usable = rd64(header + 48);

    uint64_t table_lba = rd64(header + 72);
    uint32_t count = rd32(header + 80);
    uint32_t entry_len = rd32(header + 84);
    if (count > GPT_ENTRIES) count = GPT_ENTRIES;
    if (entry_len < GPT_ENTRY_LEN || entry_len > 512) return -1;

    uint32_t bytes = count * entry_len;
    uint32_t sectors = (bytes + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint8_t *table = malloc((size_t)sectors * SECTOR_SIZE);
    if (!table) return -1;
    if (read_sectors(d->fd, table_lba, sectors, table) < 0) { free(table); return -1; }

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = table + (size_t)i * entry_len;
        if (guid_is_zero(e)) continue;

        gpt_part_t *p = &d->parts[i];
        memcpy(p->type_guid, e, 16);
        memcpy(p->part_guid, e + 16, 16);
        p->first_lba = rd64(e + 32);
        p->last_lba = rd64(e + 40);
        p->attributes = rd64(e + 48);
        p->index = (int)i + 1;
        p->used = true;

        int n = 0;
        for (int j = 0; j < 36 && n < (int)sizeof p->name - 1; j++) {
            uint16_t c = (uint16_t)(e[56 + j * 2] | (e[57 + j * 2] << 8));
            if (!c) break;
            p->name[n++] = c < 128 ? (char)c : '?';
        }
        p->name[n] = 0;
        if ((int)i + 1 > d->part_count) d->part_count = (int)i + 1;
    }

    free(table);
    return 0;
}

/* ---------------------------------------------------------------- geometry */

uint64_t gpt_largest_gap(const gpt_disk_t *d, uint64_t *start_out) {
    /* Walk the disk in order, tracking the end of the last partition seen. */
    uint64_t best_start = 0, best_len = 0;
    uint64_t cursor = d->first_usable;

    for (;;) {
        /* Find the partition that starts next at or after the cursor. */
        const gpt_part_t *next = NULL;
        for (int i = 0; i < GPT_ENTRIES; i++) {
            const gpt_part_t *p = &d->parts[i];
            if (!p->used || p->last_lba < cursor) continue;
            if (!next || p->first_lba < next->first_lba) next = p;
        }

        uint64_t gap_end = next ? next->first_lba : d->last_usable + 1;
        if (gap_end > cursor) {
            uint64_t aligned = (cursor + ALIGN_SECTORS - 1) & ~(uint64_t)(ALIGN_SECTORS - 1);
            if (gap_end > aligned) {
                uint64_t len = gap_end - aligned;
                if (len > best_len) { best_len = len; best_start = aligned; }
            }
        }
        if (!next) break;
        cursor = next->last_lba + 1;
    }

    if (start_out) *start_out = best_start;
    return best_len;
}

void gpt_init_empty(gpt_disk_t *d) {
    memset(d->parts, 0, sizeof d->parts);
    d->part_count = 0;
    d->has_gpt = true;
    guid_generate(d->disk_guid);
    d->first_usable = 2 + GPT_TABLE_SECTORS;
    if (d->first_usable < ALIGN_SECTORS) d->first_usable = ALIGN_SECTORS;
    d->last_usable = d->sectors - GPT_TABLE_SECTORS - 2;
}

int gpt_add(gpt_disk_t *d, const uint8_t type[16], uint64_t first, uint64_t last, const char *name) {
    for (int i = 0; i < GPT_ENTRIES; i++) {
        if (d->parts[i].used) continue;

        gpt_part_t *p = &d->parts[i];
        memset(p, 0, sizeof *p);
        memcpy(p->type_guid, type, 16);
        guid_generate(p->part_guid);
        p->first_lba = first;
        p->last_lba = last;
        p->index = i + 1;
        p->used = true;
        strlcpy(p->name, name ? name : "", sizeof p->name);
        if (i + 1 > d->part_count) d->part_count = i + 1;
        return i + 1;
    }
    return -1;
}

/* ------------------------------------------------------------------- write */

static void build_header(uint8_t *h, uint64_t self, uint64_t other, uint64_t table_lba,
                         const gpt_disk_t *d, uint32_t table_crc) {
    memset(h, 0, SECTOR_SIZE);
    memcpy(h, "EFI PART", 8);
    wr32(h + 8, 0x00010000);          /* revision 1.0   */
    wr32(h + 12, 92);                 /* header size    */
    wr32(h + 16, 0);                  /* CRC, set below */
    wr32(h + 20, 0);
    wr64(h + 24, self);
    wr64(h + 32, other);
    wr64(h + 40, d->first_usable);
    wr64(h + 48, d->last_usable);
    memcpy(h + 56, d->disk_guid, 16);
    wr64(h + 72, table_lba);
    wr32(h + 80, GPT_ENTRIES);
    wr32(h + 84, GPT_ENTRY_LEN);
    wr32(h + 88, table_crc);
    wr32(h + 16, crc32_of(h, 92));
}

int gpt_write(gpt_disk_t *d) {
    uint8_t *table = calloc(GPT_ENTRIES, GPT_ENTRY_LEN);
    if (!table) return -1;

    for (int i = 0; i < GPT_ENTRIES; i++) {
        const gpt_part_t *p = &d->parts[i];
        if (!p->used) continue;
        uint8_t *e = table + (size_t)i * GPT_ENTRY_LEN;
        memcpy(e, p->type_guid, 16);
        memcpy(e + 16, p->part_guid, 16);
        wr64(e + 32, p->first_lba);
        wr64(e + 40, p->last_lba);
        wr64(e + 48, p->attributes);
        for (int j = 0; j < 36 && p->name[j]; j++) {
            e[56 + j * 2] = (uint8_t)p->name[j];
            e[57 + j * 2] = 0;
        }
    }

    uint32_t table_crc = crc32_of(table, GPT_ENTRIES * GPT_ENTRY_LEN);
    uint64_t backup_lba = d->sectors - 1;
    uint64_t backup_table = backup_lba - GPT_TABLE_SECTORS;

    /* Protective MBR: one 0xEE partition covering the disk, so anything that
     * only understands MBR sees the space as taken rather than free. */
    uint8_t mbr[SECTOR_SIZE];
    memset(mbr, 0, sizeof mbr);
    mbr[446 + 1] = 0x00; mbr[446 + 2] = 0x02; mbr[446 + 3] = 0x00;
    mbr[446 + 4] = 0xEE;
    mbr[446 + 5] = 0xFF; mbr[446 + 6] = 0xFF; mbr[446 + 7] = 0xFF;
    wr32(mbr + 446 + 8, 1);
    wr32(mbr + 446 + 12, d->sectors - 1 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)(d->sectors - 1));
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    uint8_t primary[SECTOR_SIZE], backup[SECTOR_SIZE];
    build_header(primary, 1, backup_lba, 2, d, table_crc);
    build_header(backup, backup_lba, 1, backup_table, d, table_crc);

    int result = 0;
    if (write_sectors(d->fd, 0, 1, mbr) < 0) result = -1;
    if (!result && write_sectors(d->fd, 2, GPT_TABLE_SECTORS, table) < 0) result = -1;
    if (!result && write_sectors(d->fd, backup_table, GPT_TABLE_SECTORS, table) < 0) result = -1;
    /* Headers last: until they are valid the tables are just bytes, so an
     * interruption leaves the old layout rather than a half-written new one. */
    if (!result && write_sectors(d->fd, 1, 1, primary) < 0) result = -1;
    if (!result && write_sectors(d->fd, backup_lba, 1, backup) < 0) result = -1;

    free(table);
    if (result == 0) sync();
    return result;
}
