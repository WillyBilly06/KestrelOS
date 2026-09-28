/* usbmsc_model.c - a USB memory stick, in software.
 *
 * There is no USB storage device on any machine this is built or tested on:
 * the virtual machine boots from an emulated NVMe disk, and the one place a
 * real stick is used is the user's own hardware, where there is no debugger
 * and no console.  So the code that makes a stick into a writable disk had
 * never been executed once - not "was thought to work", had never run.
 *
 * That matters more here than for most drivers.  This system's whole purpose
 * is to boot from a stick onto unfamiliar hardware and leave an account of
 * what happened, and the account is written through this path.  "Nothing logs
 * on the USB stick" is what an untested driver looks like from outside.
 *
 * So the device is modelled.  It answers Bulk-Only Transport with a SCSI
 * command set on top, backed by memory, and it is deliberately awkward in the
 * four ways real sticks are awkward - because a model that answers everything
 * immediately and correctly proves only that the driver can talk to a model.
 */
#include "kernel.h"
#include "klog.h"
#include "mm.h"
#include "usbstick_image.h"


#define CBW_SIGNATURE 0x43425355u
#define CSW_SIGNATURE 0x53425355u

#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_REQUEST_SENSE    0x03
#define SCSI_INQUIRY          0x12
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10          0x28
#define SCSI_WRITE_10         0x2A

/* Small enough to sit in memory, large enough that a capacity read has a
 * number worth getting wrong: 512 sectors of 512 bytes. */
#define MODEL_SECTOR_SIZE  512
#define MODEL_SECTORS      512

typedef struct {
    u32 signature;
    u32 tag;
    u32 transfer_length;
    u8  flags;
    u8  lun;
    u8  cb_length;
    u8  cb[16];
} __attribute__((packed)) cbw_t;

typedef struct {
    u32 signature;
    u32 tag;
    u32 residue;
    u8  status;
} __attribute__((packed)) csw_t;

static bool attached;
static u8  *store;

/* What the device is part-way through. */
static cbw_t pending;
static bool  have_pending;
static u8    pending_status;      /* what the status block will say  */
static u32   pending_residue;

/* The awkwardness, and the counters a test reads.
 *
 * `not_ready_left` makes the first few readiness checks fail, which is what a
 * stick does while it wakes up.  `sense_owed` then refuses everything until
 * the failure has been collected - that is real behaviour and it is the reason
 * the driver asks for sense after a failure; a driver that does not gets
 * wedged here rather than on somebody's desk.
 */
static int  not_ready_left;
static bool sense_owed;
static int  wedged_by_uncollected_sense;
static int  commands_seen;
static bool bad_signature_seen;
static bool wrote_past_the_end;

void usbmsc_model_attach(void) {
    if (!store) store = kzalloc(MODEL_SECTOR_SIZE * MODEL_SECTORS);
    attached = store != NULL;

    have_pending = false;
    sense_owed = false;
    wedged_by_uncollected_sense = 0;
    commands_seen = 0;
    bad_signature_seen = false;
    wrote_past_the_end = false;

    /* Two refusals before it is ready.  Enough to prove the driver waits and
     * collects the sense between attempts; few enough that a driver which
     * does both is not slowed down. */
    not_ready_left = 2;

    /* A real filesystem on it, rather than a recognisable pattern.
     *
     * The pattern that used to be here proved a read returned something other
     * than zeros, which is worth proving and is not the question the user
     * asked.  "Nothing logs on the USB stick" is about a FILESYSTEM: whether a
     * volume can be mounted from the device, a file created on it, and the
     * write pushed all the way down through the block layer to the hardware.
     *
     * So the stick arrives formatted.  The volume was built by the same code
     * that formats the boot volume and NOT by anything in this kernel - a
     * filesystem this system created and then read back would only prove it
     * agrees with itself.
     *
     * Stored as the runs of it that are not zero, because a fresh volume is
     * a boot sector, two allocation tables and an empty root directory in a
     * quarter of a megabyte of nothing. */
    if (store) {
        memset(store, 0, MODEL_SECTOR_SIZE * MODEL_SECTORS);
        for (int i = 0; i < USBSTICK_RUNS; i++) {
            const usbstick_run_t *r = &usbstick_image[i];
            if (r->at + r->len <= MODEL_SECTOR_SIZE * MODEL_SECTORS)
                memcpy(store + r->at, r->bytes, r->len);
        }
    }
}

/* Put the formatted volume back.
 *
 * The block-level checks write to sectors 42 and 100 onwards to prove the
 * address reaches the device.  Those sectors are inside the filesystem, so by
 * the time they are done the volume is damaged - correctly, deliberately, and
 * fatally for anything that then tries to mount it.
 *
 * Restoring between the two is not papering over that: a device really would
 * be corrupted by writing over its filesystem, and the two things being
 * checked - that sectors go where they are addressed, and that a filesystem
 * works - cannot both be checked on the same untouched volume. */
void usbmsc_model_reformat(void) {
    if (!store) return;
    memset(store, 0, MODEL_SECTOR_SIZE * MODEL_SECTORS);
    for (int i = 0; i < USBSTICK_RUNS; i++) {
        const usbstick_run_t *r = &usbstick_image[i];
        if (r->at + r->len <= MODEL_SECTOR_SIZE * MODEL_SECTORS)
            memcpy(store + r->at, r->bytes, r->len);
    }
}

void usbmsc_model_detach(void) { attached = false; }

int  usbmsc_model_commands(void) { return commands_seen; }
bool usbmsc_model_bad_signature(void) { return bad_signature_seen; }
int  usbmsc_model_wedged(void) { return wedged_by_uncollected_sense; }
bool usbmsc_model_wrote_past_the_end(void) { return wrote_past_the_end; }

