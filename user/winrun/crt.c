/* crt.c - the Microsoft C runtime.
 *
 * Every Windows program written in C reaches this library before it reaches
 * anything else: the startup code that runs before main is part of it, and so
 * is printf.  A program linked against the dynamic runtime imports a hundred
 * or so functions from msvcrt or ucrtbase, and if any of the ones it actually
 * calls is missing it dies before printing anything.
 *
 * The formatting is written out here rather than borrowed from this system's
 * own printf, because Microsoft's has to cope with what Windows code writes:
 * floating point, the I64 length modifier, and the S and hs conversions that
 * switch between narrow and wide strings mid-string.
 */
#include "win.h"

/* ------------------------------------------------- Microsoft's variable lists
 *
 * Under the Microsoft convention a variadic argument list is nothing but a
 * pointer into the caller's stack: the first four arguments arrive in
 * registers, but the caller has already reserved space above the return
 * address for them, so a callee that spills them there ends up with every
 * argument laid out contiguously.  Each slot is eight bytes whatever it holds,
 * and a floating-point argument is passed in the integer register as well as
 * the vector one, so reading the slot as bits and treating them as a double is
 * exactly right.
 *
 * This is written out rather than using the compiler's own va_start, which
 * refuses to appear in a function using a convention other than the target's.
 * The spilling is done by the small assembly stubs at the end of this file.
 */
typedef char *ms_va_list;

static uint64_t ms_next(ms_va_list *ap) {
    uint64_t v;
    memcpy(&v, *ap, 8);
    *ap += 8;
    return v;
}
static double ms_next_double(ms_va_list *ap) {
    uint64_t bits = ms_next(ap);
    double d;
    memcpy(&d, &bits, 8);
    return d;
}
#define MS_INT(ap)    ((int)(uint32_t)ms_next(&(ap)))
#define MS_UINT(ap)   ((unsigned)(uint32_t)ms_next(&(ap)))
#define MS_LONG(ap)   ((int64_t)ms_next(&(ap)))
#define MS_ULONG(ap)  (ms_next(&(ap)))
#define MS_PTR(ap, t) ((t)(uintptr_t)ms_next(&(ap)))
#define MS_DOUBLE(ap) (ms_next_double(&(ap)))

/* --------------------------------------------------------------- formatting */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    int    fd;            /* -1 when writing into buf */
} outsink;

static void put(outsink *s, char c) {
    if (s->fd >= 0) {
        char one = c;
        write(s->fd, &one, 1);
        s->len++;
        return;
    }
    if (s->buf && s->len + 1 < s->cap) s->buf[s->len] = c;
    s->len++;
}

static void puts_n(outsink *s, const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) put(s, p[i]);
}

static void pad(outsink *s, char c, int n) { while (n-- > 0) put(s, c); }

/* Ten to the power of n, for the fractional part.  A table beats repeated
 * multiplication both for speed and because it does not accumulate error. */
static double pow10i(int n) {
    static const double t[] = {
        1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9,
        1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18,
    };
    if (n < 0) return 1.0;
    if (n < (int)(sizeof t / sizeof t[0])) return t[n];
    double v = 1e18;
    for (int i = 18; i < n; i++) v *= 10.0;
    return v;
}

static bool is_nan(double v) { return v != v; }
static bool is_inf(double v) { return v != 0 && v * 0.5 == v; }

/* A decimal fraction, written out digit by digit.  The integer part is
 * produced first so that a large value does not lose its low digits, which is
 * what happens if the whole number is scaled at once. */
static void format_fixed(outsink *s, double v, int prec, bool upper) {
    if (is_nan(v)) { puts_n(s, upper ? "NAN" : "nan", 3); return; }
    if (v < 0) { put(s, '-'); v = -v; }
    if (is_inf(v)) { puts_n(s, upper ? "INF" : "inf", 3); return; }

    /* Round once, up front, so that carrying out of the fraction bumps the
     * integer part rather than printing "1.000" beside an integer of 0. */
    double scale = pow10i(prec);
    double rounded = v * scale + 0.5;
    if (rounded >= 1.8446744073709552e19) {           /* beyond 64 bits */
        puts_n(s, "1e+19", 5);
        return;
    }
    uint64_t all = (uint64_t)rounded;
    uint64_t div = (uint64_t)scale;
    uint64_t whole = all / div;
    uint64_t frac = all % div;

    char digits[24];
    int n = 0;
    if (!whole) digits[n++] = '0';
    while (whole && n < (int)sizeof digits) { digits[n++] = (char)('0' + whole % 10); whole /= 10; }
    while (n) put(s, digits[--n]);

    if (prec > 0) {
        put(s, '.');
        char f[24];
        int fn = 0;
        for (int i = 0; i < prec && fn < (int)sizeof f; i++) { f[fn++] = (char)('0' + frac % 10); frac /= 10; }
        while (fn) put(s, f[--fn]);
    }
}

static void format_exponent(outsink *s, double v, int prec, bool upper) {
    if (is_nan(v)) { puts_n(s, upper ? "NAN" : "nan", 3); return; }
    if (v < 0) { put(s, '-'); v = -v; }
    if (is_inf(v)) { puts_n(s, upper ? "INF" : "inf", 3); return; }

    int exp = 0;
    if (v != 0) {
        while (v >= 10.0) { v /= 10.0; exp++; }
        while (v < 1.0)   { v *= 10.0; exp--; }
    }
    format_fixed(s, v, prec, upper);
    put(s, upper ? 'E' : 'e');
    put(s, exp < 0 ? '-' : '+');
    if (exp < 0) exp = -exp;
    /* Two digits, or three when it needs them: what every C library has
     * printed since the standard settled it. */
    if (exp >= 100) put(s, (char)('0' + (exp / 100) % 10));
    put(s, (char)('0' + (exp / 10) % 10));
    put(s, (char)('0' + exp % 10));
}

/* %g drops the trailing zeros of whatever it produced, and the point with
 * them if nothing is left after it.  Trimming afterwards is far simpler than
 * working out in advance how many digits will survive. */
static void trim_trailing_zeros(char *body, int *len) {
    int n = *len;
    int point = -1, exponent = -1;
    for (int i = 0; i < n; i++) {
        if (body[i] == '.') point = i;
        else if (body[i] == 'e' || body[i] == 'E') { exponent = i; break; }
    }
    if (point < 0) return;

    int last = (exponent >= 0 ? exponent : n) - 1;
    while (last > point && body[last] == '0') last--;
    if (last == point) last--;                    /* the point goes too */

    if (exponent >= 0) {
        int tail = n - exponent;
        memmove(body + last + 1, body + exponent, (size_t)tail);
        *len = last + 1 + tail;
    } else {
        *len = last + 1;
    }
}

