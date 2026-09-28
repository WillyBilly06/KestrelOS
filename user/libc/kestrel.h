/* kestrel.h - the KestrelOS user-space C library.
 *
 * One header covers the whole library: there is no hosted C environment here,
 * so the usual split across a dozen standard headers would only add noise.
 */
#ifndef KESTREL_USER_H
#define KESTREL_USER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#include "../../include/kestrel/syscall.h"
#include "../../include/kestrel/video.h"

typedef int64_t ssize_t;
typedef int64_t off_t;

/* ------------------------------------------------------------------ errors */

#define EPERM      1
#define ENOENT     2
#define EIO        5
#define EBADF      9
#define EAGAIN     11
#define ENOMEM     12
#define EACCES     13
#define EBUSY      16
#define EEXIST     17
#define EXDEV      18
#define ENODEV     19
#define ENOTDIR    20
#define EISDIR     21
#define EINVAL     22
#define EMFILE     24
#define ENOSPC     28
#define ESPIPE     29
#define EROFS      30
#define ENAMETOOLONG 36
#define ENOSYS     38
#define ENOTEMPTY  39

extern int errno;
const char *strerror(int err);

/* ------------------------------------------------------------------- files */

#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_CREAT    0x0040
#define O_TRUNC    0x0200
#define O_APPEND   0x0400
#define O_DIRECTORY 0x1000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* File types, matching the kernel's vnode types. */
#define FT_FILE 1
#define FT_DIR  2
#define FT_CHR  3
#define FT_BLK  4

int      open(const char *path, int flags);
int      close(int fd);
ssize_t  read(int fd, void *buf, size_t len);
ssize_t  write(int fd, const void *buf, size_t len);
off_t    lseek(int fd, off_t off, int whence);
int      stat(const char *path, kstat_t *st);
int      mkdir(const char *path);
int      unlink(const char *path);
int      rmdir(const char *path);
int      rename(const char *from, const char *to);
int      ftruncate(int fd, uint64_t size);
int      ioctl(int fd, unsigned cmd, void *arg);
int      sync(void);
bool     file_exists(const char *path);
int      chdir(const char *path);
char    *getcwd(char *buf, size_t cap);

/* Whole-file convenience. */
ssize_t  read_file(const char *path, void *buf, size_t cap);
int      write_file(const char *path, const void *buf, size_t len);
int      copy_file(const char *from, const char *to);

/* ------------------------------------------------------------- directories */

typedef struct {
    int      fd;
    uint32_t index;
} DIR;

DIR  *opendir(const char *path);
int   readdir(DIR *d, kdirent_t *out);
void  closedir(DIR *d);

/* ------------------------------------------------------------------ stdio */

int  putchar(int c);
int  puts(const char *s);
int  printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  fprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int  snprintf(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int  vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int  getchar(void);
/* Reads a line into buf without the trailing newline.  Returns the length, or
 * -1 at end of input. */
int  readline(char *buf, size_t cap);
void flush_output(void);

/* ------------------------------------------------------------------ memory */

void *malloc(size_t n);
void *calloc(size_t count, size_t size);
void *realloc(void *p, size_t n);
void  free(void *p);
void *sbrk(intptr_t delta);

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);

/* ----------------------------------------------------------------- strings */

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, size_t n);
size_t strlcpy(char *d, const char *s, size_t cap);
size_t strlcat(char *d, const char *s, size_t cap);
char  *strcat(char *d, const char *s);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *hay, const char *needle);
char  *strdup(const char *s);
char  *strtok_r(char *s, const char *sep, char **save);

long   strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
int    atoi(const char *s);

/* ---------------------------------------------------------------- processes */

int   getpid(void);
int   spawn(const char *path, const char *const argv[], int argc);
int   wait(int pid, int *status);
int   run(const char *path, const char *const argv[], int argc);   /* spawn + wait */
int   kill(int pid);
void  sleep_ms(uint64_t ms);
void  yield(void);
void  exit(int code) __attribute__((noreturn));
void  abort(void) __attribute__((noreturn));

/* --------------------------------------------------------------- threads */

/* Threads share everything with the process that made them - memory, open
 * files, the working directory - and have only registers and a stack of their
 * own.  `fn` runs on that stack; returning from it ends the thread. */
int  thread_create(void (*fn)(void *), void *arg);
void thread_exit(void) __attribute__((noreturn));
int  thread_id(void);
void thread_set_gs(void *base);

/* Sleeping on a word of memory.  `futex_wait` sleeps only while the word still
 * holds `expect`, so a wake racing with the decision to sleep is not lost;
 * pass -1 for no timeout.  Returns 0 when woken, -1 when it timed out or the
 * word had already changed. */
