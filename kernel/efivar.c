/* efivar.c - put the boot log somewhere the firmware keeps it.
 *
 * The problem this solves is specific and was arrived at the hard way.
 *
 * A log is only useful if it can be read after the machine is switched off.
 * There were two ways to do that and both are closed on the machine that
 * matters:
 *
 *   Write it to the volume the system booted from.  That needs the kernel to
 *   reach the USB stick with its OWN driver - the loader reads the stick
 *   through firmware that is gone by the time the kernel runs - and on the
 *   target machine no volume is ever found to write to.  Which is the very
 *   fault the log was wanted for.
 *
 *   Keep it in memory and let the next boot's loader write it out.  That is
 *   what ramlog does and it works where memory survives a reset.  Linux says
 *   the same thing about its own version: "Ramoops needs a system with
 *   persistent RAM so that the content of that area can survive after a
 *   restart."  The target board clears memory on reset - proved over three
 *   consecutive boots, each reserving the same region and each reporting boot
 *   number one, when the loader writes the marker itself and a surviving
 *   region would have come back as boot two.
 *
 * So neither storage nor memory.  What is left is the firmware.
 *
 * SetVariable is a RUNTIME service, not a boot service: unlike file access it
 * is still callable after ExitBootServices, and what it writes goes into the
 * board's own non-volatile store.  It survives the power going off, needs no
 * driver of ours at all, and - the part that makes it worth the trouble - can
 * be read back from Windows with GetFirmwareEnvironmentVariable.  This is the
 * same mechanism Linux's efi-pstore uses for exactly this problem.
 *
 * WHY THIS IS SAFE TO CALL AT ALL.  Runtime services live at their physical
 * addresses until somebody calls SetVirtualAddressMap.  Nothing here ever
 * does, and the loader maps low memory identity as well as at the direct map -
 * writable AND executable, since only the direct-map copy carries the
 * no-execute bit.  So the firmware's own code is still where it expects to be.
 *
 * AND WHY IT IS KEPT SMALL.  This store is shared with the firmware's own
 * settings and is measured in tens of kilobytes.  Filling it has wedged real
 * machines - efi-pstore has a history of it.  So: one variable, overwritten
 * rather than accumulated, hard-capped, and written when asked rather than
 * continuously.  A boot log fits in that budget many times over; anything
 * that would not fit does not belong here.
 */
#include "kernel.h"
#include "klog.h"
#include "mm.h"

/* The slice of EFI_RUNTIME_SERVICES this needs.
 *
 * Written out rather than included, because boot/efi.h is the loader's and
 * drags in the whole firmware interface.  Every offset is asserted against the
 * specification's layout below - a table walked with a wrong offset calls
 * whatever happens to sit there, which on a firmware table is somebody else's
 * function with somebody else's arguments. */
#define EFIAPI __attribute__((ms_abi))

typedef struct { u32 a; u16 b, c; u8 d[8]; } efi_guid_t;

typedef struct {
    u64 signature;
    u32 revision, header_size, crc32, reserved;
} efi_table_header_t;

typedef struct {
    efi_table_header_t hdr;                                   /*  0 */
    void *get_time, *set_time;                                /* 24, 32 */
    void *get_wakeup_time, *set_wakeup_time;                  /* 40, 48 */
    void *set_virtual_address_map;                            /* 56 */
    void *convert_pointer;                                    /* 64 */
    u64 (EFIAPI *get_variable)(u16 *name, efi_guid_t *vendor, /* 72 */
                               u32 *attr, u64 *size, void *data);
    void *get_next_variable_name;                             /* 80 */
    u64 (EFIAPI *set_variable)(u16 *name, efi_guid_t *vendor, /* 88 */
                               u32 attr, u64 size, void *data);
    void *get_next_high_monotonic_count;                      /* 96 */
    void *reset_system;                                       /* 104 */
} efi_runtime_t;

_Static_assert(sizeof(efi_table_header_t) == 24, "EFI table header is 24 bytes");
_Static_assert(__builtin_offsetof(efi_runtime_t, get_variable) == 72,
               "GetVariable is the seventh entry after the header");
_Static_assert(__builtin_offsetof(efi_runtime_t, set_variable) == 88,
               "SetVariable is the ninth entry after the header");

#define EFI_VARIABLE_NON_VOLATILE        0x01u
#define EFI_VARIABLE_BOOTSERVICE_ACCESS  0x02u
#define EFI_VARIABLE_RUNTIME_ACCESS      0x04u

