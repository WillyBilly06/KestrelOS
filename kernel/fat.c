/* fat.c - FAT12/16/32 with long file name support, read and write.
 *
 * This is the filesystem the EFI system partition uses, so the installer needs
 * it to write the loader, and it is what the persistent data partition is
 * formatted with.  Directory entries are addressed as a byte stream: for FAT32
 * and for subdirectories that stream is a cluster chain, and for the FAT12/16
 * root it is the fixed root area.  One helper hides the difference.
 */
#include "kernel.h"
#include "vfs.h"
#include "block.h"
#include "mm.h"
#include "klog.h"
#include "time.h"

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LFN       0x0F

#define FAT_EOC        0x0FFFFFF8
#define FAT_BAD        0x0FFFFFF7
#define FAT_FREE       0

typedef struct __attribute__((packed)) {
    u8  jump[3];
    char oem[8];
    u16 bytes_per_sector;
    u8  sectors_per_cluster;
    u16 reserved_sectors;
    u8  num_fats;
    u16 root_entries;
    u16 total_sectors_16;
    u8  media;
    u16 fat_size_16;
    u16 sectors_per_track;
    u16 num_heads;
    u32 hidden_sectors;
    u32 total_sectors_32;
    union {
        struct __attribute__((packed)) {
            u8  drive;
            u8  reserved;
            u8  boot_sig;
            u32 volume_id;
            char label[11];
            char fs_type[8];
        } f16;
        struct __attribute__((packed)) {
            u32 fat_size_32;
            u16 ext_flags;
            u16 fs_version;
            u32 root_cluster;
            u16 fs_info;
            u16 backup_boot;
            u8  reserved[12];
            u8  drive;
            u8  reserved1;
            u8  boot_sig;
            u32 volume_id;
            char label[11];
            char fs_type[8];
        } f32;
    } ext;
} fat_bpb_t;

typedef struct __attribute__((packed)) {
    char name[11];
    u8   attr;
    u8   nt_reserved;
    u8   create_tenth;
    u16  create_time;
    u16  create_date;
    u16  access_date;
    u16  cluster_hi;
    u16  write_time;
    u16  write_date;
    u16  cluster_lo;
    u32  size;
} fat_dirent_t;

typedef struct __attribute__((packed)) {
    u8  order;
    u16 name1[5];
    u8  attr;
    u8  type;
    u8  checksum;
    u16 name2[6];
    u16 zero;
    u16 name3[2];
} fat_lfn_t;

typedef struct {
    blockdev_t *dev;
    int  type;                  /* 12, 16 or 32 */
    u32  bytes_per_sector;
    u32  sectors_per_cluster;
    u32  bytes_per_cluster;
    u32  reserved_sectors;
    u32  num_fats;
    u32  fat_size;              /* sectors per FAT */
    u32  root_entries;          /* FAT12/16 */
    u32  fat_start;
    u32  root_start;            /* FAT12/16 fixed root area */
    u32  root_sectors;
    u32  data_start;
    u32  cluster_count;
    u32  root_cluster;          /* FAT32 */
    u32  fsinfo_sector;
    u32  alloc_hint;
    char label[12];
    bool dirty;
} fat_fs_t;

typedef struct fat_node {
    fat_fs_t *fs;
    u32  first_cluster;
    u32  size;
    bool is_dir;
    bool is_fixed_root;         /* the FAT12/16 root area */

    /* Where this file's 8.3 entry lives, so size and cluster updates can be
     * written back.  Both are zero for a root directory. */
    u32  dir_first_cluster;
    bool dir_is_fixed_root;
    u32  dir_entry_index;

    /* Cache the last cluster walk to keep sequential access linear. */
    u32  walk_index;
    u32  walk_cluster;

    vnode_t *vn;
    struct fat_node *next;      /* open node list, for identity */
    u32  refs;
} fat_node_t;

static const vnode_ops_t fat_vops;

/* ------------------------------------------------------------------------- */
/* FAT table access                                                          */
/* ------------------------------------------------------------------------- */

static u32 fat_get(fat_fs_t *fs, u32 cluster) {
    if (cluster >= fs->cluster_count + 2) return FAT_EOC;

    u64 base = (u64)fs->fat_start * fs->bytes_per_sector;
    u32 value = 0;

    if (fs->type == 32) {
        u32 raw;
        if (block_read_bytes(fs->dev, base + (u64)cluster * 4, &raw, 4) < 0) return FAT_EOC;
        value = raw & 0x0FFFFFFF;
    } else if (fs->type == 16) {
        u16 raw;
        if (block_read_bytes(fs->dev, base + (u64)cluster * 2, &raw, 2) < 0) return FAT_EOC;
        value = raw;
        if (value >= 0xFFF8) value = FAT_EOC;
        else if (value == 0xFFF7) value = FAT_BAD;
    } else {
        /* FAT12 packs 1.5 bytes per entry, so a value can straddle a sector. */
        u64 off = base + (u64)cluster + (cluster / 2);
        u8 pair[2];
        if (block_read_bytes(fs->dev, off, pair, 2) < 0) return FAT_EOC;
        u16 raw = (u16)(pair[0] | (pair[1] << 8));
        value = (cluster & 1) ? (raw >> 4) : (raw & 0x0FFF);
        if (value >= 0xFF8) value = FAT_EOC;
        else if (value == 0xFF7) value = FAT_BAD;
    }
    return value;
}

