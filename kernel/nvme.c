/* nvme.c - NVMe controller driver.
 *
 * One admin queue and one I/O queue pair, polled for completion.  Data
 * transfers use a bounce buffer plus a PRP list, which keeps the addressing
 * rules simple: PRP1 is the first (possibly offset) page and PRP2 either the
 * second page or a list of the rest.
 */
#include "kernel.h"
#include "block.h"
#include "pci.h"
#include "cpu.h"
#include "mm.h"
#include "klog.h"
#include "time.h"
#include "vfs.h"
#include "proc.h"

/* Controller registers. */
#define NVME_CAP    0x00
#define NVME_VS     0x08
#define NVME_INTMS  0x0C
#define NVME_INTMC  0x10
#define NVME_CC     0x14
#define NVME_CSTS   0x1C
#define NVME_AQA    0x24
#define NVME_ASQ    0x28
#define NVME_ACQ    0x30

#define CC_EN       (1u << 0)
#define CSTS_RDY    (1u << 0)
#define CSTS_CFS    (1u << 1)

/* Admin opcodes. */
#define ADM_DELETE_SQ   0x00
#define ADM_CREATE_SQ   0x01
#define ADM_DELETE_CQ   0x04
#define ADM_CREATE_CQ   0x05
#define ADM_IDENTIFY    0x06
#define ADM_SET_FEATURES 0x09

/* I/O opcodes. */
#define IO_FLUSH    0x00
#define IO_WRITE    0x01
#define IO_READ     0x02

#define QUEUE_DEPTH 16
#define BOUNCE_BYTES (128 * 1024)

typedef struct __attribute__((packed)) {
    u8  opcode;
    u8  flags;
    u16 command_id;
    u32 nsid;
    u64 reserved;
    u64 metadata;
    u64 prp1, prp2;
    u32 cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} nvme_cmd_t;

typedef struct __attribute__((packed)) {
    u32 result;
    u32 reserved;
    u16 sq_head;
    u16 sq_id;
    u16 command_id;
    u16 status;      /* bit 0 is the phase tag */
} nvme_completion_t;

typedef struct {
    nvme_cmd_t        *sq;
    nvme_completion_t *cq;
    u64  sq_phys, cq_phys;
    u16  sq_tail, cq_head;
    u8   phase;
    volatile u32 *sq_doorbell;
    volatile u32 *cq_doorbell;
} nvme_queue_t;

typedef struct {
    volatile u8 *regs;
    u32          doorbell_stride;
    nvme_queue_t admin;
    nvme_queue_t io;
    u8          *bounce;
    u64          bounce_phys;
    u64          prp_list_phys;
    u64         *prp_list;
    u32          max_transfer;    /* bytes */
    u16          next_cid;
    bool         busy;            /* one transfer at a time per controller */
} nvme_dev_t;

/* A controller can present several namespaces, and each becomes its own block
 * device.  They share the controller's queues and staging buffer, so the
 * transfer path takes the controller lock. */
typedef struct {
    nvme_dev_t *ctrl;
    u32         nsid;
    u32         lba_bytes;
    u64         lba_count;
} nvme_ns_t;

/* The scheduler can preempt a driver in the middle of a transfer, so a second
 * process must wait rather than reuse the queues underneath it. */
static void ctrl_lock(nvme_dev_t *n) {
    for (;;) {
        bool irq = irq_save();
        if (!n->busy) { n->busy = true; irq_restore(irq); return; }
        irq_restore(irq);
        sched_yield();
    }
}

static void ctrl_unlock(nvme_dev_t *n) {
    bool irq = irq_save();
    n->busy = false;
    irq_restore(irq);
}



