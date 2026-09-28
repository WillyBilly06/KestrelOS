/* syscall.h - the KestrelOS system call ABI.
 *
 * Calls use the SYSCALL instruction.  RAX carries the call number, arguments
 * are in RDI, RSI, RDX, R10, R8, R9 (R10 rather than RCX, which SYSCALL
 * clobbers), and RAX carries the result: a non-negative value on success or a
 * negated error number on failure.
 */
#ifndef KESTREL_SYSCALL_H
#define KESTREL_SYSCALL_H

#define SYS_EXIT        0
#define SYS_WRITE       1
#define SYS_READ        2
#define SYS_OPEN        3
#define SYS_CLOSE       4
#define SYS_SEEK        5
#define SYS_STAT        6
#define SYS_READDIR     7
#define SYS_MKDIR       8
#define SYS_UNLINK      9
#define SYS_RENAME      10
#define SYS_SPAWN       11
#define SYS_WAIT        12
#define SYS_KILL        13
#define SYS_SLEEP       14
#define SYS_YIELD       15
#define SYS_GETPID      16
#define SYS_SBRK        17
#define SYS_MMAP        18
#define SYS_MUNMAP      19
#define SYS_UPTIME      20
#define SYS_TIME        21
#define SYS_LOG_WRITE   22
#define SYS_LOG_READ    23
#define SYS_LOG_CTL     24
#define SYS_SYSINFO     25
#define SYS_CONSOLE     26
#define SYS_POWEROFF    27
#define SYS_REBOOT      28
#define SYS_MOUNT       29
#define SYS_UNMOUNT     30
#define SYS_SYNC        31
#define SYS_CHDIR       32
#define SYS_GETCWD      33
#define SYS_TRUNCATE    34
#define SYS_IOCTL       35
#define SYS_ENUM        36
#define SYS_BLKRESCAN   37
#define SYS_PROCLIST    38
#define SYS_EFIVAR      39
#define SYS_EXEC        40
#define SYS_MOUNTLIST   41
#define SYS_MEMINFO     42
#define SYS_FRAMEBUFFER 43
#define SYS_MOUSEPOS    44
#define SYS_PIPE        45
#define SYS_DUP         46
#define SYS_DUP2        47
#define SYS_NET         48
#define SYS_WIFI        49
#define SYS_THREAD      50
#define SYS_FUTEX       51
#define SYS_GPU         52
#define SYS_BLUETOOTH   53
#define SYS_MAX         54

/* Standard descriptors, opened for every process. */
#define STDIN_FD  0
#define STDOUT_FD 1
#define STDERR_FD 2

/* What SYS_THREAD is being asked to do. */
#define THREAD_CREATE   0    /* (entry, arg) -> tid */
#define THREAD_EXIT     1    /* (code); does not return */
#define THREAD_SETGS    2    /* (base): where GS points for this thread */
#define THREAD_GETTID   3
#define THREAD_ONFAULT  4    /* (handler): where a fault is delivered */
#define THREAD_HANDLED  5    /* the handler dealt with it and is leaving */

/* What a process is told when one of its threads faults and it asked to hear
 * about it.  The registers are exactly as they were at the faulting
 * instruction, so a handler can report them or resume somewhere else. */
typedef struct {
    uint32_t vector;         /* the CPU exception number                */
    uint32_t error;          /* the CPU's error code, where there is one */
    uint64_t address;        /* the address touched, for a page fault   */
    uint64_t rip, rsp, rflags;
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
} kfault_t;

/* What SYS_FUTEX is being asked to do.  See kernel/futex.c. */
#define FUTEX_WAIT_OP   0    /* (addr, expect, timeout_ms) */
#define FUTEX_WAKE_OP   1    /* (addr, count) -> number woken */

/* What SYS_FRAMEBUFFER is being asked to do.  Only one process draws on the
 * screen at a time, so a program that wants it has to be given it and a
 * program that is finished with it has to say so. */
#define FB_MAP        0   /* map it in and take ownership          */
#define FB_RELEASE    1   /* give it back to the console           */
#define FB_REACQUIRE  2   /* take it again, without remapping      */
#define FB_UPDATE     3   /* a1 -> {x, y, w, h}: this part changed  */
#define FB_SETMODE    4   /* a1 width, a2 height, while running     */
#define FB_CURSOR     5   /* a1 -> kcursor_t, or 0 to hide it       */
#define FB_CURSORAT   6   /* a1 x, a2 y                             */
/* Work the adapter can do itself.  A fill of a large rectangle is six words
 * into a ring instead of megabytes through the processor, and a copy is how a
 * window moves without being redrawn.  Both are refused when the adapter does
 * not advertise them, so a caller has to be able to do it the slow way. */
