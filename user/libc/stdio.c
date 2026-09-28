/* stdio.c - formatted output and line input.
 *
 * Output to the console is buffered, because every write() is a system call
 * and the console repaints per character; anything else makes a full-screen
 * redraw visibly slow.  The buffer is flushed on a newline, when it fills, and
 * before anything that reads input or leaves the program.
 */
#include "kestrel.h"

#define OUT_BUF 2048

static char out_buf[OUT_BUF];
static size_t out_len;
static int    out_fd = STDOUT_FD;

void flush_output(void) {
    if (!out_len) return;
    size_t done = 0;
    while (done < out_len) {
        ssize_t n = write(out_fd, out_buf + done, out_len - done);
        if (n <= 0) break;
        done += (size_t)n;
    }
    out_len = 0;
}

static void out_char(char c) {
    if (out_len >= OUT_BUF) flush_output();
    out_buf[out_len++] = c;
    if (c == '\n') flush_output();
}

static void out_str(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) out_char(s[i]);
}

/* ------------------------------------------------------------------ printf */

typedef struct {
    char  *buf;      /* NULL means "write to the console buffer" */
    size_t cap;
    size_t len;
} sink;

static void emit(sink *s, char c) {
    if (!s->buf) { out_char(c); s->len++; return; }
    if (s->len + 1 < s->cap) s->buf[s->len] = c;
    s->len++;
}

static void emit_pad(sink *s, char c, int n) { while (n-- > 0) emit(s, c); }

static int u64_str(uint64_t v, unsigned base, bool upper, char *out) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[v % base]; v /= base; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

static int format(sink *s, const char *fmt, va_list ap) {
    while (*fmt) {
        if (*fmt != '%') { emit(s, *fmt++); continue; }
        fmt++;
        if (*fmt == '%') { emit(s, '%'); fmt++; continue; }

        bool left = false, zero = false, plus = false, space = false, alt = false;
        for (;;) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else if (*fmt == '+') plus = true;
            else if (*fmt == ' ') space = true;
            else if (*fmt == '#') alt = true;
            else break;
            fmt++;
        }

        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; if (width < 0) { left = true; width = -width; } }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');

        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }

        int lng = 0;
        bool sz = false;
        for (;;) {
            if (*fmt == 'l') { lng++; fmt++; }
            else if (*fmt == 'z') { sz = true; fmt++; }
            else if (*fmt == 'h') { fmt++; }
            else break;
        }

        char conv = *fmt++;
        char num[24];
        const char *body = num;
        int blen = 0;
        char sign = 0;
        const char *prefix = "";

        switch (conv) {
        case 'c':
            num[0] = (char)va_arg(ap, int);
            num[1] = 0;
            blen = 1;
            break;

        case 's': {
            const char *p = va_arg(ap, const char *);
            if (!p) p = "(null)";
            body = p;
            blen = prec >= 0 ? (int)strnlen(p, (size_t)prec) : (int)strlen(p);
            break;
        }

        case 'd': case 'i': {
            int64_t v;
            if (lng >= 2 || sz) v = va_arg(ap, int64_t);
            else if (lng == 1)  v = va_arg(ap, long);
            else                v = va_arg(ap, int);
            uint64_t mag;
            if (v < 0) { sign = '-'; mag = (uint64_t)(-(v + 1)) + 1; }
            else { mag = (uint64_t)v; if (plus) sign = '+'; else if (space) sign = ' '; }
            blen = u64_str(mag, 10, false, num);
            break;
        }

        case 'u': case 'x': case 'X': case 'o': case 'p': {
            uint64_t v;
            if (conv == 'p') { v = (uint64_t)(uintptr_t)va_arg(ap, void *); prefix = "0x"; }
            else if (lng >= 2 || sz) v = va_arg(ap, uint64_t);
            else if (lng == 1)       v = va_arg(ap, unsigned long);
            else                     v = va_arg(ap, unsigned int);
            unsigned base = (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : (conv == 'o' ? 8 : 10);
            if (alt && base == 16 && v) prefix = (conv == 'X') ? "0X" : "0x";
            blen = u64_str(v, base, conv == 'X', num);
            break;
        }

        default:
            emit(s, '%');
            if (conv) emit(s, conv);
            continue;
        }

        int zeros = 0;
        if (conv != 's' && conv != 'c' && prec > blen) zeros = prec - blen;

        int plen = (int)strlen(prefix);
        int total = blen + zeros + plen + (sign ? 1 : 0);

        if (zero && !left && prec < 0 && conv != 's' && conv != 'c' && width > total) {
            zeros += width - total;
            total = width;
        }

        if (!left) emit_pad(s, ' ', width - total);
        if (sign) emit(s, sign);
        for (int i = 0; i < plen; i++) emit(s, prefix[i]);
        emit_pad(s, '0', zeros);
        for (int i = 0; i < blen; i++) emit(s, body[i]);
        if (left) emit_pad(s, ' ', width - total);
    }
    return (int)s->len;
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    sink s = { buf, cap, 0 };
    int n = format(&s, fmt, ap);
    if (cap) buf[s.len < cap ? s.len : cap - 1] = 0;
    return n;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...) {
    sink s = { NULL, 0, 0 };
    va_list ap;
    va_start(ap, fmt);
    int n = format(&s, fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(int fd, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    if (fd == out_fd) {
        sink s = { NULL, 0, 0 };
        int n = format(&s, fmt, ap);
        va_end(ap);
        return n;
    }

    /* A different descriptor: render into a buffer, then write it in one go. */
    char buf[1024];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return n;

    flush_output();          /* keep the ordering of the two streams sane */
    size_t len = (size_t)n < sizeof buf - 1 ? (size_t)n : sizeof buf - 1;
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, buf + done, len - done);
        if (w <= 0) break;
        done += (size_t)w;
    }
    return n;
}

int putchar(int c) { out_char((char)c); return c; }

int puts(const char *s) {
    out_str(s, strlen(s));
    out_char('\n');
    return 0;
}

/* ------------------------------------------------------------------- input */

int getchar(void) {
    flush_output();
    char c;
    ssize_t n = read(STDIN_FD, &c, 1);
    if (n <= 0) return -1;
    return (unsigned char)c;
}

int readline(char *buf, size_t cap) {
    flush_output();
    if (!cap) return -1;

    /* The console driver does the line editing, so one read returns one line. */
    ssize_t n = read(STDIN_FD, buf, cap - 1);
    if (n <= 0) return -1;

    size_t len = (size_t)n;
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) len--;
    buf[len] = 0;
    return (int)len;
}
