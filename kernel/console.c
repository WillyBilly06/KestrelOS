/* console.c - text console on the UEFI linear framebuffer, plus the COM1 serial
 * port that mirrors everything for headless debugging.
 *
 * The console understands the slice of ANSI/VT100 that the shell and installer
 * actually use: SGR colours, cursor movement, erase, and scroll regions.  Glyphs
 * come from the generated 8x16 CP437 font.
 */
#include "kernel.h"
#include "gpu.h"
#include "mm.h"
#include "font8x16.h"

/* ------------------------------------------------------------------------- */
/* serial                                                                    */
/* ------------------------------------------------------------------------- */

#define COM1 0x3F8
static bool serial_up;

void serial_init(void) {
    outb(COM1 + 1, 0x00);       /* interrupts off while we program the divisor */
    outb(COM1 + 3, 0x80);       /* DLAB */
    outb(COM1 + 0, 0x01);       /* 115200 */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);       /* 8N1 */
    outb(COM1 + 2, 0xC7);       /* FIFO on, clear, 14-byte trigger */
    outb(COM1 + 4, 0x0B);       /* DTR/RTS/OUT2 */

    /* Loopback test: if nothing answers there is no port and we must not spin
     * on the transmit-holding bit for every character forever. */
    outb(COM1 + 4, 0x1E);
    outb(COM1 + 0, 0xAE);
    serial_up = (inb(COM1 + 0) == 0xAE);
    outb(COM1 + 4, 0x0B);
}

void serial_putc(char c) {
    if (!serial_up) return;
    if (c == '\n') serial_putc('\r');
    for (int i = 0; i < 200000; i++) if (inb(COM1 + 5) & 0x20) break;
    outb(COM1, (u8)c);
}

void serial_write(const char *s, size_t n) { while (n--) serial_putc(*s++); }

/* ------------------------------------------------------------------------- */
/* framebuffer text console                                                  */
/* ------------------------------------------------------------------------- */

static volatile u8 *fb_base;
static u32 fb_w, fb_h, fb_pitch;

/* Where the display is, for the one caller that needs to write to it in bulk
 * rather than a character at a time.  Returned as an address rather than
 * exposing the pointer, because it is a kernel mapping and every address space
 * shares it - which is what lets a processor other than the one in the system
 * call write through it. */
u64 console_framebuffer(u32 *width, u32 *height, u32 *pitch) {
    if (!fb_base) return 0;
    if (width)  *width  = fb_w;
    if (height) *height = fb_h;
    if (pitch)  *pitch  = fb_pitch;
    return (u64)(uintptr_t)fb_base;
}
static u8  r_sh, g_sh, b_sh;
static bool fb_up;

static int  cols, rows;
static int  cur_x, cur_y;
static u8   fg = C_LGRAY, bg = C_BLACK;
static bool cursor_on = true;
static bool cursor_drawn;
static int  scroll_top, scroll_bot;   /* inclusive rows of the scrolling region */

/* A shadow of the character cells, so scrolling and cursor erase can repaint
 * exactly what was there without reading back from uncached video memory. */
#define MAX_COLS 256
#define MAX_ROWS 96
static u8 cell_ch[MAX_ROWS][MAX_COLS];
static u8 cell_fg[MAX_ROWS][MAX_COLS];
static u8 cell_bg[MAX_ROWS][MAX_COLS];

static const u8 palette[16][3] = {
    {  0,   0,   0}, {  0,   0, 170}, {  0, 170,   0}, {  0, 170, 170},
    {170,   0,   0}, {170,   0, 170}, {170,  85,   0}, {170, 170, 170},
    { 85,  85,  85}, { 85,  85, 255}, { 85, 255,  85}, { 85, 255, 255},
    {255,  85,  85}, {255,  85, 255}, {255, 255,  85}, {255, 255, 255},
};

static inline u32 rgb(u8 idx) {
    const u8 *c = palette[idx & 15];
    return ((u32)c[0] << r_sh) | ((u32)c[1] << g_sh) | ((u32)c[2] << b_sh);
}

static void draw_cell(int x, int y, u8 ch, u8 f, u8 b) {
    if (!fb_up || x < 0 || y < 0 || x >= cols || y >= rows) return;
    u32 cf = rgb(f), cb = rgb(b);
    const u8 *glyph = font8x16[ch];
    volatile u8 *row = fb_base + (u64)y * FONT8X16_H * fb_pitch + (u64)x * FONT8X16_W * 4;
    for (int gy = 0; gy < FONT8X16_H; gy++) {
        volatile u32 *px = (volatile u32 *)row;
        u8 bits = glyph[gy];
        for (int gx = 0; gx < FONT8X16_W; gx++) px[gx] = (bits & (0x80 >> gx)) ? cf : cb;
        row += fb_pitch;
    }
}