static inline u32 reg32(nvme_dev_t *n, u32 off) { return *(volatile u32 *)(n->regs + off); }
static inline void reg32w(nvme_dev_t *n, u32 off, u32 v) { *(volatile u32 *)(n->regs + off) = v; }
static inline u64 reg64(nvme_dev_t *n, u32 off) { return *(volatile u64 *)(n->regs + off); }
static inline void reg64w(nvme_dev_t *n, u32 off, u64 v) { *(volatile u64 *)(n->regs + off) = v; }

/* ------------------------------------------------------------------------- */
/* queue mechanics                                                           */
/* ------------------------------------------------------------------------- */

static int submit_and_wait(nvme_dev_t *n, nvme_queue_t *q, nvme_cmd_t *cmd, int timeout_ms, u32 *result) {
    cmd->command_id = n->next_cid++;
    u16 cid = cmd->command_id;

    q->sq[q->sq_tail] = *cmd;
    q->sq_tail = (u16)((q->sq_tail + 1) % QUEUE_DEPTH);
    __asm__ volatile("" ::: "memory");
    *q->sq_doorbell = q->sq_tail;

    for (int i = 0; i < timeout_ms * 100; i++) {
        nvme_completion_t *c = &q->cq[q->cq_head];
        if ((c->status & 1) == q->phase) {
            u16 status = (u16)(c->status >> 1);
            u16 got_cid = c->command_id;
            if (result) *result = c->result;

            q->cq_head = (u16)((q->cq_head + 1) % QUEUE_DEPTH);
            if (q->cq_head == 0) q->phase ^= 1;
            __asm__ volatile("" ::: "memory");
            *q->cq_doorbell = q->cq_head;

            if (got_cid != cid) {
                kwarn("nvme", "completion for command %u arrived while waiting for %u", got_cid, cid);
            }
            if (status) {
                kerr("nvme", "command %#x failed with status %#x (type %u, code %#x)",
                     cmd->opcode, status, (status >> 8) & 7, status & 0xFF);
                return -E_IO;
            }
            return 0;
        }
        if (reg32(n, NVME_CSTS) & CSTS_CFS) {
            kcrit("nvme", "controller reported a fatal status while running command %#x", cmd->opcode);
            return -E_IO;
        }
        timer_udelay(10);
    }
    kerr("nvme", "command %#x timed out after %d ms", cmd->opcode, timeout_ms);
    return -E_IO;
}

static bool alloc_queue(nvme_dev_t *n, nvme_queue_t *q, int index) {
    /* One page each is plenty: 16 entries needs 1 KiB of SQ and 256 B of CQ. */
    u64 phys = pmm_alloc_pages_below(2, 0x100000000ULL);
    if (!phys) return false;
    memset(phys_to_virt(phys), 0, 2 * PAGE_SIZE);

    q->sq_phys = phys;
    q->cq_phys = phys + PAGE_SIZE;
    q->sq = phys_to_virt(q->sq_phys);
    q->cq = phys_to_virt(q->cq_phys);
    q->sq_tail = 0;
    q->cq_head = 0;
    q->phase = 1;

    u32 stride = n->doorbell_stride;
    q->sq_doorbell = (volatile u32 *)(n->regs + 0x1000 + (2 * index) * stride);
    q->cq_doorbell = (volatile u32 *)(n->regs + 0x1000 + (2 * index + 1) * stride);
    return true;
}

/* ------------------------------------------------------------------------- */
/* transfers                                                                 */
/* ------------------------------------------------------------------------- */

/* Fill PRP1/PRP2 for a physically contiguous buffer. */
static void setup_prp(nvme_dev_t *n, nvme_cmd_t *cmd, u64 phys, size_t bytes) {
    cmd->prp1 = phys;
    cmd->prp2 = 0;

    size_t first = PAGE_SIZE - (phys & PAGE_MASK);
    if (bytes <= first) return;

    if (bytes <= first + PAGE_SIZE) {
        cmd->prp2 = (phys + first) & ~PAGE_MASK;
        return;
    }

    /* More than two pages: PRP2 points at a list of the remaining pages. */
    u64 addr = (phys + first) & ~PAGE_MASK;
    size_t remaining = bytes - first;
    int count = (int)((remaining + PAGE_MASK) / PAGE_SIZE);
    if (count > (int)(PAGE_SIZE / 8)) count = (int)(PAGE_SIZE / 8);

    for (int i = 0; i < count; i++) n->prp_list[i] = addr + (u64)i * PAGE_SIZE;
    cmd->prp2 = n->prp_list_phys;
}