static int fat_set(fat_fs_t *fs, u32 cluster, u32 value) {
    if (cluster < 2 || cluster >= fs->cluster_count + 2) return -E_INVAL;

    /* Every copy of the FAT must be kept consistent. */
    for (u32 f = 0; f < fs->num_fats; f++) {
        u64 base = (u64)(fs->fat_start + f * fs->fat_size) * fs->bytes_per_sector;

        if (fs->type == 32) {
            u32 raw;
            if (block_read_bytes(fs->dev, base + (u64)cluster * 4, &raw, 4) < 0) return -E_IO;
            raw = (raw & 0xF0000000u) | (value & 0x0FFFFFFF);
            if (block_write_bytes(fs->dev, base + (u64)cluster * 4, &raw, 4) < 0) return -E_IO;
        } else if (fs->type == 16) {
            u16 raw = (u16)value;
            if (block_write_bytes(fs->dev, base + (u64)cluster * 2, &raw, 2) < 0) return -E_IO;
        } else {
            u64 off = base + (u64)cluster + (cluster / 2);
            u8 pair[2];
            if (block_read_bytes(fs->dev, off, pair, 2) < 0) return -E_IO;
            u16 raw = (u16)(pair[0] | (pair[1] << 8));
            if (cluster & 1) raw = (u16)((raw & 0x000F) | ((value & 0x0FFF) << 4));
            else             raw = (u16)((raw & 0xF000) | (value & 0x0FFF));
            pair[0] = (u8)raw; pair[1] = (u8)(raw >> 8);
            if (block_write_bytes(fs->dev, off, pair, 2) < 0) return -E_IO;
        }
    }
    fs->dirty = true;
    return 0;
}

static bool cluster_is_end(u32 c) { return c >= FAT_EOC || c == 0; }

static u32 alloc_cluster(fat_fs_t *fs, u32 previous) {
    u32 start = fs->alloc_hint < 2 ? 2 : fs->alloc_hint;

    for (u32 pass = 0; pass < 2; pass++) {
        u32 from = pass == 0 ? start : 2;
        u32 to   = pass == 0 ? fs->cluster_count + 2 : start;
        for (u32 c = from; c < to; c++) {
            if (fat_get(fs, c) != FAT_FREE) continue;
            if (fat_set(fs, c, FAT_EOC) < 0) return 0;
            if (previous && fat_set(fs, previous, c) < 0) return 0;
            fs->alloc_hint = c + 1;

            /* A new directory cluster must start out zeroed, or stale bytes
             * would look like directory entries. */
            return c;
        }
    }
    kerr("fat", "no free clusters left on %s", fs->dev->name);
    return 0;
}

static int free_chain(fat_fs_t *fs, u32 cluster) {
    int guard = 0;
    while (!cluster_is_end(cluster) && ++guard < 10000000) {
        u32 next = fat_get(fs, cluster);
        if (fat_set(fs, cluster, FAT_FREE) < 0) return -E_IO;
        if (cluster < fs->alloc_hint) fs->alloc_hint = cluster;
        cluster = next;
    }
    return 0;
}

static u64 cluster_offset(fat_fs_t *fs, u32 cluster) {
    return (u64)(fs->data_start + (cluster - 2) * fs->sectors_per_cluster) * fs->bytes_per_sector;
}

static int zero_cluster(fat_fs_t *fs, u32 cluster) {
    u8 *zero = kzalloc(fs->bytes_per_cluster);
    if (!zero) return -E_NOMEM;
    int r = block_write_bytes(fs->dev, cluster_offset(fs, cluster), zero, fs->bytes_per_cluster);
    kfree(zero);
    return r;
}

/* ------------------------------------------------------------------------- */
/* stream access over a cluster chain or the fixed root                      */
/* ------------------------------------------------------------------------- */

/* Resolve the cluster holding byte `offset`, extending the chain if asked. */
static u32 cluster_at(fat_node_t *n, u64 offset, bool extend) {
    fat_fs_t *fs = n->fs;
    u32 want = (u32)(offset / fs->bytes_per_cluster);

    u32 cluster, index;
    if (n->walk_cluster && n->walk_index <= want) {
        cluster = n->walk_cluster;
        index = n->walk_index;
    } else {
        cluster = n->first_cluster;
        index = 0;
    }

    if (!cluster) {
        if (!extend) return 0;
        cluster = alloc_cluster(fs, 0);
        if (!cluster) return 0;
        if (n->is_dir) zero_cluster(fs, cluster);
        n->first_cluster = cluster;
        index = 0;
    }

    while (index < want) {
        u32 next = fat_get(fs, cluster);
        if (cluster_is_end(next)) {
            if (!extend) return 0;
            next = alloc_cluster(fs, cluster);
            if (!next) return 0;
            if (n->is_dir) zero_cluster(fs, next);
        }
        cluster = next;
        index++;
    }

    n->walk_index = index;
    n->walk_cluster = cluster;
    return cluster;
}

static ssize_t_k stream_io(fat_node_t *n, u64 offset, void *buf, size_t len, bool write) {
    fat_fs_t *fs = n->fs;
    u8 *p = buf;
    size_t done = 0;

    if (n->is_fixed_root) {
        u64 limit = (u64)fs->root_entries * 32;
        if (offset >= limit) return 0;
        if (offset + len > limit) len = (size_t)(limit - offset);
        u64 base = (u64)fs->root_start * fs->bytes_per_sector + offset;
        int r = write ? block_write_bytes(fs->dev, base, buf, len)
                      : block_read_bytes(fs->dev, base, buf, len);
        return r < 0 ? r : (ssize_t_k)len;
    }

    while (done < len) {
        u32 cluster = cluster_at(n, offset + done, write);
        if (!cluster) break;

        u32 within = (u32)((offset + done) % fs->bytes_per_cluster);
        size_t chunk = fs->bytes_per_cluster - within;
        if (chunk > len - done) chunk = len - done;

        u64 disk = cluster_offset(fs, cluster) + within;
        int r = write ? block_write_bytes(fs->dev, disk, p + done, chunk)
                      : block_read_bytes(fs->dev, disk, p + done, chunk);
        if (r < 0) return r;
        done += chunk;
    }
    return (ssize_t_k)done;
}