/* Where a fault in this process is delivered instead of killing it.  The
 * handler runs on the faulting thread's stack with the registers as they were;
 * returning from it is not defined, so it must exit or jump somewhere. */
void fault_handler(void (*fn)(kfault_t *));

/* Called from inside a fault handler that is about to resume the program
 * somewhere else, so the system knows the fault is over. */
void fault_handled(void);

int  futex_wait(volatile uint32_t *word, uint32_t expect, int timeout_ms);
int  futex_wake(volatile uint32_t *word, int count);

/* The display adapter, where this machine has a driver for one. */
int fb_update(int x, int y, int w, int h);

/* Draw triangles on the graphics card, onto the screen the display reads.
 * Returns how many were drawn, or a negative number where there is no card to
 * draw them - a caller has to be able to do it the slow way. */
int gpu_can_draw(void);
/* Inspect or hardware-decode a supported H.264 IDR. See video.h for limits. */
int gpu_video(kvideo_request_t *request);
int gpu_draw(const kvertex_t *corners, unsigned triangles);
/* Read one pixel from the live NVIDIA scanout, not the logical CPU shadow. */
int gpu_read_pixel(int x, int y, unsigned int *pixel);
/* A rectangle of pixels copied onto the screen by the card. */
int gpu_image(const kimage_t *what);
/* A program's own shaders, as its compiler produced them. */
int gpu_shaders(const kshaders_t *what);
/* How a program lays its own vertices out. */
int gpu_layout(const klayout_t *how);
int fb_set_mode(unsigned width, unsigned height);
/* Switch the multi-display arrangement (#7), dl_mode_t numbering: 0 only-primary,
 * 1 extend, 2 mirror, 3 only-secondary.  Returns 0 applied live, 1 saved for the
 * next start (extend needs a boot-time wide framebuffer), <0 on error. */
int fb_set_display_mode(int mode);
int fb_set_cursor(const kcursor_t *c);
int fb_move_cursor(int x, int y);

uint64_t uptime_ms(void);
/* Microseconds since start-up, from the processor's cycle counter - fine
 * enough to measure something that happens inside a single frame. */
uint64_t uptime_us(void);

/* The processor's cycle counter, straight.  Readable from user space, and the
 * only clock fine enough to time a single piece of drawing. */
