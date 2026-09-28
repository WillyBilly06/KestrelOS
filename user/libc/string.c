/* string.c - string and memory routines for user programs. */
#include "kestrel.h"

void *memset(void *d, int c, size_t n) {
    unsigned char *p = d;
    unsigned char v = (unsigned char)c;
    while (n && ((uintptr_t)p & 7)) { *p++ = v; n--; }
    uint64_t w = 0x0101010101010101ULL * v;
    while (n >= 8) { *(uint64_t *)p = w; p += 8; n -= 8; }
    while (n--) *p++ = v;
    return d;
}

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *a = d;
    const unsigned char *b = s;
    while (n && ((uintptr_t)a & 7)) { *a++ = *b++; n--; }
    while (n >= 8) { *(uint64_t *)a = *(const uint64_t *)b; a += 8; b += 8; n -= 8; }
    while (n--) *a++ = *b++;
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    unsigned char *a = d;
    const unsigned char *b = s;
    if (a == b || !n) return d;
    if (a < b) return memcpy(d, s, n);
    a += n; b += n;
    while (n--) *--a = *--b;
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (size_t i = 0; i < n; i++) if (p[i] == (unsigned char)c) return (void *)(p + i);
    return NULL;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
size_t strnlen(const char *s, size_t max) { size_t n = 0; while (n < max && s[n]) n++; return n; }

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (int)(unsigned char)*a - (int)(unsigned char)*b : 0;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

int strcasecmp(const char *a, const char *b) {
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return (int)(unsigned char)lower(*a) - (int)(unsigned char)lower(*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n && *a && lower(*a) == lower(*b)) { a++; b++; n--; }
    return n ? (int)(unsigned char)lower(*a) - (int)(unsigned char)lower(*b) : 0;
}

char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) { } return r; }

char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

size_t strlcpy(char *d, const char *s, size_t cap) {
    size_t n = strlen(s);
    if (cap) {
        size_t c = n < cap - 1 ? n : cap - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return n;
}

size_t strlcat(char *d, const char *s, size_t cap) {
    size_t dl = strnlen(d, cap);
    if (dl == cap) return cap + strlen(s);
    return dl + strlcpy(d + dl, s, cap - dl);
}

char *strcat(char *d, const char *s) {
    char *r = d;
    while (*d) d++;
    while ((*d++ = *s++)) { }
    return r;
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

char *strstr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (*hay == *needle && !strncmp(hay, needle, n)) return (char *)hay;
    return NULL;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* Reentrant tokeniser: the caller owns the state, so nested loops are safe. */
char *strtok_r(char *s, const char *sep, char **save) {
    if (!s) s = *save;
    if (!s) return NULL;

    while (*s && strchr(sep, *s)) s++;
    if (!*s) { *save = NULL; return NULL; }

    char *start = s;
    while (*s && !strchr(sep, *s)) s++;
    if (*s) { *s = 0; *save = s + 1; }
    else *save = NULL;
    return start;
}

/* ------------------------------------------------------------------ numbers */

static int digit_value(char c, int base) {
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') v = c - 'A' + 10;
    else return -1;
    return v < base ? v : -1;
}

unsigned long strtoul(const char *s, char **end, int base) {
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;

    if (base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
        else if (p[0] == '0' && p[1]) { base = 8; p++; }
        else base = 10;
    } else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    unsigned long value = 0;
    bool any = false;
    for (;;) {
        int d = digit_value(*p, base);
        if (d < 0) break;
        value = value * (unsigned long)base + (unsigned long)d;
        p++;
        any = true;
    }
    if (end) *end = (char *)(any ? p : s);
    return value;
}

long strtol(const char *s, char **end, int base) {
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;

    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    else if (*p == '+') p++;

    char *stop = NULL;
    unsigned long v = strtoul(p, &stop, base);
    if (stop == p) { if (end) *end = (char *)s; return 0; }
    if (end) *end = stop;
    return neg ? -(long)v : (long)v;
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
