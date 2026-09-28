/* boot.c - the KestrelOS UEFI loader.
 *
 * Responsibilities, in order:
 *   1. find the volume this image was loaded from and read \KESTREL\BOOT.CFG
 *   2. offer a boot menu (KestrelOS, or chainload Windows if it is installed)
 *   3. load KERNEL.ELF and INITRD.KAR into page-aligned physical memory
 *   4. pick a 32-bit linear graphics mode
 *   5. locate the ACPI RSDP
 *   6. build a page table: identity + higher-half direct map + kernel at -2 GiB
 *   7. exit boot services, install the page table, jump to the kernel
 *
 * Everything after step 7 belongs to the kernel; the loader's own memory is
 * reported as KB_MEM_LOADER so the kernel can reclaim it.
 */
#include "efi.h"
#include "../include/kestrel/bootinfo.h"
#include "../include/kestrel/ramlog.h"

/* ------------------------------------------------------------------------- */
/* freestanding helpers                                                      */
/* ------------------------------------------------------------------------- */

void *memset(void *d, int c, size_t n) {
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *a = d; const unsigned char *b = s;
    while (n--) *a++ = *b++;
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}
/* clang emits __chkstk for frames over a page; UEFI hands us a fully committed
 * stack, so a probe loop is unnecessary and a plain return is correct here. */
__attribute__((used)) void __chkstk(void) { }

static size_t str8len(const CHAR8 *s) { size_t n = 0; while (s[n]) n++; return n; }

/* ------------------------------------------------------------------------- */
/* globals                                                                   */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* SBAT - what lets this be launched with Secure Boot switched on            */
/* ------------------------------------------------------------------------- */

/* Secure Boot will not run an image the firmware cannot trace to a key it
 * holds, and the only keys a consumer motherboard ships with are Microsoft's.
 * Getting a self-built loader signed by Microsoft is a submission process, so
 * the way everyone else solves this is shim: a small loader that Microsoft HAS
 * signed, which the firmware therefore launches, and which then checks the
 * next image against a list of keys the machine's owner has enrolled - without
 * changing a single firmware setting.
 *
 * Shim will not load an image that carries no SBAT section.  SBAT exists
 * because revoking a broken boot loader used to mean revoking its exact hash,
 * and there are thousands of those; SBAT instead gives every component a name
 * and a generation number, so one entry can revoke every build of a component
 * up to a given generation at once.  The generation is bumped only when a
 * security fix goes in, which is why it is 1 here and will stay 1 until
 * something needs revoking.
 *
 * The section must be called .sbat and must be plain comma-separated text.
 */
__attribute__((used, section(".sbat"), aligned(16)))
static const char sbat_metadata[] =
    "sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md\n"
    "kestrel,1,KestrelOS,kestrel,1,https://kestrelos.invalid/\n";

static EFI_SYSTEM_TABLE  *ST;
static EFI_BOOT_SERVICES *BS;
static EFI_HANDLE         IMG;
static int                serial_ready;

/* ------------------------------------------------------------------------- */
/* COM1 - diagnostics that keep working after ExitBootServices               */
/* ------------------------------------------------------------------------- */

static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }

static void serial_init(void) {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x01);   /* 115200 baud */
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);   /* 8N1 */
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);
    serial_ready = 1;
}
static void serial_putc(char c) {
    if (!serial_ready) return;
    if (c == '\n') serial_putc('\r');
    for (int i = 0; i < 100000 && !(inb(0x3F8 + 5) & 0x20); i++) { }
    outb(0x3F8, (uint8_t)c);
}
static void serial_puts(const char *s) { while (*s) serial_putc(*s++); }

/* ------------------------------------------------------------------------- */
/* POST codes - the diagnostic that works when the screen does not           */
/* ------------------------------------------------------------------------- */

/* Port 0x80 is where a PC has always written its progress during power-on
 * self-test, and most desktop motherboards of the last decade show the last
 * byte written to it on a two-digit display next to the memory slots.  It
 * needs no monitor, no serial cable, no keyboard, and it keeps working after
 * the firmware has been shut down - so on a machine that shows a black screen
 * and nothing else, the number sitting on that display is the only thing that
 * says where it stopped.
 *
 * The codes below are deliberately in the E0-EF range, which no firmware uses
 * for its own self-test, so a number in that range is unambiguously ours. */
#define POST_ENTERED     0xE0   /* the loader is executing                   */
#define POST_SCREEN      0xE1   /* it has found the framebuffer              */
#define POST_VOLUME      0xE2   /* it has opened the volume it booted from   */
#define POST_CONFIG      0xE3   /* it has read its configuration             */
#define POST_MENU        0xE4   /* it is showing the menu                    */
#define POST_KERNEL_READ 0xE5   /* the kernel image is in memory             */
#define POST_INITRD      0xE6   /* so is the initrd                          */
#define POST_GRAPHICS    0xE7   /* the video mode has been chosen            */
#define POST_PAGING      0xE8   /* the page tables are built                 */
#define POST_EXIT        0xE9   /* about to shut the firmware down           */
#define POST_JUMP        0xEA   /* about to enter the kernel                 */
#define POST_DIED        0xEF   /* the loader gave up and said why           */

static void post(uint8_t code) { outb(0x80, code); }
static void serial_hex(uint64_t v) {
    char buf[17]; const char *d = "0123456789ABCDEF";
    for (int i = 15; i >= 0; i--) { buf[i] = d[v & 0xF]; v >>= 4; }
    buf[16] = 0; serial_puts(buf);
}

/* ------------------------------------------------------------------------- */
/* console output                                                            */
/* ------------------------------------------------------------------------- */

/* Defined further down, once the graphics protocol's types are in scope.  Text
 * goes to three places at once and each covers a case the others do not: the
 * firmware's console, which is the only one that works before we have found a
 * framebuffer; the serial port, which works on a machine that has one; and the
 * framebuffer we draw into ourselves, which is the only one that works on a
 * machine whose firmware never rendered a text console at all. */
static void screen_putc(char c);
static void screen_colour(unsigned attr);
static void screen_clear(void);
static void wipe(void);
static void attr(UINTN a);
static void trail(const char *msg);

static void put16(const CHAR16 *s) {
    if (ST && ST->ConOut) ST->ConOut->OutputString(ST->ConOut, (CHAR16 *)s);
    while (*s) { screen_putc((char)(*s & 0x7F)); serial_putc((char)(*s & 0x7F)); s++; }
}

static void puta(const char *s) {
    CHAR16 buf[256];
    size_t i = 0;
    while (*s && i < 254) {
        if (*s == '\n') buf[i++] = '\r';
        buf[i++] = (CHAR16)(unsigned char)*s++;
        if (i >= 254) break;
    }
    buf[i] = 0;
    if (ST && ST->ConOut) ST->ConOut->OutputString(ST->ConOut, buf);
    for (size_t j = 0; j < i; j++) {
        if (buf[j] == '\r') continue;
        screen_putc((char)buf[j]);
        serial_putc((char)buf[j]);
    }
}

static void put_u64(uint64_t v, int base, int pad) {
    char tmp[24]; int n = 0;
    const char *dig = "0123456789ABCDEF";
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = dig[v % base]; v /= base; }
    while (n < pad) tmp[n++] = '0';
    char out[26]; int o = 0;
    while (n) out[o++] = tmp[--n];
    out[o] = 0;
    puta(out);
}

/* Two small appenders, because the loader has no snprintf and one message
 * wants numbers in the middle of it.  Both take and return the length so a
 * message can be built up in pieces without any of them being able to run off
 * the end of the buffer. */
static size_t note_str(char *b, size_t cap, size_t n, const char *s) {
    while (*s && n < cap - 1) b[n++] = *s++;
    return n;
}

static size_t note_num(char *b, size_t cap, size_t n, uint64_t v) {
    char tmp[24]; int t = 0;
    if (!v) tmp[t++] = '0';
    while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    while (t && n < cap - 1) b[n++] = tmp[--t];
    return n;
}

/* Minimal formatter: %s CHAR16*, %a char*, %d, %u, %x, %X (16-wide), %% */
static void bprint(const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    char chunk[2] = {0, 0};
    while (*fmt) {
        if (*fmt != '%') { chunk[0] = *fmt++; puta(chunk); continue; }
        fmt++;
        switch (*fmt++) {
        case 's': put16(__builtin_va_arg(ap, CHAR16 *)); break;
        case 'a': puta(__builtin_va_arg(ap, char *)); break;
        case 'd': { int64_t v = __builtin_va_arg(ap, int64_t);
                    if (v < 0) { puta("-"); v = -v; }
                    put_u64((uint64_t)v, 10, 0); break; }
        case 'u': put_u64(__builtin_va_arg(ap, uint64_t), 10, 0); break;
        case 'x': put_u64(__builtin_va_arg(ap, uint64_t), 16, 0); break;
        case 'X': put_u64(__builtin_va_arg(ap, uint64_t), 16, 16); break;
        case '%': puta("%"); break;
        default: break;
        }
    }
    __builtin_va_end(ap);
}

__attribute__((noreturn)) static void die(const char *msg, EFI_STATUS st) {
    post(POST_DIED);
    trail(msg);
    attr(EFI_LIGHTRED);
    bprint("\nboot: %a (status %X)\n", msg, (uint64_t)st);
    attr(EFI_LIGHTGRAY);
    puta("Press any key to return to firmware.\n");
    if (ST && ST->ConIn) {
        EFI_INPUT_KEY k; UINTN idx;
        ST->ConIn->Reset(ST->ConIn, 0);
        BS->WaitForEvent(1, &ST->ConIn->WaitForKey, &idx);
        ST->ConIn->ReadKeyStroke(ST->ConIn, &k);
    }
    BS->Exit(IMG, st ? st : EFI_LOAD_ERROR, 0, 0);
    for (;;) __asm__ volatile("hlt");
}

/* ------------------------------------------------------------------------- */
/* pool helpers                                                              */
/* ------------------------------------------------------------------------- */

static void *pool(UINTN n) {
    void *p = 0;
    if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, n, &p)) || !p) die("out of pool memory", EFI_OUT_OF_RESOURCES);
    memset(p, 0, n);
    return p;
}

