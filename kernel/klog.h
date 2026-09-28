#ifndef KESTREL_KLOG_H
#define KESTREL_KLOG_H

#include "kernel.h"

#define KLOG_SUBSYS_MAX 16
#define KLOG_MSG_MAX    168
#define KLOG_RING_SIZE  512     /* entries; oldest are overwritten */

typedef struct {
    u64 seq;                        /* monotonic, never reused        */
    u64 time_ms;                    /* milliseconds since boot        */
    u32 level;
    u32 pad;
    char subsys[KLOG_SUBSYS_MAX];
    char msg[KLOG_MSG_MAX];
} klog_entry;

/* Copy up to `max` entries with seq >= from_seq into out.  Returns the count.
 * Entries that have already been overwritten are silently skipped, so a reader
 * that falls behind sees a gap rather than stale data. */
int  klog_read(u64 from_seq, klog_entry *out, int max);
u64  klog_next_seq(void);
u64  klog_dropped(void);
void klog_counts(u32 counts[5]);
void klog_dump_tail(int n);         /* to the console, for panics             */
void klog_clear(void);

/* Persistence: once a writable filesystem is mounted the kernel appends every
 * new WARN-or-worse entry to /data/logs/events.log. */
void klog_persist_enable(const char *path);
void klog_persist_level(int level);
const char *klog_persist_path(void);
void klog_persist_flush(void);
bool klog_persist_active(void);

const char *klog_level_name(int level);

#endif