static int crt_format(outsink *s, const char *fmt, ms_va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(s, *fmt); continue; }
        fmt++;
        if (*fmt == '%') { put(s, '%'); continue; }

        bool left = false, zero = false, plus = false, space = false, alt = false;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else if (*fmt == '+') plus = true;
            else if (*fmt == ' ') space = true;
            else if (*fmt == '#') alt = true;
            else break;
        }

        int width = 0;
        if (*fmt == '*') { width = MS_INT(ap); fmt++; if (width < 0) { left = true; width = -width; } }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');

        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = MS_INT(ap); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }

        /* Length: h, hh, l, ll, L, z, t, j, and Microsoft's I64 and I32. */
        int lng = 0;          /* 0 int, 1 long, 2 long long, -1 short, -2 char */
        bool wide_text = false, narrow_text = false;
        for (;;) {
            if (*fmt == 'l') { fmt++; if (*fmt == 'l') { fmt++; lng = 2; } else { lng = 1; wide_text = true; } continue; }
            if (*fmt == 'h') { fmt++; if (*fmt == 'h') { fmt++; lng = -2; } else { lng = -1; narrow_text = true; } continue; }
            if (*fmt == 'L' || *fmt == 'j') { fmt++; lng = 2; continue; }
            if (*fmt == 'z' || *fmt == 't') { fmt++; lng = 2; continue; }
            if (*fmt == 'w') { fmt++; wide_text = true; continue; }
            if (*fmt == 'I') {
                if (fmt[1] == '6' && fmt[2] == '4') { fmt += 3; lng = 2; continue; }
                if (fmt[1] == '3' && fmt[2] == '2') { fmt += 3; lng = 0; continue; }
                fmt++; lng = 2; continue;               /* plain I is pointer-sized */
            }
            break;
        }

        char conv = *fmt;
        char body[512];
        int  blen = 0;
        char sign = 0;
        const char *prefix = "";

        switch (conv) {
        case 'c': {
            /* %C in a narrow string means wide, and vice versa; the length
             * modifiers say the same thing another way. */
            bool as_wide = (conv == 'C') || wide_text;
            (void)as_wide;
            int c = MS_INT(ap);
            body[blen++] = (char)c;
            break;
        }
        case 'C': {
            int c = MS_INT(ap);
            body[blen++] = (char)(c < 0x80 ? c : '?');
            break;
        }
        case 's': case 'S': {
            bool as_wide = (conv == 'S') ? !wide_text : wide_text;
            if (narrow_text) as_wide = false;
            if (as_wide) {
                const WCHAR *w = MS_PTR(ap, const WCHAR *);
                if (!w) { strlcpy(body, "(null)", sizeof body); blen = 6; break; }
                while (w[blen] && blen < (int)sizeof body - 1 && (prec < 0 || blen < prec))
                    { body[blen] = (char)(w[blen] < 0x80 ? w[blen] : '?'); blen++; }
            } else {
                const char *p = MS_PTR(ap, const char *);
                if (!p) { strlcpy(body, "(null)", sizeof body); blen = 6; break; }
                while (p[blen] && blen < (int)sizeof body - 1 && (prec < 0 || blen < prec))
                    { body[blen] = p[blen]; blen++; }
            }
            break;
        }
        case 'd': case 'i': {
            int64_t v;
            if (lng == 2 || lng == 1) v = MS_LONG(ap);
            else if (lng == -1) v = (short)MS_INT(ap);
            else if (lng == -2) v = (signed char)MS_INT(ap);
            else v = MS_INT(ap);
            uint64_t mag = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
            sign = v < 0 ? '-' : (plus ? '+' : (space ? ' ' : 0));
            char tmp[24]; int n = 0;
            if (!mag) tmp[n++] = '0';
            while (mag) { tmp[n++] = (char)('0' + mag % 10); mag /= 10; }
            while (n) body[blen++] = tmp[--n];
            break;
        }
        case 'u': case 'x': case 'X': case 'o': case 'p': {
            uint64_t v;
            if (conv == 'p') v = MS_ULONG(ap);
            else if (lng == 2 || lng == 1) v = MS_ULONG(ap);
            else if (lng == -1) v = (unsigned short)MS_UINT(ap);
            else if (lng == -2) v = (unsigned char)MS_UINT(ap);
            else v = MS_UINT(ap);

            unsigned base = (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : (conv == 'o' ? 8 : 10);
            const char *set = (conv == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            if (alt && base == 16 && v) prefix = (conv == 'X') ? "0X" : "0x";
            if (conv == 'p') { prefix = "0x"; if (prec < 0) prec = 16; }
            char tmp[24]; int n = 0;
            if (!v) tmp[n++] = '0';
            while (v) { tmp[n++] = set[v % base]; v /= base; }
            while (n) body[blen++] = tmp[--n];
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double v = MS_DOUBLE(ap);
            if (prec < 0) prec = 6;
            outsink into = { body, sizeof body, 0, -1 };
            if (conv == 'f' || conv == 'F') {
                format_fixed(&into, v, prec, conv == 'F');
            } else if (conv == 'e' || conv == 'E') {
                format_exponent(&into, v, prec, conv == 'E');
            } else {
                /* %g picks whichever of the two is shorter, which in practice
                 * means fixed unless the exponent is far from zero. */
                double a = v < 0 ? -v : v;
                if (a != 0 && (a < 1e-4 || a >= pow10i(prec ? prec : 1))) {
                    format_exponent(&into, v, prec ? prec - 1 : 0, conv == 'G');
                } else {
                    /* The precision of %g counts significant digits, not the
                     * ones after the point, so the fraction gets whatever is
                     * left after the integer part has taken its share. */
                    int whole = 1;
                    for (double t = a; t >= 10.0; t /= 10.0) whole++;
                    int after = (prec ? prec : 1) - whole;
                    format_fixed(&into, v, after < 0 ? 0 : after, conv == 'G');
                }
            }
            blen = (int)(into.len < sizeof body ? into.len : sizeof body - 1);
            if (conv == 'g' || conv == 'G') trim_trailing_zeros(body, &blen);
            if (v >= 0 && (plus || space)) sign = plus ? '+' : ' ';
            prec = -1;                          /* already applied */
            break;
        }
        case 'n': {
            int *out = MS_PTR(ap, int *);
            if (out) *out = (int)s->len;
            continue;
        }
        default:
            put(s, '%');
            if (conv) put(s, conv);
            continue;
        }

        /* Zero padding for the integer conversions is really a minimum number
         * of digits, which is what a precision means for them too. */
        int digits = blen;
        int zeros = 0;
        if (conv != 's' && conv != 'S' && conv != 'c' && conv != 'C' && prec > digits)
            zeros = prec - digits;

        int total = blen + zeros + (sign ? 1 : 0) + (int)strlen(prefix);
        if (zero && !left && prec < 0 && conv != 's' && conv != 'S' && conv != 'c' && width > total) {
            zeros += width - total;
            total = width;
        }

        if (!left) pad(s, ' ', width - total);
        if (sign) put(s, sign);
        puts_n(s, prefix, strlen(prefix));
        pad(s, '0', zeros);
        puts_n(s, body, (size_t)blen);
        if (left) pad(s, ' ', width - total);
    }
    if (s->fd < 0 && s->buf && s->cap) s->buf[s->len < s->cap ? s->len : s->cap - 1] = 0;
    return (int)s->len;
}