/* KestrelOS's own vendor identifier - the same one its data partitions carry,
 * so anything of this system's is findable under one name. */
static efi_guid_t kestrel_guid = {
    0xB34C1A7E, 0x9D62, 0x4E47,
    { 0x9C, 0x31, 0x5A, 0x6F, 0x2E, 0x88, 0xD1, 0x40 }
};

/* Deliberately modest.  Eight kilobytes is a few hundred log lines - enough to
 * say what happened during a boot - and a small fraction of a store the
 * firmware also needs for its own settings. */
#define EFI_LOG_MAX  8192

/* Call the firmware, from an address space where the firmware exists.
 *
 * This is the part that cost a boot to find.  Runtime services stay at their
 * physical addresses and the loader's identity map is what makes those
 * addresses mean anything - but that map lives in the LOW half of the page
 * tables, and vmm.c shares only the kernel half (slots 256 to 511) into each
 * process.  So the identity map is present in the kernel's own address space
 * and in no other.
 *
 * End of boot runs on the kernel's tables, so publishing there worked and
 * looked like proof the mechanism was sound.  A shutdown does not: it arrives
 * through a system call, on the page tables of whichever process asked, and
 * there the firmware's code is not mapped at any address.  Jumping to it gives
 * exactly what the target machine produced -
 *
 *     ERROR fault  page fault at 0xfb7a2d2: page not present while fetching
 *                  an instruction in kernel mode
 *
 * - a fault whose faulting address is the firmware's own entry point.  Which
 * is why every attempt to save a log at shutdown left nothing behind.
 *
 * Interrupts are held off across the switch because the scheduler would
 * otherwise restore the process's tables underneath the call and take the
 * firmware's code away mid-instruction.  That is safe to do here in a way it
 * is not around a driver: the firmware polls its own flash and waits on
 * nothing of ours, so no timeout of this kernel's is being asked to expire
 * while the clock is stopped. */
static u64 call_set_variable(efi_runtime_t *rt, u16 *name, efi_guid_t *vendor,
                             u32 attr, u64 size, void *data) {
    bool irq = irq_save();
    u64 mine = read_cr3();
    u64 kern = vmm_kernel_pml4();

    if (mine != kern) vmm_switch(kern);
    u64 status = rt->set_variable(name, vendor, attr, size, data);
    if (mine != kern) vmm_switch(mine);

    irq_restore(irq);
    return status;
}

static efi_runtime_t *runtime(void) {
    if (!g_boot.efi_runtime) return NULL;
    return (efi_runtime_t *)phys_to_virt(g_boot.efi_runtime);
}

bool efi_variables_available(void) {
    efi_runtime_t *rt = runtime();
    return rt && rt->set_variable;
}

/* Variable names, as the firmware wants them: sixteen bits a character.
 *
 * Two of them, and the reason is the size of the log ring rather than
 * tidiness.  The ring holds 512 entries and a boot fills most of it, so by the
 * time somebody has run a command or two and asked for a shutdown, the lines
 * describing what the storage driver found are long gone - overwritten by
 * whatever happened since.  A single variable written at shutdown therefore
 * cannot answer "why was there no disk", because the answer aged out of memory
 * before the question was asked.
 *
 * So the boot's own verdict is captured while it is still in the ring, and the
 * shutdown snapshot is kept separately for what happened afterwards. */
static u16 log_name[] = {
    'K','e','s','t','r','e','l','L','o','g', 0
};
static u16 boot_name[] = {
    'K','e','s','t','r','e','l','B','o','o','t', 0
};

/* Put the tail of this boot's log into firmware storage.
 *
 * The TAIL, not the head: when something has gone wrong the last lines are the
 * ones that say so, and eight kilobytes cannot hold a whole boot. */
static u64 last_status;

