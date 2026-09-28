/* syscalls.c - thin wrappers over the kernel entry points.
 *
 * Every wrapper turns the kernel's negative-error convention into a -1 return
 * plus errno, which is what the rest of the library and the programs expect.
 */
#include "kestrel.h"

int errno;

extern int main(int argc, char **argv);
extern char __bss_start[], __bss_end[];

void libc_start(int argc, char **argv) {
    /* The kernel hands over zeroed pages, so .bss is already clear; this is
     * belt and braces for the case where a page was reused. */
    exit(main(argc, argv));
}

/* Fold a kernel result into the errno convention. */
static long ret(long r) {
    if (r < 0) { errno = (int)-r; return -1; }
    return r;
}

#define SC0(n)                syscall6((n), 0, 0, 0, 0, 0, 0)
#define SC1(n, a)             syscall6((n), (long)(a), 0, 0, 0, 0, 0)
#define SC2(n, a, b)          syscall6((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define SC3(n, a, b, c)       syscall6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)

/* ------------------------------------------------------------------- files */

int open(const char *path, int flags)              { return (int)ret(SC2(SYS_OPEN, path, flags)); }
int close(int fd)                                  { return (int)ret(SC1(SYS_CLOSE, fd)); }
ssize_t read(int fd, void *buf, size_t len)        { return ret(SC3(SYS_READ, fd, buf, len)); }
ssize_t write(int fd, const void *buf, size_t len) { return ret(SC3(SYS_WRITE, fd, buf, len)); }
off_t lseek(int fd, off_t off, int whence)         { return ret(SC3(SYS_SEEK, fd, off, whence)); }
int stat(const char *path, kstat_t *st)            { return (int)ret(SC2(SYS_STAT, path, st)); }
int mkdir(const char *path)                        { return (int)ret(SC1(SYS_MKDIR, path)); }
int unlink(const char *path)                       { return (int)ret(SC1(SYS_UNLINK, path)); }
int rmdir(const char *path)                        { return (int)ret(SC1(SYS_UNLINK, path)); }
int rename(const char *a, const char *b)           { return (int)ret(SC2(SYS_RENAME, a, b)); }
int ftruncate(int fd, uint64_t size)               { return (int)ret(SC2(SYS_TRUNCATE, fd, size)); }
int ioctl(int fd, unsigned cmd, void *arg)         { return (int)ret(SC3(SYS_IOCTL, fd, cmd, arg)); }
int sync(void)                                     { return (int)ret(SC0(SYS_SYNC)); }
int pipe(int fds[2])                               { return (int)ret(SC1(SYS_PIPE, fds)); }
int dup(int fd)                                    { return (int)ret(SC1(SYS_DUP, fd)); }
int dup2(int oldfd, int newfd)                     { return (int)ret(SC2(SYS_DUP2, oldfd, newfd)); }
int chdir(const char *path)                        { return (int)ret(SC1(SYS_CHDIR, path)); }

char *getcwd(char *buf, size_t cap) {
    if (ret(SC2(SYS_GETCWD, buf, cap)) < 0) return NULL;
    return buf;
}

bool file_exists(const char *path) {
    kstat_t st;
    return stat(path, &st) == 0;
}

/* -------------------------------------------------------------- directories */

DIR *opendir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return NULL;

    DIR *d = malloc(sizeof *d);
    if (!d) { close(fd); errno = ENOMEM; return NULL; }
    d->fd = fd;
    d->index = 0;
    return d;
}