static int nvme_transfer(blockdev_t *dev, u64 lba, u32 count, void *buf, bool write) {
    nvme_ns_t *ns = dev->priv;
    nvme_dev_t *n = ns->ctrl;
    u8 *user = buf;

    u32 max_sectors = n->max_transfer / ns->lba_bytes;
    if (max_sectors == 0) max_sectors = 1;

    ctrl_lock(n);
    while (count) {
        u32 chunk = count > max_sectors ? max_sectors : count;
        size_t bytes = (size_t)chunk * ns->lba_bytes;

        if (write) memcpy(n->bounce, user, bytes);

        nvme_cmd_t cmd;
        memset(&cmd, 0, sizeof cmd);
        cmd.opcode = write ? IO_WRITE : IO_READ;
        cmd.nsid   = ns->nsid;
        cmd.cdw10  = (u32)lba;
        cmd.cdw11  = (u32)(lba >> 32);
        cmd.cdw12  = chunk - 1;                 /* zero-based count */
        setup_prp(n, &cmd, n->bounce_phys, bytes);

        int r = submit_and_wait(n, &n->io, &cmd, 5000, NULL);
        if (r < 0) { ctrl_unlock(n); return r; }

        if (!write) memcpy(user, n->bounce, bytes);

        user += bytes;
        lba += chunk;
        count -= chunk;
    }
    ctrl_unlock(n);
    return 0;
}

static int nvme_read(blockdev_t *dev, u64 lba, u32 count, void *buf) {
    return nvme_transfer(dev, lba, count, buf, false);
}

static int nvme_write(blockdev_t *dev, u64 lba, u32 count, const void *buf) {
    return nvme_transfer(dev, lba, count, (void *)buf, true);
}

static int nvme_flush(blockdev_t *dev) {
    nvme_ns_t *ns = dev->priv;
    nvme_dev_t *n = ns->ctrl;
    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.opcode = IO_FLUSH;
    cmd.nsid = ns->nsid;

    ctrl_lock(n);
    int r = submit_and_wait(n, &n->io, &cmd, 10000, NULL);
    ctrl_unlock(n);
    return r;
}

static const block_ops_t nvme_ops = { nvme_read, nvme_write, nvme_flush };

/* ------------------------------------------------------------------------- */
/* bring-up                                                                  */
/* ------------------------------------------------------------------------- */

static bool wait_ready(nvme_dev_t *n, bool want, int timeout_ms) {
    for (int i = 0; i < timeout_ms; i++) {
        u32 csts = reg32(n, NVME_CSTS);
        if (csts & CSTS_CFS) { kerr("nvme", "controller fatal status during %s", want ? "enable" : "disable"); return false; }
        if (((csts & CSTS_RDY) != 0) == want) return true;
        timer_mdelay(1);
    }
    return false;
}

