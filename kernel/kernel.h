/* kernel.h - types, helpers and the cross-subsystem interfaces that most of the
 * kernel needs.  Subsystem-specific declarations live next to their driver. */
#ifndef KESTREL_KERNEL_H
#define KESTREL_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include "../include/kestrel/bootinfo.h"

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

#define PAGE_SIZE 4096UL
#define PAGE_MASK (PAGE_SIZE - 1)
#define PAGE_ALIGN_UP(x)   (((u64)(x) + PAGE_MASK) & ~PAGE_MASK)
#define PAGE_ALIGN_DOWN(x) ((u64)(x) & ~PAGE_MASK)

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define KERNEL_VMA KB_KERNEL_BASE
#define HHDM       KB_HHDM_BASE

/* Physical <-> kernel virtual through the loader's direct map. */
static inline void *phys_to_virt(u64 p) { return (void *)(HHDM + p); }
static inline u64   virt_to_phys(const void *v) { return (u64)v - HHDM; }

/* ------------------------------------------------------------------- i/o */
static inline void outb(u16 p, u8 v)  { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }

/* Where a PC has always written its progress during power-on self-test.  Most
 * desktop motherboards of the last decade show the last byte written to it on
 * a two-digit display beside the memory slots, and that display is the only
 * thing that still works on a machine whose screen has gone black: no monitor,
 * no serial cable, no keyboard, and nothing that can stop working when the
 * firmware is shut down.
 *
 * The loader uses E0 to EF and the kernel uses F0 to FE, so a number in either
 * range is unambiguously ours and says which half of the boot it stopped in.
 * FF is a panic, which means the machine got far enough to know it was in
 * trouble - a different thing from stopping without a word. */
#define POST_K_ENTERED   0xF0   /* the kernel is executing                   */
#define POST_K_CONSOLE   0xF1   /* it has drawn to the screen                */
#define POST_K_TABLES    0xF2   /* descriptor tables are installed           */
#define POST_K_MEMORY    0xF3   /* the memory managers are up                */
#define POST_K_STAGE2    0xF4   /* it is on its own stack                    */
#define POST_K_TIME      0xF5   /* interrupts and the timer are running      */
#define POST_K_ROOTFS    0xF6   /* the root filesystem is mounted            */
#define POST_K_PCI       0xF7   /* the bus has been walked                   */
#define POST_K_GPU       0xF8   /* the graphics drivers have run             */
#define POST_K_USERLAND  0xF9   /* the first program has been started        */
#define POST_K_PANIC     0xFF   /* it stopped, and said why                  */

static inline void post(u8 code) { outb(0x80, code); }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline void outl(u16 p, u32 v) { __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p)); }
static inline u8   inb(u16 p)  { u8 v;  __asm__ volatile("inb %1,%0"  : "=a"(v) : "Nd"(p)); return v; }
static inline u16  inw(u16 p)  { u16 v; __asm__ volatile("inw %1,%0"  : "=a"(v) : "Nd"(p)); return v; }
static inline u32  inl(u16 p)  { u32 v; __asm__ volatile("inl %1,%0"  : "=a"(v) : "Nd"(p)); return v; }
static inline void io_wait(void) { outb(0x80, 0); }

static inline void cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile("hlt"); }
static inline void pause_cpu(void) { __asm__ volatile("pause"); }

static inline u64 read_flags(void) {
    u64 f; __asm__ volatile("pushfq; popq %0" : "=r"(f) :: "memory"); return f;
}
/* Disable interrupts and report whether they had been enabled, so nested
 * critical sections restore rather than blindly re-enable. */
static inline bool irq_save(void) { u64 f = read_flags(); cli(); return (f & 0x200) != 0; }
static inline void irq_restore(bool on) { if (on) sti(); }

static inline u64 read_cr0(void) { u64 v; __asm__ volatile("movq %%cr0,%0" : "=r"(v)); return v; }
static inline void write_cr0(u64 v) { __asm__ volatile("movq %0,%%cr0" :: "r"(v) : "memory"); }
static inline u64 read_cr2(void) { u64 v; __asm__ volatile("movq %%cr2,%0" : "=r"(v)); return v; }
static inline u64 read_cr3(void) { u64 v; __asm__ volatile("movq %%cr3,%0" : "=r"(v)); return v; }
static inline void write_cr3(u64 v) { __asm__ volatile("movq %0,%%cr3" :: "r"(v) : "memory"); }
static inline u64 read_cr4(void) { u64 v; __asm__ volatile("movq %%cr4,%0" : "=r"(v)); return v; }
static inline void write_cr4(u64 v) { __asm__ volatile("movq %0,%%cr4" :: "r"(v) : "memory"); }

static inline u64 rdmsr(u32 msr) {
    u32 lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}