#define FB_FILL       7   /* a1 -> {x, y, w, h, colour}             */
#define FB_COPY       8   /* a1 -> {from_x, from_y, to_x, to_y, w, h} */
#define FB_ACCEL      9   /* what the adapter will do: a bitmask     */
#define FB_DISPLAYMODE 11 /* a1 = multi-display mode (dl_mode_t); returns 0
                           * applied live, 1 requires restart. The caller
                           * must explicitly persist a startup preference.
                           * 10 belongs to FB_PRESENT; never reuse it. */
#define FB_CHECK_CONFIGURATION 12 /* a2 -> kdisplay_configuration_t,
                                  * a3 -> kdisplay_configuration_check_t.
                                  * Read-only geometry/cached-mode preflight;
                                  * success does not mean applied or hardware validated. */
#define FB_PREPARE_CONFIGURATION 13 /* a2 -> configuration, a3 -> prepared result;
                                    * retains private buffers, never applies. */
#define FB_CANCEL_CONFIGURATION 14 /* a2 = owner-bound prepare token */
#define FB_APPLY_OUTPUT_MODE 15 /* a2 -> output mode request; returns positive token */
#define FB_CONFIRM_OUTPUT_MODE 16 /* a2 = owner-bound live timing token */
#define FB_REVERT_OUTPUT_MODE 17 /* a2 = owner-bound live timing token */

#define FB_ACCEL_FILL   0x1
#define FB_ACCEL_COPY   0x2
#define FB_ACCEL_CURSOR 0x4
#define FB_PRESENT   10   /* a1 -> kpresent_t: copy a back buffer   *
                           * forward, using every processor          */

/* What to copy forward, and from where.
 *
 * A window system draws into its own buffer and copies the parts that changed
 * onto the display.  That copy is the largest single piece of work a desktop
 * does per frame, and doing it from one processor while the rest of the
 * machine is idle is what makes a machine with twenty cores feel like a
 * machine with one.  Handing it over lets the system spread it.
 */
typedef struct {
    unsigned long long back;    /* the program's own buffer                 */
    unsigned int stride;        /* pixels per row in it, not bytes          */
    int x, y, w, h;             /* the part that changed                    */
} kpresent_t;

/* A pointer for the display adapter to draw itself, so that moving it costs
 * nothing and never disturbs the picture underneath. */
typedef struct {
    int          width, height;      /* at most 64 by 64          */
    int          hot_x, hot_y;
    unsigned int pixels[64 * 64];    /* alpha in the top byte     */
} kcursor_t;

/* SYS_CONSOLE sub-commands. */
#define CON_GET_SIZE    1   /* arg -> struct { int cols, rows; }        */
#define CON_SET_CURSOR  2   /* arg -> struct { int col, row; }          */
#define CON_GET_CURSOR  3
#define CON_SET_RAW     4   /* arg = 1 raw, 0 cooked (line editing)     */
#define CON_CLEAR       5
#define CON_SHOW_CURSOR 6   /* arg = 1 visible, 0 hidden                */

/* SYS_LOG_CTL sub-commands. */
#define LOGCTL_COUNTS   1   /* arg -> uint32_t[5], counts per level     */
#define LOGCTL_CLEAR    2
#define LOGCTL_NEXTSEQ  3   /* returns the next sequence number         */
#define LOGCTL_FLUSH    4   /* force a write-out to the persistent log  */
#define LOGCTL_DROPPED  5
#define LOGCTL_CONSOLE_LEVEL 6   /* arg = minimum level echoed to the console */

/* What SYS_GPU is being asked to do. */
#define GPUOP_SELFTEST  0   /* drive the graphics driver against a model     */
#define GPUOP_DETAIL    1   /* what the driver read out of the card          */
#define GPUOP_CANDRAW   2   /* is there a card a program can draw on          */
#define GPUOP_DRAW      3   /* a1 -> vertices, a2 = triangles                 */
#define GPUOP_IMAGE     4   /* a1 -> kimage_t: pixels onto the screen         */
#define GPUOP_SHADERS   5   /* a1 -> kshaders_t: a program's own shaders      */
#define GPUOP_LAYOUT    6   /* a1 -> klayout_t: how to read a vertex          */
#define GPUOP_START     7   /* start the card's own co-processor - see
                             * nv_gsp.c: deliberate, and it takes the card
                             * away from whatever is driving the screen  */