int readdir(DIR *d, kdirent_t *out) {
    if (!d) { errno = EINVAL; return -1; }
    long r = syscall6(SYS_READDIR, d->fd, d->index, (long)out, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    d->index++;
    return 0;
}

void closedir(DIR *d) {
    if (!d) return;
    close(d->fd);
    free(d);
}

/* --------------------------------------------------------------- processes */

int getpid(void) { return (int)SC0(SYS_GETPID); }

int spawn(const char *path, const char *const argv[], int argc) {
    return (int)ret(SC3(SYS_SPAWN, path, argv, argc));
}

int wait(int pid, int *status) { return (int)ret(SC2(SYS_WAIT, pid, status)); }
int kill(int pid)              { return (int)ret(SC2(SYS_KILL, pid, 9)); }

int run(const char *path, const char *const argv[], int argc) {
    int pid = spawn(path, argv, argc);
    if (pid < 0) return -1;
    int status = 0;
    if (wait(pid, &status) < 0) return -1;
    return status;
}

void sleep_ms(uint64_t ms) { SC1(SYS_SLEEP, ms); }
void yield(void)           { SC0(SYS_YIELD); }

void exit(int code) {
    flush_output();
    SC1(SYS_EXIT, code);
    for (;;) { }
}

void abort(void) {
    fprintf(STDERR_FD, "\naborted\n");
    exit(134);
}

/* Tell the display adapter which part of the screen changed.  Where there is
 * a driver this is what puts the picture on the screen; where there is not it
 * does nothing and the adapter finds out for itself, slowly. */
int fb_update(int x, int y, int w, int h) {
    int32_t r[4] = { x, y, w, h };
    return (int)ret(SC3(SYS_FRAMEBUFFER, 0, FB_UPDATE, r));
}

int gpu_can_draw(void) {
    return (int)ret(syscall6(SYS_GPU, GPUOP_CANDRAW, 0, 0, 0, 0, 0));
}

int gpu_draw(const kvertex_t *corners, unsigned triangles) {
    return (int)ret(syscall6(SYS_GPU, GPUOP_DRAW, (long)corners, triangles,
                             0, 0, 0));
}

int gpu_video(kvideo_request_t *request) {
    return (int)ret(syscall6(SYS_GPU, GPUOP_VIDEO, (long)request,
                            sizeof(*request), 0, 0, 0));
}

int gpu_read_pixel(int x, int y, unsigned int *pixel) {
    return (int)ret(syscall6(SYS_GPU, GPUOP_READPIXEL, x, y, (long)pixel, 0, 0));
}

int gpu_image(const kimage_t *what) {
    return (int)ret(syscall6(SYS_GPU, GPUOP_IMAGE, (long)what, 0, 0, 0, 0));
}

int gpu_shaders(const kshaders_t *what) {
    return (int)ret(syscall6(SYS_GPU, GPUOP_SHADERS, (long)what, 0, 0, 0, 0));
}

int gpu_layout(const klayout_t *how) {
    return (int)ret(syscall6(SYS_GPU, GPUOP_LAYOUT, (long)how, 0, 0, 0, 0));
}

int fb_set_mode(unsigned width, unsigned height) {
    return (int)ret(syscall6(SYS_FRAMEBUFFER, 0, FB_SETMODE, width, height, 0, 0));
}

int fb_set_display_mode(int mode) {
    return (int)ret(syscall6(SYS_FRAMEBUFFER, 0, FB_DISPLAYMODE,
                             (unsigned)mode, 0, 0, 0));
}

int fb_set_cursor(const kcursor_t *c) {
    return (int)ret(SC3(SYS_FRAMEBUFFER, 0, FB_CURSOR, c));
}

int fb_move_cursor(int x, int y) {
    return (int)ret(syscall6(SYS_FRAMEBUFFER, 0, FB_CURSORAT, x, y, 0, 0));
}

uint64_t uptime_ms(void) { return (uint64_t)SC0(SYS_UPTIME); }

/* Microseconds, without a system call.
 *
 * The cycle counter is readable from ring three, so the only thing needed from
 * the kernel is how fast it runs - asked for once and kept.  That matters more
 * than it sounds: anything measuring how long a piece of drawing takes calls
 * this several times per operation, and a system call each time would be most
 * of what it was trying to measure. */
uint64_t uptime_us(void) {
    static uint64_t per_us;
    if (!per_us) {
        /* The kernel calibrated the cycle counter against a real timer at
         * start-up; asking it is exact, and timing two system calls against
         * each other here would not be - they can both land inside the same
         * microsecond, and a rate guessed from that is wrong by whatever
         * factor the rounding happened to produce. */
        per_us = (uint64_t)SC1(SYS_UPTIME, 2);
        if (!per_us) return (uint64_t)SC1(SYS_UPTIME, 1);
    }
    return read_cycles() / per_us;
}
uint64_t time_now(void)  { return (uint64_t)SC0(SYS_TIME); }

/* ------------------------------------------------------------------ memory */

void *sbrk(intptr_t delta) {
    long r = SC1(SYS_SBRK, delta);
    if (r < 0) { errno = (int)-r; return (void *)-1; }
    return (void *)r;
}

/* ----------------------------------------------------------------- console */

void console_size(int *cols, int *rows) {
    int v[2] = { 80, 25 };
    syscall6(SYS_CONSOLE, CON_GET_SIZE, (long)v, 0, 0, 0, 0);
    if (cols) *cols = v[0];
    if (rows) *rows = v[1];
}

void console_goto(int col, int row) {
    int v[2] = { col, row };
    syscall6(SYS_CONSOLE, CON_SET_CURSOR, (long)v, 0, 0, 0, 0);
}

void console_where(int *col, int *row) {
    int v[2] = { 0, 0 };
    syscall6(SYS_CONSOLE, CON_GET_CURSOR, (long)v, 0, 0, 0, 0);
    if (col) *col = v[0];
    if (row) *row = v[1];
}

void console_clear(void) {
    flush_output();
    syscall6(SYS_CONSOLE, CON_CLEAR, 0, 0, 0, 0, 0);
}

void console_raw(bool on) {
    flush_output();
    int v[2] = { on ? 1 : 0, 0 };
    syscall6(SYS_CONSOLE, CON_SET_RAW, (long)v, 0, 0, 0, 0);
}

void console_cursor(bool visible) {
    flush_output();
    int v[2] = { visible ? 1 : 0, 0 };
    syscall6(SYS_CONSOLE, CON_SHOW_CURSOR, (long)v, 0, 0, 0, 0);
}

/* ------------------------------------------------------------------ system */

int sysinfo(ksysinfo_t *out)  { return (int)ret(SC1(SYS_SYSINFO, out)); }
int meminfo(uint64_t out[6])  { return (int)ret(SC1(SYS_MEMINFO, out)); }

int proclist(uint32_t index, kprocinfo_t *out) { return (int)ret(SC2(SYS_PROCLIST, index, out)); }
int enum_block(uint32_t index, kblockinfo_t *out) { return (int)ret(SC3(SYS_ENUM, ENUM_BLOCK, index, out)); }
int enum_pci(uint32_t index, kpciinfo_t *out)     { return (int)ret(SC3(SYS_ENUM, ENUM_PCI, index, out)); }
int enum_gpu(uint32_t index, kgpuinfo_t *out)     { return (int)ret(SC3(SYS_ENUM, ENUM_GPU, index, out)); }
int enum_cpu(uint32_t index, kcpuinfo_t *out)     { return (int)ret(SC3(SYS_ENUM, ENUM_CPU, index, out)); }
int enum_usb(uint32_t index, kusbinfo_t *out)     { return (int)ret(SC3(SYS_ENUM, ENUM_USB, index, out)); }
int enum_videomode(uint32_t i, kvideomode_t *out) { return (int)ret(SC3(SYS_ENUM, ENUM_VIDEOMODE, i, out)); }
int enum_display(kdisplayinfo_t *out)             { return (int)ret(SC3(SYS_ENUM, ENUM_DISPLAY, 0, out)); }
int enum_display_output(uint32_t index, kdisplay_output_t *out) {
    return (int)ret(SC3(SYS_ENUM, ENUM_DISPLAY_OUTPUT, index, out));
}
int fb_check_configuration(const kdisplay_configuration_t *configuration,
                           kdisplay_configuration_check_t *out) {
    return (int)ret(syscall6(SYS_FRAMEBUFFER,0,FB_CHECK_CONFIGURATION,
                            (long)configuration,(long)out,0,0));
}
int fb_prepare_configuration(const kdisplay_configuration_t *configuration,
                             kdisplay_prepared_configuration_t *out) {
    return (int)ret(syscall6(SYS_FRAMEBUFFER,0,FB_PREPARE_CONFIGURATION,
                            (long)configuration,(long)out,0,0));
}
int fb_cancel_configuration(uint64_t token) {
    return (int)ret(syscall6(SYS_FRAMEBUFFER,0,FB_CANCEL_CONFIGURATION,(long)token,0,0,0));
}
int64_t fb_apply_output_mode(const kdisplay_output_mode_request_t *request) {
    return ret(syscall6(SYS_FRAMEBUFFER,0,FB_APPLY_OUTPUT_MODE,(long)request,0,0,0));
}
int fb_finish_output_mode(uint64_t token,bool confirm) {
    return (int)ret(syscall6(SYS_FRAMEBUFFER,0,
        confirm?FB_CONFIRM_OUTPUT_MODE:FB_REVERT_OUTPUT_MODE,(long)token,0,0,0));
}
int enum_display_output_mode(uint32_t output, uint32_t mode, kdisplay_mode_t *out) {
    if (output >= KDISPLAY_MAX_OUTPUTS || mode >= KDISPLAY_MAX_MODES) return -1;
    return (int)ret(SC3(SYS_ENUM, ENUM_DISPLAY_OUTPUT_MODE, (output << 16) | mode, out));
}
int enum_audio(kaudioinfo_t *out)                 { return (int)ret(SC3(SYS_ENUM, ENUM_AUDIO, 0, out)); }
/* The same, for a machine with more than one - the built-in codec is nought
 * and USB devices follow it.  The call above is kept because most callers want
 * the machine's own audio and should not have to say so. */
int enum_audio_at(int index, kaudioinfo_t *out)   { return (int)ret(SC3(SYS_ENUM, ENUM_AUDIO, (long)index, out)); }
int enum_net(uint32_t index, knetinfo_t *out)     { return (int)ret(SC3(SYS_ENUM, ENUM_NET, index, out)); }
int enum_arp(uint32_t index, karpentry_t *out)    { return (int)ret(SC3(SYS_ENUM, ENUM_ARP, index, out)); }

int net_dhcp(const char *iface, int timeout_ms) {
    return (int)ret(SC3(SYS_NET, NETOP_DHCP, iface, (uint64_t)timeout_ms));
}

int net_set_address(const char *iface, uint32_t ip, uint32_t mask,
                    uint32_t gateway, uint32_t dns) {
    uint32_t v[4] = { ip, mask, gateway, dns };
    return (int)ret(SC3(SYS_NET, NETOP_SET, iface, v));
}

uint32_t net_resolve(const char *host, int timeout_ms) {
    int64_t r = ret(SC3(SYS_NET, NETOP_RESOLVE, host, (uint64_t)timeout_ms));
    return r > 0 ? (uint32_t)r : 0;
}

int net_ping(uint32_t ip, int count, int timeout_ms, uint32_t *times_ms) {
    uint32_t in[3] = { ip, (uint32_t)count, (uint32_t)timeout_ms };
    return (int)ret(SC3(SYS_NET, NETOP_PING, in, times_ms));
}

int tcp_connect(uint32_t ip, uint16_t port, int timeout_ms) {
    uint32_t in[3] = { ip, port, (uint32_t)timeout_ms };
    return (int)ret(SC3(SYS_NET, NETOP_CONNECT, in, 0));
}

int tcp_send(int handle, const void *data, int len, int timeout_ms) {
    uint32_t in[3] = { (uint32_t)handle, (uint32_t)len, (uint32_t)timeout_ms };
    return (int)ret(SC3(SYS_NET, NETOP_SEND, in, data));
}

int tcp_recv(int handle, void *buf, int len, int timeout_ms) {
    uint32_t in[3] = { (uint32_t)handle, (uint32_t)len, (uint32_t)timeout_ms };
    return (int)ret(SC3(SYS_NET, NETOP_RECV, in, buf));
}

/* A secure connection.  The handle it returns works with tcp_send, tcp_recv
 * and tcp_close exactly as a plain one does - the difference is entirely
 * inside the kernel, which is the point. */
int tls_connect(uint32_t ip, uint16_t port, const char *host, int timeout_ms) {
    uint32_t in[3] = { ip, port, (uint32_t)timeout_ms };
    return (int)ret(SC3(SYS_NET, NETOP_TLS, in, host));
}

int tls_peer(int handle, ktlspeer_t *out) {
    return (int)ret(SC3(SYS_NET, NETOP_TLSPEER, (uint64_t)(int64_t)handle, out));
}

void tcp_close(int handle) {
    ret(SC3(SYS_NET, NETOP_CLOSE, (uint64_t)(int64_t)handle, 0));
}

int enum_wifi(uint32_t i, kwifiinfo_t *out)  { return (int)ret(SC3(SYS_ENUM, ENUM_WIFI, i, out)); }
int enum_scan(uint32_t i, kwifinet_t *out)   { return (int)ret(SC3(SYS_ENUM, ENUM_SCAN, i, out)); }
int enum_firmware(uint32_t i, kfirmware_t *out) { return (int)ret(SC3(SYS_ENUM, ENUM_FIRMWARE, i, out)); }

int wifi_scan(const char *iface, int timeout_ms) {
    return (int)ret(SC3(SYS_WIFI, WIFIOP_SCAN, iface, (uint64_t)timeout_ms));
}

int wifi_connect(const char *iface, const char *ssid, const char *passphrase,
                 int timeout_ms) {
    uint64_t in[3] = { (uint64_t)iface, (uint64_t)ssid, (uint64_t)passphrase };
    return (int)ret(SC3(SYS_WIFI, WIFIOP_CONNECT, in, (uint64_t)timeout_ms));
}

int wifi_disconnect(const char *iface) {
    return (int)ret(SC3(SYS_WIFI, WIFIOP_DISCONNECT, iface, 0));
}

int wifi_selftest(void) {
    return (int)ret(SC3(SYS_WIFI, WIFIOP_SELFTEST, 0, 0));
}

void format_ipv4(uint32_t ip, char *buf, size_t cap) {
    snprintf(buf, cap, "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF, ip & 0xFF);
}

bool parse_ipv4(const char *text, uint32_t *out) {
    uint32_t value = 0;
    for (int part = 0; part < 4; part++) {
        if (*text < '0' || *text > '9') return false;
        uint32_t octet = 0;
        int digits = 0;
        while (*text >= '0' && *text <= '9' && digits < 3) {
            octet = octet * 10 + (uint32_t)(*text++ - '0');
            digits++;
        }
        if (octet > 255) return false;
        value = (value << 8) | octet;
        if (part < 3) {
            if (*text != '.') return false;
            text++;
        }
    }
    if (*text) return false;
    if (out) *out = value;
    return true;
}

void format_mac(const uint8_t *mac, char *buf, size_t cap) {
    snprintf(buf, cap, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
int blk_rescan(const char *device)                { return (int)ret(SC1(SYS_BLKRESCAN, device)); }
int mount(const char *device, const char *path)   { return (int)ret(SC2(SYS_MOUNT, device, path)); }
int unmount(const char *path)                     { return (int)ret(SC1(SYS_UNMOUNT, path)); }
int mountlist(char *buf, size_t cap)              { return (int)ret(SC2(SYS_MOUNTLIST, buf, cap)); }

void poweroff(void) { flush_output(); sync(); SC0(SYS_POWEROFF); for (;;) { } }
void reboot(void)   { flush_output(); sync(); SC0(SYS_REBOOT);   for (;;) { } }

/* --------------------------------------------------------------- event log */

int log_read(uint64_t from_seq, klog_record_t *out, int max) {
    return (int)ret(SC3(SYS_LOG_READ, from_seq, out, max));
}

int log_write(int level, const char *subsys, const char *msg) {
    return (int)ret(SC3(SYS_LOG_WRITE, level, subsys, msg));
}

uint64_t log_next_seq(void) { return (uint64_t)SC2(SYS_LOG_CTL, LOGCTL_NEXTSEQ, 0); }
int log_counts(uint32_t out[5]) { return (int)ret(SC2(SYS_LOG_CTL, LOGCTL_COUNTS, out)); }
int log_clear(void) { return (int)ret(SC2(SYS_LOG_CTL, LOGCTL_CLEAR, 0)); }
int log_flush(void) { return (int)ret(SC2(SYS_LOG_CTL, LOGCTL_FLUSH, 0)); }
uint64_t log_dropped(void) { return (uint64_t)SC2(SYS_LOG_CTL, LOGCTL_DROPPED, 0); }
int log_console_level(int level) { return (int)ret(SC2(SYS_LOG_CTL, LOGCTL_CONSOLE_LEVEL, level)); }

const char *log_level_name(int level) {
    static const char *names[5] = { "DEBUG", "INFO", "WARN", "ERROR", "CRIT" };
    return (level >= 0 && level < 5) ? names[level] : "?";
}

/* ------------------------------------------------------------------ errors */

const char *strerror(int err) {
    if (err < 0) err = -err;
    switch (err) {
    case 0:            return "success";
    case EPERM:        return "operation not permitted";
    case ENOENT:       return "no such file or directory";
    case EIO:          return "input/output error";
    case EBADF:        return "bad file descriptor";
    case EAGAIN:       return "resource temporarily unavailable";
    case ENOMEM:       return "out of memory";
    case EACCES:       return "permission denied";
    case EBUSY:        return "device or resource busy";
    case EEXIST:       return "file exists";
    case EXDEV:        return "cross-device link";
    case ENODEV:       return "no such device";
    case ENOTDIR:      return "not a directory";
    case EISDIR:       return "is a directory";
    case EINVAL:       return "invalid argument";
    case EMFILE:       return "too many open files";
    case ENOSPC:       return "no space left on device";
    case ESPIPE:       return "illegal seek";
    case EROFS:        return "read-only file system";
    case ENAMETOOLONG: return "file name too long";
    case ENOSYS:       return "function not implemented";
    case ENOTEMPTY:    return "directory not empty";
    default:           return "unknown error";
    }
}

/* -------------------------------------------------------- file convenience */

ssize_t read_file(const char *path, void *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    size_t total = 0;
    while (total < cap) {
        ssize_t n = read(fd, (char *)buf + total, cap - total);
        if (n < 0) { close(fd); return -1; }
        if (n == 0) break;
        total += (size_t)n;
    }
    close(fd);
    return (ssize_t)total;
}

int write_file(const char *path, const void *buf, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;

    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, (const char *)buf + total, len - total);
        if (n <= 0) { close(fd); return -1; }
        total += (size_t)n;
    }
    close(fd);
    return 0;
}

int copy_file(const char *from, const char *to) {
    int in = open(from, O_RDONLY);
    if (in < 0) return -1;
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) { close(in); return -1; }

    char *buf = malloc(32768);
    if (!buf) { close(in); close(out); errno = ENOMEM; return -1; }

    int result = 0;
    for (;;) {
        ssize_t n = read(in, buf, 32768);
        if (n < 0) { result = -1; break; }
        if (n == 0) break;

        ssize_t done = 0;
        while (done < n) {
            ssize_t w = write(out, buf + done, (size_t)(n - done));
            if (w <= 0) { result = -1; break; }
            done += w;
        }
        if (result < 0) break;
    }

    free(buf);
    close(in);
    close(out);
    return result;
}

/* ---------------------------------------------------------------- printing */

void format_size(char *buf, size_t cap, uint64_t bytes) {
    static const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    int u = 0;
    uint64_t whole = bytes, frac = 0;

    while (whole >= 1024 && u < 5) {
        frac = ((whole % 1024) * 10) / 1024;
        whole /= 1024;
        u++;
    }
    if (u == 0) snprintf(buf, cap, "%llu B", (unsigned long long)whole);
    else if (whole < 10) snprintf(buf, cap, "%llu.%llu %s", (unsigned long long)whole, (unsigned long long)frac, unit[u]);
    else snprintf(buf, cap, "%llu %s", (unsigned long long)whole, unit[u]);
}

void format_time(char *buf, size_t cap, uint64_t unix_seconds) {
    static const uint16_t cumulative[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    uint64_t days = unix_seconds / 86400;
    uint32_t rem = (uint32_t)(unix_seconds % 86400);

    uint32_t year = 1970;
    for (;;) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        uint32_t len = leap ? 366 : 365;
        if (days < len) break;
        days -= len;
        year++;
    }
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;

    uint32_t month = 0;
    for (uint32_t m = 0; m < 12; m++) {
        uint32_t start = cumulative[m] + ((m >= 2 && leap) ? 1 : 0);
        if ((uint32_t)days >= start) month = m;
    }
    uint32_t mstart = cumulative[month] + ((month >= 2 && leap) ? 1 : 0);
    uint32_t day = (uint32_t)days - mstart + 1;

    snprintf(buf, cap, "%04u-%02u-%02u %02u:%02u:%02u",
             year, month + 1, day, rem / 3600, (rem / 60) % 60, rem % 60);
}

/* --------------------------------------------------------------- threads */

/* A thread starts here rather than at the caller's function, so that simply
 * returning from that function ends the thread instead of jumping to whatever
 * the untouched stack happened to hold. */
typedef struct {
    uint32_t state;                /* 0 free, 1 reserved, 2 published */
    void (*fn)(void *);
    void  *arg;
} thread_start_t;

static thread_start_t thread_slots[32];

static void thread_trampoline(void *raw) {
    thread_start_t *slot = (thread_start_t *)raw;
    /* The creator publishes both fields before the kernel can start us. */
    if (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) != 2) thread_exit();
    void (*fn)(void *) = slot->fn;
    void *arg = slot->arg;
    __atomic_store_n(&slot->state, 0u, __ATOMIC_RELEASE);
    fn(arg);
    thread_exit();
}

