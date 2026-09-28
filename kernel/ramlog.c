/* ramlog.c - the kernel half of the reboot-surviving log.
 *
 * Every line that goes to the serial port also goes here, into a ring in a
 * region of memory the loader set aside.  The loader reads it back on the next
 * boot and writes it to the boot device as a file.  See ramlog.h for why the
 * log takes this route at all.
 *
 * This has to work when nothing else does - it exists for the boots where the
 * storage driver has failed - so it depends on nothing beyond a pointer and a
 * spinlock: no allocator, no filesystem, no driver, and no interrupts.
 */
#include "kernel.h"
#include "klog.h"
#include "../include/kestrel/ramlog.h"

/* Keep the compiler from moving the header update ahead of the text it
 * describes: a reset landing between the two must leave the header looking
 * damaged, not looking valid and pointing at bytes that were never written. */
static void barrier(void) { __asm__ volatile("" ::: "memory"); }

static ramlog_header *rl;         /* NULL until the loader has provided one */
static char          *rl_text;
static u32            rl_recovered;

void ramlog_init(void) {
    /* A loader older than this field never wrote it, and `size` is how the
     * contract says whether a field is there.  Reading past what the loader
     * actually filled in would be reading whatever happened to follow it. */
    if (g_boot.size < offsetof(kboot_info, ramlog_prev) + sizeof(u32)) {
        kwarn("ramlog", "this loader is older than the log region (contract "
                        "size %u); the log stays in memory only", g_boot.size);
        return;
    }
    if (!g_boot.ramlog_base || g_boot.ramlog_size <= sizeof(ramlog_header)) {
        kwarn("ramlog", "no region was reserved (base %#llx, %u bytes)",
              (unsigned long long)g_boot.ramlog_base, g_boot.ramlog_size);
        return;
    }

    ramlog_header *h = phys_to_virt(g_boot.ramlog_base);

    /* The loader has already set the header up and, if it found one from the
     * previous boot, has already written that out.  Anything that does not
     * validate means the two sides disagree about the region, and writing into
     * it on a guess would be writing over whatever really lives there. */
    if (!ramlog_valid(h)) {
        kwarn("ramlog", "the region does not describe itself consistently "
                        "(magic %#llx, %u bytes, head %u, check %u vs %u); "
                        "not writing into it",
              (unsigned long long)h->magic, h->bytes, h->head,
              h->check, ramlog_check(h));
        return;
    }
    if ((u64)h->bytes + sizeof *h > g_boot.ramlog_size) {
        kwarn("ramlog", "the region is smaller than its header claims");
        return;
    }

    rl = h;
    rl_text = (char *)(h + 1);
    rl_recovered = g_boot.ramlog_prev;
}

/* Append to the ring.  Called from klog() with interrupts already off. */
void ramlog_write(const char *s, size_t n) {
    if (!rl) return;

    u32 bytes = rl->bytes;
    u32 head  = rl->head;

    for (size_t i = 0; i < n; i++) {
        rl_text[head] = s[i];
        if (++head == bytes) { head = 0; rl->wrapped = 1; }
    }

    rl->head = head;
    rl->used = rl->wrapped ? bytes : head;

    /* The checksum goes last, so a header caught mid-update by a reset reads
     * as damaged rather than as a valid header describing the wrong bytes. */
    barrier();
    rl->check = ramlog_check(rl);
}

bool ramlog_active(void) { return rl != NULL; }
u32  ramlog_used(void)   { return rl ? rl->used : 0; }
u32  ramlog_capacity(void) { return rl ? rl->bytes : 0; }

/* How much of the previous boot's log the loader recovered and wrote out.
 * Reported on screen so it is possible to tell "the log is on the stick" from
 * "the log was lost", which are otherwise the same blank look. */
u32  ramlog_recovered(void) { return rl_recovered; }