#define GPUOP_READPIXEL 8   /* a1=x, a2=y, a3->pixel read from live GPU VRAM */
#define GPUOP_SURFACE   9   /* a1 -> kg2d_request_t, a2 = sizeof request */
#define GPUOP_VIDEO    10   /* a1 -> kvideo_request_t, a2 = sizeof request */

/* How one thing inside a vertex is found: which buffer it comes from, how far
 * into each vertex it sits, what kind of numbers it is, and which of the
 * program's inputs it arrives as.  A program that lays its vertices out its
 * own way describes them this way rather than being made to match. */
typedef struct {
    unsigned int slot;
    unsigned int offset;
    unsigned int format;         /* as this system numbers formats */
    unsigned int reg;            /* which input of the program     */
} kelement_t;

#define KLAYOUT_MAX 16

typedef struct {
    kelement_t   elements[KLAYOUT_MAX];
    unsigned int count;
    unsigned int stride;         /* one vertex to the next, in bytes */
} klayout_t;

/* Legacy SVGA-DX shader bytecode and signatures, not portable native GPU code.
 * GPUOP_SHADERS/GPUOP_LAYOUT require this format on the selected adapter;
 * unsupported adapters return NOSYS without changing a different GPU. The
 * programmable native surface API is separate. */
/* One line of what a program is handed or hands on: which register carries it,
 * and whether what it carries means something to the pipeline itself or is
 * only a value being passed along.  The compiler writes this down beside the
 * instructions, so it is read rather than guessed. */
typedef struct { unsigned int reg, meaning; } ksigline_t;

#define KSIG_MAX 16

typedef struct {
    const unsigned int *vertex;
    unsigned int vertex_words;
    const unsigned int *pixel;
    unsigned int pixel_words;

    ksigline_t   vertex_takes[KSIG_MAX], vertex_gives[KSIG_MAX];
    unsigned int vertex_takes_count, vertex_gives_count;
    ksigline_t   pixel_takes[KSIG_MAX], pixel_gives[KSIG_MAX];
    unsigned int pixel_takes_count, pixel_gives_count;
} kshaders_t;

/* A rectangle of pixels for the card to put on the screen, and where it goes.
 * What is already there shows through wherever the pixels are not solid, which
 * is what makes this enough to compose a window rather than only to fill. */
typedef struct {
    const unsigned int *pixels;
    unsigned int width, height;      /* of the picture, at most 1024 each */
    int          x, y;               /* where on the screen it lands     */
    unsigned int draw_width, draw_height;
    /* How wide the thing the picture was cut out of is, in pixels.  Zero
     * means the picture is not part of anything larger.
     *
     * This is what lets a window system hand over the part of a frame that
     * changed rather than the whole of it - a rectangle inside a frame has the
     * rest of each row in between, and without a stride the only way to pass
     * it is to copy it out somewhere contiguous first, which is the copy the
     * card was supposed to save. */
    unsigned int source_stride;
} kimage_t;

/* One corner of a triangle a program hands to the card: where it is, in the
 * coordinates the pipeline expects - across and up from the middle of the
 * screen, from minus one to one - and what colour it carries.  What lands on
 * each pixel between three of these is the card's work. */
typedef struct {
    float x, y, z, w;
    float r, g, b, a;
} kvertex_t;

/* What the graphics driver found, beyond what the enumeration reports: the
 * card's own description of itself, its sensors and its monitors. */
typedef struct {
    uint32_t chipset;
    uint8_t  revision;
    uint8_t  outputs;
    uint8_t  monitors;
    uint8_t  modelled;         /* a model is standing in for a card          */
    int32_t  temperature_c;    /* -1000 when it could not be read            */
    int32_t  fan_percent;      /* -1 when there is no reading                */
    uint64_t vram_bytes;
    char     architecture[24];
    char     codename[16];
    char     vbios_version[32];
    char     vbios_source[40];
    /* One line per connector: what it is and what is plugged into it. */
    char     connector[8][80];
} kgpudetail_t;