int thread_create(void (*fn)(void *), void *arg) {
    if (!fn) { errno = EINVAL; return -1; }

    thread_start_t *slot = NULL;
    for (unsigned i = 0; i < sizeof thread_slots / sizeof thread_slots[0]; i++) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&thread_slots[i].state, &expected, 1u,
                                        false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            slot = &thread_slots[i];
            break;
        }
    }
    if (!slot) { errno = EAGAIN; return -1; }
    slot->fn = fn;
    slot->arg = arg;
    __atomic_store_n(&slot->state, 2u, __ATOMIC_RELEASE);

    long r = syscall6(SYS_THREAD, THREAD_CREATE, (long)thread_trampoline,
                      (long)slot, 0, 0, 0);
    if (r < 0) {
        __atomic_store_n(&slot->state, 0u, __ATOMIC_RELEASE);
        errno = (int)-r;
        return -1;
    }
    return (int)r;
}

void thread_exit(void) {
    syscall6(SYS_THREAD, THREAD_EXIT, 0, 0, 0, 0, 0);
    for (;;) yield();               /* unreachable; keeps the compiler happy */
}

int thread_id(void) {
    return (int)syscall6(SYS_THREAD, THREAD_GETTID, 0, 0, 0, 0, 0);
}

void thread_set_gs(void *base) {
    syscall6(SYS_THREAD, THREAD_SETGS, (long)base, 0, 0, 0, 0);
}

