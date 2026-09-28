/* main.c - kernel entry and bring-up order.
 *
 * The loader hands over with paging on, interrupts off and a stack of its own.
 * The first job is to take a private copy of everything the loader allocated,
 * because that memory is reclaimed as soon as the physical allocator starts.
 */
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "klog.h"
#include "acpi.h"
#include "smbios.h"
#include "apic.h"
#include "smp.h"
#include "time.h"
#include "vfs.h"
#include "pci.h"
#include "svga.h"
#include "block.h"
#include "input.h"
#include "proc.h"
#include "usb.h"
#include "gpu.h"
#include "hda.h"
#include "net.h"
#include "crypto.h"
#include "firmware.h"
#include "wifi.h"

void tty_init(void);
void fat_register(void);
void ntfs_register(void);
void exfat_register(void);
void ext4_register(void);
void xfs_register(void);
void btrfs_register(void);
void devfs_publish_block_devices(void);
bool block_start_boot_log(void);
void mouse_init(void);
void eventq_init(void);

/* Defined in isr.S: move onto `stack_top` and continue in `fn`. */
extern void switch_stack_and_call(u64 stack_top, void (*fn)(void)) __attribute__((noreturn));

#define BOOT_STACK_SIZE (32 * 1024)

/* The boot task runs on this once the loader's stack is abandoned. */
u64 g_boot_stack;

static void kmain_stage2(void);

kboot_info      g_boot;
kboot_mmap_entry g_mmap[320];
u32             g_mmap_count;

extern char __kernel_start[], __kernel_end[];

/* --------------------------------------------------------------- cmdline */

const char *cmdline_get(const char *key) {
    static char value[128];
    size_t klen = strlen(key);
    const char *p = g_boot.cmdline;

    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ') p++;

        if ((size_t)(p - start) > klen && !memcmp(start, key, klen) && start[klen] == '=') {
            size_t n = (size_t)(p - start) - klen - 1;
            if (n >= sizeof value) n = sizeof value - 1;
            memcpy(value, start + klen + 1, n);
            value[n] = 0;
            return value;
        }
    }
    return NULL;
}