static uint64_t alloc_pages_at_any(UINTN pages, EFI_MEMORY_TYPE t) {
    EFI_PHYSICAL_ADDRESS a = 0;
    if (EFI_ERROR(BS->AllocatePages(AllocateAnyPages, t, pages, &a))) return 0;
    return a;
}

/* ------------------------------------------------------------------------- */
/* file access                                                               */
/* ------------------------------------------------------------------------- */

static EFI_GUID gLoadedImage = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID gSimpleFs    = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID gFileInfo    = EFI_FILE_INFO_GUID;
static EFI_GUID gGop         = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GUID gDevPath     = EFI_DEVICE_PATH_PROTOCOL_GUID;

/* ------------------------------------------------------------------------- */
/* a screen of our own                                                       */
/* ------------------------------------------------------------------------- */

/* Why a boot loader should draw its own text.
 *
 * The firmware offers a text console and everything above printed to it, and
 * on most machines that is enough.  It is not enough on all of them: a
 * firmware set to boot fast, or built with no legacy video support, brings the
 * graphics output up for the operating system and never renders a text console
 * at all.  OutputString then succeeds, returns success, and puts nothing
 * anywhere.  A loader that trusts it has no way to tell that apart from
 * working, and the machine shows the vendor logo and then a black screen for
 * the rest of the boot - which is exactly the failure this was written for.
 *
 * So the framebuffer is found first, before anything else happens, and text is
 * drawn into it directly with the same glyphs the kernel uses.  Two things
 * follow that are worth having on their own:
 *
 *   The screen changes colour the moment the loader starts.  That single fact
 *   separates "the loader never ran" from "the loader ran and something after
 *   it went wrong", which is otherwise the hardest thing to find out on a
 *   machine with no serial cable.
 *
 *   The mode is left exactly as the firmware set it.  Whatever put the vendor
 *   logo on the screen is, by definition, a mode this monitor is displaying;
 *   changing it before we have said anything risks losing the picture in
 *   order to get a bigger one.  The larger mode is chosen later, once the
 *   loader has already proved it can be seen.
 */
#include "../kernel/font8x16.h"

static struct {
    volatile uint8_t *base;
    uint32_t width, height, pitch;      /* pitch in bytes                    */
    uint8_t  rshift, gshift, bshift;
    int      ok;
    uint32_t cx, cy;                    /* the text cursor, in pixels        */
    uint32_t fg, bg;
} scr;

static void screen_pixel(uint32_t x, uint32_t y, uint32_t rgb) {
    if (!scr.ok || x >= scr.width || y >= scr.height) return;
    uint32_t v = ((rgb >> 16 & 0xFF) << scr.rshift) |
                 ((rgb >> 8  & 0xFF) << scr.gshift) |
                 ((rgb       & 0xFF) << scr.bshift);
    *(volatile uint32_t *)(scr.base + (uint64_t)y * scr.pitch + x * 4) = v;
}

static void screen_fill(uint32_t rgb) {
    if (!scr.ok) return;
    for (uint32_t y = 0; y < scr.height; y++)
        for (uint32_t x = 0; x < scr.width; x++) screen_pixel(x, y, rgb);
}

static void screen_colour(unsigned attr) {
    /* The firmware's attribute values, so one call sets both consoles and the
     * two say the same thing. */
    switch (attr & 0x0F) {
    case EFI_WHITE:     scr.fg = 0xFFFFFF; break;
    case EFI_YELLOW:    scr.fg = 0xFFD75F; break;
    case EFI_LIGHTRED:  scr.fg = 0xFF6B5B; break;
    case EFI_DARKGRAY:  scr.fg = 0x8A8A8A; break;
    default:            scr.fg = 0xC8C8C8; break;
    }
}

static void screen_scroll(void) {
    if (!scr.ok) return;
    uint32_t line = FONT8X16_H;
    uint64_t row = scr.pitch;
    for (uint32_t y = 0; y + line < scr.height; y++)
        memcpy((void *)(scr.base + (uint64_t)y * row),
               (void *)(scr.base + (uint64_t)(y + line) * row), scr.width * 4);
    for (uint32_t y = scr.height > line ? scr.height - line : 0; y < scr.height; y++)
        for (uint32_t x = 0; x < scr.width; x++) screen_pixel(x, y, scr.bg);
}

static void screen_putc(char c) {
    if (!scr.ok) return;

    if (c == '\n') {
        scr.cx = 8;
        scr.cy += FONT8X16_H;
        if (scr.cy + FONT8X16_H > scr.height) { screen_scroll(); scr.cy -= FONT8X16_H; }
        return;
    }
    if (c == '\r') return;
    if (c == '\t') { scr.cx += FONT8X16_W * 4; return; }
    if ((unsigned char)c < 0x20) return;

    if (scr.cx + FONT8X16_W > scr.width) screen_putc('\n');

    const uint8_t *g = font8x16[(unsigned char)c];
    for (uint32_t r = 0; r < FONT8X16_H; r++)
        for (uint32_t b = 0; b < FONT8X16_W; b++)
            screen_pixel(scr.cx + b, scr.cy + r,
                         (g[r] & (0x80u >> b)) ? scr.fg : scr.bg);
    scr.cx += FONT8X16_W;
}

static void screen_clear(void) {
    if (!scr.ok) return;
    screen_fill(scr.bg);
    scr.cx = 8;
    scr.cy = 8;
}

/* Which graphics output, when the machine has more than one.
 *
 * LocateProtocol answers "give me any one of these", and on a machine with a
 * single display that is the same thing as "give me the one being used".  On a
 * machine with three monitors plugged into one card it is not: the firmware
 * publishes an output for each, LocateProtocol returns whichever it finds
 * first, and there is nothing about that one which makes it the one somebody
 * is looking at.  Pick wrong and everything works perfectly, on a screen
 * nobody can see.
 *
 * So every one of them is looked at and scored, and the reasoning is worth
 * stating because none of it is a guess:
 *
 *   An output with no framebuffer address is not displaying anything.  It is
 *   published because the connector exists, not because a monitor is on it.
 *
 *   An output the firmware has already set a mode on is one the firmware
 *   chose, and the firmware chose it to put its own logo somewhere visible.
 *   That is the single best piece of evidence available, and it costs nothing
 *   to read.
 *
 *   Between two that both look used, the larger is preferred, because a
 *   secondary output left in a minimal mode is common and a primary one is
 *   not.
 *
 * How many were found is recorded either way.  On a machine that shows nothing
 * that number is the first thing worth knowing, and it cannot be recovered
 * afterwards.
 */
static int gop_count;

static EFI_GRAPHICS_OUTPUT_PROTOCOL *best_gop(void) {
    EFI_HANDLE *handles = 0;
    UINTN count = 0;

    if (EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, &gGop, 0, &count, &handles))
        || !handles || !count) {
        /* No handle list: fall back to asking for any one at all, which is
         * what a machine with a single output would have given anyway. */
        EFI_GRAPHICS_OUTPUT_PROTOCOL *one = 0;
        if (EFI_ERROR(BS->LocateProtocol(&gGop, 0, (void **)&one))) return 0;
        gop_count = one ? 1 : 0;
        return one;
    }

    gop_count = (int)count;

    EFI_GRAPHICS_OUTPUT_PROTOCOL *best = 0;
    UINT64 best_score = 0;

    for (UINTN i = 0; i < count; i++) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *g = 0;
        if (EFI_ERROR(BS->HandleProtocol(handles[i], &gGop, (void **)&g)) || !g)
            continue;
        if (!g->Mode || !g->Mode->Info) continue;
        if (!g->Mode->FrameBufferBase) continue;      /* nothing on this one  */

        UINT64 w = g->Mode->Info->HorizontalResolution;
        UINT64 h = g->Mode->Info->VerticalResolution;
        if (!w || !h) continue;

        /* Already in a mode, with somewhere to draw: the firmware is using
         * this one.  Everything else is a tie-break on size. */
        UINT64 score = (1ULL << 40) + w * h;
        if (!best || score > best_score) { best = g; best_score = score; }
    }

    BS->FreePool(handles);
    return best;
}

/* Find the framebuffer and take the mode the firmware is already in.  Called
 * before anything else, so that everything the loader says is visible. */
static void screen_open(void) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = best_gop();
    if (!gop) return;
    if (!gop->Mode || !gop->Mode->Info || !gop->Mode->FrameBufferBase) return;

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
    scr.base   = (volatile uint8_t *)(uintptr_t)gop->Mode->FrameBufferBase;
    scr.width  = info->HorizontalResolution;
    scr.height = info->VerticalResolution;
    scr.pitch  = info->PixelsPerScanLine * 4;

    if (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
        scr.rshift = 16; scr.gshift = 8; scr.bshift = 0;
    } else if (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
        scr.rshift = 0; scr.gshift = 8; scr.bshift = 16;
    } else {
        /* A masked format needs the masks worked out, and getting that wrong
         * writes the wrong colours rather than nothing - which is still far
         * better than an unreadable screen, so it is taken as a best effort
         * with the common arrangement assumed. */
        scr.rshift = 16; scr.gshift = 8; scr.bshift = 0;
    }

    if (!scr.width || !scr.height || scr.pitch < scr.width * 4) return;

    scr.ok = 1;
    /* Deep blue, which is not a colour any firmware leaves behind.  If the
     * screen turns this colour the loader is running and this address really
     * is the picture; if it stays black, neither of those is true, and that is
     * worth knowing before anything else is investigated. */
    scr.bg = 0x061024;
    screen_colour(EFI_LIGHTGRAY);
    screen_clear();
}

/* Both consoles at once, so no call site has to remember there are two. */
static void wipe(void) {
    /* Both consoles cleared together.  The firmware call is spelled out here
     * rather than reached through any helper: this IS the helper, and a
     * helper that calls itself is a reset rather than a clear. */
    if (ST && ST->ConOut) ST->ConOut->ClearScreen(ST->ConOut);
    screen_clear();
}

static void attr(UINTN a) {
    if (ST && ST->ConOut) ST->ConOut->SetAttribute(ST->ConOut, a);
    screen_colour((unsigned)a);
}

static EFI_GUID gAcpi20      = ACPI_20_TABLE_GUID;
static EFI_GUID gAcpi10      = ACPI_10_TABLE_GUID;
/* The monitor's own description of itself.  "Active" is what the firmware is
 * driving; "discovered" is the raw block it read back from the panel. */