static void put_cell(int x, int y, u8 ch, u8 f, u8 b) {
    if (x < 0 || y < 0 || x >= cols || y >= rows) return;
    cell_ch[y][x] = ch; cell_fg[y][x] = f; cell_bg[y][x] = b;
    draw_cell(x, y, ch, f, b);
}

static void cursor_erase(void) {
    if (!cursor_drawn) return;
    draw_cell(cur_x, cur_y, cell_ch[cur_y][cur_x], cell_fg[cur_y][cur_x], cell_bg[cur_y][cur_x]);
    cursor_drawn = false;
}

static void cursor_draw(void) {
    if (!cursor_on || !fb_up || cur_x >= cols || cur_y >= rows) return;
    /* An underline bar in the foreground colour, drawn over the glyph. */
    u32 c = rgb(cell_fg[cur_y][cur_x]);
    volatile u8 *row = fb_base + (u64)cur_y * FONT8X16_H * fb_pitch + (u64)cur_x * FONT8X16_W * 4
                     + (u64)(FONT8X16_H - 2) * fb_pitch;
    for (int gy = 0; gy < 2; gy++) {
        volatile u32 *px = (volatile u32 *)row;
        for (int gx = 0; gx < FONT8X16_W; gx++) px[gx] = c;
        row += fb_pitch;
    }
    cursor_drawn = true;
}

void console_show_cursor(bool on) {
    if (on == cursor_on) return;
    cursor_erase();
    cursor_on = on;
    if (on) cursor_draw();
}

static void fill_rows(int from, int to, u8 f, u8 b) {
    for (int y = from; y <= to; y++)
        for (int x = 0; x < cols; x++) put_cell(x, y, ' ', f, b);
}

static void scroll_up(void) {
    int top = scroll_top, bot = scroll_bot;
    if (bot <= top) return;

    /* Move the shadow first, then blit the framebuffer in one pass so the
     * screen never shows a half-scrolled frame. */
    for (int y = top; y < bot; y++) {
        memcpy(cell_ch[y], cell_ch[y + 1], (size_t)cols);
        memcpy(cell_fg[y], cell_fg[y + 1], (size_t)cols);
        memcpy(cell_bg[y], cell_bg[y + 1], (size_t)cols);
    }
    for (int x = 0; x < cols; x++) { cell_ch[bot][x] = ' '; cell_fg[bot][x] = fg; cell_bg[bot][x] = bg; }

    /* Repaint from the shadow rather than sliding the pixels up.  Moving them
     * would mean reading video memory back, and a write-combining mapping -
     * which is what makes drawing fast in the first place - reads at bus speed
     * with no cache behind it.  Painting every cell again writes more bytes but
     * never reads one, and comes out several times quicker. */
    if (fb_up)
        for (int y = top; y <= bot; y++)
            for (int x = 0; x < cols; x++)
                draw_cell(x, y, cell_ch[y][x], cell_fg[y][x], cell_bg[y][x]);
}

static void newline(void) {
    cur_x = 0;
    if (cur_y >= scroll_bot) { cur_y = scroll_bot; scroll_up(); }
    else cur_y++;
}

/* ------------------------------------------------------------------------- */
/* escape sequence handling                                                  */
/* ------------------------------------------------------------------------- */

static enum { E_NONE, E_ESC, E_CSI } esc_state;
static int  esc_arg[8];
static int  esc_argc;
static bool esc_private;

static void sgr(int n) {
    if (n == 0)                       { fg = C_LGRAY; bg = C_BLACK; }
    else if (n == 1)                  { fg |= 8; }                       /* bold -> bright */
    else if (n == 22)                 { fg &= 7; }
    else if (n == 7)                  { u8 t = fg; fg = bg; bg = t; }
    else if (n >= 30 && n <= 37)      { fg = (u8)((fg & 8) | (n - 30)); }
    else if (n == 39)                 { fg = C_LGRAY; }
    else if (n >= 40 && n <= 47)      { bg = (u8)(n - 40); }
    else if (n == 49)                 { bg = C_BLACK; }
    else if (n >= 90 && n <= 97)      { fg = (u8)(8 + n - 90); }
    else if (n >= 100 && n <= 107)    { bg = (u8)(8 + n - 100); }
}