/* SYS_ENUM kinds. */
#define ENUM_BLOCK      1
#define ENUM_PCI        2
#define ENUM_GPU        3
#define ENUM_USB        4
#define ENUM_VIDEOMODE  5
#define ENUM_DISPLAY    6
#define ENUM_AUDIO      7
#define ENUM_NET        8
#define ENUM_ARP        9
#define ENUM_WIFI      10
#define ENUM_SCAN      11
#define ENUM_FIRMWARE  12
#define ENUM_DISPLAY_OUTPUT 13
/* Index packs output ordinal in bits 16..31 and mode ordinal in 0..15. */
#define ENUM_DISPLAY_OUTPUT_MODE 14
#define ENUM_CPU 15 /* enabled logical processor, indexed independently of MADT */
#define KCPU_FREQ_EFFECTIVE_VALID 1u /* APERF / wall interval, including idle */
#define KCPU_FREQ_ACTIVE_VALID    2u /* reference * APERF / MPERF, while active */

typedef struct {
    uint32_t apic_id, core_id, package_id;
    uint32_t kind; /* 0 unknown/non-hybrid, 1 performance, 2 efficiency */
    uint32_t online, effective_mhz; /* idle-inclusive average, not active clock */
    uint32_t frequency_valid, active_mhz; /* validity bits above; former reserved word */
    uint64_t sampled_ms;
} kcpuinfo_t;

/* SYS_EFIVAR operations. */
#define EFIVAR_SET_BOOT_ENTRY 1   /* install a Boot#### entry for KestrelOS */
#define EFIVAR_AVAILABLE      2

/* Shapes shared with userland. */
#ifndef __ASSEMBLER__
#include <stdint.h>

typedef struct {
    uint32_t type;      /* VN_FILE, VN_DIR, ... */
    uint32_t mode;
    uint64_t size;
    uint64_t mtime;
} kstat_t;

typedef struct {
    char     name[256];
    uint32_t type;
    uint64_t size;
} kdirent_t;

typedef struct {
    uint64_t seq;
    uint64_t time_ms;
    uint32_t level;
    uint32_t pad;
    char     subsys[16];
    char     msg[168];
} klog_record_t;

typedef struct {
    char     cpu[64];
    char     kernel[32];
    uint64_t uptime_ms;
    uint64_t mem_total;
    uint64_t mem_free;
    uint64_t heap_total;
    uint64_t heap_used;
    uint32_t proc_count;
    uint32_t thread_count;   /* processes plus their extra threads */
    uint32_t cpu_count;

    /* How the processors are actually arranged.
     *
     * cpu_count above is the number of logical processors and was for a long
     * time the only figure reported, labelled as though it were the core
     * count.  These are the real ones.  A zero means the processor would not
     * say, and is to be shown as nothing rather than as a zero. */
    uint32_t cpu_sockets;
    uint32_t cpu_cores;
    uint32_t cpu_threads;
    uint32_t cpu_threads_per_core;

    /* On a hybrid part, how many of each kind - counted by asking each core its
     * own type once they are all running.  Zero on a non-hybrid part or before
     * that pass; shown only when non-zero. */
    uint32_t cpu_perf_cores;
    uint32_t cpu_eff_cores;

    /* How many of them this system is actually running on.
     *
     * Not the same question as how many the machine has, and the difference is
     * the one worth showing: a report of twenty cores says what was bought,
     * while this says what is being used.  They were not equal - everything
     * ran on one - and nothing distinguished the two, so a machine using a
     * twentieth of its processor looked identical to one using all of it. */
    uint32_t cpu_running;
    uint32_t cpu_base_mhz, cpu_max_mhz, cpu_bus_mhz;
    uint64_t cpu_l1, cpu_l2, cpu_l3;      /* totals across the machine      */
    uint8_t  cpu_virtualization;
    uint8_t  cpu_hybrid;
    /* Former padding: zero means unavailable, not zero running CPUs. Distinct
     * from cpu_running, which also counts restricted kernel workers. */
    uint8_t  cpu_application_threads;
    uint8_t  cpu_time_valid; /* complete BSP + online-worker accounting snapshot */
    char     cpu_virt_name[8];            /* "VT-x", "AMD-V", or empty      */
    char     cpu_boot_core_kind[16];      /* on a hybrid part               */

    /* What the firmware says about the machine itself.  Empty means the
     * firmware did not say, which is common and is shown as nothing. */
    char     board_maker[48], board_product[48];
    char     system_maker[48], system_product[48];
    char     bios_version[32], bios_date[16];
    uint32_t mem_slots_total, mem_slots_used;
    uint32_t mem_speed_mts;
    char     mem_kind[12];                /* "DDR5"                         */
    char     mem_form[12];                /* "DIMM"                         */
    uint32_t fb_width, fb_height;
    uint32_t pci_count;
    uint32_t block_count;

    /* What the machine has been doing.  These are totals since boot, on
     * purpose: a percentage is only meaningful over a stated interval, so the
     * kernel reports the raw counters and whoever wants a percentage takes two
     * readings and divides.  A kernel that picked the interval itself would be
     * picking it for every caller. */
    uint64_t cpu_busy_ms;
    uint64_t cpu_idle_ms;
    uint64_t disk_read_bytes;
    uint64_t disk_write_bytes;

    /* Whether anything can drive this machine.  A graphical shell on a
     * computer with no keyboard and no pointer is not a shell, it is a
     * picture - and worse, it covers the console, which on such a machine is
     * the only thing that could still say what went wrong. */
    uint32_t keyboards;
    uint32_t pointers;

    /* Input reports that have actually arrived from the hardware, and the
     * first bytes of the most recent one.  A device can be enumerated,
     * claimed, and counted above while never sending anything - and from
     * outside those two situations look identical. */
    uint32_t hid_reports;
    uint32_t hid_rejected;   /* arrived but had no driver to deliver to */
    uint8_t  hid_last[8];
    uint8_t  hid_last_len;
    uint8_t  logging;        /* the event log has found somewhere to write */
    /* And where.  "log on disk: no" was the whole of what this could say, and
     * the useful half was missing: which volume it chose, or - when it chose
     * none - that there was none to choose. */
    char     log_path[64];

    /* What the USB controllers have seen.  Reports arriving but the pointer
     * not moving is a different fault from no reports at all, and an endpoint
     * that is not Running is a third - and from outside they look the same. */
    uint32_t usb_events;
    uint32_t usb_transfers;
    uint8_t  usb_last_code;
    uint8_t  usb_ep_state;   /* 1 is Running; anything else is a fault */
    uint8_t  usb_disks;      /* USB storage devices brought up             */
    uint8_t  pad3[3];
    char     storage_stage[48];   /* how far the last one got              */
} ksysinfo_t;