static bool publish_as(u16 *name, const char *what, size_t cap) {
    efi_runtime_t *rt = runtime();
    if (!rt || !rt->set_variable) {
        kwarn("efivar", "this firmware exposes no variable service, so there "
                        "is nowhere to leave a log that survives the power");
        return false;
    }

    /* Not on the stack: this is eight kilobytes and it is assembled while the
     * rest of the system is still running. */
    static char text[EFI_LOG_MAX];
    static klog_entry batch[32];
    if (cap > sizeof text) cap = sizeof text;

    /* Walk the ring to the end, keeping the last EFI_LOG_MAX bytes.  Writing
     * from the beginning would fill the budget with the parts of a boot that
     * always look the same. */
    size_t len = 0;
    u32 seq = 1;
    for (;;) {
        int n = klog_read(seq, batch, 32);
        if (n <= 0) break;

        for (int i = 0; i < n; i++) {
            char line[220];
            int w = snprintf(line, sizeof line, "[%5u.%03u] %-5s %-8s %s\n",
                             (unsigned)(batch[i].time_ms / 1000),
                             (unsigned)(batch[i].time_ms % 1000),
                             klog_level_name((int)batch[i].level),
                             batch[i].subsys, batch[i].msg);
            if (w <= 0) continue;

            /* Keep the newest: when it no longer fits, drop whole lines from
             * the front rather than truncating and leaving a half line that
             * reads as corruption. */
            while (len + (size_t)w >= cap) {
                /* Searched by hand rather than with memchr: this kernel has no
                 * such function, and adding one for a single search here would
                 * be the tail wagging the dog. */
                size_t at = 0;
                while (at < len && text[at] != '\n') at++;
                if (at >= len) { len = 0; break; }

                size_t drop = at + 1;
                memmove(text, text + drop, len - drop);
                len -= drop;
            }
            memcpy(text + len, line, (size_t)w);
            len += (size_t)w;
        }
        seq = batch[n - 1].seq + 1;
        if (n < 32) break;
    }

    if (!len) return false;

    u32 attrs = EFI_VARIABLE_NON_VOLATILE |
                EFI_VARIABLE_BOOTSERVICE_ACCESS |
                EFI_VARIABLE_RUNTIME_ACCESS;

    u64 status = call_set_variable(rt, name, &kestrel_guid, attrs,
                                   (u64)len, text);
    last_status = status;
    if (status) {
        /* The high bit is set on every EFI error, so the number is large and
         * unhelpful printed as a decimal. */
        kwarn("efivar", "the firmware refused to store the log (status %llx)",
              (unsigned long long)status);
        return false;
    }

    kinfo("efivar", "%u bytes are in firmware storage as %s - it survives the "
                    "power going off and can be read from another operating "
                    "system", (unsigned)len, what);
    return true;
}

/* What happened up to and including the shutdown. */
bool efi_log_publish(void) {
    return publish_as(log_name, "KestrelLog", EFI_LOG_MAX);
}

/* What the boot itself concluded, written while it is still in the ring.
 *
 * Called once, from the thread that spends the first ten seconds trying to
 * find something writable - by then every driver has either claimed its
 * hardware or not, and that verdict is the one thing a shutdown snapshot
 * taken minutes later can no longer contain. */
static int   snapshot_tried;
static u64   snapshot_status;

bool efi_boot_snapshot(void) {
    snapshot_tried++;
    bool ok = publish_as(boot_name, "KestrelBoot", 6144);
    snapshot_status = ok ? 0 : last_status;
    return ok;
}

/* What became of the boot snapshot, for the shutdown log to report.
 *
 * The snapshot is written ten seconds in, and if the firmware refuses it the
 * refusal is logged there and then - where it is overwritten long before
 * anybody reads anything.  On the target machine KestrelBoot simply was not
 * in NVRAM afterwards and there was no way to tell "never ran" from "the
 * firmware would not create a second variable at runtime", which are different
 * problems with different fixes. */
void efi_snapshot_report(void) {
    if (!snapshot_tried) {
        kwarn("efivar", "the boot snapshot never ran - the thread that takes it "
                        "did not reach ten seconds");
    } else if (!snapshot_status) {
        kinfo("efivar", "the boot snapshot was stored as KestrelBoot");
    } else {
        kwarn("efivar", "the firmware refused to store KestrelBoot (status %llx)"
                        " - it accepts writes to a variable that already exists "
                        "but would not create this one",
              (unsigned long long)snapshot_status);
    }
}

/* Take it away again.
 *
 * A zero-length write is how UEFI deletes a variable, and leaving one behind
 * on somebody's board after it has been read is bad manners at best. */
bool efi_log_clear(void) {
    efi_runtime_t *rt = runtime();
    if (!rt || !rt->set_variable) return false;

    u32 attrs = EFI_VARIABLE_NON_VOLATILE |
                EFI_VARIABLE_BOOTSERVICE_ACCESS |
                EFI_VARIABLE_RUNTIME_ACCESS;
    return call_set_variable(rt, log_name, &kestrel_guid, attrs, 0, NULL) == 0;
}