/* ------------------------------------------------------------------------- */
/* names                                                                     */
/* ------------------------------------------------------------------------- */

static u8 lfn_checksum(const char short_name[11]) {
    u8 sum = 0;
    for (int i = 0; i < 11; i++) sum = (u8)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + (u8)short_name[i]);
    return sum;
}

/* Expand "NAME    EXT" into "name.ext". */
static void short_to_name(const char raw[11], char *out, size_t cap) {
    size_t n = 0;
    for (int i = 0; i < 8 && raw[i] != ' ' && n + 1 < cap; i++) out[n++] = raw[i];
    if (raw[8] != ' ') {
        if (n + 1 < cap) out[n++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' ' && n + 1 < cap; i++) out[n++] = raw[i];
    }
    out[n] = 0;
    /* An all-uppercase short name reads better lower-cased, which is the
     * convention every FAT driver follows. */
    bool has_lower = false;
    for (size_t i = 0; i < n; i++) if (out[i] >= 'a' && out[i] <= 'z') has_lower = true;
    if (!has_lower)
        for (size_t i = 0; i < n; i++)
            if (out[i] >= 'A' && out[i] <= 'Z') out[i] = (char)(out[i] + 32);
}

static bool char_valid_short(char c) {
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    return strchr("$%'-_@~`!(){}^#&", c) != NULL;
}

static void name_to_short(const char *name, char out[11]) {
    memset(out, ' ', 11);

    const char *dot = strrchr(name, '.');
    if (dot == name) dot = NULL;      /* a leading dot is part of the name */

    size_t base_len = dot ? (size_t)(dot - name) : strlen(name);
    size_t n = 0;
    for (size_t i = 0; i < base_len && n < 8; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c == ' ' || c == '.') continue;
        out[n++] = char_valid_short(c) ? c : '_';
    }
    if (!n) out[n++] = '_';

    if (dot) {
        size_t e = 0;
        for (const char *p = dot + 1; *p && e < 3; p++) {
            char c = *p;
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            out[8 + e++] = char_valid_short(c) ? c : '_';
        }
    }
}