bool cmdline_has(const char *flag) {
    size_t flen = strlen(flag);
    const char *p = g_boot.cmdline;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ') p++;
        if ((size_t)(p - start) == flen && !memcmp(start, flag, flen)) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ boot */

static void banner(void) {
    console_set_color(C_LCYAN, C_BLACK);
    kprintf("\n  KestrelOS");
    console_set_color(C_DGRAY, C_BLACK);
    kprintf("  x86-64  build " __DATE__ "\n");
    console_set_color(C_LGRAY, C_BLACK);
    kprintf("  ");
    for (int i = 0; i < 60; i++) kprintf("\xC4");
    kprintf("\n\n");
}

static void copy_boot_info(kboot_info *bi) {
    g_boot = *bi;

    u32 n = bi->mmap_count;
    if (n > ARRAY_LEN(g_mmap)) n = ARRAY_LEN(g_mmap);
    memcpy(g_mmap, phys_to_virt(bi->mmap), n * sizeof(kboot_mmap_entry));
    g_mmap_count = n;
    g_boot.mmap = 0;    /* the loader's copy is about to be reclaimed */

    if (n < bi->mmap_count)
        kwarn("boot", "memory map truncated from %u to %u entries", bi->mmap_count, n);
}

/* Keep the loader's page tables, the kernel image and the initrd out of the
 * physical allocator's hands.  Everything else the loader used is fair game. */
static void reserve_boot_regions(void) {
    pmm_reserve(g_boot.kernel_phys, (size_t)(PAGE_ALIGN_UP(g_boot.kernel_size) / PAGE_SIZE));

    if (g_boot.initrd_base)
        pmm_reserve(g_boot.initrd_base, (size_t)(PAGE_ALIGN_UP(g_boot.initrd_size) / PAGE_SIZE));

    /* The page tables the loader built are still live in CR3.  The loader
     * allocated them as one run; 64 pages covers the largest layout it builds. */
    pmm_reserve(g_boot.pml4_phys, 64);

    /* We are still running on the loader's stack. */
    if (g_boot.loader_stack_top)
        pmm_reserve(g_boot.loader_stack_top - 64 * 1024, 16);
}

void kmain(kboot_info *bi) {
    /* First instruction of anything: a number on the motherboard's own display
     * saying the kernel is running.  On a machine with a black screen this is
     * what separates "the loader never reached the kernel" from "the kernel
     * started and something in it went wrong", and there is no other way to
     * find that out without a serial cable. */
    post(POST_K_ENTERED);
    serial_init();
    klog_init();

    if (!bi || bi->magic != KB_MAGIC) {
        char msg[192];
        int n = snprintf(msg, sizeof msg,
                         "kernel: bad boot info at %p: magic %llx (want %llx), version %u, size %u (want %u)\n",
                         (void *)bi,
                         (unsigned long long)(bi ? bi->magic : 0),
                         (unsigned long long)KB_MAGIC,
                         bi ? bi->version : 0,
                         bi ? bi->size : 0, (unsigned)sizeof(kboot_info));
        serial_write(msg, (size_t)n);
        for (;;) { cli(); hlt(); }
    }

    console_init(&bi->fb);
    post(POST_K_CONSOLE);
    banner();

    kinfo("boot", "KestrelOS kernel starting");
    kinfo("boot", "framebuffer %ux%u, %u bpp, pitch %u",
          bi->fb.width, bi->fb.height, bi->fb.bpp, bi->fb.pitch);

    copy_boot_info(bi);

    /* Now the hand-off has been copied, and before anything that can fail: the
     * region it names is where this boot's log is kept, and the loader writes
     * it out on the next boot.
     *
     * This has to come after the copy.  Placed before it, it read a structure
     * that was still all zeroes and concluded the loader was too old to have
     * reserved a region - which is exactly what it reported, and exactly why
     * saying so was worth the four lines. */
    ramlog_init();
    if (ramlog_active())
        kinfo("ramlog", "this boot's log is kept in %u KiB of memory; the "
                        "loader writes it to the boot device next boot",
              ramlog_capacity() / 1024);
    if (ramlog_recovered())
        kinfo("ramlog", "the previous boot's log (%u bytes) was written to the "
                        "boot device", ramlog_recovered());
    if (g_boot.cmdline[0]) kinfo("boot", "command line: %s", g_boot.cmdline);

    cpu_features_init();
    gdt_init();
    idt_init();
    post(POST_K_TABLES);
    kinfo("boot", "descriptor tables installed");

    pmm_init();
    reserve_boot_regions();
    vmm_init();
    heap_init();

    /* Now there is a page table to put a write-combining mapping in. */
    pat_init();
    console_remap_wc();
    post(POST_K_MEMORY);

    /* Leave the loader's stack behind.  It lives in the identity map, which is
     * private to the kernel's own page table; the moment the scheduler switched
     * to a user address space the stack would vanish underneath us.  Every
     * kernel stack from here on is in the direct map, which every address space
     * shares. */
    {
        u64 phys = pmm_alloc_pages(BOOT_STACK_SIZE / PAGE_SIZE);
        if (!phys) panic("boot: cannot allocate the kernel stack");
        g_boot_stack = (u64)phys_to_virt(phys);
        memset((void *)g_boot_stack, 0, BOOT_STACK_SIZE);
        kinfo("boot", "kernel stack at %p", (void *)g_boot_stack);
        switch_stack_and_call(g_boot_stack + BOOT_STACK_SIZE, kmain_stage2);
    }
    /* not reached */
}

/* Run one piece of bring-up, having first written down that it is about to
 * run - and having pushed that writing all the way to the disk before the
 * thing itself starts.
 *
 * The order is the whole point.  A log flushed AFTER a step contains every
 * step that finished, which is the one set of steps nobody needs to be told
 * about; the step anyone cares about is the one that never came back, and that
 * step's own line is exactly what such a log is missing.  Writing "about to X"
 * and flushing it before calling X means the last line on the stick names the
 * thing that hung, and powering the machine off - which is all anybody can do
 * with a machine that has stopped - loses nothing.
 *
 * It costs one small write to a USB stick per step, perhaps twenty in a boot.
 *
 * What this cannot do is cover the part of start-up that runs before there is
 * a disk driver to write with.  Interrupts, memory and the timer all come up
 * before storage does, and if the machine stops in there the screen is the
 * only record there is - which is why the screen says what it is doing too.
 */
static void boot_step(const char *what, void (*fn)(void)) {
    kinfo("boot", "starting: %s", what);
    klog_persist_flush();
    fn();
    kinfo("boot", "finished: %s", what);
}

static void kmain_stage2(void) {
    post(POST_K_STAGE2);
    acpi_init();

    /* After the firmware's processor table, because working out how the
     * processors are arranged needs both the identifiers from that table and
     * the field widths from CPUID. */
    cpu_topology_init();

    /* And what the firmware knows that no chip can be asked: the board's name,
     * and which memory slots are filled with what. */
    smbios_init();

    apic_init();
    timer_init();
    post(POST_K_TIME);

    /* The rest of the processors.  This needs the local APIC to send the
     * startup messages and the timer to wait between them, so it comes after
     * both - and before anything that would benefit from more than one core. */
    smp_init();

    /* Now every core is up, ask each its own kind so a hybrid part's P/E split
     * is an exact count rather than derived from the boot core alone. */
    cpu_topology_refine();

    proc_init();
    syscall_init();

    vfs_init();

    if (g_boot.initrd_base && g_boot.initrd_size) {
        filesystem_t *root = ramfs_create();
        if (!root) panic("boot: cannot create the root filesystem");
        if (vfs_mount("/", root) < 0) panic("boot: cannot mount the root filesystem");
        int n = ramfs_load_kar(root, phys_to_virt(g_boot.initrd_base), (size_t)g_boot.initrd_size);
        if (n < 0) panic("boot: the initrd is corrupt");
        kinfo("boot", "root filesystem loaded, %d entries from the initrd", n);
    } else {
        panic("boot: no initrd; there is no userland to start");
    }

    vfs_mkdir("/dev");
    /* Somewhere to put working files.  Every system has one and every program
     * assumes it: a Windows program reads it out of the environment as %TEMP%
     * and expects to be able to write there without checking. */
    vfs_mkdir("/tmp");
    filesystem_t *dev = devfs_create();
    if (dev && vfs_mount("/dev", dev) < 0) kerr("boot", "cannot mount /dev");

    post(POST_K_ROOTFS);
    tty_init();          /* /dev/console, which every process inherits */
    fat_register();      /* so vfs_probe can recognise ESP and data volumes */
    ntfs_register();     /* and the NTFS partition the system's files live on */
    exfat_register();    /* which is what a large stick or an SD card is */
    ext4_register();     /* and what a Linux disk is */
    xfs_register();      /* as is this, on a server */
    btrfs_register();    /* and this, on a desktop one */

    eventq_init();
    input_init();
    /* "nops2" leaves the PS/2 mouse alone.
     *
     * Not a user-facing option so much as a way to reproduce a machine that
     * has no PS/2 port at all - which every machine built in the last decade
     * is, and which is where the USB input path has to work on its own.  Under
     * a virtual machine the PS/2 device answers and masks whatever the USB
     * path is or is not doing. */
    if (!cmdline_has("nops2")) mouse_init();
    else kinfo("boot", "leaving the PS/2 mouse alone, as asked");
    pci_init();
    post(POST_K_PCI);

    /* Before anything draws: if this machine's display adapter can be told
     * what changed, take it over.  Left alone, the adapter has to find changes
     * by taking the framebuffer's pages away and waiting for a fault on each
     * one - which costs more than the drawing does. */
    if (svga_init()) {
        u32 max_w = 0, max_h = 0;
        svga_limits(&max_w, &max_h, NULL);
        if (svga_set_mode(g_boot.fb.width, g_boot.fb.height))
            svga_adopt_framebuffer();
        (void)max_w; (void)max_h;
        svga_selftest();
        /* And the one piece of this system's graphics that runs on real
         * graphics hardware rather than on the processor or against a
         * model of a card that is not here. */
        svga3d_selftest();
        /* And the interface that replaced the one above, which is the
         * only one a modern adapter still answers. */
        svga3d_gb_selftest();
        /* And the card drawing, rather than only moving. */
        svga3d_render_selftest();

        /* And then, if the card will take it, the display itself: the picture
         * moves into memory this system owns and the card reads directly, so
         * the window system presents a frame by naming the rectangle that
         * changed instead of copying every pixel of it into video memory.
         * Where the card will not, everything below carries on as before. */
        if (svga3d_screen_attach(g_boot.fb.width, g_boot.fb.height)) {
            u32 sw = 0, sh = 0, spitch = 0;
            u64 sphys = svga3d_screen_memory(&sw, &sh, &spitch);
            if (sphys) {
                g_boot.fb.base = sphys;
                g_boot.fb.pitch = spitch;
                g_boot.fb.size = spitch * sh;
                console_remap_wc();
            }
            /* If the adapter offers a second display, prove the card can drive
             * one independently of the first (#7 - the extend/mirror/only-other
             * modes rest on there being two real scan-outs, not one aliased). */
            svga3d_second_screen_selftest();

            /* With two displays, arrange them the way BOOT.CFG asked (#7):
             * mirror and only-the-other are a mode set on the second screen;
             * extend (the default) needs a desktop as wide as both, set up here
             * BEFORE the desktop starts so it simply sees a wide screen - no
             * runtime relayout - with svga_set_present_size so a present
             * rectangle reaching the second display is not clamped to the
             * first's width.  display_mode values: KB_DISPLAY_* (bootinfo.h);
             * svga3d modes are numbered as dl_mode_t. */
            if (svga3d_second_attached()) {
                uint8_t want = g_boot.display.display_mode;
                if (want == KB_DISPLAY_MIRROR) {
                    svga3d_set_second_mode(2 /* dl_mode_t DL_MIRROR */);
                } else if (want == KB_DISPLAY_ONLY_OTHER) {
                    svga3d_set_second_mode(3 /* dl_mode_t DL_ONLY_SECONDARY */);
                } else {                         /* KB_DISPLAY_EXTEND (default) */
                    u64 ephys = 0; u32 ew = 0, eh = 0, epitch = 0;
                    if (svga3d_extend_setup(&ephys, &ew, &eh, &epitch)) {
                        g_boot.fb.base  = ephys;
                        g_boot.fb.width = ew;
                        g_boot.fb.height = eh;
                        g_boot.fb.pitch = epitch;
                        g_boot.fb.size  = (u64)epitch * eh;
                        svga_set_present_size(ew, eh, epitch);
                        console_remap_wc();
                    } else {
                        svga3d_set_second_mode(2 /* mirror */);  /* wide alloc failed */
                    }
                }
            }
        }
    }

    gpu_init();

    /* Cover the driver takeover itself, not only the later pattern routine.
     * The old placement armed this after nvidia_driver_init(), so a firmware
     * or RM initialization stall happened before any automatic-reboot safety
     * net existed. */
    if (cmdline_has("gpustart")) {
        gpu_test_watchdog_arm();
        kthread_create("gpu-watchdog", gpu_test_watchdog_kthread, NULL);
    }
    /* The NVIDIA driver proper: identification from the silicon, the card's
     * own description out of its ROM, its memory, its sensors, and the
     * monitors on its connectors.  gpu_init above only names what is on the
     * bus. */
    /* The graphics drivers talk to a card directly, and until this system has
     * been on a machine before, that card is one they have only ever been
     * tested against a model of.  Starting with "nogpu" leaves them alone, so
     * that a first boot on unfamiliar hardware has something to fall back to
     * rather than a screen that never comes back. */
    if (cmdline_has("nogpu")) {
        kinfo("gpu", "asked not to touch the graphics card, so the drivers "
                     "for it are not started and the display stays as the "
                     "firmware left it");
    } else {
        /* Marked before the drivers run, not after.  These are the only two
         * pieces of this system that write to a graphics card's own registers,
         * and a card that does not like what it is told can take the picture
         * with it - at which point the number left on the motherboard's
         * display is the only thing that says the drivers are where it
         * stopped.  Afterwards would be too late to be worth anything. */
        post(POST_K_GPU);
        nvidia_driver_init();
        amd_driver_init();
    }
    display_report();
    block_init();

    /* Before anything else is brought up.  Everything below this line is a
     * driver talking to hardware this system has never met, and any of them is
     * somewhere a first boot can stop; a log that starts after them would be
     * empty in exactly the case it exists for.
     *
     * On a machine booted from a USB stick this finds nothing, because USB is
     * one of the things brought up below.  That is expected and not a failure:
     * the flusher thread keeps looking, and picks the stick up as soon as it
     * appears. */
    block_start_boot_log();

    /* Each of these is a driver meeting hardware for the first time, and each
     * is a place a machine can stop dead.  Pushing the log out between them
     * costs a few milliseconds and means the record on the stick ends at the
     * step that did not come back, rather than at the last step before a
     * flush that never happened. */
    boot_step("USB", usb_init);   /* the only keyboard with no PS/2 port */
    /* And the older controller, separately: it is a different design with a
     * different driver, and bringing it up must not be able to disturb the one
     * that carries this machine's keyboard. */
    boot_step("USB 1.1", uhci_init);
    boot_step("sound", hda_init);
    boot_step("networking", net_init);
    devfs_publish_block_devices();

    /* Now that storage is up, find the persistent partition and start writing
     * the event log to it. */
    boot_step("mounting storage", block_mount_system_volumes);

    /* Now that a writable volume is mounted, firmware the user supplied can be
     * found - so the crypto is checked and the wireless hardware looked at
     * after the storage stack rather than before it. */
    boot_step("looking for firmware files", firmware_init);

    /* gpustart is a dedicated validation boot.  Its GPU work is intentionally
     * deferred until every ordinary kernel driver/self-test below has finished;
     * userland must not race the RGB/pattern suite or paint a software desktop
     * over it. */

    /* The generator has to be stirred before anything asks it for a key, and
     * before the self-test that checks it. */
    boot_step("the random generator", random_init);
    /* The report descriptor parser, which is what decides whether a mouse
     * moves.  It needs no hardware - a report descriptor is bytes - and it is
     * the piece most likely to be wrong for a device nobody here owns. */
    {
        int usbhid_selftest(void);
        if (usbhid_selftest())
            kerr("boot", "the HID report parser is not reading descriptors "
                         "correctly; input devices may not work");

        /* And the isochronous endpoint contexts, for the same reason: audio
         * bandwidth is reserved by bit fields nothing reports back on, so a
         * field one shift out shows up only as silence. */
        int usb_isoch_selftest(void);
        if (usb_isoch_selftest())
            kerr("boot", "isochronous endpoint contexts are built wrongly; "
                         "USB audio devices will open and stay silent");

        /* And the path that makes a USB stick into a writable disk.
         *
         * This one is different from the others in kind.  The rest are
         * arithmetic that no hardware here exercises; this is a whole driver
         * that had never been executed at all, because every machine this is
         * tested on boots from something else - and the stick is this
         * system's primary medium.  Everything a user would read afterwards
         * is written through it. */
        int usbmsc_selftest(void);
        if (usbmsc_selftest())
            kerr("boot", "USB storage is not working; a stick will be found "
                         "and will not carry a log or a filesystem");

        /* And the Bluetooth event parsing, for the same reason as both of
         * those: every field is at an offset counted from a document, and a
         * wrong one produces a plausible answer rather than a failure. */
        int btusb_selftest(void);
        if (btusb_selftest())
            kerr("boot", "Bluetooth events are being read wrongly; devices "
                         "will be found under the wrong addresses or names");

        /* On a machine with no real Bluetooth radio, a modelled one can be
         * made live for demonstration with the `btmodel` boot flag, so `bt
         * scan` and `bt connect` have something to find and open. */
        if (cmdline_has("btmodel")) {
            void btusb_model_register(void);
            btusb_model_register();
        }

        /* And the division of a frame between processors, which is arithmetic
         * whose failure looks like a display fault rather than like a sum. */
        int present_band_selftest(void);
        if (present_band_selftest())
            kerr("boot", "frames are divided between processors wrongly; the "
                         "screen will tear or show stale rows");

        /* And the ring audio travels through, whose failure is a click rather
         * than an error. */
        int usbaudio_ring_selftest(void);
        if (usbaudio_ring_selftest())
            kerr("boot", "the audio ring returns the wrong bytes; USB sound "
                         "will play corrupted");

        /* And the recording side's wrapping, which fails the same way. */
        int hda_capture_selftest(void);
        if (hda_capture_selftest())
            kerr("boot", "recording reads the wrong bytes across the end of "
                         "its buffer; captured sound will be corrupted");

        /* And the framing of messages to the graphics card's co-processor,
         * which fails the same quiet way: a header a bit out of place is
         * ignored rather than refused. */
        int nv_gsp_msg_selftest(void);
        if (nv_gsp_msg_selftest())
            kerr("boot", "messages to the card's co-processor are framed "
                         "wrongly; it would ignore every one of them");

        int nv_gsp_ring_selftest(void);
        if (nv_gsp_ring_selftest())
            kerr("boot", "the rings to the card's co-processor count their "
                         "entries wrongly; messages would be lost or repeated");

        int nv_gsp_rpc_selftest(void);
        if (nv_gsp_rpc_selftest())
            kerr("boot", "requests to the card's firmware carry the wrong "
                         "header; it would answer none of them");

        /* (The GPU co-processor boot ran earlier, right after firmware was
         * found - moved ahead of these hardware self-tests so a stalling one
         * cannot stop its result from reaching the stick.) */

        /* The arithmetic behind writing to NTFS: run lists, name ordering,
         * and the per-sector fixups.  Checked here because every one of them
         * fails silently on a real volume - a run list encoded wrongly writes
         * to the wrong place and reads back from it just as happily, and a
         * name ordered wrongly leaves a file that Windows cannot find. */
        /* The start-up sequence for the Wi-Fi 7 part, against a model of it.
         * Every way this can be wrong - a wait in the wrong direction, two
         * steps swapped, a bit dropped from one of the long enable masks -
         * produces a card that never reports ready, which is independent of
         * whether the card is actually broken. */
        int rtw89_selftest(void);
        if (rtw89_selftest())
            kerr("boot", "the Wi-Fi 7 start-up sequence is wrong; that card "
                         "would never come out of reset");

        /* The exFAT checksums and its name hash.  All three fail silently:
         * a wrong name hash finds no file, a wrong entry checksum makes every
         * file look corrupt, and a wrong boot checksum makes a volume this
         * wrote unmountable by anything that checks it. */
        /* XFS packs four fields of an extent across 128 bits with not one of
         * them starting on a byte, and reads every number most significant
         * byte first while every other filesystem here does the opposite.
         * Both are ways of being plausibly wrong rather than failing. */
        /* Btrfs keeps the map from its own addresses to real ones inside
         * itself, so a reader that gets the bootstrap wrong reads a plausible
         * wrong part of the disk rather than failing. */
        int btrfs_selftest(void);
        if (btrfs_selftest())
            kerr("boot", "the Btrfs address map is wrong; such a volume would "
                         "be read from the wrong place");

        int xfs_selftest(void);
        if (xfs_selftest())
            kerr("boot", "the XFS extent arithmetic is wrong; files on such a "
                         "volume would read from the wrong place");

        int exfat_selftest(void);
        if (exfat_selftest())
            kerr("boot", "the exFAT checksums are wrong; files on such a "
                         "volume would not be found");

        int ntfs_write_selftest(void);
        if (ntfs_write_selftest())
            kerr("boot", "the NTFS writer's arithmetic is wrong; files "
                         "created on an NTFS volume would be misplaced");
    }

    kinfo("boot", "starting: checking the cryptography");
    klog_persist_flush();
    crypto_selftest();               /* returns a failure count, not void */
    kinfo("boot", "finished: checking the cryptography");
    boot_step("wireless", wifi_init);

    /* Every driver has now had its chance at the bus, so what is left over is
     * the real answer to "what hardware does this system not support yet".
     * Last, deliberately: asking any earlier would blame a driver that had
     * simply not run. */
    pci_report_coverage();

    kinfo("boot", "start-up complete in %lu ms", g_uptime_ms);

    bool gpu_test_boot = cmdline_has("gpustart");
    bool gpu_desktop_ready = false;
    if (gpu_test_boot) {
        kinfo("boot", "kernel setup complete; starting the exclusive NVIDIA pattern/acceleration suite");
        /* Start this before the synchronous transaction: if a GPU method or
         * removable-media checkpoint never returns, the normal post-test
         * 20-second worker cannot be created.  The watchdog guarantees the
         * test machine still reboots without manual intervention. */
        klog_persist_flush();
        gpu_desktop_ready = gpu_boot_test_sync();
    }

    /* Leave the account of this boot somewhere that outlives the power.
     *
     * Here, at the end of start-up, because this is the moment the log is
     * complete AND still describes only start-up: every driver has reported
     * whether it found its hardware, which is what somebody debugging a
     * machine they cannot see needs.
     *
     * It goes into the firmware's own variable store rather than to a disk,
     * because on a machine where the storage driver is the thing that failed
     * there is no disk to write to - and that is exactly the machine where
     * the log matters.  Shutting down publishes it again, so anything done
     * afterwards is not lost either. */
    {
        bool efi_log_publish(void);
        efi_log_publish();
    }
    if (!gpu_test_boot || gpu_desktop_ready)
        post(POST_K_USERLAND);
    /* In GPU-test mode the time-critical transaction has already emitted its
     * summary into the in-memory/serial log.  Let boot/logflush persist it;
     * a synchronous FAT append here previously prevented the reboot worker
     * from ever being created. */
    if (!gpu_test_boot) klog_persist_flush();

    if (!gpu_test_boot || gpu_desktop_ready) {
        if (gpu_test_boot)
            kthread_create("gpu-reboot", gpu_test_reboot_kthread, (void *)1);
        proc_start_init();
    } else {
        kerr("boot", "GPU GUI/2D post-codec validation failed; desktop withheld so software rendering cannot masquerade as acceleration");
        kthread_create("gpu-reboot", gpu_test_reboot_kthread, NULL);
    }

    /* Boot remains an ordinary task, separate from the scheduler's lock-free
     * idle context. Keep the fallback log flusher even if userland was withheld;
     * sleep between flushes instead of remaining runnable around HLT. */
    sti();
    for (;;) {
        klog_persist_flush();
        sched_sleep_ms(250);
    }
}
