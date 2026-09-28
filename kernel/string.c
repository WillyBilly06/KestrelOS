/* string.c - the freestanding string and formatting routines the kernel needs.
 * clang lowers struct copies and array initialisation to memcpy/memset calls,
 * so these must exist even where the code never names them. */
#include "kernel.h"

void *memset(void *d, int c, size_t n) {
    u8 *p = d;
    u8 v = (u8)c;
    /* Fill to an 8-byte boundary, then a word at a time. */
    while (n && ((uintptr_t)p & 7)) { *p++ = v; n--; }
    u64 w = 0x0101010101010101ULL * v;
    while (n >= 8) { *(u64 *)p = w; p += 8; n -= 8; }
    while (n--) *p++ = v;
    return d;
}

void *memcpy(void *d, const void *s, size_t n) {
    u8 *a = d; const u8 *b = s;
    while (n && ((uintptr_t)a & 7)) { *a++ = *b++; n--; }
    while (n >= 8) { *(u64 *)a = *(const u64 *)b; a += 8; b += 8; n -= 8; }
    while (n--) *a++ = *b++;
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    u8 *a = d; const u8 *b = s;
    if (a == b || n == 0) return d;
    if (a < b) return memcpy(d, s, n);
    a += n; b += n;
    while (n--) *--a = *--b;
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const u8 *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

size_t strnlen(const char *s, size_t max) { size_t n = 0; while (n < max && s[n]) n++; return n; }

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(u8)*a - (int)(u8)*b;
}

static inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

int strcasecmp(const char *a, const char *b) {
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return (int)(u8)lower(*a) - (int)(u8)lower(*b);
}

char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) { } return r; }

char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

/* Truncating copy that always terminates; returns the length it wanted. */
size_t strlcpy(char *d, const char *s, size_t cap) {
    size_t n = strlen(s);
    if (cap) {
        size_t c = n < cap - 1 ? n : cap - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return n;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) return (char *)last;
    }
}

/* ------------------------------------------------------------------------- */
/* vsnprintf                                                                 */
/*                                                                           */
/* Supports: %c %s %d %i %u %x %X %o %p %% with the flags '-', '0', '+', ' ', */
/* a field width (or '*'), a precision, and the length modifiers l, ll, z, h. */
/* ------------------------------------------------------------------------- */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;   /* characters that would have been written */
} sbuf;

static void sb_putc(sbuf *s, char c) {
    if (s->len + 1 < s->cap) s->buf[s->len] = c;
    s->len++;
}

static void sb_pad(sbuf *s, char c, int n) { while (n-- > 0) sb_putc(s, c); }

static int u64_to_str(u64 v, unsigned base, bool upper, char *out) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[v % base]; v /= base; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    sbuf s = { buf, cap, 0 };

    while (*fmt) {
        if (*fmt != '%') { sb_putc(&s, *fmt++); continue; }
        fmt++;
        if (*fmt == '%') { sb_putc(&s, '%'); fmt++; continue; }

        bool left = false, zero = false, plus = false, space = false, alt = false;
        for (;;) {
            if (*fmt == '-')      left = true;
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

        int lng = 0;    /* 0 int, 1 long, 2 long long */
        bool sz = false, half = false;
        for (;;) {
            if (*fmt == 'l') { lng++; fmt++; }
            else if (*fmt == 'z') { sz = true; fmt++; }
            else if (*fmt == 'h') { half = true; fmt++; }
            else break;
        }

        char conv = *fmt++;
        char num[24];
        const char *body = num;
        int blen = 0;
        char sign = 0;
        const char *prefix = "";

        switch (conv) {
        case 'c': {
            num[0] = (char)va_arg(ap, int);
            num[1] = 0;
            blen = 1;
            break;
        }
        case 's': {
            const char *p = va_arg(ap, const char *);
            if (!p) p = "(null)";
            body = p;
            blen = prec >= 0 ? (int)strnlen(p, (size_t)prec) : (int)strlen(p);
            break;
        }
        case 'd': case 'i': {
            s64 v;
            if (lng >= 2 || sz) v = va_arg(ap, s64);
            else if (lng == 1)  v = va_arg(ap, long);
            else                v = va_arg(ap, int);
            if (half) v = (s16)v;
            u64 mag;
            if (v < 0) { sign = '-'; mag = (u64)(-(v + 1)) + 1; }   /* avoids overflow at INT64_MIN */
            else { mag = (u64)v; if (plus) sign = '+'; else if (space) sign = ' '; }
            blen = u64_to_str(mag, 10, false, num);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': case 'p': {
            u64 v;
            if (conv == 'p') { v = (u64)(uintptr_t)va_arg(ap, void *); prefix = "0x"; }
            else if (lng >= 2 || sz) v = va_arg(ap, u64);
            else if (lng == 1)       v = va_arg(ap, unsigned long);
            else                     v = va_arg(ap, unsigned int);
            if (half) v = (u16)v;
            unsigned base = (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : (conv == 'o' ? 8 : 10);
            if (alt && base == 16 && v) prefix = (conv == 'X') ? "0X" : "0x";
            blen = u64_to_str(v, base, conv == 'X', num);
            break;
        }
        default:
            sb_putc(&s, '%');
            if (conv) sb_putc(&s, conv);
            continue;
        }

        /* Integer precision means "at least this many digits". */
        int zeros = 0;
        if (conv != 's' && conv != 'c' && prec > blen) zeros = prec - blen;

        int plen = (int)strlen(prefix);
        int total = blen + zeros + plen + (sign ? 1 : 0);

        /* '0' padding is ignored when a precision is given or when left aligned. */
        if (zero && !left && prec < 0 && conv != 's' && conv != 'c') {
            zeros += width - total > 0 ? width - total : 0;
            total = blen + zeros + plen + (sign ? 1 : 0);
        }

        if (!left) sb_pad(&s, ' ', width - total);
        if (sign) sb_putc(&s, sign);
        for (int i = 0; i < plen; i++) sb_putc(&s, prefix[i]);
        sb_pad(&s, '0', zeros);
        for (int i = 0; i < blen; i++) sb_putc(&s, body[i]);
        if (left) sb_pad(&s, ' ', width - total);
    }

    if (s.cap) s.buf[s.len < s.cap ? s.len : s.cap - 1] = 0;
    return (int)s.len;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}
