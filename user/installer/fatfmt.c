/* fatfmt.c - create an empty FAT16/FAT32 volume.
 *
 * Only the metadata is written here.  Once the kernel re-reads the partition
 * table it can mount the result, and files are copied in through the ordinary
 * filesystem calls - which means the driver that will read the volume at boot
 * is also the one that populated it.
 */
#include "disk.h"

#define FAT12_SAFE_MAX  4000
#define FAT16_SAFE_MIN  4200
#define FAT16_SAFE_MAX  64000
#define FAT32_SAFE_MIN  68000

typedef struct {
    int      type;                /* 16 or 32 */
    uint32_t sectors_per_cluster;
    uint32_t reserved;
    uint32_t num_fats;
    uint32_t fat_sectors;
    uint32_t root_entries;
    uint32_t root_sectors;
    uint32_t cluster_count;
    uint32_t root_cluster;
} fat_geom_t;

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* Solve for the cluster count: the FAT's own size depends on it, so iterate
 * until the two agree. */
static bool estimate(uint64_t total, uint32_t spc, int type, fat_geom_t *g) {
    uint32_t reserved = (type == 32) ? 32 : 1;
    uint32_t root_entries = (type == 32) ? 0 : 512;
    uint32_t root_sectors = (root_entries * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint32_t bits = (type == 32) ? 32 : 16;
    uint32_t num_fats = 2;

    uint32_t fat_sectors = 1;
    for (int i = 0; i < 64; i++) {
        int64_t data = (int64_t)total - reserved - root_sectors - (int64_t)num_fats * fat_sectors;
        if (data <= 0) return false;
        uint64_t count = (uint64_t)data / spc;
        uint64_t need_bytes = ((count + 2) * bits + 7) / 8;
        uint32_t need = (uint32_t)((need_bytes + SECTOR_SIZE - 1) / SECTOR_SIZE);
        if (need == fat_sectors) break;
        fat_sectors = need;
    }

    int64_t data = (int64_t)total - reserved - root_sectors - (int64_t)num_fats * fat_sectors;
    if (data <= 0) return false;

    g->type = type;
    g->sectors_per_cluster = spc;
    g->reserved = reserved;
    g->num_fats = num_fats;
    g->fat_sectors = fat_sectors;
    g->root_entries = root_entries;
    g->root_sectors = root_sectors;
    g->cluster_count = (uint32_t)((uint64_t)data / spc);
    g->root_cluster = (type == 32) ? 2 : 0;
    return true;
}

/* Choose a geometry that puts the cluster count well inside the chosen type's
 * range.  Sitting a cluster or two from a boundary invites a driver that
 * rounds differently to read the volume as the wrong FAT width. */
static bool choose_geometry(uint64_t total, fat_geom_t *g) {
    static const uint32_t sizes[] = { 1, 2, 4, 8, 16, 32, 64 };

    bool found = false;
    fat_geom_t best;
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        fat_geom_t t;
        if (!estimate(total, sizes[i], 32, &t)) continue;
        if (t.cluster_count < FAT32_SAFE_MIN || t.cluster_count > 268435444) continue;
        best = t;                       /* keep the largest cluster that fits */
        found = true;
    }
    if (found) { *g = best; return true; }

    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        fat_geom_t t;
        if (!estimate(total, sizes[i], 16, &t)) continue;
        if (t.cluster_count < FAT16_SAFE_MIN || t.cluster_count > FAT16_SAFE_MAX) continue;
        best = t;
        found = true;
    }
    if (found) { *g = best; return true; }
    return false;
}

static int write_at(int fd, uint64_t lba, const void *buf, size_t len) {
    if (lseek(fd, (off_t)(lba * SECTOR_SIZE), SEEK_SET) < 0) return -1;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, (const char *)buf + done, len - done);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static int zero_range(int fd, uint64_t lba, uint64_t sectors) {
    size_t chunk_sectors = 256;
    uint8_t *zeros = calloc(chunk_sectors, SECTOR_SIZE);
    if (!zeros) return -1;

    while (sectors) {
        uint64_t n = sectors > chunk_sectors ? chunk_sectors : sectors;
        if (write_at(fd, lba, zeros, (size_t)n * SECTOR_SIZE) < 0) { free(zeros); return -1; }
        lba += n;
        sectors -= n;
    }
    free(zeros);
    return 0;
}