static EFI_GUID gEdidActive     = { 0xbd8c1056, 0x9f36, 0x44ec,
                                    { 0x92, 0xa8, 0xa6, 0x33, 0x7f, 0x81, 0x79, 0x86 } };
static EFI_GUID gEdidDiscovered = { 0x1c0c34f6, 0xd380, 0x41fa,
                                    { 0xa0, 0x49, 0x8a, 0xd0, 0x6c, 0x1a, 0x66, 0xaa } };

static EFI_FILE_PROTOCOL *g_root;

/* ---------------------------------------------------------- a trail on disk
 *
 * The third place a milestone is recorded, and the one that keeps working when
 * the machine is not the machine this was written on.  Each line is opened,
 * appended and closed, so whatever the loader reached is on the stick even if
 * the next step took the machine down with it - and the stick can then be read
 * on any computer that has one.
 *
 * On a machine with a black screen and no debug display, this is what turns
 * "it did not work" into a line number.  It costs a few milliseconds per step
 * and is written to the volume the loader booted from, which by definition it
 * can already read.
 */
static int trail_ready;

static void trail(const char *msg) {
    if (!trail_ready || !g_root) return;

    EFI_FILE_PROTOCOL *f = 0;
    if (EFI_ERROR(g_root->Open(g_root, &f, (CHAR16 *)u"\\KESTREL\\BOOT.LOG",
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                               EFI_FILE_MODE_CREATE, 0)) || !f) return;

    /* Straight to the end, so a run appends to the runs before it and the file
     * is a history rather than only the last attempt. */
    UINT64 end = 0;
    if (!EFI_ERROR(f->SetPosition(f, 0xFFFFFFFFFFFFFFFFULL)))
        f->GetPosition(f, &end);

    char line[160];
    UINTN n = 0;
    while (msg[n] && n < sizeof line - 2) { line[n] = msg[n]; n++; }
    line[n++] = '\r';
    line[n++] = '\n';

    UINTN wrote = n;
    f->Write(f, &wrote, line);
    f->Flush(f);
    f->Close(f);
}

/* ------------------------------------------------------------------------- */
/* the kernel's log, carried over from the previous boot                      */
/* ------------------------------------------------------------------------- */

/* The kernel cannot write files.  It keeps its log in a region of memory
 * instead, and this is the other half of that arrangement: on the next boot,
 * while the firmware's own storage stack is still running and files can still
 * be written, the loader finds that region and writes what is in it to the
 * boot device.  <kestrel/ramlog.h> explains why the log goes this way round.
 *
 * The region is found by searching rather than by trusting a fixed address.
 * The loader asks for the same address every boot, so the search almost always
 * succeeds on its first probe - but "almost always" is not "always": the
 * address can be refused, and then the region is wherever the firmware put it.
 * A search costs a few thousand reads of memory the firmware has already told
 * us is free, and removes the assumption entirely.
 */
static ramlog_header *find_previous_ramlog(void) {
    UINTN size = 0, key = 0, dsz = 0;
    UINT32 dver = 0;
    BS->GetMemoryMap(&size, 0, &key, &dsz, &dver);
    if (!size || !dsz) return 0;

    size += dsz * 16;                      /* room for the map to grow       */
    EFI_MEMORY_DESCRIPTOR *map = pool(size);
    if (EFI_ERROR(BS->GetMemoryMap(&size, map, &key, &dsz, &dver))) return 0;

    for (UINTN off = 0; off < size; off += dsz) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)map + off);

        /* Only memory the firmware says is free.  Anything else either belongs
         * to the firmware or does not survive being read this early. */
        if (d->Type != EfiConventionalMemory) continue;

        uint64_t start = (d->PhysicalStart + RAMLOG_ALIGN - 1) & ~(RAMLOG_ALIGN - 1);
        uint64_t end   = d->PhysicalStart + d->NumberOfPages * 4096;
        if (end > 0x100000000ULL) end = 0x100000000ULL;   /* below 4 GiB only */

        for (uint64_t a = start; a + sizeof(ramlog_header) <= end; a += RAMLOG_ALIGN) {
            ramlog_header *h = (ramlog_header *)(uintptr_t)a;
            if (h->magic != RAMLOG_MAGIC) continue;        /* the common case */
            if (!ramlog_valid(h)) continue;
            if ((uint64_t)a + sizeof *h + h->bytes > end) continue;
            return h;
        }
    }
    return 0;
}

/* Write one recovered log out, oldest text first.  The ring is a plain
 * character buffer with no framing, so unwrapping it is two writes. */
static uint32_t write_previous_log(ramlog_header *h) {
    if (!h->used || !g_root) return 0;

    EFI_FILE_PROTOCOL *f = 0;
    if (EFI_ERROR(g_root->Open(g_root, &f, (CHAR16 *)u"\\KESTREL\\KERNEL.LOG",
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                               EFI_FILE_MODE_CREATE, 0)) || !f) return 0;

    /* Append, so the file is a history of boots rather than only the last one:
     * comparing a failing boot against the one before it is most of what a log
     * like this is for. */
    UINT64 end = 0;
    if (!EFI_ERROR(f->SetPosition(f, 0xFFFFFFFFFFFFFFFFULL)))
        f->GetPosition(f, &end);

    char banner[96];
    UINTN bn = 0;
    const char *b = "\r\n==== kernel log recovered from the previous boot ====\r\n";
    while (b[bn] && bn < sizeof banner) { banner[bn] = b[bn]; bn++; }
    UINTN w = bn;
    f->Write(f, &w, banner);

    char *text = (char *)(h + 1);
    if (h->wrapped) {
        /* Oldest text runs from the write position to the end of the ring. */
        w = h->bytes - h->head;
        if (w) f->Write(f, &w, text + h->head);
    }
    w = h->head;
    if (w) f->Write(f, &w, text);

    f->Flush(f);
    f->Close(f);
    return h->used;
}

/* Set the region up for this boot, and hand back what the previous boot left.
 * Returns the physical address, or zero if no region could be reserved - in
 * which case the kernel simply logs to screen and serial as before. */
static uint64_t prepare_ramlog(uint32_t *out_size, uint32_t *out_recovered,
                               uint32_t *out_boot) {
    *out_size = 0; *out_recovered = 0; *out_boot = 1;

    ramlog_header *prev = find_previous_ramlog();
    if (prev) {
        *out_recovered = write_previous_log(prev);
        *out_boot = prev->boot + 1;
        /* Retire it before the allocation below, which may well hand back this
         * very region: a stale magic left in place would be found again next
         * boot and written out a second time. */
        prev->magic = 0;
    }

    UINTN pages = (sizeof(ramlog_header) + RAMLOG_BYTES + 4095) / 4096;

    /* Ask for the usual address first.  Getting it means the next boot's
     * search finds the region on its first probe. */
    EFI_PHYSICAL_ADDRESS a = RAMLOG_ADDR;
    if (EFI_ERROR(BS->AllocatePages(AllocateAddress, EfiReservedMemoryType, pages, &a))) {
        /* The fallback, which used to be a coin toss it always lost.
         *
         * It asked for any pages and then GAVE UP if what came back was not on
         * a 64 KiB boundary.  Firmware hands out page-aligned memory - four
         * kilobytes - so the chance of it also landing on the boundary the
         * next boot's search steps over is about one in sixteen.  Fifteen
         * times out of sixteen this returned zero, the region was never
         * reserved, and the whole mechanism was silently off for the rest of
         * that boot.  It also leaked the pages it had just been given.
         *
         * On the machine where the preferred address is free none of this ever
         * runs, which is why it survived: it only fails where the low memory
         * is already spoken for, and that is the real hardware rather than the
         * virtual machine.
         *
         * Asking for enough extra to align INSIDE the allocation always works.
         * The slack either side stays reserved, which costs at most 64 KiB and
         * is memory the kernel would not have used anyway. */
        UINTN slack = (UINTN)(RAMLOG_ALIGN / 4096);
        EFI_PHYSICAL_ADDRESS raw = 0;

        if (EFI_ERROR(BS->AllocatePages(AllocateAnyPages, EfiReservedMemoryType,
                                        pages + slack, &raw)))
            return 0;

        a = (raw + RAMLOG_ALIGN - 1) & ~(EFI_PHYSICAL_ADDRESS)(RAMLOG_ALIGN - 1);
    }

    ramlog_header *h = (ramlog_header *)(uintptr_t)a;
    memset(h, 0, sizeof *h + RAMLOG_BYTES);
    h->bytes = RAMLOG_BYTES;
    h->used = 0;
    h->head = 0;
    h->wrapped = 0;
    h->boot = *out_boot;
    h->check = ramlog_check(h);
    h->magic = RAMLOG_MAGIC;      /* last: an incomplete header is not valid */

    *out_size = (uint32_t)(sizeof *h + RAMLOG_BYTES);
    return (uint64_t)a;
}

/* One call for all three: the motherboard's display, the trail on the stick,
 * and the screen the user is looking at. */
static void milestone(uint8_t code, const char *what) {
    post(code);
    trail(what);
    serial_puts("[boot] ");
    serial_puts(what);
    serial_puts("\n");
}

static int file_read_all(const CHAR16 *path, void **out, UINTN *out_size, int page_aligned, uint64_t *phys_out) {
    EFI_FILE_PROTOCOL *f = 0;
    if (!g_root) return 0;
    if (EFI_ERROR(g_root->Open(g_root, &f, (CHAR16 *)path, EFI_FILE_MODE_READ, 0)) || !f) return 0;

    UINTN isz = sizeof(EFI_FILE_INFO) + 512;
    EFI_FILE_INFO *fi = pool(isz);
    if (EFI_ERROR(f->GetInfo(f, &gFileInfo, &isz, fi))) { f->Close(f); return 0; }
    UINT64 size = fi->FileSize;
    BS->FreePool(fi);

    void *buf;
    if (page_aligned) {
        UINTN pages = (UINTN)((size + 0xFFF) / 0x1000);
        if (pages == 0) pages = 1;
        uint64_t p = alloc_pages_at_any(pages, EfiLoaderData);
        if (!p) { f->Close(f); return 0; }
        buf = (void *)(uintptr_t)p;
        memset(buf, 0, pages * 0x1000);
        if (phys_out) *phys_out = p;
    } else {
        buf = pool((UINTN)size + 1);
    }

    UINTN remaining = (UINTN)size, off = 0;
    while (remaining) {
        UINTN chunk = remaining;
        if (EFI_ERROR(f->Read(f, &chunk, (uint8_t *)buf + off)) || chunk == 0) break;
        off += chunk; remaining -= chunk;
    }
    f->Close(f);
    if (off != size) return 0;
    *out = buf;
    *out_size = (UINTN)size;
    return 1;
}