u32 usbmsc_model_sector_size(void) { return MODEL_SECTOR_SIZE; }
u64 usbmsc_model_sectors(void) { return MODEL_SECTORS; }

/* Read one sector out of the model, so a test can check what the driver wrote
 * actually landed where it said. */
bool usbmsc_model_peek(u64 lba, void *out) {
    if (!store || lba >= MODEL_SECTORS || !out) return false;
    memcpy(out, store + lba * MODEL_SECTOR_SIZE, MODEL_SECTOR_SIZE);
    return true;
}

/* Where the driver's bytes arrive.
 *
 * Three calls make one command: the envelope out, the data in whichever
 * direction, then the status block in.  The model holds the envelope between
 * them, which is also what lets it notice a driver that sends two envelopes
 * without collecting the status in between.
 */
int usbmsc_model_bulk(bool in, void *data, u32 length) {
    if (!attached || !store) return -1;

    /* ---- the envelope going out ---- */
    if (!in && length == sizeof(cbw_t) && !have_pending) {
        const cbw_t *cbw = data;

        if (cbw->signature != CBW_SIGNATURE) {
            /* A real device stalls here and needs resetting.  Recording it is
             * what matters: an envelope with the wrong signature means the two
             * sides are not talking about the same thing at all. */
            bad_signature_seen = true;
            return -1;
        }

        pending = *cbw;
        have_pending = true;
        pending_status = 0;
        pending_residue = 0;
        commands_seen++;

        u8 op = pending.cb[0];

        /* Anything but a request for sense is refused while a failure is
         * uncollected.  This is not the model being difficult - it is what the
         * hardware does, and it is why the driver's failure path asks for
         * sense before trying anything else. */
        if (sense_owed && op != SCSI_REQUEST_SENSE) {
            wedged_by_uncollected_sense++;
            pending_status = 1;
            return (int)length;
        }

        switch (op) {
        case SCSI_TEST_UNIT_READY:
            if (not_ready_left > 0) {
                not_ready_left--;
                sense_owed = true;
                pending_status = 1;
            }
            break;

        case SCSI_REQUEST_SENSE:
            sense_owed = false;
            break;

        default:
            break;
        }

        return (int)length;
    }

    /* ---- the data phase ---- */
    if (have_pending && length && length == pending.transfer_length) {
        u8 op = pending.cb[0];

        if (pending_status != 0) {
            /* The command already failed; there is nothing to move.  Saying
             * so rather than moving zeroes keeps "failed" distinct from
             * "succeeded and returned nothing". */
            if (op == SCSI_REQUEST_SENSE && in) {
                memset(data, 0, length);
                ((u8 *)data)[0] = 0x70;      /* current error   */
                ((u8 *)data)[2] = 0x02;      /* not ready       */
                return (int)length;
            }
            return -1;
        }

        u32 lba = ((u32)pending.cb[2] << 24) | ((u32)pending.cb[3] << 16) |
                  ((u32)pending.cb[4] << 8) | pending.cb[5];
        u32 blocks = ((u32)pending.cb[7] << 8) | pending.cb[8];

        switch (op) {
        case SCSI_INQUIRY: {
            memset(data, 0, length);
            u8 *q = data;
            q[0] = 0x00;                     /* a direct-access block device */
            q[1] = 0x80;                     /* removable                    */
            q[3] = 0x02;
            q[4] = 31;
            /* Vendor, product and revision, as three padded fields - which is
             * how SCSI has always reported a name, and the shape the driver
             * has to unpad. */
            memcpy(q + 8,  "Kestrel ", 8);
            memcpy(q + 16, "Model Stick     ", 16);
            memcpy(q + 32, "1.00", 4);
            return (int)length;
        }

        case SCSI_READ_CAPACITY_10: {
            u8 *q = data;
            /* The number of the LAST block, not the count.  Getting this wrong
             * gives a disk one sector too large, and the only symptom is a
             * read off the end much later. */
            u32 last = MODEL_SECTORS - 1;
            q[0] = (u8)(last >> 24); q[1] = (u8)(last >> 16);
            q[2] = (u8)(last >> 8);  q[3] = (u8)last;
            u32 size = MODEL_SECTOR_SIZE;
            q[4] = (u8)(size >> 24); q[5] = (u8)(size >> 16);
            q[6] = (u8)(size >> 8);  q[7] = (u8)size;
            return 8;
        }

        case SCSI_READ_10:
            if ((u64)lba + blocks > MODEL_SECTORS) { pending_status = 1; return -1; }
            memcpy(data, store + (u64)lba * MODEL_SECTOR_SIZE,
                   (size_t)blocks * MODEL_SECTOR_SIZE);
            return (int)length;

        case SCSI_WRITE_10:
            if ((u64)lba + blocks > MODEL_SECTORS) {
                /* A write off the end must not be quietly clamped: on a real
                 * stick it is refused, and a driver that relies on clamping
                 * corrupts the sector it lands on instead. */
                wrote_past_the_end = true;
                pending_status = 1;
                return -1;
            }
            memcpy(store + (u64)lba * MODEL_SECTOR_SIZE, data,
                   (size_t)blocks * MODEL_SECTOR_SIZE);
            return (int)length;

        default:
            memset(data, 0, length);
            return (int)length;
        }
    }

    /* ---- the status block coming back ---- */
    if (in && length == sizeof(csw_t) && have_pending) {
        csw_t *csw = data;
        memset(csw, 0, sizeof *csw);
        csw->signature = CSW_SIGNATURE;
        /* The SAME tag the envelope carried.  A driver that does not check it
         * will pair one command's data with another's status the first time
         * anything is slow. */
        csw->tag = pending.tag;
        csw->residue = pending_residue;
        csw->status = pending_status;
        have_pending = false;
        return (int)length;
    }

    return -1;
}