static void attach(pci_dev_t *d) {
    if (!d->bar[0]) { kerr("nvme", "controller has no register BAR"); return; }

    pci_enable_bus_master(d);

    nvme_dev_t *n = kzalloc(sizeof *n);
    if (!n) return;

    size_t window = d->bar_size[0] ? (size_t)d->bar_size[0] : 0x2000;
    if (window < 0x2000) window = 0x2000;
    n->regs = vmm_map_mmio(d->bar[0], window);
    if (!n->regs) { kerr("nvme", "cannot map registers at %p", (void *)d->bar[0]); kfree(n); return; }

    /* Mapped and answering: driven from here, with or without namespaces. */
    pci_claim(d, "nvme");

    u64 cap = reg64(n, NVME_CAP);
    u32 version = reg32(n, NVME_VS);
    n->doorbell_stride = 4u << ((cap >> 32) & 0xF);
    u32 max_queue = (u32)(cap & 0xFFFF) + 1;
    u32 mpsmin = (u32)((cap >> 48) & 0xF);

    kinfo("nvme", "controller %04x:%04x, NVMe %u.%u.%u, max queue %u, doorbell stride %u",
          d->vendor, d->device, (version >> 16) & 0xFFFF, (version >> 8) & 0xFF, version & 0xFF,
          max_queue, n->doorbell_stride);

    if (mpsmin > 0) {
        kerr("nvme", "controller requires a minimum page size of %u bytes; unsupported", 1u << (12 + mpsmin));
        kfree(n);
        return;
    }
    if (max_queue < QUEUE_DEPTH) {
        kerr("nvme", "controller queue depth %u is below the %u this driver needs", max_queue, QUEUE_DEPTH);
        kfree(n);
        return;
    }

    /* Disable, then set up the admin queue before enabling again. */
    reg32w(n, NVME_CC, reg32(n, NVME_CC) & ~CC_EN);
    if (!wait_ready(n, false, 5000)) { kerr("nvme", "controller did not go idle"); kfree(n); return; }

    if (!alloc_queue(n, &n->admin, 0)) { kerr("nvme", "no memory for the admin queue"); kfree(n); return; }

    reg32w(n, NVME_AQA, ((QUEUE_DEPTH - 1) << 16) | (QUEUE_DEPTH - 1));
    reg64w(n, NVME_ASQ, n->admin.sq_phys);
    reg64w(n, NVME_ACQ, n->admin.cq_phys);

    /* CC: 4 KiB pages, NVM command set, 64-byte SQ entries, 16-byte CQ entries. */
    u32 cc = CC_EN | (0u << 4) | (0u << 7) | (6u << 16) | (4u << 20);
    reg32w(n, NVME_CC, cc);
    if (!wait_ready(n, true, 5000)) { kerr("nvme", "controller did not become ready"); kfree(n); return; }

    /* Staging buffers. */
    u64 bounce = pmm_alloc_pages_below(BOUNCE_BYTES / PAGE_SIZE, 0x100000000ULL);
    u64 prp    = pmm_alloc_pages_below(1, 0x100000000ULL);
    if (!bounce || !prp) { kerr("nvme", "no DMA memory for transfers"); kfree(n); return; }
    n->bounce = phys_to_virt(bounce);
    n->bounce_phys = bounce;
    n->prp_list = phys_to_virt(prp);
    n->prp_list_phys = prp;

    /* IDENTIFY CONTROLLER. */
    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.opcode = ADM_IDENTIFY;
    cmd.cdw10  = 1;                      /* controller structure */
    cmd.prp1   = bounce;
    if (submit_and_wait(n, &n->admin, &cmd, 5000, NULL) < 0) {
        kerr("nvme", "IDENTIFY CONTROLLER failed");
        kfree(n);
        return;
    }

    char model[42], serial[22];
    memcpy(serial, n->bounce + 4, 20);  serial[20] = 0;
    memcpy(model, n->bounce + 24, 40);  model[40] = 0;
    for (int i = 20; i > 0 && (serial[i - 1] == ' ' || !serial[i - 1]); i--) serial[i - 1] = 0;
    for (int i = 40; i > 0 && (model[i - 1] == ' ' || !model[i - 1]); i--) model[i - 1] = 0;

    u8 mdts = n->bounce[77];
    n->max_transfer = mdts ? (u32)(PAGE_SIZE << mdts) : BOUNCE_BYTES;
    if (n->max_transfer > BOUNCE_BYTES) n->max_transfer = BOUNCE_BYTES;

    u32 nn = *(u32 *)(n->bounce + 516);   /* number of namespaces */
    if (!nn) { kerr("nvme", "controller reports no namespaces"); kfree(n); return; }

    /* Create the I/O completion and submission queues. */
    if (!alloc_queue(n, &n->io, 1)) { kerr("nvme", "no memory for the I/O queue"); kfree(n); return; }

    memset(&cmd, 0, sizeof cmd);
    cmd.opcode = ADM_CREATE_CQ;
    cmd.prp1   = n->io.cq_phys;
    cmd.cdw10  = ((QUEUE_DEPTH - 1) << 16) | 1;      /* size-1, queue id 1 */
    cmd.cdw11  = 1;                                  /* physically contiguous */
    if (submit_and_wait(n, &n->admin, &cmd, 5000, NULL) < 0) { kerr("nvme", "CREATE CQ failed"); kfree(n); return; }

    memset(&cmd, 0, sizeof cmd);
    cmd.opcode = ADM_CREATE_SQ;
    cmd.prp1   = n->io.sq_phys;
    cmd.cdw10  = ((QUEUE_DEPTH - 1) << 16) | 1;
    cmd.cdw11  = (1u << 16) | 1;                     /* CQ id 1, contiguous */
    if (submit_and_wait(n, &n->admin, &cmd, 5000, NULL) < 0) { kerr("nvme", "CREATE SQ failed"); kfree(n); return; }

    /* Every active namespace becomes a block device of its own: a controller
     * with two disks attached presents them as namespace 1 and 2, and stopping
     * at the first would hide the second disk entirely. */
    int attached = 0;
    for (u32 nsid = 1; nsid <= nn && nsid <= 32; nsid++) {
        memset(&cmd, 0, sizeof cmd);
        cmd.opcode = ADM_IDENTIFY;
        cmd.nsid   = nsid;
        cmd.cdw10  = 0;                  /* namespace structure */
        cmd.prp1   = bounce;
        if (submit_and_wait(n, &n->admin, &cmd, 5000, NULL) < 0) continue;

        u64 nsze = *(u64 *)(n->bounce + 0);
        if (!nsze) continue;             /* inactive namespace */

        u8  flbas = n->bounce[26];
        u32 fmt_index = flbas & 0x0F;
        u32 lbaf = *(u32 *)(n->bounce + 128 + fmt_index * 4);
        u32 lbads = (lbaf >> 16) & 0xFF;
        u32 lba_bytes = 1u << lbads;
        if (lba_bytes < 512 || lba_bytes > 4096) {
            kwarn("nvme", "namespace %u uses an unsupported %u-byte block; skipped", nsid, lba_bytes);
            continue;
        }

        nvme_ns_t *ns = kzalloc(sizeof *ns);
        if (!ns) break;
        ns->ctrl = n;
        ns->nsid = nsid;
        ns->lba_bytes = lba_bytes;
        ns->lba_count = nsze;

        char name[BLOCK_NAME_MAX];
        snprintf(name, sizeof name, "disk%d", block_next_disk_index());
        blockdev_t *dev = block_register(name, &nvme_ops, ns, lba_bytes, nsze, model);
        if (!dev) { kfree(ns); block_release_disk_index(); continue; }

        kinfo("nvme", "namespace %u -> %s: %s (serial %s), max transfer %u KiB",
              nsid, name, model, serial, n->max_transfer / 1024);
        attached++;
    }

    if (!attached) {
        kerr("nvme", "no usable namespace found on %04x:%04x", d->vendor, d->device);
        kfree(n);
    }
}

void nvme_init(void) {
    int found = 0;
    for (pci_dev_t *d = pci_find(0x01, 0x08, 0x02, NULL); d; d = pci_find(0x01, 0x08, 0x02, d)) {
        attach(d);
        found++;
    }
    if (!found) kinfo("nvme", "no NVMe controllers present");
}