static int file_exists(const CHAR16 *path) {
    EFI_FILE_PROTOCOL *f = 0;
    if (!g_root) return 0;
    if (EFI_ERROR(g_root->Open(g_root, &f, (CHAR16 *)path, EFI_FILE_MODE_READ, 0)) || !f) return 0;
    f->Close(f);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* BOOT.CFG                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct {
    char kernel[128];
    char initrd[128];
    char cmdline[KB_CMDLINE_MAX];
    char data[KB_BOOTDEV_MAX];
    char video[16];         /* "1920x1080", empty to choose automatically */
    char displaymode[16];   /* extend | mirror | onlyother (#7)            */
    char default_entry[16]; /* main | gputest                              */
    int  timeout;
    int  verbose;
} bootcfg;

static void cfg_defaults(bootcfg *c) {
    memset(c, 0, sizeof *c);
    memcpy(c->kernel, "\\KESTREL\\KERNEL.ELF", 20);
    memcpy(c->initrd, "\\KESTREL\\INITRD.KAR", 20);
    c->timeout = 3;
}

static void cfg_set(bootcfg *c, const char *k, const char *v) {
    size_t vn = str8len((const CHAR8 *)v);
    if (!memcmp(k, "kernel", 7))      { if (vn < sizeof c->kernel)  memcpy(c->kernel, v, vn + 1); }
    else if (!memcmp(k, "initrd", 7)) { if (vn < sizeof c->initrd)  memcpy(c->initrd, v, vn + 1); }
    else if (!memcmp(k, "cmdline", 8)){ if (vn < sizeof c->cmdline) memcpy(c->cmdline, v, vn + 1); }
    else if (!memcmp(k, "data", 5))   { if (vn < sizeof c->data)    memcpy(c->data, v, vn + 1); }
    else if (!memcmp(k, "video", 6))  { if (vn < sizeof c->video)   memcpy(c->video, v, vn + 1); }
    else if (!memcmp(k, "displaymode", 12)) { if (vn < sizeof c->displaymode) memcpy(c->displaymode, v, vn + 1); }
    else if (!memcmp(k, "default", 8)){ if (vn < sizeof c->default_entry) memcpy(c->default_entry, v, vn + 1); }
    else if (!memcmp(k, "timeout", 8)){ int t = 0; for (const char *p = v; *p >= '0' && *p <= '9'; p++) t = t * 10 + (*p - '0'); c->timeout = t; }
    else if (!memcmp(k, "verbose", 8)){ c->verbose = (v[0] == '1' || v[0] == 'y' || v[0] == 't'); }
}

static void cfg_parse(bootcfg *c, char *text, UINTN len) {
    UINTN i = 0;
    char key[32], val[KB_CMDLINE_MAX];
    while (i < len) {
        while (i < len && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) i++;
        if (i >= len) break;
        if (text[i] == '#') { while (i < len && text[i] != '\n') i++; continue; }
        int kn = 0;
        while (i < len && text[i] != '=' && text[i] != '\n' && kn < 31) key[kn++] = text[i++];
        key[kn] = 0;
        if (i >= len || text[i] != '=') { while (i < len && text[i] != '\n') i++; continue; }
        i++;
        int vn = 0;
        while (i < len && text[i] != '\n' && text[i] != '\r' && vn < (int)sizeof(val) - 1) val[vn++] = text[i++];
        while (vn > 0 && (val[vn - 1] == ' ' || val[vn - 1] == '\t')) vn--;
        val[vn] = 0;
        cfg_set(c, key, val);
    }
}

static void a2w(const char *a, CHAR16 *w, size_t cap) {
    size_t i = 0;
    while (a[i] && i < cap - 1) { w[i] = (CHAR16)(unsigned char)a[i]; i++; }
    w[i] = 0;
}

/* ------------------------------------------------------------------------- */
/* ELF64                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} elf64_ehdr;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} elf64_phdr;

#define PT_LOAD 1

/* ------------------------------------------------------------------------- */
/* page tables                                                               */
/* ------------------------------------------------------------------------- */

#define PTE_P    (1ULL << 0)
#define PTE_W    (1ULL << 1)
#define PTE_PS   (1ULL << 7)
#define PTE_NX   (1ULL << 63)

static uint64_t *pt_pool;       /* bump allocator over a contiguous run */
static UINTN     pt_pool_pages;
static UINTN     pt_pool_used;

static uint64_t *pt_alloc(void) {
    if (pt_pool_used >= pt_pool_pages) die("page table pool exhausted", EFI_OUT_OF_RESOURCES);
    uint64_t *p = (uint64_t *)((uint8_t *)pt_pool + pt_pool_used * 0x1000);
    pt_pool_used++;
    memset(p, 0, 0x1000);
    return p;
}

static uint64_t *pt_next(uint64_t *tbl, int idx) {
    if (!(tbl[idx] & PTE_P)) {
        uint64_t *n = pt_alloc();
        tbl[idx] = (uint64_t)(uintptr_t)n | PTE_P | PTE_W;
    }
    return (uint64_t *)(uintptr_t)(tbl[idx] & 0x000FFFFFFFFFF000ULL);
}

static void map_4k(uint64_t *pml4, uint64_t va, uint64_t pa, uint64_t flags) {
    uint64_t *pdpt = pt_next(pml4, (int)((va >> 39) & 0x1FF));
    uint64_t *pd   = pt_next(pdpt, (int)((va >> 30) & 0x1FF));
    uint64_t *pt   = pt_next(pd,   (int)((va >> 21) & 0x1FF));
    pt[(va >> 12) & 0x1FF] = (pa & 0x000FFFFFFFFFF000ULL) | flags | PTE_P;
}

static void map_2m(uint64_t *pml4, uint64_t va, uint64_t pa, uint64_t flags) {
    uint64_t *pdpt = pt_next(pml4, (int)((va >> 39) & 0x1FF));
    uint64_t *pd   = pt_next(pdpt, (int)((va >> 30) & 0x1FF));
    pd[(va >> 21) & 0x1FF] = (pa & 0x000FFFFFFFE00000ULL) | flags | PTE_PS | PTE_P;
}

static void map_1g(uint64_t *pml4, uint64_t va, uint64_t pa, uint64_t flags) {
    uint64_t *pdpt = pt_next(pml4, (int)((va >> 39) & 0x1FF));
    pdpt[(va >> 30) & 0x1FF] = (pa & 0x000FFFFFC0000000ULL) | flags | PTE_PS | PTE_P;
}

static int have_1g_pages(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000), "c"(0));
    if (a < 0x80000001) return 0;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001), "c"(0));
    return (d >> 26) & 1;
}

/* Bit 63 of a page table entry is the no-execute bit only once EFER.NXE is
 * set; until then the processor treats it as reserved and every access through
 * such an entry raises a page fault.  Firmware does not reliably leave NXE
 * enabled, so turn it on here - and if the processor has no NX support at all,
 * leave the bit out of the entries entirely. */
static uint64_t nx_bit;

static void enable_nx(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000), "c"(0));
    if (a < 0x80000001) { nx_bit = 0; return; }
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001), "c"(0));
    if (!((d >> 20) & 1)) { nx_bit = 0; return; }

    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    lo |= (1u << 11);                       /* EFER.NXE */
    __asm__ volatile("wrmsr" :: "c"(0xC0000080), "a"(lo), "d"(hi));
    nx_bit = PTE_NX;
}

/* Map [0,len) both identity and at HHDM, using the largest page size we can. */
static void map_range_both(uint64_t *pml4, uint64_t len, int gig) {
    uint64_t step = gig ? (1ULL << 30) : (1ULL << 21);
    for (uint64_t p = 0; p < len; p += step) {
        if (gig) { map_1g(pml4, p, p, PTE_W); map_1g(pml4, KB_HHDM_BASE + p, p, PTE_W | nx_bit); }
        else     { map_2m(pml4, p, p, PTE_W); map_2m(pml4, KB_HHDM_BASE + p, p, PTE_W | nx_bit); }
    }
}

/* ------------------------------------------------------------------------- */
/* graphics                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct { UINT32 SizeOfEdid; UINT8 *Edid; } EFI_EDID_PROTOCOL;

/* EDID 1.x.  The 128-byte base block holds the manufacturer id, the panel's
 * physical size, and four 18-byte descriptors.  The first descriptor is the
 * preferred timing: the resolution and pixel clock the panel was designed
 * around, which is both its native resolution and its best refresh rate at that
 * resolution.  Reading it is how the loader picks the right mode rather than
 * guessing from pixel counts - and on a flat panel the native mode is the only
 * one that is not being scaled by the monitor. */