typedef struct {
    char     name[24];        /* disk0, disk0p1, ...          */
    char     model[48];
    char     label[40];
    uint64_t size;            /* bytes                        */
    uint32_t sector_size;
    uint32_t part_index;      /* 0 for a whole disk           */
    char     type_guid[40];   /* GPT partition type, if any   */
    char     part_guid[40];
    uint8_t  is_partition;
    uint8_t  readonly;
    uint8_t  pad[6];
} kblockinfo_t;

typedef struct {
    uint16_t segment;
    uint8_t  bus, slot, func;
    uint16_t vendor, device;
    uint8_t  class_code, subclass, prog_if, revision;
    /* The driver that took this device on, empty when nothing did.  Without
     * it, a device nothing drives and a device driven perfectly well look
     * identical from userland - and which is which is the whole question. */
    char     driver[24];
    char     description[96];
} kpciinfo_t;

/* One graphics card.  `note` is the honest sentence about what is and is not
 * driven, which belongs next to the card's name rather than in a manual. */
typedef struct {
    uint16_t pci_vendor, pci_device;
    uint8_t  bus, slot, func;
    uint8_t  boot_display;      /* the one the firmware left a framebuffer on */
    uint8_t  vram_exact;        /* 0 when vram_bytes is only the aperture     */
    uint8_t  accel;             /* 0 none, 1 framebuffer, 2 the card's 2D     */
    uint32_t chipset;           /* what the silicon reports about itself      */
    uint64_t vram_bytes;
    char     name[64];
    char     arch[40];
    char     note[112];
    uint8_t  firmware_needed;
    uint8_t  firmware_present;
    uint16_t pad2;
    char     firmware_name[64];

    /* What the card is doing now.
     *
     * A percentage is a percentage; -1 means the card has no such engine and
     * -2 means it has one whose activity this driver cannot see without the
     * firmware that owns the card.  Those two are kept apart on purpose: shown
     * as zero they would both read as "idle", which is true of neither. */
    int32_t  engine_percent[4];   /* 3D, copy, video encode, video decode  */
    uint8_t  engines_sampled;     /* 0 until a window has been collected   */
    uint8_t  gpu_pad[3];
    int32_t  temperature_c;       /* -1000 when it could not be read       */
    int32_t  fan_percent;
    uint64_t vram_used;           /* what the driver handed out; a floor   */
    char     driver_version[24];
    char     driver_date[16];

    /* Which drawing interfaces programs can use, and what runs them.
     *
     * There is no DirectX version to report in the sense a Windows machine
     * reports one: DirectX is Microsoft's, and what this system has is its own
     * implementation of the Direct3D 9 interface alongside OpenGL.  Naming a
     * DirectX version would be claiming compatibility that has not been
     * established; naming the interfaces actually offered, and saying honestly
     * what executes them, is the true answer to the same question. */
    char     graphics_apis[64];   /* "OpenGL 1.2, Direct3D 9"              */
    char     renderer[48];        /* what actually draws                    */
} kgpuinfo_t;