/* CGA colour order differs from ANSI in the red/blue axis, so translate. */
static const u8 ansi_to_cga[8] = { C_BLACK, C_RED, C_GREEN, C_BROWN, C_BLUE, C_MAGENTA, C_CYAN, C_LGRAY };

static void sgr_fixed(int n) {
    if (n >= 30 && n <= 37)        fg = (u8)((fg & 8) | ansi_to_cga[n - 30]);
    else if (n >= 40 && n <= 47)   bg = ansi_to_cga[n - 40];
    else if (n >= 90 && n <= 97)   fg = (u8)(8 | ansi_to_cga[n - 90]);
    else if (n >= 100 && n <= 107) bg = (u8)(8 | ansi_to_cga[n - 100]);
    else sgr(n);
}

static void csi_dispatch(char c) {
    int a0 = esc_argc > 0 ? esc_arg[0] : 0;
    int a1 = esc_argc > 1 ? esc_arg[1] : 0;

    switch (c) {
    case 'A': cur_y -= a0 ? a0 : 1; if (cur_y < 0) cur_y = 0; break;
    case 'B': cur_y += a0 ? a0 : 1; if (cur_y >= rows) cur_y = rows - 1; break;
    case 'C': cur_x += a0 ? a0 : 1; if (cur_x >= cols) cur_x = cols - 1; break;
    case 'D': cur_x -= a0 ? a0 : 1; if (cur_x < 0) cur_x = 0; break;
    case 'G': cur_x = (a0 ? a0 : 1) - 1; break;
    case 'd': cur_y = (a0 ? a0 : 1) - 1; break;
    case 'H': case 'f':
        cur_y = (a0 ? a0 : 1) - 1;
        cur_x = (a1 ? a1 : 1) - 1;
        break;
    case 'J':
        if (a0 == 2 || a0 == 3) { fill_rows(0, rows - 1, fg, bg); cur_x = cur_y = 0; }
        else if (a0 == 1) { for (int x = 0; x <= cur_x && x < cols; x++) put_cell(x, cur_y, ' ', fg, bg);
                            fill_rows(0, cur_y - 1, fg, bg); }
        else { for (int x = cur_x; x < cols; x++) put_cell(x, cur_y, ' ', fg, bg);
               fill_rows(cur_y + 1, rows - 1, fg, bg); }
        break;
    case 'K':
        if (a0 == 1)      for (int x = 0; x <= cur_x && x < cols; x++) put_cell(x, cur_y, ' ', fg, bg);
        else if (a0 == 2) for (int x = 0; x < cols; x++) put_cell(x, cur_y, ' ', fg, bg);
        else              for (int x = cur_x; x < cols; x++) put_cell(x, cur_y, ' ', fg, bg);
        break;
    case 'm':
        if (esc_argc == 0) sgr_fixed(0);
        else for (int i = 0; i < esc_argc; i++) sgr_fixed(esc_arg[i]);
        break;
    case 'r':
        scroll_top = (a0 ? a0 : 1) - 1;
        scroll_bot = (a1 ? a1 : rows) - 1;
        if (scroll_top < 0) scroll_top = 0;
        if (scroll_bot >= rows) scroll_bot = rows - 1;
        if (scroll_bot < scroll_top) { scroll_top = 0; scroll_bot = rows - 1; }
        cur_x = 0; cur_y = scroll_top;
        break;
    case 'h': if (esc_private && a0 == 25) console_show_cursor(true);  break;
    case 'l': if (esc_private && a0 == 25) console_show_cursor(false); break;
    case 'X': { int n = a0 ? a0 : 1;
                for (int x = cur_x; x < cur_x + n && x < cols; x++) put_cell(x, cur_y, ' ', fg, bg); }
              break;
    default: break;
    }
    if (cur_x < 0) cur_x = 0;
    if (cur_y < 0) cur_y = 0;
    if (cur_x >= cols) cur_x = cols - 1;
    if (cur_y >= rows) cur_y = rows - 1;
}