/* ------------------------------------------------------------- FILE streams */

/* Microsoft's FILE is opaque and programs only ever hold pointers to it, so
 * this one can be whatever is convenient. */
typedef struct {
    int  fd;
    bool eof, err, used;
} CRTFILE;

#define MAX_STREAMS 24
static CRTFILE streams[MAX_STREAMS];
static CRTFILE std_streams[3];

static CRTFILE *stream_for(int fd) {
    CRTFILE *s = &std_streams[fd];
    s->fd = fd;
    s->used = true;
    return s;
}

/* The array a program reaches through stdin/stdout/stderr, which in
 * Microsoft's headers are macros over an exported array. */
static CRTFILE *WINAPI w___iob_func(void) {
    for (int i = 0; i < 3; i++) { std_streams[i].fd = i; std_streams[i].used = true; }
    return std_streams;
}
static CRTFILE *WINAPI w___acrt_iob_func(unsigned which) {
    if (which > 2) which = 2;
    return stream_for((int)which);
}

static CRTFILE *WINAPI m_fopen(const char *name, const char *mode) {
    if (!name || !mode) return NULL;
    char host[512];
    win_path_to_host(name, host, sizeof host);

    int flags = O_RDONLY;
    if (strchr(mode, 'w')) flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (strchr(mode, 'a')) flags = O_WRONLY | O_CREAT | O_APPEND;
    else if (strchr(mode, '+')) flags = O_RDWR;
    if (strchr(mode, '+') && (strchr(mode, 'w') || strchr(mode, 'a'))) flags |= O_RDWR;

    int fd = open(host, flags);
    if (fd < 0) return NULL;

    for (int i = 0; i < MAX_STREAMS; i++) {
        if (streams[i].used) continue;
        streams[i].fd = fd;
        streams[i].eof = streams[i].err = false;
        streams[i].used = true;
        return &streams[i];
    }
    close(fd);
    return NULL;
}

static int WINAPI m_fclose(CRTFILE *f) {
    if (!f || !f->used) return -1;
    if (f->fd > 2) close(f->fd);
    f->used = false;
    return 0;
}

static size_t WINAPI m_fread(void *buf, size_t size, size_t count, CRTFILE *f) {
    if (!f || !buf || !size) return 0;
    ssize_t n = read(f->fd, buf, size * count);
    if (n <= 0) { if (f) f->eof = true; return 0; }
    return (size_t)n / size;
}

static size_t WINAPI m_fwrite(const void *buf, size_t size, size_t count, CRTFILE *f) {
    if (!f || !buf || !size) return 0;
    ssize_t n = write(f->fd, buf, size * count);
    return n <= 0 ? 0 : (size_t)n / size;
}

static int WINAPI m_fseek(CRTFILE *f, long off, int whence) {
    if (!f) return -1;
    f->eof = false;
    return lseek(f->fd, off, whence) < 0 ? -1 : 0;
}
static long WINAPI m_ftell(CRTFILE *f) { return f ? (long)lseek(f->fd, 0, SEEK_CUR) : -1; }
static void WINAPI m_rewind(CRTFILE *f) { if (f) { lseek(f->fd, 0, SEEK_SET); f->eof = false; } }
static int  WINAPI m_feof(CRTFILE *f) { return f && f->eof; }
static int  WINAPI m_ferror(CRTFILE *f) { return f && f->err; }
static void WINAPI m_clearerr(CRTFILE *f) { if (f) { f->eof = f->err = false; } }
static int  WINAPI m_fflush(CRTFILE *f) { (void)f; flush_output(); return 0; }

static int WINAPI m_fgetc(CRTFILE *f) {
    if (!f) return -1;
    char c;
    if (read(f->fd, &c, 1) != 1) { f->eof = true; return -1; }
    return (unsigned char)c;
}
static int WINAPI m_fputc(int c, CRTFILE *f) {
    char b = (char)c;
    return (f && write(f->fd, &b, 1) == 1) ? c : -1;
}
static int WINAPI m_fputs(const char *s, CRTFILE *f) {
    if (!s || !f) return -1;
    return write(f->fd, s, strlen(s)) < 0 ? -1 : 0;
}
static char *WINAPI m_fgets(char *buf, int cap, CRTFILE *f) {
    if (!buf || cap <= 1 || !f) return NULL;
    int n = 0;
    while (n < cap - 1) {
        char c;
        if (read(f->fd, &c, 1) != 1) { f->eof = true; break; }
        buf[n++] = c;
        if (c == '\n') break;
    }
    buf[n] = 0;
    return n ? buf : NULL;
}

static int WINAPI m_remove(const char *name) {
    char host[512];
    win_path_to_host(name, host, sizeof host);
    return unlink(host);
}
static int WINAPI m_rename(const char *from, const char *to) {
    char a[512], b[512];
    win_path_to_host(from, a, sizeof a);
    win_path_to_host(to, b, sizeof b);
    return rename(a, b);
}

/* --------------------------------------------------------------- the printfs */

/* Everything that formats ends up in one of these.  They take the argument
 * list as a pointer, which is what a Microsoft variadic list is; the variadic
 * spellings a program actually calls are the stubs at the end of the file,
 * each of which spills its register arguments and comes straight here. */

int WINAPI m_vfprintf(CRTFILE *f, const char *fmt, ms_va_list ap) {
    outsink s = { NULL, 0, 0, f ? f->fd : STDOUT_FD };
    return crt_format(&s, fmt, ap);
}
int WINAPI m_vprintf(const char *fmt, ms_va_list ap) {
    outsink s = { NULL, 0, 0, STDOUT_FD };
    return crt_format(&s, fmt, ap);
}
int WINAPI m_vsnprintf(char *buf, size_t cap, const char *fmt, ms_va_list ap) {
    outsink s = { buf, cap, 0, -1 };
    return crt_format(&s, fmt, ap);
}
int WINAPI m_vsprintf(char *buf, const char *fmt, ms_va_list ap) {
    /* sprintf has no limit, so the sink is given one large enough that the
     * bound never fires before the caller's buffer does. */
    outsink s = { buf, (size_t)1 << 20, 0, -1 };
    return crt_format(&s, fmt, ap);
}