/* One display mode the firmware offers. */
typedef struct {
    uint16_t width, height;
    uint8_t  current;      /* the mode in use now                    */
    uint8_t  native;       /* matches what the panel says it wants   */
    uint16_t pad;
} kvideomode_t;

/* What the monitor reported about itself over EDID.  All zero when there was
 * no EDID to read, which is normal on a virtual machine. */
typedef struct {
    uint16_t native_width, native_height;
    uint32_t refresh_mhz;        /* millihertz: 59940 is 59.94 Hz; u32 because
                                  * 120/144/240 Hz overflow 16 bits (65.5 Hz) */
    uint32_t max_refresh_mhz;
    uint16_t phys_width_mm, phys_height_mm;
    uint16_t mode_count;
    uint8_t  present;
    uint8_t  pad;
    char     manufacturer[4];
    char     model[16];

    /* What the display adapter itself will do, which is not the same question
     * as what the firmware happened to offer at start-up.  The firmware's list
     * is a handful of sizes it thought were sensible; the adapter's limit is
     * how much video memory there is and how wide its scan-out can go.  A
     * system that only ever offers the firmware's list cannot reach the
     * resolutions the hardware is capable of. */
    uint32_t driver_max_width, driver_max_height;
    uint32_t driver_vram_bytes;
    uint8_t  driver_present;      /* a driver is running the adapter */
    uint8_t  display_count;       /* independent displays available (#7) */
    uint8_t  pad2[2];
} kdisplayinfo_t;

/* What SYS_BLUETOOTH is being asked to do.
 *
 * The kernel holds the adapter, the discovery results and the open link.  A
 * program asks it to look, reads what was found, and asks it to open a link to
 * one of them - which is the whole of what a person does with Bluetooth before
 * a profile gets involved.
 */
#define BTOP_ADAPTER     0   /* a1 -> kbtadapter_t: is there one, and what   */
#define BTOP_SCAN        1   /* a1 seconds: listen, and return when done     */
#define BTOP_FOUND       2   /* a1 index, a2 -> kbtdevice_t                  */
#define BTOP_CONNECT     3   /* a1 -> six bytes of address                   */
#define BTOP_DISCONNECT  4
#define BTOP_LINK        5   /* a1 -> kbtlink_t: what is open, if anything   */

typedef struct {
    unsigned int present;
    unsigned char address[6];
    char maker[40];
} kbtadapter_t;

typedef struct {
    unsigned char address[6];
    unsigned int  low_energy;
    int           rssi;            /* 127 when the adapter did not say       */
    unsigned int  device_class;
    char          name[32];
    char          kind[24];        /* "audio", "keyboard or mouse", ...      */
} kbtdevice_t;

typedef struct {
    unsigned int  open;
    unsigned char address[6];
    unsigned int  handle;
    unsigned int  interval_us;
} kbtlink_t;

/* What SYS_WIFI is being asked to do. */
#define WIFIOP_SCAN       1   /* a1 interface, a2 timeout                  */
#define WIFIOP_CONNECT    2   /* a1 -> {iface, ssid, passphrase}, a2 timeout */
#define WIFIOP_DISCONNECT 3   /* a1 interface                              */
#define WIFIOP_SELFTEST   4   /* exercise the protocol with no radio       */
#define WIFIOP_ENABLE     5   /* a1 interface, a2 on or off                */
#define WIFIOP_ENABLED    6   /* a1 interface: is the radio switched on    */