static void putc_raw(char c) {
    switch (c) {
    case '\n': newline(); return;
    case '\r': cur_x = 0; return;
    case '\b': if (cur_x > 0) cur_x--; return;
    case '\t': do { put_cell(cur_x, cur_y, ' ', fg, bg); cur_x++; } while (cur_x % 8 && cur_x < cols);
               if (cur_x >= cols) newline();
               return;
    case 0x07: return;   /* bell */
    default: break;
    }
    if (cur_x >= cols) newline();
    put_cell(cur_x, cur_y, (u8)c, fg, bg);
    cur_x++;
}

/* klog writes its own, better-formatted line to the serial port before echoing
 * the bare message to the console; without this the port would see both. */
static bool mirror_serial = true;

/* Drawing through the direct map leaves pixels sitting in the cache: harmless
 * where the "framebuffer" is really host memory, visibly broken on a real card
 * that scans out of video memory.  Once the memory manager is up, move to a
 * write-combining mapping, which is both correct and the fastest way to write
 * video memory. */
/* Who is drawing on the screen.
 *
 * The kernel console and a graphical program cannot both own the framebuffer:
 * whichever wrote last wins, and the loser's pixels are simply gone.  So the
 * console gives the screen up when a process maps it, and takes it back - with
 * a full repaint from the character shadow - when that process is finished.
 * Serial output is unaffected either way, so a log line during a graphical
 * session is still recorded, just not painted over the desktop. */
void console_release_framebuffer(void) {
    fb_up = false;
}

/* A GPU reset invalidates the GOP display contract, not merely the console's
 * ownership of it.  Forget both views of the surface so no later process can
 * map the stale physical range and console_take_framebuffer() cannot repaint
 * it when a process exits.  Width/height remain as historical boot metadata;
 * base and size are the authority for whether the surface is usable.  A
 * native display driver may publish a newly allocated surface later. */
void console_abandon_framebuffer(void) {
    gpu_accel_clear();
    console_release_framebuffer();
    cursor_drawn = false;
    fb_base = NULL;
    g_boot.fb.base = 0;
    g_boot.fb.size = 0;
}

/* Make a native driver's logical compositor buffer available to userland.
 * This is deliberately system memory, not a CPU alias of the active VRAM
 * surface: FB_PRESENT hands damaged rectangles to the GPU copy engine, which
 * fans them out to the appropriate NVKMS scanouts.  Keeping the ordinary
 * framebuffer ABI means the existing desktop can start after GOP was retired
 * without ever mapping stale video memory. */
void console_publish_framebuffer(u64 phys, u32 width, u32 height, u32 pitch) {
    if (!phys || !width || !height || pitch < width * 4u) return;

    g_boot.fb.base = phys;
    g_boot.fb.size = (u64)pitch * height;
    g_boot.fb.width = width;
    g_boot.fb.height = height;
    g_boot.fb.pitch = pitch;
    g_boot.fb.bpp = 32;
    g_boot.fb.red_shift = 16;   g_boot.fb.red_bits = 8;
    g_boot.fb.green_shift = 8;  g_boot.fb.green_bits = 8;
    g_boot.fb.blue_shift = 0;   g_boot.fb.blue_bits = 8;

    fb_base = (volatile u8 *)phys_to_virt(phys);
    fb_w = width;
    fb_h = height;
    fb_pitch = pitch;
    r_sh = 16; g_sh = 8; b_sh = 0;
    cols = (int)(width / FONT8X16_W);
    rows = (int)(height / FONT8X16_H);
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    scroll_top = 0;
    scroll_bot = rows - 1;
    if (cur_x >= cols) cur_x = cols - 1;
    if (cur_y >= rows) cur_y = rows - 1;
    cursor_drawn = false;
    /* The display tests remain visible until the desktop explicitly takes
     * ownership and presents its first hardware frame. */
    fb_up = false;
    kinfo("console", "native compositor framebuffer published: %ux%u pitch %u at PA %#llx",
          width, height, pitch, (unsigned long long)phys);
}

void console_take_framebuffer(void) {
    if (!g_boot.fb.base) return;
    fb_up = true;
    cursor_drawn = false;

    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++)
            draw_cell(x, y, cell_ch[y][x], cell_fg[y][x], cell_bg[y][x]);
    cursor_draw();
}

bool console_owns_framebuffer(void) { return fb_up; }

void console_remap_wc(void) {
    if (!g_boot.fb.base || !g_boot.fb.size) return;
    void *mapped = vmm_map_wc(g_boot.fb.base, g_boot.fb.size);
    if (mapped) fb_base = (volatile u8 *)mapped;
}

void console_mirror_serial(bool on) { mirror_serial = on; }