static void parse_edid(const UINT8 *e, UINTN len, kboot_display *out) {
    static const UINT8 header[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    if (!e || len < 128) return;
    for (int i = 0; i < 8; i++) if (e[i] != header[i]) return;

    UINT8 sum = 0;
    for (int i = 0; i < 128; i++) sum = (UINT8)(sum + e[i]);
    if (sum != 0) return;                      /* a corrupt block says nothing */

    out->present = 1;

    /* Keep the raw EDID for the kernel's fuller parse (kernel/edid.c) - up to
     * one extension block, where the high-refresh modes live. */
    UINTN keep = len < sizeof out->edid ? len : sizeof out->edid;
    out->edid_len = (UINT16)keep;
    for (UINTN i = 0; i < keep; i++) out->edid[i] = e[i];

    /* Three five-bit letters packed into two big-endian bytes. */
    UINT16 id = (UINT16)((e[8] << 8) | e[9]);
    out->manufacturer[0] = (char)('A' + ((id >> 10) & 0x1F) - 1);
    out->manufacturer[1] = (char)('A' + ((id >> 5) & 0x1F) - 1);
    out->manufacturer[2] = (char)('A' + (id & 0x1F) - 1);
    out->manufacturer[3] = 0;

    out->phys_width_mm  = (UINT16)(e[21] * 10);
    out->phys_height_mm = (UINT16)(e[22] * 10);

    for (int d = 0; d < 4; d++) {
        const UINT8 *t = e + 54 + d * 18;

        /* A zero pixel clock marks a text descriptor rather than a timing. */
        UINT32 clock_10khz = (UINT32)(t[0] | (t[1] << 8));
        if (clock_10khz == 0) {
            if (t[3] == 0xFC) {                        /* the monitor's name */
                int n = 0;
                for (int i = 5; i < 18 && n < 15; i++) {
                    if (t[i] == 0x0A) break;
                    out->model[n++] = (char)t[i];
                }
                while (n > 0 && out->model[n - 1] == ' ') n--;
                out->model[n] = 0;
            }
            continue;
        }

        UINT32 h_active = (UINT32)(t[2] | ((t[4] & 0xF0) << 4));
        UINT32 h_blank  = (UINT32)(t[3] | ((t[4] & 0x0F) << 8));
        UINT32 v_active = (UINT32)(t[5] | ((t[7] & 0xF0) << 4));
        UINT32 v_blank  = (UINT32)(t[6] | ((t[7] & 0x0F) << 8));

        UINT32 h_total = h_active + h_blank;
        UINT32 v_total = v_active + v_blank;
        if (!h_total || !v_total || !h_active || !v_active) continue;

        /* Refresh is the pixel clock over the whole frame, blanking included -
         * which is why it comes out as 59.94 rather than 60. */
        UINT64 milli = ((UINT64)clock_10khz * 10000ULL * 1000ULL) /
                       ((UINT64)h_total * v_total);
        if (milli > 1000000ULL) continue;              /* nonsense; skip it */

        if (!out->native_width) {                 /* the first is preferred */
            out->native_width  = (UINT16)h_active;
            out->native_height = (UINT16)v_active;
            out->refresh_mhz   = (UINT32)milli;   /* u32: 240 Hz is 240000 mHz */
        }
        if ((UINT32)milli > out->max_refresh_mhz) out->max_refresh_mhz = (UINT32)milli;
    }
}

static void read_edid(kboot_display *out) {
    /* The EDID protocols hang off whichever handle owns the display, so the
     * handle database has to be searched rather than LocateProtocol used. */
    EFI_GUID *which[2] = { &gEdidActive, &gEdidDiscovered };

    for (int pass = 0; pass < 2 && !out->present; pass++) {
        UINTN count = 0;
        EFI_HANDLE *handles = 0;
        if (EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, which[pass], 0,
                                             &count, &handles)) || !handles)
            continue;

        for (UINTN i = 0; i < count && !out->present; i++) {
            EFI_EDID_PROTOCOL *edid = 0;
            if (EFI_ERROR(BS->HandleProtocol(handles[i], which[pass],
                                             (void **)&edid)) || !edid)
                continue;
            parse_edid(edid->Edid, edid->SizeOfEdid, out);
        }
        BS->FreePool(handles);
    }
}

/* The largest mode worth taking.  At 8K the framebuffer alone is 132 MiB, and a
 * software renderer that has to redraw it has already lost; the cap is there so
 * the loader never picks a mode the rest of the system cannot drive. */
#define MAX_MODE_W 7680
#define MAX_MODE_H 4320

static int mode_format(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info) {
    if (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) return 0;
    if (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) return 1;
    return 2;
}