/* Microsoft's _snprintf differs from the standard one: it returns -1 when the
 * text did not fit rather than the length it would have needed. */
int WINAPI m_vsnprintf_ms(char *buf, size_t cap, const char *fmt, ms_va_list ap) {
    int n = m_vsnprintf(buf, cap, fmt, ap);
    return (size_t)n >= cap ? -1 : n;
}
int WINAPI m_vsnprintf_s(char *buf, size_t cap, size_t count, const char *fmt, ms_va_list ap) {
    (void)count;
    return m_vsnprintf(buf, cap, fmt, ap);
}

/* The variadic entry points themselves.  Each spills the argument registers
 * into the space the caller already reserved for them - which is what that
 * space is for - and hands the address of the first variadic slot to the
 * routine above.  The jump is a tail call: both sides use the same convention,
 * so there is nothing to undo afterwards.
 *
 * The slot addresses come straight from the convention: [rsp] holds the return
 * address, and the four home slots follow it at 8, 16, 24 and 32. */
__asm__(
".text\n"

".globl m_printf\n"                 /* printf(fmt, ...) -> vprintf(fmt, ap) */
"m_printf:\n"
"   movq %rdx, 16(%rsp)\n"
"   movq %r8,  24(%rsp)\n"
"   movq %r9,  32(%rsp)\n"
"   leaq 16(%rsp), %rdx\n"
"   jmp  m_vprintf\n"

".globl m_fprintf\n"                /* fprintf(f, fmt, ...) */
"m_fprintf:\n"
"   movq %r8,  24(%rsp)\n"
"   movq %r9,  32(%rsp)\n"
"   leaq 24(%rsp), %r8\n"
"   jmp  m_vfprintf\n"

".globl m_sprintf\n"                /* sprintf(buf, fmt, ...) */
"m_sprintf:\n"
"   movq %r8,  24(%rsp)\n"
"   movq %r9,  32(%rsp)\n"
"   leaq 24(%rsp), %r8\n"
"   jmp  m_vsprintf\n"

".globl m_snprintf\n"               /* snprintf(buf, cap, fmt, ...) */
"m_snprintf:\n"
"   movq %r9,  32(%rsp)\n"
"   leaq 32(%rsp), %r9\n"
"   jmp  m_vsnprintf\n"

".globl m_snprintf_ms\n"            /* _snprintf, with the other return rule */
"m_snprintf_ms:\n"
"   movq %r9,  32(%rsp)\n"
"   leaq 32(%rsp), %r9\n"
"   jmp  m_vsnprintf_ms\n"
);

int WINAPI m_printf(const char *fmt, ...);
int WINAPI m_fprintf(CRTFILE *f, const char *fmt, ...);
int WINAPI m_sprintf(char *buf, const char *fmt, ...);
int WINAPI m_snprintf(char *buf, size_t cap, const char *fmt, ...);
int WINAPI m_snprintf_ms(char *buf, size_t cap, const char *fmt, ...);

static int WINAPI m_puts(const char *s) {
    if (!s) return -1;
    write(STDOUT_FD, s, strlen(s));
    write(STDOUT_FD, "\n", 1);
    return 0;
}
static int WINAPI m_putchar(int c) { char b = (char)c; write(STDOUT_FD, &b, 1); return c; }
static int WINAPI m_getchar(void) { return getchar(); }

/* ----------------------------------------------------------------- strings */