int fat_format(int fd, uint64_t first_lba, uint64_t sectors, const char *label) {
    fat_geom_t g;
    if (!choose_geometry(sectors, &g)) {
        errno = EINVAL;
        return -1;
    }

    char padded[12];
    memset(padded, ' ', 11);
    padded[11] = 0;
    for (int i = 0; i < 11 && label && label[i]; i++) {
        char c = label[i];
        padded[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }

    /* Boot sector. */
    uint8_t boot[SECTOR_SIZE];
    memset(boot, 0, sizeof boot);
    boot[0] = 0xEB;
    boot[1] = (g.type == 32) ? 0x58 : 0x3C;
    boot[2] = 0x90;
    memcpy(boot + 3, "KESTREL ", 8);

    put16(boot + 11, SECTOR_SIZE);
    boot[13] = (uint8_t)g.sectors_per_cluster;
    put16(boot + 14, (uint16_t)g.reserved);
    boot[16] = (uint8_t)g.num_fats;
    put16(boot + 17, (uint16_t)g.root_entries);
    put16(boot + 19, sectors > 0xFFFF ? 0 : (uint16_t)sectors);
    boot[21] = 0xF8;
    put16(boot + 22, (g.type == 32) ? 0 : (uint16_t)g.fat_sectors);
    put16(boot + 24, 63);
    put16(boot + 26, 255);
    put32(boot + 28, (uint32_t)first_lba);           /* hidden sectors */
    put32(boot + 32, sectors > 0xFFFF ? (uint32_t)sectors : 0);

    if (g.type == 32) {
        put32(boot + 36, g.fat_sectors);
        put16(boot + 40, 0);                          /* mirror every FAT */
        put16(boot + 42, 0);                          /* version          */
        put32(boot + 44, g.root_cluster);
        put16(boot + 48, 1);                          /* FSInfo sector    */
        put16(boot + 50, 6);                          /* backup boot      */
        boot[64] = 0x80;
        boot[66] = 0x29;
        put32(boot + 67, 0x4B455354);
        memcpy(boot + 71, padded, 11);
        memcpy(boot + 82, "FAT32   ", 8);
    } else {
        boot[36] = 0x80;
        boot[38] = 0x29;
        put32(boot + 39, 0x4B455354);
        memcpy(boot + 43, padded, 11);
        memcpy(boot + 54, "FAT16   ", 8);
    }
    boot[510] = 0x55;
    boot[511] = 0xAA;

    /* Clear the reserved area, the FATs and the root directory.  Stale bytes
     * left there would be read as directory entries or chain links. */
    uint64_t meta_sectors = g.reserved + (uint64_t)g.num_fats * g.fat_sectors + g.root_sectors;
    if (g.type == 32) meta_sectors += g.sectors_per_cluster;      /* the root cluster */
    if (zero_range(fd, first_lba, meta_sectors) < 0) return -1;

    if (write_at(fd, first_lba, boot, SECTOR_SIZE) < 0) return -1;

    if (g.type == 32) {
        uint8_t fsinfo[SECTOR_SIZE];
        memset(fsinfo, 0, sizeof fsinfo);
        put32(fsinfo + 0, 0x41615252);
        put32(fsinfo + 484, 0x61417272);
        put32(fsinfo + 488, g.cluster_count - 1);      /* free clusters   */
        put32(fsinfo + 492, 3);                        /* next free hint  */
        fsinfo[510] = 0x55;
        fsinfo[511] = 0xAA;

        if (write_at(fd, first_lba + 1, fsinfo, SECTOR_SIZE) < 0) return -1;
        if (write_at(fd, first_lba + 6, boot, SECTOR_SIZE) < 0) return -1;
        if (write_at(fd, first_lba + 7, fsinfo, SECTOR_SIZE) < 0) return -1;
    }

    /* The first two FAT entries are reserved; on FAT32 the root cluster is
     * also marked as the end of its chain. */
    uint8_t fat_start[SECTOR_SIZE];
    memset(fat_start, 0, sizeof fat_start);
    if (g.type == 32) {
        put32(fat_start + 0, 0x0FFFFFF8);
        put32(fat_start + 4, 0x0FFFFFFF);
        put32(fat_start + 8, 0x0FFFFFFF);              /* cluster 2: the root */
    } else {
        put16(fat_start + 0, 0xFFF8);
        put16(fat_start + 2, 0xFFFF);
    }
    for (uint32_t i = 0; i < g.num_fats; i++) {
        uint64_t lba = first_lba + g.reserved + (uint64_t)i * g.fat_sectors;
        if (write_at(fd, lba, fat_start, SECTOR_SIZE) < 0) return -1;
    }

    /* The volume label lives in the root directory as its own entry. */
    uint8_t root[SECTOR_SIZE];
    memset(root, 0, sizeof root);
    memcpy(root, padded, 11);
    root[11] = 0x08;                                   /* volume id attribute */

    uint64_t root_lba;
    if (g.type == 32) {
        root_lba = first_lba + g.reserved + (uint64_t)g.num_fats * g.fat_sectors;
    } else {
        root_lba = first_lba + g.reserved + (uint64_t)g.num_fats * g.fat_sectors;
    }
    if (write_at(fd, root_lba, root, SECTOR_SIZE) < 0) return -1;

    sync();
    return g.type;
}