static inline uint64_t read_cycles(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
uint64_t time_now(void);
void     format_time(char *buf, size_t cap, uint64_t unix_seconds);

/* ----------------------------------------------------------------- console */

void console_size(int *cols, int *rows);
void console_goto(int col, int row);
void console_where(int *col, int *row);
void console_clear(void);
void console_raw(bool on);
void console_cursor(bool visible);

/* ANSI helpers, so programs do not have to remember the codes. */
#define A_RESET   "\x1b[0m"
#define A_BOLD    "\x1b[1m"
#define A_REVERSE "\x1b[7m"
#define A_RED     "\x1b[31m"
#define A_GREEN   "\x1b[32m"
#define A_YELLOW  "\x1b[33m"
#define A_BLUE    "\x1b[34m"
#define A_MAGENTA "\x1b[35m"
#define A_CYAN    "\x1b[36m"
#define A_WHITE   "\x1b[37m"
#define A_GREY    "\x1b[90m"
#define A_BG_BLUE "\x1b[44m"

/* ------------------------------------------------------------------ system */

int  sysinfo(ksysinfo_t *out);
int  meminfo(uint64_t out[6]);
int  proclist(uint32_t index, kprocinfo_t *out);
int  enum_block(uint32_t index, kblockinfo_t *out);
int  enum_pci(uint32_t index, kpciinfo_t *out);
int  enum_gpu(uint32_t index, kgpuinfo_t *out);
int  enum_cpu(uint32_t index, kcpuinfo_t *out);
/* What the graphics driver read out of the card itself. */
int  gpu_detail(kgpudetail_t *out);
/* Drive the graphics driver against a model of a card.  Returns the number of
 * checks that failed. */
/* Start the card's own co-processor.  This is not a query: it takes the card
 * away from whatever is currently driving the screen, and if the card will not
 * accept the firmware the display does not come back until the machine is
 * power-cycled.  Nothing is damaged either way. */
/* Copy a rectangle of a back buffer onto the display, using every processor
 * the machine has.  Returns -1 when the system will not do it, in which case
 * the caller copies it itself. */
int  fb_present(const kpresent_t *what);

/* What the display adapter will do on the caller's behalf, as FB_ACCEL_* bits.
 * Zero means everything has to be drawn by the processor. */
int  fb_accel(void);
/* Both return 0 when the adapter did it and -1 when it declined, in which case
 * the caller has to do it itself. */
int  fb_fill(int x, int y, int w, int h, uint32_t colour);
int  fb_copy(int from_x, int from_y, int to_x, int to_y, int w, int h);

/* Bluetooth.  Scanning blocks for as long as it was asked to listen, and
 * connecting blocks until the link opens or the attempt gives up. */
int  bt_adapter(kbtadapter_t *out);
int  bt_scan(int seconds);
int  bt_found(int index, kbtdevice_t *out);
int  bt_connect(const unsigned char *address);
int  bt_disconnect(void);
int  bt_link(kbtlink_t *out);

int  gpu_start(void);
int  gpu_selftest(void);
int  enum_usb(uint32_t index, kusbinfo_t *out);
int  enum_videomode(uint32_t index, kvideomode_t *out);
int  enum_display(kdisplayinfo_t *out);
#include "../../include/kestrel/display_info.h"
int enum_display_output(uint32_t index, kdisplay_output_t *out);
int enum_display_output_mode(uint32_t output, uint32_t mode, kdisplay_mode_t *out);
int fb_check_configuration(const kdisplay_configuration_t *configuration,
                           kdisplay_configuration_check_t *out);
int fb_prepare_configuration(const kdisplay_configuration_t *configuration,
                             kdisplay_prepared_configuration_t *out);
int fb_cancel_configuration(uint64_t token);
int64_t fb_apply_output_mode(const kdisplay_output_mode_request_t *request);
int fb_finish_output_mode(uint64_t token,bool confirm);
int  enum_audio(kaudioinfo_t *out);
int  enum_audio_at(int index, kaudioinfo_t *out);
int  enum_net(uint32_t index, knetinfo_t *out);
int  enum_arp(uint32_t index, karpentry_t *out);

/* Networking.  Each of these blocks until it finishes or times out. */
int      net_dhcp(const char *iface, int timeout_ms);
int      net_set_address(const char *iface, uint32_t ip, uint32_t mask,
                         uint32_t gateway, uint32_t dns);
uint32_t net_resolve(const char *host, int timeout_ms);
int      net_ping(uint32_t ip, int count, int timeout_ms, uint32_t *times_ms);

/* Client-side TCP.  Each call blocks until it finishes or times out. */
int  tcp_connect(uint32_t ip, uint16_t port, int timeout_ms);
int  tcp_send(int handle, const void *data, int len, int timeout_ms);
int  tcp_recv(int handle, void *buf, int len, int timeout_ms);
void tcp_close(int handle);

/* The same connection, with nobody in between able to read or change it.
 * `host` is both the name sent to the server so it knows which certificate to
 * present, and the name its certificate is checked against. */
int  tls_connect(uint32_t ip, uint16_t port, const char *host, int timeout_ms);
int  tls_peer(int handle, ktlspeer_t *out);

/* Wireless. */
int  enum_wifi(uint32_t index, kwifiinfo_t *out);
int  enum_scan(uint32_t index, kwifinet_t *out);
int  enum_firmware(uint32_t index, kfirmware_t *out);
int  wifi_scan(const char *iface, int timeout_ms);
int  wifi_connect(const char *iface, const char *ssid, const char *passphrase,
                  int timeout_ms);
int  wifi_disconnect(const char *iface);
/* Switch a radio on or off.  Returns 1 if it ended up on, 0 if off, -1 on a
 * failure such as there being no such interface. */
int  wifi_enable(const char *iface, bool on);
int  wifi_is_enabled(const char *iface);
int  wifi_selftest(void);

void format_ipv4(uint32_t ip, char *buf, size_t cap);
bool parse_ipv4(const char *text, uint32_t *out);
void format_mac(const uint8_t *mac, char *buf, size_t cap);
int  blk_rescan(const char *device);
int  mount(const char *device, const char *path);
int  unmount(const char *path);
int  mountlist(char *buf, size_t cap);
void poweroff(void) __attribute__((noreturn));
void reboot(void) __attribute__((noreturn));

/* Event log. */
int      log_read(uint64_t from_seq, klog_record_t *out, int max);
int      log_write(int level, const char *subsys, const char *msg);
uint64_t log_next_seq(void);
int      log_counts(uint32_t out[5]);
int      log_clear(void);
int      log_flush(void);
uint64_t log_dropped(void);
int      log_console_level(int level);
const char *log_level_name(int level);

/* Pipes and descriptor duplication, which is how a program's output is
 * redirected into another program. */
int pipe(int fds[2]);
int dup(int fd);
int dup2(int oldfd, int newfd);

/* Raw entry point, for anything the wrappers do not cover. */
long syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5);

/* Human-readable byte counts, e.g. "1.4 GiB". */
void format_size(char *buf, size_t cap, uint64_t bytes);

#endif