/* Does this name fit in 8.3 exactly, so no LFN entries are needed? */
static bool fits_short(const char *name) {
    const char *dot = strrchr(name, '.');
    if (dot == name) return false;
    size_t base = dot ? (size_t)(dot - name) : strlen(name);
    size_t ext = dot ? strlen(dot + 1) : 0;
    if (base == 0 || base > 8 || ext > 3) return false;
    for (const char *p = name; *p; p++) {
        if (*p == '.') continue;
        if (*p >= 'a' && *p <= 'z') return false;      /* lower case needs an LFN */
        if (!char_valid_short(*p)) return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* directory scanning                                                        */
/* ------------------------------------------------------------------------- */

typedef struct {
    char         name[VFS_NAME_MAX + 1];
    fat_dirent_t entry;
    u32          index;         /* index of the 8.3 entry            */
    u32          first_index;   /* index of the first LFN entry      */
} fat_found_t;

/* Read directory entries in order, assembling long names.  Returns 1 when an
 * entry was produced, 0 at the end of the directory, negative on error.
 * `*index` is advanced past the entry that was read. */
static int dir_next(fat_node_t *dir, u32 *index, fat_found_t *out) {
    u16 lfn_chars[260];
    int lfn_len = 0;
    u8  lfn_check = 0;
    u32 lfn_start = 0;
    bool have_lfn = false;

    for (;;) {
        fat_dirent_t e;
        ssize_t_k n = stream_io(dir, (u64)*index * 32, &e, 32, false);
        if (n < 0) return (int)n;
        if (n < 32) return 0;

        u8 first = (u8)e.name[0];
        if (first == 0x00) return 0;                    /* end of directory */
        if (first == 0xE5) { (*index)++; have_lfn = false; lfn_len = 0; continue; }

        if ((e.attr & ATTR_LFN) == ATTR_LFN) {
            fat_lfn_t *l = (fat_lfn_t *)&e;
            int order = l->order & 0x3F;
            if (l->order & 0x40) {                      /* last physical entry */
                lfn_len = 0;
                have_lfn = true;
                lfn_check = l->checksum;
                lfn_start = *index;
                memset(lfn_chars, 0, sizeof lfn_chars);
            }
            if (have_lfn && order >= 1 && order <= 20) {
                int base = (order - 1) * 13;
                for (int i = 0; i < 5; i++) lfn_chars[base + i] = l->name1[i];
                for (int i = 0; i < 6; i++) lfn_chars[base + 5 + i] = l->name2[i];
                for (int i = 0; i < 2; i++) lfn_chars[base + 11 + i] = l->name3[i];
                if (base + 13 > lfn_len) lfn_len = base + 13;
            }
            (*index)++;
            continue;
        }

        if (e.attr & ATTR_VOLUME_ID) { (*index)++; have_lfn = false; lfn_len = 0; continue; }

        out->entry = e;
        out->index = *index;
        out->first_index = have_lfn ? lfn_start : *index;

        if (have_lfn && lfn_checksum(e.name) == lfn_check) {
            int n2 = 0;
            for (int i = 0; i < lfn_len && n2 < VFS_NAME_MAX; i++) {
                u16 c = lfn_chars[i];
                if (c == 0 || c == 0xFFFF) break;
                /* Non-ASCII is transliterated rather than dropped, so a file
                 * still has a usable name. */
                out->name[n2++] = (c < 128) ? (char)c : '?';
            }
            out->name[n2] = 0;
            if (!n2) short_to_name(e.name, out->name, sizeof out->name);
        } else {
            short_to_name(e.name, out->name, sizeof out->name);
        }

        (*index)++;
        return 1;
    }
}

static int dir_find(fat_node_t *dir, const char *name, fat_found_t *out) {
    u32 index = 0;
    for (;;) {
        int r = dir_next(dir, &index, out);
        if (r <= 0) return r == 0 ? -E_NOENT : r;
        if (!strcasecmp(out->name, name)) return 0;
    }
}

/* Find a run of `count` consecutive free entries, extending the directory if
 * necessary.  Returns the starting index or a negative error. */
static s64 dir_find_space(fat_node_t *dir, u32 count) {
    u32 index = 0, run = 0, start = 0;

    for (;;) {
        fat_dirent_t e;
        ssize_t_k n = stream_io(dir, (u64)index * 32, &e, 32, false);
        if (n < 0) return n;
        if (n < 32) break;                    /* ran off the end */

        u8 first = (u8)e.name[0];
        if (first == 0x00 || first == 0xE5) {
            if (run == 0) start = index;
            if (++run == count) return start;
            if (first == 0x00) {
                /* Everything past here is free too, as long as it exists. */
                u32 need = count - run;
                u64 end_bytes = (u64)(index + 1 + need) * 32;
                if (dir->is_fixed_root) {
                    if (end_bytes > (u64)dir->fs->root_entries * 32) return -E_NOSPC;
                }
                return start;
            }
        } else {
            run = 0;
        }
        index++;
    }

    if (dir->is_fixed_root) return -E_NOSPC;

    /* Extend the directory by one cluster and use the start of it. */
    if (run == 0) start = index;
    u32 cluster = cluster_at(dir, (u64)index * 32, true);
    if (!cluster) return -E_NOSPC;
    zero_cluster(dir->fs, cluster);
    return start;
}

/* ------------------------------------------------------------------------- */
/* timestamps                                                                */
/* ------------------------------------------------------------------------- */

static void fat_now(u16 *date, u16 *time) {
    datetime_t t;
    rtc_read(&t);
    if (t.year < 1980) t.year = 1980;
    *date = (u16)(((t.year - 1980) << 9) | (t.month << 5) | t.day);
    *time = (u16)((t.hour << 11) | (t.minute << 5) | (t.second / 2));
}

static u64 fat_to_unix(u16 date, u16 time) {
    static const u16 cumulative[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    u32 year = 1980 + ((date >> 9) & 0x7F);
    u32 month = (date >> 5) & 0x0F;
    u32 day = date & 0x1F;
    if (month < 1) month = 1;
    if (day < 1) day = 1;

    u64 days = 0;
    for (u32 y = 1970; y < year; y++) days += ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365;
    days += cumulative[(month - 1) % 12];
    if (month > 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) days++;
    days += day - 1;

    return days * 86400ULL + ((time >> 11) & 0x1F) * 3600ULL + ((time >> 5) & 0x3F) * 60ULL + (time & 0x1F) * 2ULL;
}

/* ------------------------------------------------------------------------- */
/* node bookkeeping                                                          */
/* ------------------------------------------------------------------------- */

static u32 entry_cluster(const fat_dirent_t *e) {
    return ((u32)e->cluster_hi << 16) | e->cluster_lo;
}

/* Write the file's current size and first cluster back into its 8.3 entry. */
static int flush_metadata(fat_node_t *n) {
    if (!n->dir_first_cluster && !n->dir_is_fixed_root) return 0;   /* a root */

    fat_node_t dir = {0};
    dir.fs = n->fs;
    dir.first_cluster = n->dir_first_cluster;
    dir.is_dir = true;
    dir.is_fixed_root = n->dir_is_fixed_root;

    fat_dirent_t e;
    ssize_t_k r = stream_io(&dir, (u64)n->dir_entry_index * 32, &e, 32, false);
    if (r < 32) return -E_IO;

    e.size = n->is_dir ? 0 : n->size;
    e.cluster_lo = (u16)(n->first_cluster & 0xFFFF);
    e.cluster_hi = (u16)(n->first_cluster >> 16);
    fat_now(&e.write_date, &e.write_time);
    e.access_date = e.write_date;
    e.attr |= ATTR_ARCHIVE;

    r = stream_io(&dir, (u64)n->dir_entry_index * 32, &e, 32, true);
    return r == 32 ? 0 : -E_IO;
}

static vnode_t *make_vnode(filesystem_t *fs, fat_node_t *n) {
    if (n->vn) { n->vn->size = n->size; return vnode_ref(n->vn); }

    vnode_t *vn = kzalloc(sizeof *vn);
    if (!vn) return NULL;
    vn->type = n->is_dir ? VN_DIR : VN_FILE;
    vn->size = n->size;
    vn->priv = n;
    vn->fs   = fs;
    vn->ops  = &fat_vops;
    vn->refs = 1;
    n->vn = vn;
    return vn;
}

/* ------------------------------------------------------------------------- */
/* vnode operations                                                          */
/* ------------------------------------------------------------------------- */

static ssize_t_k fat_read(vnode_t *vn, void *buf, size_t len, u64 off) {
    fat_node_t *n = vn->priv;
    if (!n) return -E_NOENT;
    if (n->is_dir) return -E_ISDIR;
    if (off >= n->size) return 0;
    if (off + len > n->size) len = (size_t)(n->size - off);
    return stream_io(n, off, buf, len, false);
}

static ssize_t_k fat_write(vnode_t *vn, const void *buf, size_t len, u64 off) {
    fat_node_t *n = vn->priv;
    if (!n) return -E_NOENT;
    if (n->is_dir) return -E_ISDIR;
    if (vn->fs->readonly) return -E_ROFS;
    if (off + len > 0xFFFFFFFFULL) return -E_NOSPC;     /* FAT size field is 32-bit */

    /* Writing past the end must leave zeros, not whatever the clusters held. */
    if (off > n->size) {
        u64 gap = off - n->size;
        u8 zeros[512];
        memset(zeros, 0, sizeof zeros);
        u64 at = n->size;
        while (gap) {
            size_t chunk = gap > sizeof zeros ? sizeof zeros : (size_t)gap;
            ssize_t_k w = stream_io(n, at, zeros, chunk, true);
            if (w <= 0) return w < 0 ? w : -E_NOSPC;
            at += (u64)w;
            gap -= (u64)w;
        }
    }

    ssize_t_k w = stream_io(n, off, (void *)buf, len, true);
    if (w < 0) return w;
    if (w == 0 && len) return -E_NOSPC;

    if (off + (u64)w > n->size) {
        n->size = (u32)(off + (u64)w);
        vn->size = n->size;
    }
    int r = flush_metadata(n);
    if (r < 0) return r;
    return w;
}

static int fat_truncate(vnode_t *vn, u64 size) {
    fat_node_t *n = vn->priv;
    if (!n) return -E_NOENT;
    if (n->is_dir) return -E_ISDIR;
    if (vn->fs->readonly) return -E_ROFS;
    if (size > 0xFFFFFFFFULL) return -E_INVAL;

    if (size < n->size) {
        u32 keep = (u32)((size + n->fs->bytes_per_cluster - 1) / n->fs->bytes_per_cluster);
        if (keep == 0) {
            if (n->first_cluster) free_chain(n->fs, n->first_cluster);
            n->first_cluster = 0;
        } else {
            /* Walk to the last cluster we keep and cut the chain after it. */
            u32 c = n->first_cluster;
            for (u32 i = 1; i < keep && !cluster_is_end(c); i++) c = fat_get(n->fs, c);
            if (!cluster_is_end(c)) {
                u32 rest = fat_get(n->fs, c);
                fat_set(n->fs, c, FAT_EOC);
                if (!cluster_is_end(rest)) free_chain(n->fs, rest);
            }
        }
        n->walk_cluster = 0;
        n->walk_index = 0;
    } else if (size > n->size) {
        /* Extending is the same as writing zeros to the new end. */
        u64 gap = size - n->size;
        u8 zeros[512];
        memset(zeros, 0, sizeof zeros);
        u64 at = n->size;
        while (gap) {
            size_t chunk = gap > sizeof zeros ? sizeof zeros : (size_t)gap;
            ssize_t_k w = stream_io(n, at, zeros, chunk, true);
            if (w <= 0) return w < 0 ? (int)w : -E_NOSPC;
            at += (u64)w;
            gap -= (u64)w;
        }
    }

    n->size = (u32)size;
    vn->size = n->size;
    return flush_metadata(n);
}

static int fat_readdir(vnode_t *vn, u32 index, dirent_k *out) {
    fat_node_t *n = vn->priv;
    if (!n) return -E_NOENT;
    if (!n->is_dir) return -E_NOTDIR;

    u32 pos = 0;
    fat_found_t found;
    for (u32 seen = 0;; seen++) {
        int r = dir_next(n, &pos, &found);
        if (r <= 0) return r == 0 ? -E_NOENT : r;
        if (seen != index) continue;

        strlcpy(out->name, found.name, sizeof out->name);
        out->type = (found.entry.attr & ATTR_DIRECTORY) ? VN_DIR : VN_FILE;
        out->size = found.entry.size;
        return 0;
    }
}

static int fat_lookup(vnode_t *vn, const char *name, vnode_t **out) {
    fat_node_t *n = vn->priv;
    if (!n) return -E_NOENT;
    if (!n->is_dir) return -E_NOTDIR;

    /* "." and ".." are stored on disk for subdirectories, but resolving them
     * here would produce a node whose parent pointers are wrong; the VFS has
     * already removed them during normalisation. */
    fat_found_t found;
    int r = dir_find(n, name, &found);
    if (r < 0) return r;

    fat_node_t *c = kzalloc(sizeof *c);
    if (!c) return -E_NOMEM;
    c->fs = n->fs;
    c->first_cluster = entry_cluster(&found.entry);
    c->size = found.entry.size;
    c->is_dir = (found.entry.attr & ATTR_DIRECTORY) != 0;
    c->dir_first_cluster = n->first_cluster;
    c->dir_is_fixed_root = n->is_fixed_root;
    c->dir_entry_index = found.index;

    vnode_t *cv = make_vnode(vn->fs, c);
    if (!cv) { kfree(c); return -E_NOMEM; }
    *out = cv;
    return 0;
}

/* Write the 8.3 entry plus any LFN entries for `name` at `start`. */
static int write_entry(fat_node_t *dir, u32 start, const char *name,
                       const char short_name[11], u8 attr, u32 cluster, u32 size) {
    size_t len = strlen(name);
    u32 lfn_count = fits_short(name) ? 0 : (u32)((len + 12) / 13);

    if (lfn_count) {
        u8 sum = lfn_checksum(short_name);
        for (u32 i = 0; i < lfn_count; i++) {
            fat_lfn_t l;
            memset(&l, 0xFF, sizeof l);
            u32 order = lfn_count - i;              /* stored last-part-first */
            l.order = (u8)(order | (i == 0 ? 0x40 : 0));
            l.attr = ATTR_LFN;
            l.type = 0;
            l.checksum = sum;
            l.zero = 0;

            u32 base = (order - 1) * 13;
            u16 chars[13];
            for (int j = 0; j < 13; j++) {
                u32 k = base + (u32)j;
                if (k < len) chars[j] = (u16)(u8)name[k];
                else if (k == len) chars[j] = 0;
                else chars[j] = 0xFFFF;
            }
            for (int j = 0; j < 5; j++) l.name1[j] = chars[j];
            for (int j = 0; j < 6; j++) l.name2[j] = chars[5 + j];
            for (int j = 0; j < 2; j++) l.name3[j] = chars[11 + j];

            if (stream_io(dir, (u64)(start + i) * 32, &l, 32, true) != 32) return -E_IO;
        }
    }

    fat_dirent_t e;
    memset(&e, 0, sizeof e);
    memcpy(e.name, short_name, 11);
    e.attr = attr;
    e.cluster_lo = (u16)(cluster & 0xFFFF);
    e.cluster_hi = (u16)(cluster >> 16);
    e.size = size;
    fat_now(&e.create_date, &e.create_time);
    e.write_date = e.create_date;
    e.write_time = e.create_time;
    e.access_date = e.create_date;

    if (stream_io(dir, (u64)(start + lfn_count) * 32, &e, 32, true) != 32) return -E_IO;
    return (int)(start + lfn_count);
}

/* Pick a short name that no existing entry uses. */
static int unique_short_name(fat_node_t *dir, const char *name, char out[11]) {
    name_to_short(name, out);
    if (fits_short(name)) return 0;

    for (int attempt = 1; attempt < 1000; attempt++) {
        char suffix[8];
        int slen = snprintf(suffix, sizeof suffix, "~%d", attempt);
        char candidate[11];
        memcpy(candidate, out, 11);
        int at = 8 - slen;
        if (at < 1) at = 1;
        for (int i = 0; i < slen; i++) candidate[at + i] = suffix[i];

        char text[16];
        short_to_name(candidate, text, sizeof text);

        fat_found_t found;
        if (dir_find(dir, text, &found) == -E_NOENT) {
            memcpy(out, candidate, 11);
            return 0;
        }
    }
    return -E_EXIST;
}

static int fat_create(vnode_t *vn, const char *name, u32 type, vnode_t **out) {
    fat_node_t *n = vn->priv;
    if (!n) return -E_NOENT;
    if (!n->is_dir) return -E_NOTDIR;
    if (vn->fs->readonly) return -E_ROFS;
    if (!name[0] || strlen(name) > VFS_NAME_MAX) return -E_INVAL;

    fat_found_t existing;
    if (dir_find(n, name, &existing) == 0) return -E_EXIST;

    char short_name[11];
    int r = unique_short_name(n, name, short_name);
    if (r < 0) return r;

    size_t len = strlen(name);
    u32 lfn_count = fits_short(name) ? 0 : (u32)((len + 12) / 13);
    s64 start = dir_find_space(n, lfn_count + 1);
    if (start < 0) return (int)start;

    u32 cluster = 0;
    if (type == VN_DIR) {
        cluster = alloc_cluster(n->fs, 0);
        if (!cluster) return -E_NOSPC;
        if (zero_cluster(n->fs, cluster) < 0) return -E_IO;

        /* Every subdirectory starts with "." and "..". */
        fat_node_t child = {0};
        child.fs = n->fs;
        child.first_cluster = cluster;
        child.is_dir = true;

        fat_dirent_t dot;
        memset(&dot, 0, sizeof dot);
        memcpy(dot.name, ".          ", 11);
        dot.attr = ATTR_DIRECTORY;
        dot.cluster_lo = (u16)(cluster & 0xFFFF);
        dot.cluster_hi = (u16)(cluster >> 16);
        fat_now(&dot.create_date, &dot.create_time);
        dot.write_date = dot.create_date;
        dot.write_time = dot.create_time;
        if (stream_io(&child, 0, &dot, 32, true) != 32) return -E_IO;

        memcpy(dot.name, "..         ", 11);
        /* ".." in a child of the root must reference cluster 0. */
        u32 parent_cluster = n->is_fixed_root ? 0 : n->first_cluster;
        if (n->fs->type == 32 && parent_cluster == n->fs->root_cluster) parent_cluster = 0;
        dot.cluster_lo = (u16)(parent_cluster & 0xFFFF);
        dot.cluster_hi = (u16)(parent_cluster >> 16);
        if (stream_io(&child, 32, &dot, 32, true) != 32) return -E_IO;
    }

    int entry_index = write_entry(n, (u32)start, name, short_name,
                                  type == VN_DIR ? ATTR_DIRECTORY : ATTR_ARCHIVE, cluster, 0);
    if (entry_index < 0) {
        if (cluster) free_chain(n->fs, cluster);
        return entry_index;
    }

    if (out) {
        fat_node_t *c = kzalloc(sizeof *c);
        if (!c) return -E_NOMEM;
        c->fs = n->fs;
        c->first_cluster = cluster;
        c->size = 0;
        c->is_dir = (type == VN_DIR);
        c->dir_first_cluster = n->first_cluster;
        c->dir_is_fixed_root = n->is_fixed_root;
        c->dir_entry_index = (u32)entry_index;

        vnode_t *cv = make_vnode(vn->fs, c);
        if (!cv) { kfree(c); return -E_NOMEM; }
        *out = cv;
    }
    n->fs->dirty = true;
    return 0;
}

static int fat_unlink(vnode_t *vn, const char *name) {
    fat_node_t *n = vn->priv;
    if (!n) return -E_NOENT;
    if (vn->fs->readonly) return -E_ROFS;

    fat_found_t found;
    int r = dir_find(n, name, &found);
    if (r < 0) return r;

    if (found.entry.attr & ATTR_DIRECTORY) {
        /* Refuse unless the directory holds nothing but "." and "..". */
        fat_node_t child = {0};
        child.fs = n->fs;
        child.first_cluster = entry_cluster(&found.entry);
        child.is_dir = true;

        u32 pos = 0;
        fat_found_t inner;
        for (;;) {
            int q = dir_next(&child, &pos, &inner);
            if (q <= 0) break;
            if (!strcmp(inner.name, ".") || !strcmp(inner.name, "..")) continue;
            return -E_NOTEMPTY;
        }
    }

    /* Mark the 8.3 entry and every LFN entry ahead of it as deleted. */
    for (u32 i = found.first_index; i <= found.index; i++) {
        u8 marker = 0xE5;
        if (stream_io(n, (u64)i * 32 + 0, &marker, 1, true) != 1) return -E_IO;
    }

    u32 cluster = entry_cluster(&found.entry);
    if (cluster) free_chain(n->fs, cluster);
    n->fs->dirty = true;
    return 0;
}

static int fat_rename(vnode_t *vn, const char *from, const char *to) {
    fat_node_t *n = vn->priv;
    if (!n) return -E_NOENT;
    if (vn->fs->readonly) return -E_ROFS;

    fat_found_t src;
    int r = dir_find(n, from, &src);
    if (r < 0) return r;

    fat_found_t clash;
    if (dir_find(n, to, &clash) == 0) return -E_EXIST;

    char short_name[11];
    r = unique_short_name(n, to, short_name);
    if (r < 0) return r;

    size_t len = strlen(to);
    u32 lfn_count = fits_short(to) ? 0 : (u32)((len + 12) / 13);
    s64 start = dir_find_space(n, lfn_count + 1);
    if (start < 0) return (int)start;

    int idx = write_entry(n, (u32)start, to, short_name, src.entry.attr,
                          entry_cluster(&src.entry), src.entry.size);
    if (idx < 0) return idx;

    /* Only now remove the old name, so a failure above leaves the file intact. */
    for (u32 i = src.first_index; i <= src.index; i++) {
        u8 marker = 0xE5;
        if (stream_io(n, (u64)i * 32, &marker, 1, true) != 1) return -E_IO;
    }
    n->fs->dirty = true;
    return 0;
}

static int fat_stat(vnode_t *vn, vstat_t *st) {
    fat_node_t *n = vn->priv;
    if (!n) return -E_NOENT;

    st->type = n->is_dir ? VN_DIR : VN_FILE;
    st->size = n->size;
    st->mode = n->is_dir ? 0755 : 0644;
    st->mtime = 0;

    if (n->dir_first_cluster || n->dir_is_fixed_root) {
        fat_node_t dir = {0};
        dir.fs = n->fs;
        dir.first_cluster = n->dir_first_cluster;
        dir.is_dir = true;
        dir.is_fixed_root = n->dir_is_fixed_root;

        fat_dirent_t e;
        if (stream_io(&dir, (u64)n->dir_entry_index * 32, &e, 32, false) == 32)
            st->mtime = fat_to_unix(e.write_date, e.write_time);
    }
    return 0;
}

static int fat_sync_node(vnode_t *vn) {
    fat_node_t *n = vn->priv;
    if (!n) return 0;
    int r = flush_metadata(n);
    block_flush(n->fs->dev);
    return r;
}

static void fat_release(vnode_t *vn) {
    fat_node_t *n = vn->priv;
    if (n) { n->vn = NULL; kfree(n); }
    kfree(vn);
}

static const vnode_ops_t fat_vops = {
    .read     = fat_read,
    .write    = fat_write,
    .truncate = fat_truncate,
    .readdir  = fat_readdir,
    .lookup   = fat_lookup,
    .create   = fat_create,
    .unlink   = fat_unlink,
    .rename   = fat_rename,
    .stat     = fat_stat,
    .sync     = fat_sync_node,
    .release  = fat_release,
};

/* ------------------------------------------------------------------------- */
/* mount                                                                     */
/* ------------------------------------------------------------------------- */

static u64 fat_free_bytes(filesystem_t *fs) {
    fat_fs_t *f = fs->priv;
    u64 free = 0;
    for (u32 c = 2; c < f->cluster_count + 2; c++)
        if (fat_get(f, c) == FAT_FREE) free++;
    return free * f->bytes_per_cluster;
}

static u64 fat_total_bytes(filesystem_t *fs) {
    fat_fs_t *f = fs->priv;
    return (u64)f->cluster_count * f->bytes_per_cluster;
}

static int fat_sync_fs(filesystem_t *fs) {
    fat_fs_t *f = fs->priv;
    if (f->dirty) { block_flush(f->dev); f->dirty = false; }
    return 0;
}

static int fat_unmount_fs(filesystem_t *fs) {
    fat_sync_fs(fs);
    fat_fs_t *f = fs->priv;
    if (fs->root) { kfree(fs->root->priv); kfree(fs->root); }
    kfree(f);
    kfree(fs);
    return 0;
}

filesystem_t *fat_probe(blockdev_t *dev) {
    /* One sector, and a sector can be four kilobytes.  Kept out of the frame
     * because probing happens with the mount path below it on the same kernel
     * stack. */
    static u8 sector[4096];
    if (dev->sector_size > sizeof sector) return NULL;
    if (block_read(dev, 0, 1, sector) < 0) return NULL;

    fat_bpb_t *b = (fat_bpb_t *)sector;

    /* Sanity-check the BPB before believing any of it. */
    if (sector[510] != 0x55 || sector[511] != 0xAA) return NULL;
    if (b->bytes_per_sector != 512 && b->bytes_per_sector != 1024 &&
        b->bytes_per_sector != 2048 && b->bytes_per_sector != 4096) return NULL;
    if (b->sectors_per_cluster == 0 || (b->sectors_per_cluster & (b->sectors_per_cluster - 1))) return NULL;
    if (b->num_fats == 0 || b->num_fats > 4) return NULL;
    if (b->reserved_sectors == 0) return NULL;

    fat_fs_t *f = kzalloc(sizeof *f);
    if (!f) return NULL;

    f->dev = dev;
    f->bytes_per_sector = b->bytes_per_sector;
    f->sectors_per_cluster = b->sectors_per_cluster;
    f->bytes_per_cluster = f->bytes_per_sector * f->sectors_per_cluster;
    f->reserved_sectors = b->reserved_sectors;
    f->num_fats = b->num_fats;
    f->root_entries = b->root_entries;
    f->fat_size = b->fat_size_16 ? b->fat_size_16 : b->ext.f32.fat_size_32;

    u32 total = b->total_sectors_16 ? b->total_sectors_16 : b->total_sectors_32;
    if (!f->fat_size || !total) { kfree(f); return NULL; }

    f->root_sectors = ((u32)f->root_entries * 32 + f->bytes_per_sector - 1) / f->bytes_per_sector;
    f->fat_start = f->reserved_sectors;
    f->root_start = f->fat_start + f->num_fats * f->fat_size;
    f->data_start = f->root_start + f->root_sectors;

    if (f->data_start >= total) { kfree(f); return NULL; }
    f->cluster_count = (total - f->data_start) / f->sectors_per_cluster;

    /* The cluster count is what actually determines the FAT width. */
    if (f->cluster_count < 4085) f->type = 12;
    else if (f->cluster_count < 65525) f->type = 16;
    else f->type = 32;

    if (f->type == 32) {
        f->root_cluster = b->ext.f32.root_cluster;
        f->fsinfo_sector = b->ext.f32.fs_info;
        if (f->root_cluster < 2) { kfree(f); return NULL; }
        memcpy(f->label, b->ext.f32.label, 11);
    } else {
        memcpy(f->label, b->ext.f16.label, 11);
    }
    f->label[11] = 0;
    for (int i = 10; i >= 0 && (f->label[i] == ' ' || !f->label[i]); i--) f->label[i] = 0;

    /* Seed the allocation hint from FSInfo when it looks trustworthy. */
    f->alloc_hint = 2;
    if (f->type == 32 && f->fsinfo_sector) {
        u8 fsi[4096];
        if (f->bytes_per_sector <= sizeof fsi && block_read(dev, f->fsinfo_sector, 1, fsi) == 0) {
            if (*(u32 *)fsi == 0x41615252 && *(u32 *)(fsi + 484) == 0x61417272) {
                u32 next = *(u32 *)(fsi + 492);
                if (next >= 2 && next < f->cluster_count + 2) f->alloc_hint = next;
            }
        }
    }

    filesystem_t *fs = kzalloc(sizeof *fs);
    fat_node_t *root = kzalloc(sizeof *root);
    if (!fs || !root) { kfree(fs); kfree(root); kfree(f); return NULL; }

    snprintf(fs->name, sizeof fs->name, "fat%d", f->type);
    fs->dev = dev;
    fs->priv = f;
    fs->readonly = dev->readonly;
    fs->unmount = fat_unmount_fs;
    fs->sync = fat_sync_fs;
    fs->free_bytes = fat_free_bytes;
    fs->total_bytes = fat_total_bytes;

    root->fs = f;
    root->is_dir = true;
    if (f->type == 32) {
        root->first_cluster = f->root_cluster;
    } else {
        root->is_fixed_root = true;
        root->first_cluster = 0;
    }

    fs->root = make_vnode(fs, root);
    if (!fs->root) { kfree(root); kfree(fs); kfree(f); return NULL; }

    kinfo("fat", "%s: FAT%d, %u clusters of %u bytes, %u MiB%s%s",
          dev->name, f->type, f->cluster_count, f->bytes_per_cluster,
          (unsigned)(fat_total_bytes(fs) / (1024 * 1024)),
          f->label[0] ? ", label " : "", f->label[0] ? f->label : "");
    return fs;
}

void fat_register(void) { vfs_register_fs("fat", fat_probe); }