void console_putc(char c) {
    if (mirror_serial) serial_putc(c);
    cursor_erase();

    switch (esc_state) {
    case E_NONE:
        if (c == 0x1B) { esc_state = E_ESC; break; }
        putc_raw(c);
        break;

    case E_ESC:
        if (c == '[') {
            esc_state = E_CSI;
            esc_argc = 0;
            esc_private = false;
            for (int i = 0; i < 8; i++) esc_arg[i] = 0;
        } else if (c == 'c') {
            console_clear();
            esc_state = E_NONE;
        } else {
            esc_state = E_NONE;
        }
        break;

    case E_CSI:
        if (c == '?') { esc_private = true; break; }
        if (c >= '0' && c <= '9') {
            if (esc_argc == 0) esc_argc = 1;
            if (esc_argc <= 8) esc_arg[esc_argc - 1] = esc_arg[esc_argc - 1] * 10 + (c - '0');
            break;
        }
        if (c == ';') { if (esc_argc < 8) esc_argc++; else esc_argc = 8; break; }
        csi_dispatch(c);
        esc_state = E_NONE;
        break;
    }

    cursor_draw();
}

void console_write(const char *s, size_t n) { while (n--) console_putc(*s++); }

void console_clear(void) {
    cursor_erase();
    fill_rows(0, rows - 1, fg, bg);
    cur_x = cur_y = 0;
    cursor_draw();
}

/* Fill the ENTIRE firmware framebuffer with one RGB colour.  This is the OS
 * writing real pixels onto whatever output the firmware left scanning (on this
 * machine, the NVIDIA-connected monitor showing the boot log) - no GPU driver /
 * GSP modeset needed.  Used by the `redfill` display proof. */
void console_fill_rgb(u8 r, u8 g, u8 b) {
    if (!fb_base) return;
    u32 px = ((u32)r << r_sh) | ((u32)g << g_sh) | ((u32)b << b_sh);
    for (u32 y = 0; y < fb_h; y++) {
        volatile u32 *row = (volatile u32 *)(fb_base + (u64)y * fb_pitch);
        for (u32 x = 0; x < fb_w; x++) row[x] = px;
    }
}

void console_set_color(u8 f, u8 b) { fg = f; bg = b; }
void console_get_size(int *c, int *r) { if (c) *c = cols; if (r) *r = rows; }
void console_get_cursor(int *x, int *y) { if (x) *x = cur_x; if (y) *y = cur_y; }

void console_set_cursor(int x, int y) {
    cursor_erase();
    cur_x = x < 0 ? 0 : (x >= cols ? cols - 1 : x);
    cur_y = y < 0 ? 0 : (y >= rows ? rows - 1 : y);
    cursor_draw();
}

void console_scroll_region(int top, int bottom) {
    scroll_top = top < 0 ? 0 : top;
    scroll_bot = bottom >= rows ? rows - 1 : bottom;
    if (scroll_bot < scroll_top) { scroll_top = 0; scroll_bot = rows - 1; }
}

void console_init(const kboot_framebuffer *fb) {
    /* The console has to work before the memory manager exists, so it starts
     * out drawing through the loader's direct map.  console_remap_wc moves it
     * somewhere better once there is a page table to put it in. */
    fb_base  = (volatile u8 *)phys_to_virt(fb->base);
    fb_w     = fb->width;
    fb_h     = fb->height;
    fb_pitch = fb->pitch;
    r_sh = fb->red_shift; g_sh = fb->green_shift; b_sh = fb->blue_shift;

    cols = (int)(fb_w / FONT8X16_W);
    rows = (int)(fb_h / FONT8X16_H);
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;

    fb_up = true;
    scroll_top = 0;
    scroll_bot = rows - 1;
    cur_x = cur_y = 0;
    fg = C_LGRAY; bg = C_BLACK;

    /* Paint the whole surface, including any margin the character grid does not
     * cover, so no firmware logo pixels are left behind. */
    u32 c = rgb(bg);
    for (u32 y = 0; y < fb_h; y++) {
        volatile u32 *px = (volatile u32 *)(fb_base + (u64)y * fb_pitch);
        for (u32 x = 0; x < fb_w; x++) px[x] = c;
    }
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) { cell_ch[y][x] = ' '; cell_fg[y][x] = fg; cell_bg[y][x] = bg; }

    cursor_drawn = false;
    cursor_draw();
}

void kprintf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    console_write(buf, (size_t)n);
}