static inline void wrmsr(u32 msr, u64 v) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((u32)v), "d"((u32)(v >> 32)));
}
static inline u64 rdtsc(void) {
    u32 lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((u64)hi << 32) | lo;
}
static inline void cpuid_raw(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}
static inline void invlpg(u64 va) { __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory"); }

#define MSR_EFER        0xC0000080
#define MSR_STAR        0xC0000081
#define MSR_LSTAR       0xC0000082
#define MSR_SFMASK      0xC0000084
#define MSR_FS_BASE     0xC0000100
#define MSR_GS_BASE     0xC0000101
#define MSR_KGS_BASE    0xC0000102
#define MSR_APIC_BASE   0x0000001B

/* --------------------------------------------------------------- strings */
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, size_t n);
size_t strlcpy(char *d, const char *s, size_t cap);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);

/* ------------------------------------------------------------- formatting */
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int snprintf(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* ------------------------------------------------------------------- log */
/* Severity levels.  KLOG_WARN and above are what the persistent event log and
 * the `events` tool show by default. */
enum {
    KLOG_DEBUG = 0,
    KLOG_INFO  = 1,
    KLOG_WARN  = 2,
    KLOG_ERROR = 3,
    KLOG_CRIT  = 4,
};

void klog(int level, const char *subsys, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void klog_init(void);
void klog_set_console_level(int level);

#define kdebug(sub, ...) klog(KLOG_DEBUG, sub, __VA_ARGS__)
#define kinfo(sub, ...)  klog(KLOG_INFO,  sub, __VA_ARGS__)
#define kwarn(sub, ...)  klog(KLOG_WARN,  sub, __VA_ARGS__)
#define kerr(sub, ...)   klog(KLOG_ERROR, sub, __VA_ARGS__)
#define kcrit(sub, ...)  klog(KLOG_CRIT,  sub, __VA_ARGS__)

__attribute__((noreturn)) void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define KASSERT(c) do { if (!(c)) panic("assertion failed: %s (%s:%d)", #c, __FILE__, __LINE__); } while (0)

/* ---------------------------------------------------------------- serial */
void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s, size_t n);

/* ramlog.c - the kernel log kept in memory across a reboot, so that a boot
 * with no working disk still leaves a log the loader can write out. */
void ramlog_init(void);
void ramlog_write(const char *s, size_t n);
bool ramlog_active(void);
u32  ramlog_used(void);
u32  ramlog_capacity(void);
u32  ramlog_recovered(void);

/* --------------------------------------------------------------- console */
u64  console_framebuffer(u32 *width, u32 *height, u32 *pitch);
void console_init(const kboot_framebuffer *fb);
void console_putc(char c);
void console_write(const char *s, size_t n);
void console_clear(void);
void console_fill_rgb(u8 r, u8 g, u8 b);   /* solid-fill the firmware framebuffer */
void console_set_color(u8 fg, u8 bg);
void console_get_size(int *cols, int *rows);
void console_set_cursor(int col, int row);
void console_get_cursor(int *col, int *row);
void console_show_cursor(bool on);
void console_scroll_region(int top, int bottom);
void console_mirror_serial(bool on);
void console_remap_wc(void);

/* The console and a graphical program cannot both own the screen; these hand
 * it over and take it back. */
void console_release_framebuffer(void);
void console_take_framebuffer(void);
bool console_owns_framebuffer(void);
/* Permanently retire the loader's framebuffer after its GPU has been reset.
 * Unlike release/take ownership, this cannot be undone accidentally by a
 * process exit or a later framebuffer syscall. */
void console_abandon_framebuffer(void);
/* Publish a native driver's system-memory compositor surface after the
 * firmware framebuffer has been retired.  `phys` must name a contiguous,
 * page-aligned allocation that remains alive for the display session. */
void console_publish_framebuffer(u64 phys, u32 width, u32 height, u32 pitch);
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* A native multi-head modeset can change the logical desktop geometry after
 * input was initialized from the firmware's single-output framebuffer. */
void mouse_set_bounds(int width, int height);

/* CGA-style palette indices used by console_set_color. */
enum {
    C_BLACK = 0, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN, C_LGRAY,
    C_DGRAY, C_LBLUE, C_LGREEN, C_LCYAN, C_LRED, C_LMAGENTA, C_YELLOW, C_WHITE
};

/* ------------------------------------------------------------------ boot */
extern kboot_info g_boot;                    /* private copy owned by the kernel */
extern kboot_mmap_entry g_mmap[];
extern u32 g_mmap_count;
const char *cmdline_get(const char *key);    /* "key=value" lookup, NULL if absent */
bool cmdline_has(const char *flag);

#endif /* KESTREL_KERNEL_H */
