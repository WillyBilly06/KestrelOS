#ifndef KESTREL_BLOCK_H
#define KESTREL_BLOCK_H

#include "kernel.h"
#include "vfs.h"

#define BLOCK_NAME_MAX 24
#define GUID_LEN 16

typedef struct blockdev blockdev_t;

typedef struct block_ops {
    int (*read)(blockdev_t *dev, u64 lba, u32 count, void *buf);
    int (*write)(blockdev_t *dev, u64 lba, u32 count, const void *buf);
    int (*flush)(blockdev_t *dev);
} block_ops_t;

struct blockdev {
    char  name[BLOCK_NAME_MAX];      /* disk0, disk0p1, ... */
    char  model[48];
    u32   sector_size;
    u64   sector_count;
    bool  readonly;
    bool  removable;

    const block_ops_t *ops;
    void *priv;

    /* Serialises the driver on a physical disk.  The sector cache has its own
     * lock, but the driver call (read/write/flush) escapes it on the flush and
     * large-transfer paths - so without this two threads could enter the same
     * usbmsc/nvme driver at once and corrupt a transfer.  Held only on the
     * PARENT (whole-disk) device; partitions share their parent's. */
    volatile u32 io_held;

    /* Set for partitions. */
    blockdev_t *parent;
    u64         lba_offset;
    u32         part_index;
    u8          part_type_guid[GUID_LEN];
    u8          part_guid[GUID_LEN];
    char        part_label[40];

    blockdev_t *next;
};

void        block_init(void);
blockdev_t *block_register(const char *name, const block_ops_t *ops, void *priv,
                           u32 sector_size, u64 sector_count, const char *model);
void        block_unregister(blockdev_t *dev);
blockdev_t *block_find(const char *name);

/* The next free "diskN" number.  Shared by every storage driver so that two
 * controllers cannot both name a disk "disk0". */
int  block_next_disk_index(void);
void block_release_disk_index(void);
blockdev_t *block_first(void);

/* Log why there is or is not a disk to keep a log on, derived from what
 * the drivers still hold rather than recovered from the log ring. */
void storage_report_verdict(void);

/* Filesystem drivers announce a volume's unchanging facts only the first time
 * it is probed; repeat probes say nothing. */
bool block_described(blockdev_t *dev);
void block_mark_described(blockdev_t *dev);
int         block_count(void);
int         block_disk_count(void);

int block_read(blockdev_t *dev, u64 lba, u32 count, void *buf);
int block_write(blockdev_t *dev, u64 lba, u32 count, const void *buf);
int block_flush(blockdev_t *dev);

/* Byte-granular helpers that go through the cache. */
int block_read_bytes(blockdev_t *dev, u64 offset, void *buf, size_t len);
int block_write_bytes(blockdev_t *dev, u64 offset, const void *buf, size_t len);

/* Scan a disk for GPT (preferred) or MBR partitions and register each one. */
int  block_scan_partitions(blockdev_t *disk);

/* Mount the root data volume named by the boot command line, plus the ESP, and
 * turn on persistent event logging. */
void block_mount_system_volumes(void);

void guid_format(const u8 guid[GUID_LEN], char *buf, size_t cap);
bool guid_parse(const char *text, u8 out[GUID_LEN]);
bool guid_equal(const u8 a[GUID_LEN], const u8 b[GUID_LEN]);

/* Well-known GPT partition type GUIDs. */
extern const u8 GPT_TYPE_ESP[GUID_LEN];
extern const u8 GPT_TYPE_MSDATA[GUID_LEN];
extern const u8 GPT_TYPE_KESTREL[GUID_LEN];

/* Sector cache. */
void block_cache_init(void);
void block_cache_flush_all(void);
void block_cache_invalidate(blockdev_t *dev);


/* Whether this system may write to a device at all.
 *
 * False for every disk except the one it booted from, so that a machine that
 * dual boots does not get its other operating system written to by accident.
 * `writeanywhere` on the command line lifts it. */
bool block_writes_allowed(const blockdev_t *d);
void block_note_boot_disk(const char *devname);
void block_note_write_intent(const blockdev_t *d);

#endif
