/* ramlog.h - a kernel log that survives a reboot, in memory.
 *
 * The loader can write files: the firmware's own USB stack is running when it
 * is, which is why \KESTREL\BOOT.LOG exists and is reliable.  The kernel
 * cannot, until its own storage driver works - and on a machine where that
 * driver is the thing being debugged, the log explaining why it failed has
 * nowhere to go.  The result is a screen that has to be photographed, which is
 * a poor way to read a log and a worse way to ask somebody else to.
 *
 * So the kernel writes its log into a fixed region of memory instead, and the
 * loader picks it up on the next boot and writes it to the stick, where it can
 * be read as a file.  Nothing is required of the machine except that it keep
 * RAM contents across a warm reboot, which ordinary PCs do: memory is only
 * retrained, and its contents only lost, on a cold start.  Linux persists
 * kernel logs across a crash the same way, and for the same reason.
 *
 * The cost of that is honest and worth stating: this is best-effort.  A log
 * recovered this way is one boot old, and a cold start - the power actually
 * removed - loses it.  In exchange it needs no working disk, no filesystem and
 * no driver of ours at all, so it keeps working precisely when everything else
 * has stopped.
 *
 * Both the loader and the kernel compile this header, so the layout below is a
 * contract between them and neither may change it alone.
 */
#ifndef KESTREL_RAMLOG_H
#define KESTREL_RAMLOG_H

#include <stdint.h>

/* "KSTRLOG1".  Written last when the region is set up and cleared first when
 * it is retired, so a partially initialised region is never mistaken for a
 * complete one. */
#define RAMLOG_MAGIC   0x31474F4C5254534BULL

/* Where the loader asks for the region.  Any address does, because the loader
 * searches for the magic rather than trusting a fixed address - but asking for
 * the same place every time means the search almost always succeeds on the
 * first probe, and means two boots of the same machine agree. */
#define RAMLOG_ADDR    0x30000000ULL      /* 768 MiB */
#define RAMLOG_BYTES   (1u << 20)         /* 1 MiB, about 8000 log lines */

/* The region must start on a boundary the search can step over, or the search
 * would have to look at every byte of RAM. */
#define RAMLOG_ALIGN   0x10000ULL         /* 64 KiB */

typedef struct {
    uint64_t magic;        /* RAMLOG_MAGIC once the region is usable        */
    uint32_t bytes;        /* size of text[] that follows this header       */
    uint32_t used;         /* bytes written, or bytes when it has wrapped   */
    uint32_t head;         /* where the next byte goes                      */
    uint32_t wrapped;      /* non-zero once older text has been overwritten */
    uint32_t boot;         /* increments each boot, so logs can be told apart */
    uint32_t check;        /* bytes ^ used ^ head ^ boot, against stray writes */
    /* text[bytes] follows, a ring of plain characters with no framing. */
} ramlog_header;

/* The check field exists because this region is, by construction, memory that
 * nothing else is meant to touch but that nothing prevents anything touching.
 * A header that does not agree with itself is one that something else has
 * written over, and is discarded rather than believed. */
static inline uint32_t ramlog_check(const ramlog_header *h) {
    return h->bytes ^ h->used ^ h->head ^ h->boot;
}

static inline int ramlog_valid(const ramlog_header *h) {
    return h->magic == RAMLOG_MAGIC &&
           h->bytes >= 4096 && h->bytes <= (64u << 20) &&
           h->head < h->bytes && h->used <= h->bytes &&
           h->check == ramlog_check(h);
}

#endif /* KESTREL_RAMLOG_H */