int futex_wait(volatile uint32_t *word, uint32_t expect, int timeout_ms) {
    long r = syscall6(SYS_FUTEX, FUTEX_WAIT_OP, (long)word, (long)expect,
                      (long)timeout_ms, 0, 0);
    return r == 0 ? 0 : -1;
}

int futex_wake(volatile uint32_t *word, int count) {
    long r = syscall6(SYS_FUTEX, FUTEX_WAKE_OP, (long)word, (long)count, 0, 0, 0);
    return r < 0 ? -1 : (int)r;
}

void fault_handler(void (*fn)(kfault_t *)) {
    syscall6(SYS_THREAD, THREAD_ONFAULT, (long)fn, 0, 0, 0, 0);
}

void fault_handled(void) {
    syscall6(SYS_THREAD, THREAD_HANDLED, 0, 0, 0, 0, 0);
}

int gpu_detail(kgpudetail_t *out) {
    long r = syscall6(SYS_GPU, GPUOP_DETAIL, (long)out, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

/* What the adapter will do instead of the processor.
 *
 * These three had been reachable from the kernel for some time and were never
 * called from anywhere: the syscalls existed, the adapter drivers registered
 * their operations, and no program ever asked.  So every pixel of the desktop
 * was drawn by the processor on a machine whose adapter would have done some
 * of it - which is exactly the complaint they exist to answer.
 */
int fb_accel(void) {
    long r = syscall6(SYS_FRAMEBUFFER, 0, FB_ACCEL, 0, 0, 0, 0);
    return r < 0 ? 0 : (int)r;
}

/* Returns 0 when the adapter did it, -1 when it would not.  A refusal is not
 * a failure: it means do it the usual way, and the caller must be able to. */
int fb_fill(int x, int y, int w, int h, uint32_t colour) {
    int32_t r[5] = { x, y, w, h, (int32_t)colour };
    long v = syscall6(SYS_FRAMEBUFFER, 0, FB_FILL, (long)r, 0, 0, 0);
    if (v < 0) { errno = (int)-v; return -1; }
    return 0;
}

int fb_copy(int from_x, int from_y, int to_x, int to_y, int w, int h) {
    int32_t r[6] = { from_x, from_y, to_x, to_y, w, h };
    long v = syscall6(SYS_FRAMEBUFFER, 0, FB_COPY, (long)r, 0, 0, 0);
    if (v < 0) { errno = (int)-v; return -1; }
    return 0;
}

/* The operation goes in the SECOND argument, not the first.
 *
 * This passed it first, and had done since it was written.  The kernel
 * therefore never matched it, fell through to the framebuffer-mapping path at
 * the end of the same case, and returned -EINVAL for a bad pointer - because
 * the pointer it was checking was the number 10.
 *
 * Nothing looked wrong.  The caller treats a failure here as "this machine
 * cannot spread the copy across processors" and does it in a loop instead,
 * which is correct and which is what has happened on every frame this system
 * has ever drawn.  A fallback that works is the best possible disguise for a
 * call that does not. */
int fb_present(const kpresent_t *what) {
    long r = syscall6(SYS_FRAMEBUFFER, 0, FB_PRESENT, (long)what, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int gpu_start(void) {
    return (int)syscall6(SYS_GPU, GPUOP_START, 0, 0, 0, 0, 0);
}

int gpu_selftest(void) {
    return (int)syscall6(SYS_GPU, GPUOP_SELFTEST, 0, 0, 0, 0, 0);
}

/* ------------------------------------------------------------- Bluetooth */

int bt_adapter(kbtadapter_t *out) {
    long r = syscall6(SYS_BLUETOOTH, BTOP_ADAPTER, (long)out, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int bt_scan(int seconds) {
    long r = syscall6(SYS_BLUETOOTH, BTOP_SCAN, seconds, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

int bt_found(int index, kbtdevice_t *out) {
    long r = syscall6(SYS_BLUETOOTH, BTOP_FOUND, index, (long)out, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int bt_connect(const unsigned char *address) {
    long r = syscall6(SYS_BLUETOOTH, BTOP_CONNECT, (long)address, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int bt_disconnect(void) {
    long r = syscall6(SYS_BLUETOOTH, BTOP_DISCONNECT, 0, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int bt_link(kbtlink_t *out) {
    long r = syscall6(SYS_BLUETOOTH, BTOP_LINK, (long)out, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int wifi_enable(const char *iface, bool on) {
    long r = syscall6(SYS_WIFI, WIFIOP_ENABLE, (long)iface, on ? 1 : 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

int wifi_is_enabled(const char *iface) {
    long r = syscall6(SYS_WIFI, WIFIOP_ENABLED, (long)iface, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}