/* One wireless interface. */
typedef struct {
    char     name[16];
    char     model[64];
    uint8_t  mac[6];
    uint8_t  radio_up;
    /* The card is a generation this system has no driver for.  Not the same as
     * a radio that would not start. */
    uint8_t  unsupported_generation;
    uint8_t  enabled;            /* the user has not switched it off        */
    uint8_t  firmware_needed;
    uint8_t  firmware_present;
    uint8_t  state;              /* WIFI_* */
    uint8_t  security;           /* WIFI_SECURITY_* */
    uint8_t  channel;
    int8_t   signal_dbm;
    char     ssid[33];
    char     firmware_name[64];
    char     vendor[24];
} kwifiinfo_t;

/* One network a scan found.  Index the interface in the high half of the
 * index and the network in the low half. */
typedef struct {
    char    ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t  signal_dbm;
    uint8_t security;
    uint8_t hidden;
    uint16_t pad;
} kwifinet_t;

/* A firmware file some hardware wants. */
typedef struct {
    char     name[64];
    char     source[96];
    uint8_t  present;
    uint8_t  pad[7];
    uint64_t size;
} kfirmware_t;

/* What SYS_NET is being asked to do. */
#define NETOP_DHCP     1     /* a1 interface name, a2 timeout in ms      */
#define NETOP_SET      2     /* a1 name, a2 -> {ip, mask, gateway, dns}  */
#define NETOP_RESOLVE  3     /* a1 host name, a2 timeout; returns the ip */
#define NETOP_PING     4     /* a1 -> {ip, count, timeout}, a2 -> times  */
#define NETOP_CONNECT  5     /* a1 -> {ip, port, timeout}                */
#define NETOP_SEND     6     /* a1 -> {handle, len, timeout}, a2 data    */
#define NETOP_RECV     7     /* a1 -> {handle, len, timeout}, a2 buffer  */
#define NETOP_CLOSE    8     /* a1 handle                                */
#define NETOP_TLS      9     /* a1 -> {ip, port, timeout}, a2 host name  */
#define NETOP_TLSPEER 10     /* a1 handle, a2 -> ktlspeer_t              */

/* A handle from NETOP_TLS carries this bit, so that send, receive and close
 * know which of the two kinds of connection they are being given without the
 * caller having to keep track. */
#define NET_TLS_HANDLE 0x4000

/* What the far end proved it was, for anything that wants to show it. */
typedef struct {
    char subject[128];
    char issuer[128];
    char organisation[128];
    unsigned short valid_until_year;
    unsigned char  valid_until_month, valid_until_day;
    int  chain_length;
    char cipher[32];
} ktlspeer_t;

/* One network interface. */
typedef struct {
    char     name[16];
    char     model[48];
    uint8_t  mac[6];
    uint8_t  link_up;
    uint8_t  configured;
    uint32_t link_speed_mbps;
    uint32_t ip, netmask, gateway, dns;
    uint64_t rx_packets, tx_packets;
    uint64_t rx_bytes, tx_bytes;
    uint64_t rx_dropped, tx_dropped;
    uint64_t rx_errors, tx_errors;
} knetinfo_t;

/* One entry in the address resolution table. */
typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    uint16_t pad;
    uint64_t seen_ms;
} karpentry_t;

/* The sound hardware.  `note` says what was found in one line, including when
 * a controller is present but nothing usable is behind it. */
typedef struct {
    uint16_t pci_vendor, pci_device;
    uint32_t codec_id;
    uint32_t sample_rate;
    uint8_t  present;
    uint8_t  output_ready;
    uint8_t  channels, bits;
    uint8_t  dac_node, pin_node;

    /* Recording, which is a separate converter with its own capabilities: a
     * codec that plays at 192 kHz frequently records at less, so this is what
     * the input converter said rather than a copy of the output's. */
    uint8_t  input_ready;
    uint32_t input_rate;
    uint8_t  input_bits, input_channels;
    uint8_t  adc_node, in_pin_node;
    uint8_t  pad;
    char     controller[48];
    char     codec[48];
    char     note[96];
} kaudioinfo_t;

/* One USB device on the root hub. */
typedef struct {
    uint8_t  port, slot, speed;
    uint8_t  pad;
    uint16_t vendor, product;
    char     name[48];
    char     driver[24];
} kusbinfo_t;

typedef struct {
    int      pid;
    int      parent;
    uint32_t state;
    uint32_t pad;
    char     name[32];
    uint64_t cpu_ms;
    uint64_t mem_bytes;
} kprocinfo_t;

#endif /* __ASSEMBLER__ */

#endif