static size_t WINAPI m_strlen(const char *s) { return strlen(s); }
static char  *WINAPI m_strcpy(char *d, const char *s) { return strcpy(d, s); }
static char  *WINAPI m_strncpy(char *d, const char *s, size_t n) { return strncpy(d, s, n); }
static char  *WINAPI m_strcat(char *d, const char *s) { return strcat(d, s); }
static char  *WINAPI m_strncat(char *d, const char *s, size_t n) {
    size_t l = strlen(d);
    for (size_t i = 0; i < n && s[i]; i++) d[l + i] = s[i];
    d[l + (strlen(s) < n ? strlen(s) : n)] = 0;
    return d;
}
static int    WINAPI m_strcmp(const char *a, const char *b) { return strcmp(a, b); }
static int    WINAPI m_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
static int    WINAPI m_stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
static int    WINAPI m_strnicmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
static char  *WINAPI m_strchr(const char *s, int c) { return strchr(s, c); }
static char  *WINAPI m_strrchr(const char *s, int c) { return strrchr(s, c); }
static char  *WINAPI m_strstr(const char *h, const char *n) { return strstr(h, n); }
static char  *WINAPI m_strdup(const char *s) { return strdup(s); }
static char  *WINAPI m_strerror(int e) { return (char *)strerror(e); }
static size_t WINAPI m_strspn(const char *s, const char *set) {
    size_t n = 0;
    while (s[n] && strchr(set, s[n])) n++;
    return n;
}
static size_t WINAPI m_strcspn(const char *s, const char *set) {
    size_t n = 0;
    while (s[n] && !strchr(set, s[n])) n++;
    return n;
}
static char *WINAPI m_strpbrk(const char *s, const char *set) {
    for (; *s; s++) if (strchr(set, *s)) return (char *)s;
    return NULL;
}
static char *WINAPI m_strtok(char *s, const char *sep) {
    static char *save;
    return strtok_r(s, sep, &save);
}
static char *WINAPI m_strupr(char *s) {
    for (char *p = s; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    return s;
}
static char *WINAPI m_strlwr(char *s) {
    for (char *p = s; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    return s;
}
static int WINAPI m_strcpy_s(char *d, size_t cap, const char *s) {
    if (!d || !s || strlen(s) + 1 > cap) return 22 /* EINVAL */;
    strcpy(d, s);
    return 0;
}
static int WINAPI m_strcat_s(char *d, size_t cap, const char *s) {
    if (!d || !s || strlen(d) + strlen(s) + 1 > cap) return 22;
    strcat(d, s);
    return 0;
}

/* Wide strings, which Windows programs use for anything user-visible. */
static size_t WINAPI m_wcslen(const WCHAR *s) { size_t n = 0; if (s) while (s[n]) n++; return n; }
static WCHAR *WINAPI m_wcscpy(WCHAR *d, const WCHAR *s) {
    size_t i = 0;
    if (d && s) { for (; s[i]; i++) d[i] = s[i]; d[i] = 0; }
    return d;
}
static int WINAPI m_wcscmp(const WCHAR *a, const WCHAR *b) {
    size_t i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return (int)a[i] - (int)b[i];
}
static int WINAPI m_wcsncmp(const WCHAR *a, const WCHAR *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
        if (!a[i]) break;
    }
    return 0;
}
static WCHAR *WINAPI m_wcschr(const WCHAR *s, WCHAR c) {
    for (; *s; s++) if (*s == c) return (WCHAR *)s;
    return c ? NULL : (WCHAR *)s;
}
static WCHAR *WINAPI m_wcscat(WCHAR *d, const WCHAR *s) {
    size_t n = m_wcslen(d), i = 0;
    for (; s[i]; i++) d[n + i] = s[i];
    d[n + i] = 0;
    return d;
}

/* ------------------------------------------------------------------ memory */

static void *WINAPI m_malloc(size_t n) { return malloc(n); }
static void  WINAPI m_free(void *p) { free(p); }
static void *WINAPI m_calloc(size_t c, size_t n) { return calloc(c, n); }
static void *WINAPI m_realloc(void *p, size_t n) { return realloc(p, n); }
static void *WINAPI m_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static void *WINAPI m_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static void *WINAPI m_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static int   WINAPI m_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static void *WINAPI m_memchr(const void *s, int c, size_t n) { return memchr(s, c, n); }

/* ------------------------------------------------------- what C++ needs
 *
 * A program written in C++ asks the runtime for several things the language
 * never mentions.  Memory comes through operators rather than functions, and
 * the names they are exported under are the compiler's own encoding of their
 * shape rather than anything a person would write.  Calling a function that
 * every derived class was supposed to provide and none did has to end the
 * program rather than jump to nothing.  And every type that can be asked what
 * it is at run time carries a pointer to a table this has to provide, even
 * when nothing in the program ever asks.
 *
 * None of this is the language being clever.  It is the floor it stands on.
 */
static void *WINAPI cpp_new(size_t n) {
    /* Zero bytes is a legitimate request and must not come back as nothing:
     * two objects of no size still have to be at different addresses. */
    void *p = malloc(n ? n : 1);
    return p;
}
static void WINAPI cpp_delete(void *p) { free(p); }
static void WINAPI cpp_delete_sized(void *p, size_t n) { (void)n; free(p); }

/* Calling a function a base class declared and no derived class defined.  It
 * cannot return, because there is nowhere to return to that makes sense. */
static void WINAPI cpp_purecall(void) {
    static const char message[] =
        "a program called a function that was never given a body\n";
    write(STDERR_FD, message, sizeof message - 1);
    exit(3);
}

/* The table every type that can be identified at run time points at.  Nothing
 * here reads it; it exists because every such type's description refers to it
 * and a program will not link without it. */
void *type_info_vftable[4] = { (void *)cpp_delete, 0, 0, 0 };

/* ------------------------------------------------------------- conversions */

static int  WINAPI m_atoi(const char *s) { return atoi(s); }
static long WINAPI m_atol(const char *s) { return strtol(s, NULL, 10); }
static long WINAPI m_strtol(const char *s, char **end, int base) { return strtol(s, end, base); }
static unsigned long WINAPI m_strtoul(const char *s, char **end, int base) { return strtoul(s, end, base); }

static double parse_double(const char *s, char **end) {
    while (*s == ' ' || *s == '\t') s++;
    bool neg = false;
    if (*s == '-') { neg = true; s++; } else if (*s == '+') s++;
    double v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    if (*s == '.') {
        s++;
        double scale = 0.1;
        while (*s >= '0' && *s <= '9') { v += (*s++ - '0') * scale; scale *= 0.1; }
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        bool eneg = false;
        if (*s == '-') { eneg = true; s++; } else if (*s == '+') s++;
        int e = 0;
        while (*s >= '0' && *s <= '9') e = e * 10 + (*s++ - '0');
        double p = pow10i(e);
        v = eneg ? v / p : v * p;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}
static double WINAPI m_atof(const char *s) { return parse_double(s, NULL); }
static double WINAPI m_strtod(const char *s, char **end) { return parse_double(s, end); }

static char *WINAPI m_itoa(int v, char *buf, int base) {
    char tmp[36];
    int n = 0;
    bool neg = v < 0 && base == 10;
    unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    if (!u) tmp[n++] = '0';
    while (u) { tmp[n++] = "0123456789abcdefghijklmnopqrstuvwxyz"[u % (unsigned)base]; u /= (unsigned)base; }
    int k = 0;
    if (neg) buf[k++] = '-';
    while (n) buf[k++] = tmp[--n];
    buf[k] = 0;
    return buf;
}

/* --------------------------------------------------------------------- math */

/* Written out rather than borrowed: there is no maths library here, and a
 * Windows program that prints a square root expects the right answer. */
/* A quiet not-a-number, built from its bits rather than from an operation the
 * compiler is entitled to fold away. */
static double quiet_nan(void) {
    uint64_t bits = 0x7FF8000000000000ULL;
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

static double m_sqrt_impl(double v) {
    if (v < 0) return quiet_nan();
    if (v == 0) return 0;
    /* Newton's method, from a decent starting guess taken from the exponent. */
    double x = v;
    for (int i = 0; i < 40; i++) {
        double next = 0.5 * (x + v / x);
        if (next == x) break;
        x = next;
    }
    return x;
}
static double WINAPI m_sqrt(double v) { return m_sqrt_impl(v); }
static double WINAPI m_fabs(double v) { return v < 0 ? -v : v; }
static double WINAPI m_floor(double v) {
    double t = (double)(int64_t)v;
    return (v < 0 && t != v) ? t - 1 : t;
}
static double WINAPI m_ceil(double v) {
    double t = (double)(int64_t)v;
    return (v > 0 && t != v) ? t + 1 : t;
}
static double WINAPI m_fmod(double a, double b) {
    if (b == 0) return 0;
    double q = a / b;
    double t = (double)(int64_t)q;
    return a - t * b;
}
static double m_exp_impl(double x) {
    /* Split off the integer part so the series only ever sees a small
     * argument, where it converges in a handful of terms. */
    bool neg = x < 0;
    if (neg) x = -x;
    int whole = (int)x;
    double frac = x - whole;
    double term = 1, sum = 1;
    for (int i = 1; i < 20; i++) { term *= frac / i; sum += term; }
    const double e = 2.718281828459045;
    double p = 1;
    for (int i = 0; i < whole; i++) p *= e;
    double r = sum * p;
    return neg ? 1 / r : r;
}
static double WINAPI m_exp(double x) { return m_exp_impl(x); }
static double m_log_impl(double x) {
    if (x <= 0) return -1e308;
    /* Scale into [1, 2) where the series behaves, counting the halvings. */
    int e = 0;
    while (x >= 2) { x /= 2; e++; }
    while (x < 1) { x *= 2; e--; }
    double y = (x - 1) / (x + 1), y2 = y * y, term = y, sum = 0;
    for (int i = 1; i < 40; i += 2) { sum += term / i; term *= y2; }
    return 2 * sum + e * 0.6931471805599453;
}
static double WINAPI m_log(double x) { return m_log_impl(x); }
static double WINAPI m_log10(double x) { return m_log_impl(x) / 2.302585092994046; }
static double WINAPI m_pow(double a, double b) {
    if (a == 0) {
        if (b == 0) return 1;
        if (b < 0) return 1e308 * 10;                 /* infinity */
        return 0;
    }
    if (b == (double)(int)b && b >= -64 && b <= 64) {
        /* Whole powers are exact this way, and much the commonest case. */
        int n = (int)b;
        double r = 1, base = n < 0 ? 1 / a : a;
        for (int i = 0; i < (n < 0 ? -n : n); i++) r *= base;
        return r;
    }
    return m_exp_impl(b * m_log_impl(a));
}
static double m_sin_impl(double x) {
    const double pi2 = 6.283185307179586;
    x = x - pi2 * (double)(int64_t)(x / pi2);
    double term = x, sum = x, x2 = x * x;
    for (int i = 1; i < 12; i++) {
        term *= -x2 / (double)((2 * i) * (2 * i + 1));
        sum += term;
    }
    return sum;
}
static double WINAPI m_sin(double x) { return m_sin_impl(x); }
static double WINAPI m_cos(double x) { return m_sin_impl(x + 1.5707963267948966); }
static double WINAPI m_tan(double x) { double c = m_sin_impl(x + 1.5707963267948966); return c == 0 ? 0 : m_sin_impl(x) / c; }
static double m_atan_impl(double x) {
    bool inv = false;
    if (x > 1) { x = 1 / x; inv = true; }
    bool neg = x < 0;
    if (neg) x = -x;
    bool inv2 = false;
    if (x > 1) { x = 1 / x; inv2 = true; }
    double term = x, sum = 0, x2 = x * x;
    for (int i = 0; i < 40; i++) {
        sum += (i & 1 ? -1 : 1) * term / (2 * i + 1);
        term *= x2;
    }
    if (inv2) sum = 1.5707963267948966 - sum;
    if (neg) sum = -sum;
    if (inv) sum = 1.5707963267948966 - sum;
    return sum;
}
static double WINAPI m_atan(double x) { return m_atan_impl(x); }
static double WINAPI m_atan2(double y, double x) {
    if (x > 0) return m_atan_impl(y / x);
    if (x < 0) return m_atan_impl(y / x) + (y >= 0 ? 3.141592653589793 : -3.141592653589793);
    return y > 0 ? 1.5707963267948966 : (y < 0 ? -1.5707963267948966 : 0);
}

static int WINAPI m_abs(int v) { return v < 0 ? -v : v; }
static long WINAPI m_labs(long v) { return v < 0 ? -v : v; }

/* ------------------------------------------------------------------ ctype */

static int WINAPI m_isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int WINAPI m_isdigit(int c) { return c >= '0' && c <= '9'; }
static int WINAPI m_isalnum(int c) { return m_isalpha(c) || m_isdigit(c); }
static int WINAPI m_isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
static int WINAPI m_isupper(int c) { return c >= 'A' && c <= 'Z'; }
static int WINAPI m_islower(int c) { return c >= 'a' && c <= 'z'; }
static int WINAPI m_isprint(int c) { return c >= 0x20 && c < 0x7F; }
static int WINAPI m_iscntrl(int c) { return c < 0x20 || c == 0x7F; }
static int WINAPI m_ispunct(int c) { return m_isprint(c) && !m_isalnum(c) && c != ' '; }
static int WINAPI m_isxdigit(int c) { return m_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int WINAPI m_toupper(int c) { return m_islower(c) ? c - 32 : c; }
static int WINAPI m_tolower(int c) { return m_isupper(c) ? c + 32 : c; }

/* ---------------------------------------------------------------- sorting */

static void swap_bytes(char *a, char *b, size_t n) {
    for (size_t i = 0; i < n; i++) { char t = a[i]; a[i] = b[i]; b[i] = t; }
}

/* An insertion sort with a binary search for the position.  For the array
 * sizes a program running here will hand it, that is faster than quicksort and
 * far less code. */
static void WINAPI m_qsort(void *base, size_t count, size_t size,
                           int WINAPI (*cmp)(const void *, const void *)) {
    char *a = base;
    for (size_t i = 1; i < count; i++)
        for (size_t k = i; k > 0 && cmp(a + (k - 1) * size, a + k * size) > 0; k--)
            swap_bytes(a + (k - 1) * size, a + k * size, size);
}

static void *WINAPI m_bsearch(const void *key, const void *base, size_t count, size_t size,
                              int WINAPI (*cmp)(const void *, const void *)) {
    const char *a = base;
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int r = cmp(key, a + mid * size);
        if (!r) return (void *)(a + mid * size);
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

/* ------------------------------------------------------------- odds and ends */

static uint32_t rand_state = 1;
static int  WINAPI m_rand(void) {
    rand_state = rand_state * 1103515245 + 12345;
    return (int)((rand_state >> 16) & 0x7FFF);
}
static void WINAPI m_srand(unsigned seed) { rand_state = seed; }

static int64_t WINAPI m_time(int64_t *out) {
    int64_t t = (int64_t)time_now();
    if (out) *out = t;
    return t;
}
static int64_t WINAPI m_clock(void) { return (int64_t)uptime_ms(); }

static void WINAPI m_exit(int code) {
    u32_shutdown();
    flush_output();
    exit(code);
}
static void WINAPI m_abort(void) {
    fprintf(STDERR_FD, "\nwinrun: the program called abort()\n");
    flush_output();
    exit(3);
}
static int WINAPI m_atexit(void (*fn)(void)) { (void)fn; return 0; }
DWORD win_getenv(const char *name, char *out, DWORD cap);

static char *WINAPI m_getenv(const char *name) {
    static char value[256];
    return win_getenv(name, value, sizeof value) ? value : NULL;
}
static int WINAPI m_system(const char *cmd) { (void)cmd; return -1; }

static int errno_slot;
static int *WINAPI m__errno(void) { errno_slot = errno; return &errno_slot; }

/* The startup sequence.  A program built against the dynamic runtime calls
 * these before main, and they have to behave even though there is nothing for
 * most of them to do. */
static int app_type;
static int commode, fmode;
static int argc_slot;
static char **argv_slot;

static void WINAPI m___set_app_type(int t) { app_type = t; }
static int  *WINAPI m___p__commode(void) { return &commode; }
static int  *WINAPI m___p__fmode(void) { return &fmode; }
static int  *WINAPI m___p___argc(void) { return &argc_slot; }
static char ***WINAPI m___p___argv(void) { return &argv_slot; }
static int  WINAPI m__configthreadlocale(int what) { (void)what; return 0; }
static void WINAPI m___setusermatherr(void *fn) { (void)fn; }
static int  WINAPI m__set_new_mode(int mode) { (void)mode; return 0; }
static void *WINAPI m__set_invalid_parameter_handler(void *fn) { (void)fn; return NULL; }
static int  WINAPI m__controlfp_s(unsigned *current, unsigned newval, unsigned mask) {
    (void)newval; (void)mask;
    if (current) *current = 0;
    return 0;
}
static void WINAPI m__cexit(void) { flush_output(); }
static void WINAPI m__c_exit(void) { flush_output(); }
static void WINAPI m__amsg_exit(int code) {
    fprintf(STDERR_FD, "\nwinrun: the C runtime reported fatal error %d\n", code);
    flush_output();
    exit(code);
}
static void *WINAPI m__onexit(void *fn) { return fn; }
static int WINAPI m__crt_atexit(void (*fn)(void)) { (void)fn; return 0; }
static int WINAPI m__register_thread_local_exe_atexit_callback(void *fn) { (void)fn; return 0; }
static int WINAPI m__configure_narrow_argv(int mode) { (void)mode; return 0; }
static int WINAPI m__initialize_narrow_environment(void) { return 0; }
static char **WINAPI m__get_initial_narrow_environment(void) {
    static char *empty[1] = { NULL };
    return empty;
}

/* The tables of initialisers the compiler emits, walked in order.  A program
 * with global constructors will not have run any of them without this. */
static void WINAPI m__initterm(void (**first)(void), void (**last)(void)) {
    for (void (**p)(void) = first; p < last; p++) if (*p) (*p)();
}
static int WINAPI m__initterm_e(int (**first)(void), int (**last)(void)) {
    for (int (**p)(void) = first; p < last; p++) {
        if (!*p) continue;
        int r = (*p)();
        if (r) return r;
    }
    return 0;
}

/* The older runtime's way of handing a program its arguments. */
int win_build_argv(char ***out);

static int WINAPI m___getmainargs(int *argc, char ***argv, char ***env, int expand, void *info) {
    (void)expand; (void)info;
    static char **built;
    static int built_count;
    if (!built) built_count = win_build_argv(&built);
    if (argc) *argc = built_count;
    if (argv) *argv = built;
    if (env) { static char *empty[1] = { NULL }; *env = empty; }
    argc_slot = built_count;
    argv_slot = built;
    return 0;
}

static void WINAPI m__assert(const char *expr, const char *file, unsigned line) {
    fprintf(STDERR_FD, "\nwinrun: assertion failed: %s, %s:%u\n",
            expr ? expr : "?", file ? file : "?", line);
    flush_output();
    exit(3);
}

/* A threaded C program starts its threads through the runtime rather than
 * through kernel32, because the runtime has per-thread state to set up. */
HANDLE win_create_thread(void *start, void *arg, unsigned flags, unsigned *tid);
void   win_exit_thread(unsigned code);

static uintptr_t WINAPI m__beginthreadex(void *security, unsigned stack, void *start,
                                         void *arg, unsigned flags, unsigned *tid) {
    (void)security; (void)stack;
    return (uintptr_t)win_create_thread(start, arg, flags, tid);
}
static void WINAPI m__endthreadex(unsigned code) { win_exit_thread(code); }

/* ---------------------------------------------------------------- the table */

static const win_export_t msvcrt[] = {
    { "printf",       (void *)m_printf },
    { "vprintf",      (void *)m_vprintf },
    { "fprintf",      (void *)m_fprintf },
    { "vfprintf",     (void *)m_vfprintf },
    { "sprintf",      (void *)m_sprintf },
    { "vsprintf",     (void *)m_vsprintf },
    { "snprintf",     (void *)m_snprintf },
    { "_snprintf",    (void *)m_snprintf_ms },
    { "vsnprintf",    (void *)m_vsnprintf },
    { "_vsnprintf",   (void *)m_vsnprintf },
    { "sprintf_s",    (void *)m_snprintf_ms },
    { "_vsnprintf_s", (void *)m_vsnprintf_s },
    { "puts",         (void *)m_puts },
    { "putchar",      (void *)m_putchar },
    { "getchar",      (void *)m_getchar },
    { "__iob_func",   (void *)w___iob_func },
    { "__acrt_iob_func", (void *)w___acrt_iob_func },

    { "fopen",        (void *)m_fopen },
    { "fclose",       (void *)m_fclose },
    { "fread",        (void *)m_fread },
    { "fwrite",       (void *)m_fwrite },
    { "fseek",        (void *)m_fseek },
    { "ftell",        (void *)m_ftell },
    { "rewind",       (void *)m_rewind },
    { "feof",         (void *)m_feof },
    { "ferror",       (void *)m_ferror },
    { "clearerr",     (void *)m_clearerr },
    { "fflush",       (void *)m_fflush },
    { "fgetc",        (void *)m_fgetc },
    { "getc",         (void *)m_fgetc },
    { "fputc",        (void *)m_fputc },
    { "putc",         (void *)m_fputc },
    { "fputs",        (void *)m_fputs },
    { "fgets",        (void *)m_fgets },
    { "remove",       (void *)m_remove },
    { "rename",       (void *)m_rename },

    { "strlen",       (void *)m_strlen },
    { "strcpy",       (void *)m_strcpy },
    { "strncpy",      (void *)m_strncpy },
    { "strcat",       (void *)m_strcat },
    { "strncat",      (void *)m_strncat },
    { "strcmp",       (void *)m_strcmp },
    { "strncmp",      (void *)m_strncmp },
    { "_stricmp",     (void *)m_stricmp },
    { "stricmp",      (void *)m_stricmp },
    { "_strcmpi",     (void *)m_stricmp },
    { "_strnicmp",    (void *)m_strnicmp },
    { "strchr",       (void *)m_strchr },
    { "strrchr",      (void *)m_strrchr },
    { "strstr",       (void *)m_strstr },
    { "_strdup",      (void *)m_strdup },
    { "strdup",       (void *)m_strdup },
    { "strerror",     (void *)m_strerror },
    { "strspn",       (void *)m_strspn },
    { "strcspn",      (void *)m_strcspn },
    { "strpbrk",      (void *)m_strpbrk },
    { "strtok",       (void *)m_strtok },
    { "_strupr",      (void *)m_strupr },
    { "_strlwr",      (void *)m_strlwr },
    { "strcpy_s",     (void *)m_strcpy_s },
    { "strcat_s",     (void *)m_strcat_s },

    { "wcslen",       (void *)m_wcslen },
    { "wcscpy",       (void *)m_wcscpy },
    { "wcscat",       (void *)m_wcscat },
    { "wcscmp",       (void *)m_wcscmp },
    { "wcsncmp",      (void *)m_wcsncmp },
    { "wcschr",       (void *)m_wcschr },

    /* The operators C++ gets its memory through.  The names are the
     * compiler's encoding of what each one takes and gives back, which is why
     * they look like this and not like their spelling. */
    { "??2@YAPEAX_K@Z",   (void *)cpp_new },          /* new                */
    { "??_U@YAPEAX_K@Z",  (void *)cpp_new },          /* new[]              */
    { "??3@YAXPEAX@Z",    (void *)cpp_delete },       /* delete             */
    { "??3@YAXPEAX_K@Z",  (void *)cpp_delete_sized }, /* delete, size known */
    { "??_V@YAXPEAX@Z",   (void *)cpp_delete },       /* delete[]           */
    { "??_V@YAXPEAX_K@Z", (void *)cpp_delete_sized },
    { "_purecall",        (void *)cpp_purecall },
    { "??_7type_info@@6B@", (void *)type_info_vftable },

    { "malloc",       (void *)m_malloc },
    { "free",         (void *)m_free },
    { "calloc",       (void *)m_calloc },
    { "realloc",      (void *)m_realloc },
    { "memcpy",       (void *)m_memcpy },
    { "memmove",      (void *)m_memmove },
    { "memset",       (void *)m_memset },
    { "memcmp",       (void *)m_memcmp },
    { "memchr",       (void *)m_memchr },

    { "atoi",         (void *)m_atoi },
    { "atol",         (void *)m_atol },
    { "atof",         (void *)m_atof },
    { "strtol",       (void *)m_strtol },
    { "strtoul",      (void *)m_strtoul },
    { "strtod",       (void *)m_strtod },
    { "_itoa",        (void *)m_itoa },
    { "itoa",         (void *)m_itoa },

    { "sqrt",         (void *)m_sqrt },
    { "fabs",         (void *)m_fabs },
    { "floor",        (void *)m_floor },
    { "ceil",         (void *)m_ceil },
    { "fmod",         (void *)m_fmod },
    { "exp",          (void *)m_exp },
    { "log",          (void *)m_log },
    { "log10",        (void *)m_log10 },
    { "pow",          (void *)m_pow },
    { "sin",          (void *)m_sin },
    { "cos",          (void *)m_cos },
    { "tan",          (void *)m_tan },
    { "atan",         (void *)m_atan },
    { "atan2",        (void *)m_atan2 },
    { "abs",          (void *)m_abs },
    { "labs",         (void *)m_labs },

    { "isalpha",      (void *)m_isalpha },
    { "isdigit",      (void *)m_isdigit },
    { "isalnum",      (void *)m_isalnum },
    { "isspace",      (void *)m_isspace },
    { "isupper",      (void *)m_isupper },
    { "islower",      (void *)m_islower },
    { "isprint",      (void *)m_isprint },
    { "iscntrl",      (void *)m_iscntrl },
    { "ispunct",      (void *)m_ispunct },
    { "isxdigit",     (void *)m_isxdigit },
    { "toupper",      (void *)m_toupper },
    { "tolower",      (void *)m_tolower },

    { "qsort",        (void *)m_qsort },
    { "bsearch",      (void *)m_bsearch },
    { "rand",         (void *)m_rand },
    { "srand",        (void *)m_srand },
    { "time",         (void *)m_time },
    { "clock",        (void *)m_clock },
    { "exit",         (void *)m_exit },
    { "_exit",        (void *)m_exit },
    { "abort",        (void *)m_abort },
    { "atexit",       (void *)m_atexit },
    { "getenv",       (void *)m_getenv },
    { "system",       (void *)m_system },
    { "_errno",       (void *)m__errno },
    { "_assert",      (void *)m__assert },
    { "_wassert",     (void *)m__assert },

    { "__set_app_type",       (void *)m___set_app_type },
    { "_set_app_type",        (void *)m___set_app_type },
    { "__p__commode",         (void *)m___p__commode },
    { "__p__fmode",           (void *)m___p__fmode },
    { "__p___argc",           (void *)m___p___argc },
    { "__p___argv",           (void *)m___p___argv },
    { "_configthreadlocale",  (void *)m__configthreadlocale },
    { "__setusermatherr",     (void *)m___setusermatherr },
    { "_set_new_mode",        (void *)m__set_new_mode },
    { "_set_invalid_parameter_handler", (void *)m__set_invalid_parameter_handler },
    { "_controlfp_s",         (void *)m__controlfp_s },
    { "_cexit",               (void *)m__cexit },
    { "_c_exit",              (void *)m__c_exit },
    { "_amsg_exit",           (void *)m__amsg_exit },
    { "_onexit",              (void *)m__onexit },
    { "_crt_atexit",          (void *)m__crt_atexit },
    { "_register_thread_local_exe_atexit_callback", (void *)m__register_thread_local_exe_atexit_callback },
    { "_configure_narrow_argv", (void *)m__configure_narrow_argv },
    { "_initialize_narrow_environment", (void *)m__initialize_narrow_environment },
    { "_get_initial_narrow_environment", (void *)m__get_initial_narrow_environment },
    { "_initterm",            (void *)m__initterm },
    { "_initterm_e",          (void *)m__initterm_e },
    { "__getmainargs",        (void *)m___getmainargs },
    { "_beginthreadex",       (void *)m__beginthreadex },
    { "_endthreadex",         (void *)m__endthreadex },
    { NULL, NULL }
};

void crt_init(void) {
    win_register("msvcrt.dll", msvcrt);
    win_register("ucrtbase.dll", msvcrt);
    win_register("vcruntime140.dll", msvcrt);
    win_register("msvcp140.dll", msvcrt);
    win_register("api-ms-win-crt-runtime-l1-1-0.dll", msvcrt);
}