static void pick_gop_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, kboot_info *bi,
                          const char *want) {
    kboot_framebuffer *fb = &bi->fb;

    read_edid(&bi->display);

    /* An explicit choice from BOOT.CFG wins over everything else, and "keep"
     * is a choice too - the strongest one available on a machine nobody has
     * booted this on.
     *
     * Whatever mode the firmware is in right now is, by definition, a mode
     * this monitor is displaying: the vendor logo proved it.  Any other mode
     * is a mode that has never been tried on this hardware, and changing to
     * one is the single easiest way to turn a working screen into a black one
     * - especially with several monitors on the card, where the mode set on
     * one output is not necessarily the one being looked at.
     *
     * So "video=keep" means take what is there and change nothing.  It is
     * what the boot stick ships with, and the reason a first boot has one
     * fewer way to fail than every boot after it. */
    int keep = (want && want[0] == 'k' && want[1] == 'e' && want[2] == 'e'
                 && want[3] == 'p');

    UINT32 want_w = 0, want_h = 0;
    if (want && want[0] && !keep) {
        const char *p = want;
        while (*p >= '0' && *p <= '9') want_w = want_w * 10 + (UINT32)(*p++ - '0');
        if (*p == 'x' || *p == 'X') p++;
        while (*p >= '0' && *p <= '9') want_h = want_h * 10 + (UINT32)(*p++ - '0');
    }

    UINT32 best = gop->Mode->Mode;
    UINT64 best_score = 0;
    int found = 0;

    bi->mode_count = 0;

    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = 0;
        UINTN sz = 0;
        if (EFI_ERROR(gop->QueryMode(gop, m, &sz, &info)) || !info) continue;
        if (info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor &&
            info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor &&
            info->PixelFormat != PixelBitMask) continue;

        UINT64 w = info->HorizontalResolution, h = info->VerticalResolution;
        if (w < 640 || h < 480 || w > MAX_MODE_W || h > MAX_MODE_H) continue;

        int native = (bi->display.present &&
                      w == bi->display.native_width &&
                      h == bi->display.native_height);

        if (bi->mode_count < KB_MAX_VIDEO_MODES) {
            kboot_video_mode *vm = &bi->modes[bi->mode_count++];
            vm->width  = (uint16_t)w;
            vm->height = (uint16_t)h;
            vm->pitch_pixels = (uint16_t)info->PixelsPerScanLine;
            vm->format = (uint8_t)mode_format(info);
            vm->flags  = (uint8_t)(native ? KB_MODE_NATIVE : 0);
        }

        /* What to boot into, in order of preference: exactly what was asked
         * for; then the panel's own native timing, which is its best refresh
         * rate and the only mode a flat panel shows without scaling; then the
         * largest mode a software renderer can still redraw at a sane rate. */
        UINT64 score;
        if (want_w && w == want_w && h == want_h) score = 1ULL << 62;
        else if (native)                          score = 1ULL << 61;
        else if (w <= 2560 && h <= 1600)          score = (1ULL << 40) + w * h;
        else                                      score = w * h;

        if (!found || score > best_score) { best_score = score; best = m; found = 1; }
    }

    if (keep) {
        /* Everything above still ran, so the list of modes the kernel is given
         * is complete and the display can be changed later from inside the
         * running system - where a mistake is recoverable, because there is
         * something there to recover with. */
        serial_puts("[boot] keeping the mode the firmware set\n");
    } else if (found && best != gop->Mode->Mode) {
        gop->SetMode(gop, best);
    }

    /* Mark which of the listed modes is the one now running. */
    bi->mode_current = 0;
    for (UINT32 i = 0; i < bi->mode_count; i++) {
        if (bi->modes[i].width == gop->Mode->Info->HorizontalResolution &&
            bi->modes[i].height == gop->Mode->Info->VerticalResolution) {
            bi->modes[i].flags |= KB_MODE_CURRENT;
            bi->mode_current = i;
            break;
        }
    }


    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
    fb->base   = gop->Mode->FrameBufferBase;
    fb->size   = gop->Mode->FrameBufferSize;
    fb->width  = info->HorizontalResolution;
    fb->height = info->VerticalResolution;
    fb->pitch  = info->PixelsPerScanLine * 4;
    fb->bpp    = 32;

    if (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
        fb->red_shift = 16; fb->green_shift = 8; fb->blue_shift = 0;
        fb->red_bits = fb->green_bits = fb->blue_bits = 8;
    } else if (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
        fb->red_shift = 0; fb->green_shift = 8; fb->blue_shift = 16;
        fb->red_bits = fb->green_bits = fb->blue_bits = 8;
    } else {
        /* PixelBitMask: derive shift/width from each channel mask. */
        struct { UINT32 mask; uint8_t *shift, *bits; } ch[3] = {
            { info->PixelInformation.RedMask,   &fb->red_shift,   &fb->red_bits   },
            { info->PixelInformation.GreenMask, &fb->green_shift, &fb->green_bits },
            { info->PixelInformation.BlueMask,  &fb->blue_shift,  &fb->blue_bits  },
        };
        for (int i = 0; i < 3; i++) {
            UINT32 m = ch[i].mask;
            uint8_t s = 0, n = 0;
            if (m) { while (!(m & 1)) { m >>= 1; s++; } while (m & 1) { m >>= 1; n++; } }
            *ch[i].shift = s; *ch[i].bits = n;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* boot device identity                                                      */
/* ------------------------------------------------------------------------- */

static void record_boot_device(EFI_LOADED_IMAGE_PROTOCOL *li, kboot_info *bi) {
    EFI_DEVICE_PATH_PROTOCOL *dp = 0;
    if (EFI_ERROR(BS->HandleProtocol(li->DeviceHandle, &gDevPath, (void **)&dp)) || !dp) return;

    while (dp->Type != END_DEVICE_PATH_TYPE) {
        UINT16 len = (UINT16)(dp->Length[0] | (dp->Length[1] << 8));
        if (len < 4) break;
        if (dp->Type == MEDIA_DEVICE_PATH && dp->SubType == MEDIA_HARDDRIVE_DP) {
            HARDDRIVE_DEVICE_PATH *hd = (HARDDRIVE_DEVICE_PATH *)dp;
            bi->boot_part_index = hd->PartitionNumber;
            if (hd->SignatureType == 2) memcpy(bi->boot_part_guid, hd->Signature, 16);
        }
        dp = (EFI_DEVICE_PATH_PROTOCOL *)((uint8_t *)dp + len);
    }
}

/* ------------------------------------------------------------------------- */
/* Windows chainload                                                         */
/* ------------------------------------------------------------------------- */

#define MEDIA_FILEPATH_DP 0x04

static UINTN dp_size(EFI_DEVICE_PATH_PROTOCOL *dp) {
    UINTN n = 0;
    while (dp->Type != END_DEVICE_PATH_TYPE) {
        UINT16 len = (UINT16)(dp->Length[0] | (dp->Length[1] << 8));
        if (len < 4) break;
        n += len;
        dp = (EFI_DEVICE_PATH_PROTOCOL *)((uint8_t *)dp + len);
    }
    return n;
}

/* Build "<device path of our volume>/<file>" so LoadImage can find a loader
 * sitting next to us on the same ESP. */
static EFI_DEVICE_PATH_PROTOCOL *dp_for_file(EFI_HANDLE dev, const CHAR16 *file) {
    EFI_DEVICE_PATH_PROTOCOL *base = 0;
    if (EFI_ERROR(BS->HandleProtocol(dev, &gDevPath, (void **)&base)) || !base) return 0;

    UINTN blen = dp_size(base);
    UINTN flen = 0; while (file[flen]) flen++;
    UINTN fnode = 4 + (flen + 1) * 2;
    UINTN total = blen + fnode + 4;

    uint8_t *buf = pool(total);
    memcpy(buf, base, blen);

    uint8_t *n = buf + blen;
    n[0] = MEDIA_DEVICE_PATH; n[1] = MEDIA_FILEPATH_DP;
    n[2] = (uint8_t)(fnode & 0xFF); n[3] = (uint8_t)(fnode >> 8);
    memcpy(n + 4, file, (flen + 1) * 2);

    uint8_t *e = n + fnode;
    e[0] = END_DEVICE_PATH_TYPE; e[1] = 0xFF; e[2] = 4; e[3] = 0;
    return (EFI_DEVICE_PATH_PROTOCOL *)buf;
}

typedef EFI_STATUS (EFIAPI *load_image_fn)(BOOLEAN, EFI_HANDLE, EFI_DEVICE_PATH_PROTOCOL *, VOID *, UINTN, EFI_HANDLE *);
typedef EFI_STATUS (EFIAPI *start_image_fn)(EFI_HANDLE, UINTN *, CHAR16 **);

static void chainload(EFI_HANDLE dev, const CHAR16 *path) {
    EFI_DEVICE_PATH_PROTOCOL *dp = dp_for_file(dev, path);
    if (!dp) { bprint("boot: cannot build device path\n"); return; }
    EFI_HANDLE h = 0;
    load_image_fn LoadImage = (load_image_fn)BS->LoadImage;
    start_image_fn StartImage = (start_image_fn)BS->StartImage;
    EFI_STATUS st = LoadImage(0, IMG, dp, 0, 0, &h);
    if (EFI_ERROR(st)) { bprint("boot: LoadImage failed (%X)\n", (uint64_t)st); return; }
    StartImage(h, 0, 0);
}

/* ------------------------------------------------------------------------- */
/* menu                                                                      */
/* ------------------------------------------------------------------------- */

static const CHAR16 *WIN_LOADER = u"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";

/* Which entries are on the menu this time, in the order they are drawn.  Built
 * once by menu() and read by menu_draw(), so the two cannot disagree about
 * what the highlighted line means. */
static int order[4];

/* The entries, in the order they appear.  "Leave the graphics card alone" was
 * a hidden keypress and is now a line on the menu, because a hidden keypress is
 * no use on the machine that needs it: the first boot on unfamiliar hardware is
 * exactly the boot where the screen may not be showing anything to read the
 * instruction from. */
#define ENTRY_KESTREL 0
#define ENTRY_WINDOWS 1
#define ENTRY_NOGPU   2
#define ENTRY_GPU     3

static void menu_draw(int sel, int count, int remaining) {
    static const char *names[4] = {
        "KestrelOS  (main - the desktop)",
        "Windows Boot Manager",
        "KestrelOS, leaving the graphics card alone",
        "GPU test  (RGB colour flash, then NVIDIA GSP + driver display re-light)",
    };
    wipe();
    attr(EFI_WHITE);
    puta("\n  KestrelOS boot loader\n");
    attr(EFI_LIGHTGRAY);
    puta("  =====================\n\n");
    for (int i = 0; i < count; i++) {
        int which = order[i];
        if (i == sel) { attr(EFI_YELLOW);    puta("   > "); }
        else          { attr(EFI_LIGHTGRAY); puta("     "); }
        puta(names[which]);
        puta("\n");
    }
    attr(EFI_DARKGRAY);
    if (remaining >= 0) bprint("\n  Up/Down to choose, Enter to boot.  Starting in %d s...\n", (int64_t)remaining);
    else                puta("\n  Up/Down to choose, Enter to boot.\n");
    /* The graphics driver talks to the card directly.  On a machine it has
     * never seen, that is worth being able to skip: a driver that upsets a
     * card can take the screen with it and leave nothing to read.  S still
     * works, for anyone who knows it; the menu entry is for everyone else. */
    puta("  S boots the main KestrelOS desktop, from anywhere on this menu.\n");
    attr(EFI_LIGHTGRAY);
}

static int menu(bootcfg *cfg, int have_windows) {
    /* KestrelOS first, then Windows if it is there, then the cautious entry -
     * which is last because it is the one to reach for when the first has
     * already failed. */
    /* Two KestrelOS entries only (plus Windows if present): the main desktop, and
     * the GPU test.  The old redundant "leaving the graphics card alone" entry
     * (ENTRY_NOGPU) is dropped - the main entry already leaves the card alone (it
     * never sends gpustart), so it was a duplicate. */
    order[0] = ENTRY_KESTREL;
    int count = 1;
    if (have_windows) order[count++] = ENTRY_WINDOWS;
    order[count++] = ENTRY_GPU;

    int sel = 0;
    if (!memcmp(cfg->default_entry, "gputest", 8) ||
        !memcmp(cfg->default_entry, "gpu", 4)) {
        for (int i = 0; i < count; i++)
            if (order[i] == ENTRY_GPU) { sel = i; break; }
    }
    if (cfg->timeout <= 0) return order[sel];

    int remaining = cfg->timeout > 0 ? cfg->timeout : 5;

    for (;;) {
        menu_draw(sel, count, remaining);

        /* One second of the countdown, polled in 100 ms slices so a keystroke
         * is picked up promptly.  Any key cancels the countdown for good. */
        for (int tick = 0; tick < 10; tick++) {
            EFI_INPUT_KEY k;
            if (!EFI_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &k))) {
                if (k.UnicodeChar == '\r') return order[sel];
                if (k.UnicodeChar == 's' || k.UnicodeChar == 'S') return ENTRY_KESTREL;
                if (k.ScanCode == SCAN_UP)   sel = (sel + count - 1) % count;
                if (k.ScanCode == SCAN_DOWN) sel = (sel + 1) % count;
                remaining = -1;
                goto redraw;
            }
            if (remaining >= 0) BS->Stall(100000);
        }
        if (remaining == 0) return order[sel];
        if (remaining > 0) { remaining--; continue; }

        /* Countdown cancelled: block until the user decides. */
        {
            EFI_INPUT_KEY k; UINTN idx;
            BS->WaitForEvent(1, &ST->ConIn->WaitForKey, &idx);
            if (EFI_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &k))) continue;
            if (k.UnicodeChar == '\r') return order[sel];
            if (k.UnicodeChar == 's' || k.UnicodeChar == 'S') return ENTRY_KESTREL;
            if (k.ScanCode == SCAN_UP)   sel = (sel + count - 1) % count;
            if (k.ScanCode == SCAN_DOWN) sel = (sel + 1) % count;
        }
    redraw:;
    }
}

