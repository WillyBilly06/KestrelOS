/* disk.h - partition table and filesystem creation for the installer. */
#ifndef KESTREL_INSTALLER_DISK_H
#define KESTREL_INSTALLER_DISK_H

#include "kestrel.h"

#define SECTOR_SIZE   512
#define GPT_ENTRIES   128
#define GPT_ENTRY_LEN 128
#define GPT_TABLE_SECTORS ((GPT_ENTRIES * GPT_ENTRY_LEN) / SECTOR_SIZE)   /* 32 */
#define ALIGN_SECTORS 2048        /* 1 MiB, which every alignment rule likes */

typedef struct {
    uint8_t  type_guid[16];
    uint8_t  part_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    char     name[40];            /* decoded from UTF-16, ASCII only */
    int      index;               /* 1-based slot in the table */
    bool     used;
} gpt_part_t;

typedef struct {
    int         fd;
    char        path[64];
    uint64_t    sectors;
    uint32_t    sector_size;
    bool        has_gpt;
    uint8_t     disk_guid[16];
    uint64_t    first_usable;
    uint64_t    last_usable;
    gpt_part_t  parts[GPT_ENTRIES];
    int         part_count;       /* highest used slot, for display */
} gpt_disk_t;

extern const uint8_t GUID_ESP[16];
extern const uint8_t GUID_KESTREL[16];
extern const uint8_t GUID_MSDATA[16];
extern const uint8_t GUID_MSRESERVED[16];

uint32_t crc32_of(const void *data, size_t len);

void guid_text(const uint8_t g[16], char *out, size_t cap);
void guid_generate(uint8_t out[16]);
bool guid_is_zero(const uint8_t g[16]);
bool guid_same(const uint8_t a[16], const uint8_t b[16]);

/* Open a whole disk by name ("disk0") and read its partition table. */
int  gpt_open(gpt_disk_t *d, const char *name);
void gpt_close(gpt_disk_t *d);
int  gpt_reload(gpt_disk_t *d);

/* Largest run of unallocated sectors.  Returns 0 when there is none. */
uint64_t gpt_largest_gap(const gpt_disk_t *d, uint64_t *start_out);

/* Discard everything and start a fresh table. */
void gpt_init_empty(gpt_disk_t *d);

/* Add a partition, choosing the first free slot.  Returns its index or -1. */
int  gpt_add(gpt_disk_t *d, const uint8_t type[16], uint64_t first, uint64_t last, const char *name);

/* Write the protective MBR, both headers and both copies of the table. */
int  gpt_write(gpt_disk_t *d);

/* Create an empty FAT volume covering `sectors` starting at `first_lba`.
 * Only the metadata is written; files are added afterwards by mounting it. */
int  fat_format(int fd, uint64_t first_lba, uint64_t sectors, const char *label);

#endif