/* ------------------------------------------------------------------------- */
/* entry                                                                     */
/* ------------------------------------------------------------------------- */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab) {
    ST = systab;
    BS = systab->BootServices;
    IMG = image;

    serial_init();
    post(POST_ENTERED);
    serial_puts("\n[boot] KestrelOS loader starting\n");

    /* Before anything else, and before anything that can fail.  Two reasons,
     * and the second is the one that matters:
     *
     *   The firmware's text console may not be going anywhere.  A machine set
     *   to boot fast brings the graphics output up and never renders a text
     *   console at all, so every message printed to it succeeds and is seen by
     *   nobody.  Drawing into the framebuffer ourselves is the only way to be
     *   sure that what the loader says can be read.
     *
     *   The screen changing colour is itself the answer to a question nothing
     *   else can answer on a machine with no serial cable: did the loader run.
     *   If the screen turns dark blue it did; if it stays on the vendor logo
     *   or goes black, it did not, and no amount of looking at later code will
     *   help. */
    screen_open();
    post(POST_SCREEN);

    ST->ConOut->EnableCursor(ST->ConOut, 0);
    BS->SetWatchdogTimer(0, 0, 0, 0);

    attr(EFI_WHITE);
    puta("\n  KestrelOS\n");
    attr(EFI_DARKGRAY);
    if (scr.ok)
        bprint("  loader running, screen %ux%u at 0x%X\n\n",
               (uint64_t)scr.width, (uint64_t)scr.height,
               (uint64_t)(uintptr_t)scr.base);
    else
        puta("  loader running; no graphics output was offered\n\n");
    attr(EFI_LIGHTGRAY);

    /* --- our volume ------------------------------------------------------ */
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    if (EFI_ERROR(BS->HandleProtocol(image, &gLoadedImage, (void **)&li)) || !li)
        die("cannot open LoadedImage protocol", EFI_NOT_FOUND);

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    if (EFI_ERROR(BS->HandleProtocol(li->DeviceHandle, &gSimpleFs, (void **)&fs)) || !fs)
        die("boot volume has no filesystem", EFI_NOT_FOUND);
    if (EFI_ERROR(fs->OpenVolume(fs, &g_root)) || !g_root)
        die("cannot open boot volume", EFI_DEVICE_ERROR);

    /* From here on every step is also written to the stick it booted from, so
     * a machine that goes dark can still be asked what happened afterwards. */
    trail_ready = 1;
    trail("");
    trail("---- boot ----");
    milestone(POST_VOLUME, "opened the volume this was loaded from");

    /* --- configuration --------------------------------------------------- */
    bootcfg cfg;
    cfg_defaults(&cfg);
    {
        void *txt = 0; UINTN tlen = 0;
        if (file_read_all(u"\\KESTREL\\BOOT.CFG", &txt, &tlen, 0, 0)) {
            cfg_parse(&cfg, (char *)txt, tlen);
            BS->FreePool(txt);
        }
    }

    milestone(POST_CONFIG, "read the configuration");

    int have_windows = file_exists(WIN_LOADER);
    milestone(POST_MENU, "showing the menu");
    int chose = menu(&cfg, have_windows);

    /* Asked to leave the graphics card alone.  The kernel is told on its
     * command line, which is where it looks for the things it has to decide
     * before it has any way of reporting a decision. */
    if (chose == ENTRY_NOGPU || chose == ENTRY_GPU) {
        UINTN at = 0;
        while (at < sizeof cfg.cmdline - 1 && cfg.cmdline[at]) at++;
        /* NOGPU leaves the card entirely alone; GPU asks the kernel to boot GSP
         * and run the accel bring-up (this freezes the display until re-light
         * exists, but the copy-engine result lands in the USB log). */
        /* GPU test also asks the kernel to flash the panel RED/GREEN/BLUE off
         * the live firmware framebuffer BEFORE the GSP boot (guaranteed-visible
         * pixels), then run the driver's own GSP-channel modeset re-light. */
        const char *word = (chose == ENTRY_GPU) ? "gpustart rgbtest" : "nogpu";
        if (at) { if (at < sizeof cfg.cmdline - 1) cfg.cmdline[at++] = ' '; }
        while (*word && at < sizeof cfg.cmdline - 1) cfg.cmdline[at++] = *word++;
        cfg.cmdline[at] = 0;
    }
    /* ENTRY_GPU boots the kernel exactly like ENTRY_KESTREL otherwise. */
    if (chose == ENTRY_GPU) chose = ENTRY_KESTREL;

    if (chose == ENTRY_WINDOWS && have_windows) {
        wipe();
        puta("Starting Windows Boot Manager...\n");
        chainload(li->DeviceHandle, WIN_LOADER);
        die("Windows Boot Manager did not start", EFI_LOAD_ERROR);
    }

    wipe();
    puta("Loading KestrelOS...\n");

    /* --- kernel ---------------------------------------------------------- */
    CHAR16 wpath[128];
    void *kbuf = 0; UINTN ksize = 0;
    a2w(cfg.kernel, wpath, 128);
    if (!file_read_all(wpath, &kbuf, &ksize, 0, 0)) die("cannot read kernel image", EFI_NOT_FOUND);

    elf64_ehdr *eh = kbuf;
    if (ksize < sizeof *eh || memcmp(eh->e_ident, "\x7F" "ELF", 4) || eh->e_ident[4] != 2 || eh->e_machine != 0x3E)
        die("kernel is not a 64-bit x86-64 ELF", EFI_LOAD_ERROR);

    uint64_t vmin = ~0ULL, vmax = 0;
    elf64_phdr *ph = (elf64_phdr *)((uint8_t *)kbuf + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr < vmin) vmin = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > vmax) vmax = ph[i].p_vaddr + ph[i].p_memsz;
    }
    if (vmin == ~0ULL || vmax <= vmin) die("kernel has no loadable segments", EFI_LOAD_ERROR);
    if (vmin < KB_KERNEL_BASE) die("kernel is not linked in the top 2 GiB", EFI_LOAD_ERROR);

    uint64_t kspan = (vmax - vmin + 0xFFF) & ~0xFFFULL;
    uint64_t kphys = alloc_pages_at_any((UINTN)(kspan / 0x1000), EfiLoaderData);
    if (!kphys) die("cannot allocate memory for the kernel", EFI_OUT_OF_RESOURCES);
    memset((void *)(uintptr_t)kphys, 0, (size_t)kspan);

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint8_t *dst = (uint8_t *)(uintptr_t)kphys + (ph[i].p_vaddr - vmin);
        memcpy(dst, (uint8_t *)kbuf + ph[i].p_offset, (size_t)ph[i].p_filesz);
    }
    uint64_t kentry = eh->e_entry;
    BS->FreePool(kbuf);
    milestone(POST_KERNEL_READ, "loaded the kernel into memory");

    /* --- initrd ---------------------------------------------------------- */
    void *ibuf = 0; UINTN isize = 0; uint64_t iphys = 0;
    a2w(cfg.initrd, wpath, 128);
    if (!file_read_all(wpath, &ibuf, &isize, 1, &iphys)) {
        bprint("boot: warning - no initrd at %a\n", cfg.initrd);
        iphys = 0; isize = 0;
    }

    milestone(POST_INITRD, "loaded the initrd");

    /* --- boot info ------------------------------------------------------- */
    kboot_info *bi = pool(sizeof *bi);
    bi->magic       = KB_MAGIC;
    bi->version     = KB_VERSION;
    bi->size        = sizeof *bi;
    bi->initrd_base = iphys;
    bi->initrd_size = isize;
    bi->kernel_phys = kphys;
    bi->kernel_virt = vmin;
    bi->kernel_size = kspan;
    bi->hhdm_base   = KB_HHDM_BASE;
    bi->efi_system_table = (uint64_t)(uintptr_t)ST;
    bi->efi_runtime = (uint64_t)(uintptr_t)ST->RuntimeServices;

    /* Reserve the kernel's log region, and write out whatever the previous
     * boot left in it.  This has to happen while boot services are alive,
     * because writing a file is the one thing the kernel cannot do. */
    {
        uint32_t rsize = 0, rprev = 0, rboot = 0;
        bi->ramlog_base = prepare_ramlog(&rsize, &rprev, &rboot);
        bi->ramlog_size = rsize;
        bi->ramlog_prev = rprev;
        if (bi->ramlog_base) {
            char note[128];
            size_t n = 0;
            n = note_str(note, sizeof note, n, "kernel log: boot ");
            n = note_num(note, sizeof note, n, rboot);
            n = note_str(note, sizeof note, n, ", region reserved; recovered ");
            n = note_num(note, sizeof note, n, rprev);
            n = note_str(note, sizeof note, n, " bytes from the previous boot");
            /* The address as well.  Reserving the region and reserving it
             * WHERE THE NEXT BOOT WILL LOOK are different things, and only the
             * second one is any use - so the number that decides it goes in
             * the log rather than being taken on trust. */
            n = note_str(note, sizeof note, n, "; region at ");
            n = note_num(note, sizeof note, n, (uint32_t)(bi->ramlog_base >> 20));
            n = note_str(note, sizeof note, n, " MiB");
            note[n] = 0;
            milestone(0xEB, note);
        } else {
            milestone(0xEB, "kernel log: no memory could be reserved for it");
        }
    }
    memcpy(bi->cmdline, cfg.cmdline, sizeof bi->cmdline);
    memcpy(bi->bootdev, cfg.data, sizeof bi->bootdev);

    /* The multi-display arrangement (#7), by first letter: 'm'irror, 'o'nly-the-
     * other, anything else (or unset) extend - which is the default for two
     * displays and harmless with one. */
    if (cfg.displaymode[0] == 'm' || cfg.displaymode[0] == 'M' ||
        cfg.displaymode[0] == 'd' || cfg.displaymode[0] == 'D')
        bi->display.display_mode = KB_DISPLAY_MIRROR;
    else if (cfg.displaymode[0] == 'o' || cfg.displaymode[0] == 'O') {
        /* "only:N" is one-based in the file/UI.  Keep "onlyother" backward
         * compatible as screen 2. */
        unsigned which = 2;
        if (cfg.displaymode[4] == ':' && cfg.displaymode[5] >= '1' &&
            cfg.displaymode[5] <= '8' && cfg.displaymode[6] == 0)
            which = (unsigned)(cfg.displaymode[5] - '0');
        bi->display.display_mode = (uint8_t)(KB_DISPLAY_ONLY_BASE + which - 1u);
    }
    else
        bi->display.display_mode = KB_DISPLAY_EXTEND;
    record_boot_device(li, bi);

    /* --- ACPI ------------------------------------------------------------ */
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &ST->ConfigurationTable[i];
        if (!memcmp(&t->VendorGuid, &gAcpi20, sizeof(EFI_GUID))) { bi->rsdp = (uint64_t)(uintptr_t)t->VendorTable; break; }
        if (!memcmp(&t->VendorGuid, &gAcpi10, sizeof(EFI_GUID))) bi->rsdp = (uint64_t)(uintptr_t)t->VendorTable;
    }
    if (!bi->rsdp) bprint("boot: warning - no ACPI RSDP found\n");

    /* --- graphics -------------------------------------------------------- */
    {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = best_gop();
        if (!gop) die("no UEFI graphics output protocol", EFI_NOT_FOUND);
        pick_gop_mode(gop, bi, cfg.video);
    }
    if (gop_count > 1) {
        /* Worth recording in its own right.  A machine with one output that
         * shows nothing and a machine with three where the wrong one was
         * picked look identical from the far side of a black screen, and this
         * is the line that tells them apart. */
        char note[96];
        const char *pre = "display outputs found: ";
        UINTN n = 0;
        while (pre[n] && n < sizeof note - 8) { note[n] = pre[n]; n++; }
        note[n++] = (char)('0' + (gop_count / 10) % 10);
        note[n++] = (char)('0' + gop_count % 10);
        note[n] = 0;
        trail(note);

        attr(EFI_DARKGRAY);
        bprint("  %d display outputs; using the one the firmware had already "
               "set a mode on\n", (int64_t)gop_count);
        attr(EFI_LIGHTGRAY);
    }
    milestone(POST_GRAPHICS, "chose the video mode");
    serial_puts("[boot] framebuffer at 0x"); serial_hex(bi->fb.base); serial_puts("\n");

    /* The mode may have changed under the console that has been writing to it,
     * so pick the new one up before saying anything else. */
    screen_open();
    bprint("  video: %ux%u, %u bits, at 0x%X\n",
           (uint64_t)bi->fb.width, (uint64_t)bi->fb.height,
           (uint64_t)bi->fb.bpp, bi->fb.base);

    /* --- memory map sizing ----------------------------------------------- */
    UINTN map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_ver = 0;
    BS->GetMemoryMap(&map_size, 0, &map_key, &desc_size, &desc_ver);
    map_size += desc_size * 16;                 /* headroom for our own allocations */
    EFI_MEMORY_DESCRIPTOR *map = pool(map_size);

    UINTN max_entries = map_size / desc_size + 16;
    UINTN kb_map_pages = ((max_entries * sizeof(kboot_mmap_entry)) + 0xFFF) / 0x1000;
    uint64_t kb_map_phys = alloc_pages_at_any(kb_map_pages, EfiLoaderData);
    if (!kb_map_phys) die("cannot allocate memory map", EFI_OUT_OF_RESOURCES);
    kboot_mmap_entry *kbmap = (kboot_mmap_entry *)(uintptr_t)kb_map_phys;
    bi->mmap = kb_map_phys;

    /* --- how much physical space must the direct map cover? --------------
     * Only real memory: MMIO apertures can sit at absurd addresses and the
     * kernel's own VMM maps those on demand.  The low 4 GiB is always covered
     * because that is where legacy MMIO and most PCI BARs live, and the
     * framebuffer is mapped explicitly below whatever its address. */
    uint64_t max_phys = 0;
    {
        UINTN ms = map_size, mk = 0, ds = 0; UINT32 dv = 0;
        if (EFI_ERROR(BS->GetMemoryMap(&ms, map, &mk, &ds, &dv))) die("GetMemoryMap failed", EFI_DEVICE_ERROR);
        for (UINTN off = 0; off < ms; off += ds) {
            EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)map + off);
            switch (d->Type) {
            case EfiConventionalMemory: case EfiLoaderCode:  case EfiLoaderData:
            case EfiBootServicesCode:   case EfiBootServicesData:
            case EfiRuntimeServicesCode: case EfiRuntimeServicesData:
            case EfiACPIReclaimMemory:  case EfiACPIMemoryNVS:
            case EfiPersistentMemory: {
                uint64_t end = d->PhysicalStart + d->NumberOfPages * 0x1000;
                if (end > max_phys) max_phys = end;
                break;
            }
            default: break;
            }
        }
    }
    if (max_phys < (4ULL << 30)) max_phys = 4ULL << 30;   /* always cover low MMIO */
    max_phys = (max_phys + (1ULL << 30) - 1) & ~((1ULL << 30) - 1);
    /* Without 1 GiB pages every gigabyte costs two page-directory pages, so keep
     * the eagerly mapped window sane and let the kernel map the rest. */
    if (!have_1g_pages() && max_phys > (64ULL << 30)) max_phys = 64ULL << 30;

    /* --- page tables ----------------------------------------------------- */
    enable_nx();
    int gig = have_1g_pages();
    /* Worst case: one PML4 + PDPTs + PDs for the direct map, one 4 KiB page
     * table per 2 MiB of kernel image, plus slack for split address regions
     * and a framebuffer above RAM.  The matching NVIDIA RM core makes the
     * kernel ~100 MiB, so the historical fixed 16-page kernel allowance is
     * no longer valid. */
    UINTN gigs = (UINTN)(max_phys >> 30);
    UINTN kernel_pt_pages = (UINTN)((kspan + (1ULL << 21) - 1) >> 21) + 4;
    pt_pool_pages = 16 + (gig ? 8 : (gigs * 2 + 8)) + kernel_pt_pages + 32;
    uint64_t ptp = alloc_pages_at_any(pt_pool_pages, EfiLoaderData);
    if (!ptp) die("cannot allocate page tables", EFI_OUT_OF_RESOURCES);
    pt_pool = (uint64_t *)(uintptr_t)ptp;
    pt_pool_used = 0;

    uint64_t *pml4 = pt_alloc();
    map_range_both(pml4, max_phys, gig);

    /* The framebuffer usually sits in MMIO space above real memory, so map
     * whatever part of it the direct map above did not already cover.  Only the
     * uncovered tail is touched: re-mapping a range that is already described
     * by a 1 GiB entry would walk into the middle of that frame. */
    {
        uint64_t fb_end = bi->fb.base + bi->fb.size;
        if (fb_end > max_phys) {
            uint64_t s = bi->fb.base < max_phys ? max_phys : (bi->fb.base & ~((1ULL << 21) - 1));
            uint64_t e = (fb_end + (1ULL << 21) - 1) & ~((1ULL << 21) - 1);
            for (uint64_t p = s; p < e; p += (1ULL << 21)) {
                map_2m(pml4, p, p, PTE_W);
                map_2m(pml4, KB_HHDM_BASE + p, p, PTE_W | nx_bit);
            }
        }
    }

    /* Kernel image at its link address, 4 KiB granularity. */
    for (uint64_t off = 0; off < kspan; off += 0x1000)
        map_4k(pml4, vmin + off, kphys + off, PTE_W);

    bi->pml4_phys = (uint64_t)(uintptr_t)pml4;

    /* --- a stack for the kernel ------------------------------------------ */
    uint64_t kstack = alloc_pages_at_any(16, EfiLoaderData);   /* 64 KiB */
    if (!kstack) die("cannot allocate kernel stack", EFI_OUT_OF_RESOURCES);
    memset((void *)(uintptr_t)kstack, 0, 16 * 0x1000);
    bi->loader_stack_top = kstack + 16 * 0x1000;

    milestone(POST_PAGING, "built the page tables");

    /* The last thing anyone can be told through the firmware.  After the call
     * below there is no console, no file system and no way to write to the
     * stick - so what a black screen after this point means is narrowed to one
     * thing, and the motherboard's own display is the only thing still
     * talking. */
    attr(EFI_DARKGRAY);
    bprint("  entering the kernel at 0x%X\n", kentry);
    attr(EFI_LIGHTGRAY);
    trail("about to shut the firmware down; anything after this is the kernel");
    milestone(POST_EXIT, "exiting boot services");
    serial_puts("[boot] exiting boot services\n");

    /* --- final memory map, then the point of no return ------------------- */
    UINTN final_size = map_size, final_key = 0, final_desc = 0;
    UINT32 final_ver = 0;
    EFI_STATUS st = BS->GetMemoryMap(&final_size, map, &final_key, &final_desc, &final_ver);
    if (EFI_ERROR(st)) die("GetMemoryMap (final) failed", st);

    UINTN n = 0;
    for (UINTN off = 0; off < final_size && n < max_entries; off += final_desc) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)map + off);
        uint32_t type;
        switch (d->Type) {
        case EfiConventionalMemory:
        case EfiBootServicesCode:
        case EfiBootServicesData:   type = KB_MEM_USABLE;       break;
        case EfiLoaderCode:
        case EfiLoaderData:         type = KB_MEM_LOADER;       break;
        case EfiACPIReclaimMemory:  type = KB_MEM_ACPI_RECLAIM; break;
        case EfiACPIMemoryNVS:      type = KB_MEM_ACPI_NVS;     break;
        case EfiUnusableMemory:     type = KB_MEM_BAD;          break;
        default:                    type = KB_MEM_RESERVED;     break;
        }
        /* Firmware maps are fragmented; folding neighbours of the same type
         * keeps the count inside what the kernel's static table can hold. */
        if (n && kbmap[n - 1].type == type &&
            kbmap[n - 1].base + kbmap[n - 1].pages * 0x1000 == d->PhysicalStart) {
            kbmap[n - 1].pages += d->NumberOfPages;
            continue;
        }
        kbmap[n].base  = d->PhysicalStart;
        kbmap[n].pages = d->NumberOfPages;
        kbmap[n].type  = type;
        kbmap[n].pad   = 0;
        n++;
    }
    bi->mmap_count = (uint32_t)n;

    /* Mark the regions the kernel must not reuse until it has copied them. */
    for (UINTN i = 0; i < n; i++) {
        uint64_t s = kbmap[i].base, e = s + kbmap[i].pages * 0x1000;
        if (kphys >= s && kphys < e) kbmap[i].type = KB_MEM_KERNEL;
        else if (iphys && iphys >= s && iphys < e) kbmap[i].type = KB_MEM_KERNEL;
        else if (kb_map_phys >= s && kb_map_phys < e) kbmap[i].type = KB_MEM_KERNEL;
        else if (ptp >= s && ptp < e) kbmap[i].type = KB_MEM_KERNEL;
        else if (kstack >= s && kstack < e) kbmap[i].type = KB_MEM_KERNEL;
        else if (bi->fb.base >= s && bi->fb.base < e) kbmap[i].type = KB_MEM_FRAMEBUFFER;
    }

    st = BS->ExitBootServices(image, final_key);
    if (EFI_ERROR(st)) {
        /* The map changed underneath us; refresh once and retry, which is the
         * behaviour the spec asks for. */
        final_size = map_size;
        if (EFI_ERROR(BS->GetMemoryMap(&final_size, map, &final_key, &final_desc, &final_ver)))
            die("GetMemoryMap (retry) failed", st);
        st = BS->ExitBootServices(image, final_key);
        if (EFI_ERROR(st)) die("ExitBootServices failed", st);
    }

    serial_puts("[boot] bootinfo at 0x"); serial_hex((uint64_t)(uintptr_t)bi);
    serial_puts(" magic 0x"); serial_hex(bi->magic);
    serial_puts(" size 0x"); serial_hex(sizeof *bi);
    serial_puts("\n[boot] entry 0x"); serial_hex(kentry);
    serial_puts(" pml4 0x"); serial_hex((uint64_t)(uintptr_t)pml4);
    serial_puts("\n[boot] jumping to kernel\n");
    post(POST_JUMP);

    /* --- hand over ------------------------------------------------------- */
    /* The boot info goes into RDI through an explicit constraint rather than a
     * move inside the block: with four plain "r" operands the compiler is free
     * to put the entry point in RDI as well, and the move would then destroy
     * it before the jump. */
    __asm__ volatile(
        "cli\n"
        "movq %[pml4], %%cr3\n"
        "movq %[stack], %%rsp\n"
        "xorq %%rbp, %%rbp\n"
        "pushq $0\n"            /* a null return address to stop unwinders */
        "jmp *%[entry]\n"
        :
        : [pml4]  "r"((uint64_t)(uintptr_t)pml4),
          [stack] "r"(bi->loader_stack_top - 16),
          [entry] "r"(kentry),
          "D"((uint64_t)(uintptr_t)bi)      /* SysV first argument */
        : "memory");

    for (;;) __asm__ volatile("hlt");
    return EFI_SUCCESS;
}
