/* k32.c - kernel32: processes, memory, modules, time and text.
 *
 * The half of kernel32 that is not files or threads.  Everything here is a
 * thin translation: a Windows call arrives with Microsoft's argument
 * convention and Windows' idea of what a path, a time or a string looks like,
 * and leaves as the equivalent call on this system.
 */
#include "win.h"

/* ------------------------------------------------------------- the objects */

static object_t objects[MAX_OBJECTS];
#define H_OBJ_BASE 0x1000

object_t *obj_any(HANDLE h) {
    uintptr_t v = (uintptr_t)h;
    if (v < H_OBJ_BASE || v >= H_OBJ_BASE + MAX_OBJECTS) return NULL;
    object_t *o = &objects[v - H_OBJ_BASE];
    return o->kind == OBJ_FREE ? NULL : o;
}

object_t *obj_get(HANDLE h, objkind_t kind) {
    object_t *o = obj_any(h);
    return (o && o->kind == kind) ? o : NULL;
}

HANDLE obj_alloc(objkind_t kind, object_t **out) {
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (objects[i].kind != OBJ_FREE) continue;
        memset(&objects[i], 0, sizeof objects[i]);
        objects[i].kind = kind;
        objects[i].refs = 1;
        objects[i].fd = -1;
        if (out) *out = &objects[i];
        return (HANDLE)(uintptr_t)(H_OBJ_BASE + i);
    }
    if (out) *out = NULL;
    return NULL;
}

void obj_release(HANDLE h) {
    object_t *o = obj_any(h);
    if (!o) return;
    if (--o->refs > 0) return;
    switch (o->kind) {
    case OBJ_FILE:    if (o->fd >= 0) close(o->fd); break;
    case OBJ_FIND:    if (o->dir) closedir(o->dir); break;
    case OBJ_SOCKET:  if (o->sock >= 0) tcp_close(o->sock); break;
    case OBJ_MAPPING: break;
    default: break;
    }
    o->kind = OBJ_FREE;
}

/* --------------------------------------------------------------- the error */

static DWORD last_error;
void  win_set_error(DWORD e) { last_error = e; }
DWORD win_get_error(void) { return last_error; }

static DWORD error_from_errno(int e) {
    switch (e) {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_FILE_EXISTS;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EINVAL: return ERROR_INVALID_PARAMETER;
    case EBADF:  return ERROR_INVALID_HANDLE;
    default:     return (DWORD)e;
    }
}

/* --------------------------------------------------------------- the text */

size_t win_wide_to_utf8(const WCHAR *w, char *out, size_t cap) {
    size_t n = 0;
    if (!cap) return 0;
    if (!w) { out[0] = 0; return 0; }
    for (; w[n] && n + 1 < cap; n++) {
        WCHAR c = w[n];
        out[n] = (char)(c < 0x80 ? c : '?');
    }
    out[n] = 0;
    return n;
}

size_t win_utf8_to_wide(const char *s, WCHAR *out, size_t cap) {
    size_t n = 0;
    if (!cap) return 0;
    if (!s) { out[0] = 0; return 0; }
    for (; s[n] && n + 1 < cap; n++) out[n] = (WCHAR)(unsigned char)s[n];
    out[n] = 0;
    return n;
}

/* --------------------------------------------------------------- the paths */

/* Windows spells a path with backslashes and a drive letter; this system
 * spells it with slashes and no drive.  C: is the root, which makes the two
 * views of the same file agree.  Anything already in this system's shape is
 * left as it is, so a program handed "/etc/motd" still finds it. */
void win_path_to_host(const char *win, char *out, size_t cap) {
    if (!win || !*win) { strlcpy(out, ".", cap); return; }

    const char *p = win;
    size_t n = 0;

    /* "\\?\C:\x" - the long-path prefix, which says nothing about the file. */
    if (!strncmp(p, "\\\\?\\", 4)) p += 4;

    if (p[0] && p[1] == ':') {
        char drive = p[0];
        if (drive >= 'a' && drive <= 'z') drive -= 32;
        p += 2;
        if (drive == 'C') {
            if (*p != '\\' && *p != '/') { out[n++] = '/'; }
        } else {
            /* Any other drive letter becomes a directory of that name, so a
             * program that insists on D: still lands somewhere sensible. */
            n += (size_t)snprintf(out + n, cap - n, "/mnt/%c", drive + 32);
        }
    }

    for (; *p && n + 1 < cap; p++) {
        char c = *p == '\\' ? '/' : *p;
        /* Collapse doubled separators, which Windows tolerates. */
        if (c == '/' && n > 0 && out[n - 1] == '/') continue;
        out[n++] = c;
    }
    out[n] = 0;
    if (!n) strlcpy(out, "/", cap);
}

void win_path_from_host(const char *host, char *out, size_t cap) {
    size_t n = 0;
    if (host[0] == '/') { n += (size_t)snprintf(out, cap, "C:"); }
    for (const char *p = host; *p && n + 1 < cap; p++)
        out[n++] = *p == '/' ? '\\' : *p;
    out[n] = 0;
}

/* ------------------------------------------------------------------ memory */

#define HEAP_ZERO_MEMORY 0x00000008

/* A heap handle is a token, not a pointer to anything: every heap here is the
 * same heap, because this program has one allocator. */
#define PROCESS_HEAP ((HANDLE)(uintptr_t)0x20)

static void *WINAPI w_GetProcessHeap(void) { return PROCESS_HEAP; }
static HANDLE WINAPI w_HeapCreate(DWORD flags, size_t initial, size_t max) {
    (void)flags; (void)initial; (void)max;
    return PROCESS_HEAP;
}
static BOOL WINAPI w_HeapDestroy(HANDLE h) { (void)h; return WIN_TRUE; }

/* Every block carries its own size, because HeapSize and HeapReAlloc are both
 * asked for it and the underlying allocator does not report it. */
typedef struct { size_t size; size_t guard; } blockhdr_t;
#define BLOCK_GUARD 0x484541504B455354ULL   /* "HEAPKEST" */

static void *heap_alloc(size_t n, bool zero) {
    blockhdr_t *b = malloc(sizeof *b + n);
    if (!b) { win_set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    b->size = n;
    b->guard = BLOCK_GUARD;
    if (zero) memset(b + 1, 0, n);
    return b + 1;
}

static blockhdr_t *heap_header(void *p) {
    if (!p) return NULL;
    blockhdr_t *b = (blockhdr_t *)p - 1;
    return b->guard == BLOCK_GUARD ? b : NULL;
}

static void *WINAPI w_HeapAlloc(HANDLE h, DWORD flags, size_t n) {
    (void)h;
    return heap_alloc(n, (flags & HEAP_ZERO_MEMORY) != 0);
}

static BOOL WINAPI w_HeapFree(HANDLE h, DWORD flags, void *p) {
    (void)h; (void)flags;
    blockhdr_t *b = heap_header(p);
    if (!b) return p == NULL;
    b->guard = 0;
    free(b);
    return WIN_TRUE;
}

static void *WINAPI w_HeapReAlloc(HANDLE h, DWORD flags, void *p, size_t n) {
    (void)h;
    if (!p) return heap_alloc(n, (flags & HEAP_ZERO_MEMORY) != 0);
    blockhdr_t *b = heap_header(p);
    if (!b) return NULL;
    void *fresh = heap_alloc(n, (flags & HEAP_ZERO_MEMORY) != 0);
    if (!fresh) return NULL;
    memcpy(fresh, p, b->size < n ? b->size : n);
    b->guard = 0;
    free(b);
    return fresh;
}

static size_t WINAPI w_HeapSize(HANDLE h, DWORD flags, void *p) {
    (void)h; (void)flags;
    blockhdr_t *b = heap_header(p);
    return b ? b->size : (size_t)-1;
}
static BOOL WINAPI w_HeapValidate(HANDLE h, DWORD flags, void *p) {
    (void)h; (void)flags;
    return p ? (heap_header(p) != NULL) : WIN_TRUE;
}
static BOOL WINAPI w_HeapSetInformation(HANDLE h, int cls, void *info, size_t len) {
    (void)h; (void)cls; (void)info; (void)len; return WIN_TRUE;
}

#define MEM_COMMIT   0x1000
#define MEM_RESERVE  0x2000
#define MEM_RELEASE  0x8000
#define PAGE_EXECUTE_READ      0x20
#define PAGE_EXECUTE_READWRITE 0x40

static void *WINAPI w_VirtualAlloc(void *addr, size_t len, DWORD type, DWORD protect) {
    (void)type;
    bool exec = (protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE ||
                 protect == 0x10 || protect == 0x80);
    long got = syscall6(SYS_MMAP, (long)(uintptr_t)addr, (long)len, exec ? 4 : 0, 0, 0, 0);
    if (got < 0) { win_set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    return (void *)(uintptr_t)got;
}

static BOOL WINAPI w_VirtualFree(void *addr, size_t len, DWORD type) {
    if (type & MEM_RELEASE) len = len ? len : 4096;
    syscall6(SYS_MUNMAP, (long)(uintptr_t)addr, (long)len, 0, 0, 0, 0);
    return WIN_TRUE;
}

/* There is no per-page protection to change here: everything a program maps is
 * already readable, writable and executable.  Reporting success is honest
 * about the outcome the caller cares about - the memory is usable that way. */
static BOOL WINAPI w_VirtualProtect(void *addr, size_t len, DWORD want, DWORD *old) {
    (void)addr; (void)len;
    if (old) *old = PAGE_EXECUTE_READWRITE;
    (void)want;
    return WIN_TRUE;
}

typedef struct {
    void *BaseAddress, *AllocationBase;
    DWORD AllocationProtect, __pad;
    size_t RegionSize;
    DWORD State, Protect, Type, __pad2;
} MEMORY_BASIC_INFORMATION;

static size_t WINAPI w_VirtualQuery(void *addr, MEMORY_BASIC_INFORMATION *info, size_t len) {
    if (!info || len < sizeof *info) return 0;
    memset(info, 0, sizeof *info);
    info->BaseAddress = addr;
    info->AllocationBase = addr;
    info->RegionSize = 4096;
    info->State = MEM_COMMIT;
    info->Protect = PAGE_EXECUTE_READWRITE;
    info->Type = 0x20000;                       /* MEM_PRIVATE */
    return sizeof *info;
}

/* Global and Local memory are the sixteen-bit heritage; on x86-64 they are
 * both just the process heap with a different spelling. */
#define GMEM_ZEROINIT 0x0040
static void *WINAPI w_GlobalAlloc(UINT flags, size_t n) { return heap_alloc(n, (flags & GMEM_ZEROINIT) != 0); }
static void *WINAPI w_GlobalFree(void *p) { w_HeapFree(NULL, 0, p); return NULL; }
static void *WINAPI w_GlobalLock(void *p) { return p; }
static BOOL  WINAPI w_GlobalUnlock(void *p) { (void)p; return WIN_TRUE; }
static size_t WINAPI w_GlobalSize(void *p) { blockhdr_t *b = heap_header(p); return b ? b->size : 0; }
static void *WINAPI w_GlobalReAlloc(void *p, size_t n, UINT flags) { return w_HeapReAlloc(NULL, (flags & GMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0, p, n); }

/* -------------------------------------------------------------- the process */

static void WINAPI w_ExitProcess(UINT code) {
    u32_shutdown();
    flush_output();
    exit((int)code);
}

static HANDLE WINAPI w_GetCurrentProcess(void) { return (HANDLE)(uintptr_t)-1; }
static DWORD  WINAPI w_GetCurrentProcessId(void) { return (DWORD)getpid(); }
static BOOL   WINAPI w_TerminateProcess(HANDLE h, UINT code) { (void)h; w_ExitProcess(code); return WIN_TRUE; }
static char  *WINAPI w_GetCommandLineA(void) { return win_cmdline; }
static WCHAR *WINAPI w_GetCommandLineW(void) { return win_cmdline_w; }

static DWORD WINAPI w_GetLastError(void) { return last_error; }
static void  WINAPI w_SetLastError(DWORD e) { last_error = e; }

/* --------------------------------------------------------------- modules */

win_module_t *pe_module_list(void);
win_module_t *pe_module_for(uint64_t address);

/* A module handle is the image's base address, which is what Windows uses and
 * what a program that pokes at its own headers expects.  Built-in libraries
 * have no image, so they get the address of their record instead - unique, and
 * never mistaken for a real base. */
static HANDLE handle_of(win_module_t *m) {
    if (!m) return NULL;
    return m->base ? (HANDLE)m->base : (HANDLE)m;
}

static win_module_t *module_from_handle(HANDLE h) {
    if (!h) return win_main_module;
    for (win_module_t *m = pe_module_list(); m; m = m->next)
        if (handle_of(m) == h) return m;
    return NULL;
}

static HANDLE WINAPI w_GetModuleHandleA(const char *name) {
    if (!name) return handle_of(win_main_module);
    win_module_t *m = pe_find_module(name);
    if (!m) { win_set_error(ERROR_MOD_NOT_FOUND); return NULL; }
    return handle_of(m);
}

static HANDLE WINAPI w_GetModuleHandleW(const WCHAR *name) {
    if (!name) return handle_of(win_main_module);
    char buf[128];
    win_wide_to_utf8(name, buf, sizeof buf);
    return w_GetModuleHandleA(buf);
}

static BOOL WINAPI w_GetModuleHandleExA(DWORD flags, const char *name, HANDLE *out) {
    (void)flags;
    if (!out) return WIN_FALSE;
    /* Asked by address rather than by name, which is how a DLL finds itself. */
    if (flags & 0x00000004) {
        win_module_t *m = pe_module_for((uint64_t)(uintptr_t)name);
        *out = handle_of(m ? m : win_main_module);
        return WIN_TRUE;
    }
    *out = w_GetModuleHandleA(name);
    return *out ? WIN_TRUE : WIN_FALSE;
}

static HANDLE WINAPI w_LoadLibraryA(const char *name) {
    if (!name) return NULL;
    win_module_t *m = pe_load(name, true);
    if (!m) { win_set_error(ERROR_MOD_NOT_FOUND); return NULL; }
    return handle_of(m);
}
static HANDLE WINAPI w_LoadLibraryExA(const char *name, HANDLE f, DWORD flags) {
    (void)f; (void)flags; return w_LoadLibraryA(name);
}
static HANDLE WINAPI w_LoadLibraryW(const WCHAR *name) {
    char buf[512];
    win_wide_to_utf8(name, buf, sizeof buf);
    return w_LoadLibraryA(buf);
}
static HANDLE WINAPI w_LoadLibraryExW(const WCHAR *name, HANDLE f, DWORD flags) {
    (void)f; (void)flags; return w_LoadLibraryW(name);
}
static BOOL WINAPI w_FreeLibrary(HANDLE h) {
    win_module_t *m = module_from_handle(h);
    if (m && m->refs > 0) m->refs--;
    return WIN_TRUE;
}

static void *WINAPI w_GetProcAddress(HANDLE h, const char *name) {
    win_module_t *m = module_from_handle(h);
    if (!m) { win_set_error(ERROR_MOD_NOT_FOUND); return NULL; }
    void *fn;
    /* A name whose top bits are clear is an ordinal in disguise. */
    if ((uintptr_t)name < 0x10000)
        fn = pe_export_of(m, NULL, (uint16_t)(uintptr_t)name);
    else
        fn = pe_export_of(m, name, 0);
    if (!fn) win_set_error(ERROR_PROC_NOT_FOUND);
    return fn;
}

static DWORD WINAPI w_GetModuleFileNameA(HANDLE h, char *out, DWORD cap) {
    win_module_t *m = module_from_handle(h);
    const char *path = (m && m->path[0]) ? m->path : win_exe_path;
    char win[512];
    win_path_from_host(path, win, sizeof win);
    size_t n = strlcpy(out, win, cap);
    return (DWORD)(n < cap ? n : cap);
}

static DWORD WINAPI w_GetModuleFileNameW(HANDLE h, WCHAR *out, DWORD cap) {
    char buf[512];
    DWORD n = w_GetModuleFileNameA(h, buf, sizeof buf);
    win_utf8_to_wide(buf, out, cap);
    return n;
}

/* ----------------------------------------------------------- the environment */

/* A small environment, because there is no shell here to inherit one from.
 * The variables a Windows program actually looks for are the ones that decide
 * where it writes: the temporary directory, the profile, the system root. */
static const char *environment =
    "PATH=C:\\bin\0"
    "TEMP=C:\\tmp\0"
    "TMP=C:\\tmp\0"
    "SystemRoot=C:\\windows\0"
    "windir=C:\\windows\0"
    "USERPROFILE=C:\\users\\user\0"
    "APPDATA=C:\\users\\user\\appdata\0"
    "COMPUTERNAME=KESTREL\0"
    "USERNAME=user\0"
    "OS=KestrelOS\0"
    "NUMBER_OF_PROCESSORS=1\0"
    "PROCESSOR_ARCHITECTURE=AMD64\0";

static char *WINAPI w_GetEnvironmentStrings(void) { return (char *)environment; }
static BOOL  WINAPI w_FreeEnvironmentStringsA(char *p) { (void)p; return WIN_TRUE; }

static WCHAR *WINAPI w_GetEnvironmentStringsW(void) {
    static WCHAR wide[512];
    static bool built;
    if (!built) {
        size_t n = 0;
        for (const char *p = environment; n < 510; ) {
            size_t len = strlen(p);
            for (size_t i = 0; i <= len && n < 510; i++) wide[n++] = (WCHAR)(unsigned char)p[i];
            p += len + 1;
            if (!*p) break;
        }
        wide[n] = 0;
        built = true;
    }
    return wide;
}

static const char *env_find(const char *name) {
    size_t len = strlen(name);
    for (const char *p = environment; *p; p += strlen(p) + 1)
        if (!strncasecmp(p, name, len) && p[len] == '=') return p + len + 1;
    return NULL;
}

static DWORD WINAPI w_GetEnvironmentVariableA(const char *name, char *out, DWORD cap) {
    const char *v = name ? env_find(name) : NULL;
    if (!v) { win_set_error(203 /* ERROR_ENVVAR_NOT_FOUND */); return 0; }
    size_t n = strlen(v);
    if (n + 1 > cap) return (DWORD)(n + 1);
    strlcpy(out, v, cap);
    return (DWORD)n;
}

static DWORD WINAPI w_GetEnvironmentVariableW(const WCHAR *name, WCHAR *out, DWORD cap) {
    char n8[128], v8[256];
    win_wide_to_utf8(name, n8, sizeof n8);
    DWORD n = w_GetEnvironmentVariableA(n8, v8, sizeof v8);
    if (!n || n >= sizeof v8) return n;
    win_utf8_to_wide(v8, out, cap);
    return n;
}

static BOOL WINAPI w_SetEnvironmentVariableA(const char *name, const char *value) {
    /* The environment here is fixed; reporting success would be a lie the
     * program would then act on. */
    (void)name; (void)value;
    win_set_error(ERROR_ACCESS_DENIED);
    return WIN_FALSE;
}

static DWORD WINAPI w_ExpandEnvironmentStringsA(const char *src, char *out, DWORD cap) {
    if (!src || !out) return 0;
    DWORD n = 0;
    for (const char *p = src; *p && n + 1 < cap; ) {
        if (*p == '%') {
            const char *end = strchr(p + 1, '%');
            if (end) {
                char name[64];
                size_t len = (size_t)(end - p - 1);
                if (len < sizeof name) {
                    memcpy(name, p + 1, len); name[len] = 0;
                    const char *v = env_find(name);
                    if (v) { n += (DWORD)strlcpy(out + n, v, cap - n); p = end + 1; continue; }
                }
            }
        }
        out[n++] = *p++;
    }
    out[n] = 0;
    return n + 1;
}

/* ------------------------------------------------------------------- time */

/* Windows counts time in hundred-nanosecond units since 1601; this system
 * counts seconds since 1970.  The difference between the two epochs is a
 * constant that has been the same since both were chosen. */
#define EPOCH_DIFF_100NS 116444736000000000ULL

static uint64_t now_filetime(void) {
    return EPOCH_DIFF_100NS + time_now() * 10000000ULL + (uptime_ms() % 1000) * 10000ULL;
}

static void WINAPI w_GetSystemTimeAsFileTime(FILETIME *ft) {
    if (!ft) return;
    uint64_t t = now_filetime();
    ft->dwLowDateTime = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
}
static void WINAPI w_GetSystemTimePreciseAsFileTime(FILETIME *ft) { w_GetSystemTimeAsFileTime(ft); }

/* The civil calendar from a count of seconds.  The algorithm is the standard
 * one: shift the year to start in March so the leap day lands at the end. */
static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (*m <= 2));
}

static void fill_systemtime(SYSTEMTIME *st, uint64_t unix_seconds, unsigned ms) {
    int64_t days = (int64_t)(unix_seconds / 86400);
    unsigned secs = (unsigned)(unix_seconds % 86400);
    int y; unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    st->wYear = (WORD)y;
    st->wMonth = (WORD)m;
    st->wDay = (WORD)d;
    st->wDayOfWeek = (WORD)((days + 4) % 7);      /* 1970-01-01 was a Thursday */
    st->wHour = (WORD)(secs / 3600);
    st->wMinute = (WORD)((secs / 60) % 60);
    st->wSecond = (WORD)(secs % 60);
    st->wMilliseconds = (WORD)ms;
}

static void WINAPI w_GetSystemTime(SYSTEMTIME *st) {
    if (st) fill_systemtime(st, time_now(), (unsigned)(uptime_ms() % 1000));
}
static void WINAPI w_GetLocalTime(SYSTEMTIME *st) { w_GetSystemTime(st); }

static BOOL WINAPI w_SystemTimeToFileTime(const SYSTEMTIME *st, FILETIME *ft) {
    if (!st || !ft) return WIN_FALSE;
    /* Days from the civil date, the inverse of the routine above. */
    int y = st->wYear; unsigned m = st->wMonth, d = st->wDay;
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + (int64_t)doe - 719468;
    uint64_t secs = (uint64_t)(days * 86400 + st->wHour * 3600 + st->wMinute * 60 + st->wSecond);
    uint64_t t = EPOCH_DIFF_100NS + secs * 10000000ULL + st->wMilliseconds * 10000ULL;
    ft->dwLowDateTime = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
    return WIN_TRUE;
}

static BOOL WINAPI w_FileTimeToSystemTime(const FILETIME *ft, SYSTEMTIME *st) {
    if (!ft || !st) return WIN_FALSE;
    uint64_t t = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (t < EPOCH_DIFF_100NS) { memset(st, 0, sizeof *st); return WIN_FALSE; }
    t -= EPOCH_DIFF_100NS;
    fill_systemtime(st, t / 10000000ULL, (unsigned)((t / 10000ULL) % 1000));
    return WIN_TRUE;
}
static BOOL WINAPI w_FileTimeToLocalFileTime(const FILETIME *in, FILETIME *out) {
    if (!in || !out) return WIN_FALSE; *out = *in; return WIN_TRUE;
}
static BOOL WINAPI w_LocalFileTimeToFileTime(const FILETIME *in, FILETIME *out) {
    if (!in || !out) return WIN_FALSE; *out = *in; return WIN_TRUE;
}
static DWORD WINAPI w_GetTimeZoneInformation(void *tz) { (void)tz; return 0; /* UTC */ }

static DWORD WINAPI w_GetTickCount(void) { return (DWORD)uptime_ms(); }
static ULONGLONG WINAPI w_GetTickCount64(void) { return uptime_ms(); }

static BOOL WINAPI w_QueryPerformanceCounter(int64_t *out) {
    if (out) *out = (int64_t)uptime_ms();
    return WIN_TRUE;
}
static BOOL WINAPI w_QueryPerformanceFrequency(int64_t *out) {
    if (out) *out = 1000;                 /* the counter above is milliseconds */
    return WIN_TRUE;
}

/* --------------------------------------------------------------- the system */

typedef struct {
    WORD  wProcessorArchitecture, wReserved;
    DWORD dwPageSize;
    void *lpMinimumApplicationAddress, *lpMaximumApplicationAddress;
    UINT_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity;
    WORD  wProcessorLevel, wProcessorRevision;
} SYSTEM_INFO;

static void WINAPI w_GetSystemInfo(SYSTEM_INFO *si) {
    if (!si) return;
    memset(si, 0, sizeof *si);
    si->wProcessorArchitecture = 9;              /* AMD64 */
    si->dwPageSize = 4096;
    si->lpMinimumApplicationAddress = (void *)0x10000;
    si->lpMaximumApplicationAddress = (void *)0x00007FF000000000ULL;
    si->dwActiveProcessorMask = 1;
    si->dwNumberOfProcessors = 1;
    si->dwProcessorType = 8664;
    si->dwAllocationGranularity = 65536;
    si->wProcessorLevel = 6;
}
static void WINAPI w_GetNativeSystemInfo(SYSTEM_INFO *si) { w_GetSystemInfo(si); }

/* The version a program is told it is running on.  Claiming to be Windows 10
 * is what makes a modern program agree to start; anything older and half of
 * them refuse before they have done anything. */
static DWORD WINAPI w_GetVersion(void) { return 0x0A00 | (0x0000 << 16); }

typedef struct {
    DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId;
    char  szCSDVersion[128];
} OSVERSIONINFOA;

static BOOL WINAPI w_GetVersionExA(OSVERSIONINFOA *vi) {
    if (!vi || vi->dwOSVersionInfoSize < sizeof *vi) return WIN_FALSE;
    vi->dwMajorVersion = 10;
    vi->dwMinorVersion = 0;
    vi->dwBuildNumber = 19045;
    vi->dwPlatformId = 2;                        /* VER_PLATFORM_WIN32_NT */
    strlcpy(vi->szCSDVersion, "", sizeof vi->szCSDVersion);
    return WIN_TRUE;
}

static BOOL WINAPI w_IsProcessorFeaturePresent(DWORD feature) {
    /* 0 floating point, 10 SSE2, 12 SSE3 - all present on anything this can
     * run on.  Anything else is answered honestly with no. */
    return (feature == 0 || feature == 10 || feature == 12 || feature == 6) ? WIN_TRUE : WIN_FALSE;
}
static BOOL WINAPI w_IsWow64Process(HANDLE h, BOOL *out) {
    (void)h; if (out) *out = WIN_FALSE; return WIN_TRUE;
}

static DWORD WINAPI w_GetSystemDirectoryA(char *out, UINT cap) {
    return (DWORD)strlcpy(out, "C:\\windows\\system32", cap);
}
static DWORD WINAPI w_GetWindowsDirectoryA(char *out, UINT cap) {
    return (DWORD)strlcpy(out, "C:\\windows", cap);
}
static BOOL WINAPI w_GetComputerNameA(char *out, DWORD *cap) {
    const char *n = "KESTREL";
    if (!out || !cap || *cap < strlen(n) + 1) { if (cap) *cap = 8; return WIN_FALSE; }
    strlcpy(out, n, *cap);
    *cap = (DWORD)strlen(n);
    return WIN_TRUE;
}

static BOOL WINAPI w_IsDebuggerPresent(void) { return WIN_FALSE; }
static void WINAPI w_DebugBreak(void) { }
static void WINAPI w_OutputDebugStringA(const char *s) { win_trace("%s", s ? s : ""); }
static void WINAPI w_OutputDebugStringW(const WCHAR *s) {
    char buf[512]; win_wide_to_utf8(s, buf, sizeof buf); win_trace("%s", buf);
}
static BOOL WINAPI w_Beep(DWORD freq, DWORD ms) { (void)freq; (void)ms; return WIN_TRUE; }

/* ------------------------------------------------------------- text helpers */

static int WINAPI w_lstrlenA(const char *s) { return s ? (int)strlen(s) : 0; }
static int WINAPI w_lstrlenW(const WCHAR *s) {
    int n = 0; if (s) while (s[n]) n++; return n;
}
static char *WINAPI w_lstrcpyA(char *d, const char *s) {
    if (d && s) strcpy(d, s); return d;
}
static char *WINAPI w_lstrcpynA(char *d, const char *s, int cap) {
    if (d && s && cap > 0) strlcpy(d, s, (size_t)cap); return d;
}
static char *WINAPI w_lstrcatA(char *d, const char *s) {
    if (d && s) strcat(d, s); return d;
}
static int WINAPI w_lstrcmpA(const char *a, const char *b) { return strcmp(a ? a : "", b ? b : ""); }
static int WINAPI w_lstrcmpiA(const char *a, const char *b) { return strcasecmp(a ? a : "", b ? b : ""); }

static UINT WINAPI w_GetACP(void) { return 65001; }         /* UTF-8 */
static UINT WINAPI w_GetOEMCP(void) { return 65001; }
static BOOL WINAPI w_IsDBCSLeadByte(BYTE b) { (void)b; return WIN_FALSE; }
static BOOL WINAPI w_GetCPInfo(UINT cp, void *info) {
    (void)cp;
    if (!info) return WIN_FALSE;
    /* MaxCharSize 1, then the default character and the lead byte ranges. */
    memset(info, 0, 24);
    *(UINT *)info = 1;
    ((BYTE *)info)[4] = '?';
    return WIN_TRUE;
}

static int WINAPI w_MultiByteToWideChar(UINT cp, DWORD flags, const char *src, int srclen,
                                        WCHAR *dst, int dstlen) {
    (void)cp; (void)flags;
    if (!src) return 0;
    int n = srclen < 0 ? (int)strlen(src) + 1 : srclen;
    if (!dstlen) return n;
    if (!dst || n > dstlen) { win_set_error(122 /* ERROR_INSUFFICIENT_BUFFER */); return 0; }
    for (int i = 0; i < n; i++) dst[i] = (WCHAR)(unsigned char)src[i];
    return n;
}

static int WINAPI w_WideCharToMultiByte(UINT cp, DWORD flags, const WCHAR *src, int srclen,
                                        char *dst, int dstlen, const char *dflt, BOOL *used) {
    (void)cp; (void)flags; (void)dflt;
    if (used) *used = WIN_FALSE;
    if (!src) return 0;
    int n = srclen;
    if (n < 0) { n = 0; while (src[n]) n++; n++; }
    if (!dstlen) return n;
    if (!dst || n > dstlen) { win_set_error(122); return 0; }
    for (int i = 0; i < n; i++) dst[i] = (char)(src[i] < 0x80 ? src[i] : '?');
    return n;
}

static int WINAPI w_CompareStringA(DWORD locale, DWORD flags, const char *a, int alen,
                                   const char *b, int blen) {
    (void)locale;
    char ba[256], bb[256];
    if (alen < 0) strlcpy(ba, a, sizeof ba); else { int n = alen < 255 ? alen : 255; memcpy(ba, a, (size_t)n); ba[n] = 0; }
    if (blen < 0) strlcpy(bb, b, sizeof bb); else { int n = blen < 255 ? blen : 255; memcpy(bb, b, (size_t)n); bb[n] = 0; }
    int r = (flags & 1) ? strcasecmp(ba, bb) : strcmp(ba, bb);
    return r < 0 ? 1 : (r > 0 ? 3 : 2);          /* LESS, EQUAL, GREATER */
}

static DWORD WINAPI w_FormatMessageA(DWORD flags, const void *src, DWORD id, DWORD lang,
                                     char *out, DWORD cap, void *args) {
    (void)flags; (void)src; (void)lang; (void)args;
    const char *text;
    switch (id) {
    case ERROR_FILE_NOT_FOUND: text = "The system cannot find the file specified."; break;
    case ERROR_PATH_NOT_FOUND: text = "The system cannot find the path specified."; break;
    case ERROR_ACCESS_DENIED:  text = "Access is denied."; break;
    case ERROR_INVALID_HANDLE: text = "The handle is invalid."; break;
    case ERROR_NOT_ENOUGH_MEMORY: text = "Not enough memory is available."; break;
    case ERROR_INVALID_PARAMETER: text = "The parameter is incorrect."; break;
    case ERROR_SUCCESS:        text = "The operation completed successfully."; break;
    default:                   text = "An error occurred."; break;
    }
    if (!out || !cap) return 0;
    return (DWORD)strlcpy(out, text, cap);
}

/* Nothing here starts another program yet: there is no Windows process model
 * to start it into.  Saying so is better than pretending it worked and
 * leaving the caller to wonder why nothing happened. */
/* What a caller is handed back when a program starts.  Windows gives out two
 * handles and two numbers; the thread half is the program's first thread,
 * which here is the program itself. */
typedef struct {
    HANDLE process, thread;
    DWORD  process_id, thread_id;
} PROCESS_INFORMATION;

/* Splitting a command line the way Windows does: spaces separate, quotes hold
 * a run together, and the first word is the program unless one was named
 * separately.  Nothing here handles the backslash rules before a quote, which
 * matter only for paths written into a quoted argument. */
static int split_command(char *line, char **argv, int max) {
    int count = 0;
    char *at = line;
    while (*at && count < max) {
        while (*at == ' ' || *at == '	') at++;
        if (!*at) break;

        char *start;
        if (*at == '"') {
            start = ++at;
            while (*at && *at != '"') at++;
        } else {
            start = at;
            while (*at && *at != ' ' && *at != '	') at++;
        }
        if (*at) *at++ = 0;
        argv[count++] = start;
    }
    return count;
}

static BOOL WINAPI w_CreateProcessA(const char *app, char *cmd, void *pa, void *ta, BOOL inherit,
                                    DWORD flags, void *env, const char *dir, void *si, void *pi) {
    (void)pa; (void)ta; (void)inherit; (void)flags; (void)env; (void)dir;
    (void)si;

    if (!pi) { win_set_error(ERROR_INVALID_PARAMETER); return WIN_FALSE; }

    /* The command line is taken apart in a copy: Windows is allowed to write
     * into the caller's buffer, but doing so to a string the caller may look
     * at again is a surprise not worth having. */
    char line[512];
    if (app && *app) strlcpy(line, app, sizeof line);
    else if (cmd && *cmd) strlcpy(line, cmd, sizeof line);
    else { win_set_error(ERROR_INVALID_PARAMETER); return WIN_FALSE; }

    char *argv[24];
    int argc = split_command(line, argv, 24);
    if (!argc) { win_set_error(ERROR_INVALID_PARAMETER); return WIN_FALSE; }

    /* If a program was named separately, the command line is still where the
     * arguments come from - so it is split too and its first word dropped. */
    char rest[512];
    if (app && *app && cmd && *cmd) {
        strlcpy(rest, cmd, sizeof rest);
        char *from[24];
        int n = split_command(rest, from, 24);
        for (int i = 1; i < n && argc < 24; i++) argv[argc++] = from[i];
    }

    /* Where to look.  A bare name is a program in the place programs live,
     * and a Windows program is expected to end in .exe whether the caller
     * said so or not. */
    char path[512];
    if (argv[0][0] == '/') strlcpy(path, argv[0], sizeof path);
    else snprintf(path, sizeof path, "/bin/%s", argv[0]);

    int pid = spawn(path, (const char *const *)argv, argc);
    if (pid < 0) {
        size_t len = strlen(path);
        if (len < sizeof path - 5 &&
            (len < 4 || strcmp(path + len - 4, ".exe") != 0)) {
            strlcat(path, ".exe", sizeof path);
            pid = spawn(path, (const char *const *)argv, argc);
        }
    }

    /* A program built for Windows is not something this system can start on
     * its own - it is started the way this program was, by the loader that
     * knows how to read one.  So if starting it directly did not work, it is
     * handed to the loader instead, with the program itself as the first
     * thing the loader is told about. */
    if (pid < 0) {
        const char *relay[26];
        int n = 0;
        relay[n++] = "winrun";
        relay[n++] = path;
        for (int i = 1; i < argc && n < 26; i++) relay[n++] = argv[i];
        pid = spawn("/bin/winrun", relay, n);
    }

    if (pid < 0) {
        win_set_error(ERROR_FILE_NOT_FOUND);
        return WIN_FALSE;
    }

    object_t *o = NULL;
    HANDLE h = obj_alloc(OBJ_PROCESS, &o);
    if (!h) {
        /* Started and now unreachable, which is worse than not starting: stop
         * it rather than leave it running with nobody able to wait for it. */
        kill(pid);
        win_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return WIN_FALSE;
    }
    o->tid = pid;
    o->exit_code = 259;                  /* still running */
    o->signalled = 0;

    PROCESS_INFORMATION *out = (PROCESS_INFORMATION *)pi;
    out->process = h;
    out->thread = h;                     /* the program is its own first thread */
    out->process_id = (DWORD)pid;
    out->thread_id = (DWORD)pid;
    return WIN_TRUE;
}

/* How a program that has finished finished.  Before it has, the answer is the
 * number Windows uses to mean exactly that, which is what a caller polling in
 * a loop is looking for. */
static BOOL WINAPI w_GetExitCodeProcess(HANDLE h, DWORD *code) {
    object_t *o = obj_get(h, OBJ_PROCESS);
    if (!o || !code) { win_set_error(ERROR_INVALID_HANDLE); return WIN_FALSE; }
    *code = o->exit_code;
    return WIN_TRUE;
}

typedef struct {
    DWORD cb; char *lpReserved, *lpDesktop, *lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow, cbReserved2;
    BYTE *lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOA;

static void WINAPI w_GetStartupInfoA(STARTUPINFOA *si) {
    if (!si) return;
    memset(si, 0, sizeof *si);
    si->cb = sizeof *si;
    si->hStdInput = H_STDIN;
    si->hStdOutput = H_STDOUT;
    si->hStdError = H_STDERR;
    si->wShowWindow = 1;                          /* SW_SHOWNORMAL */
}
static void WINAPI w_GetStartupInfoW(void *si) { w_GetStartupInfoA(si); }

/* ---------------------------------------------------------------- the table */

/* Declared in the files that implement them. */
extern const win_export_t k32_file_exports[];
extern const win_export_t k32_thread_exports[];

static const win_export_t kernel32_core[] = {
    { "ExitProcess",                (void *)w_ExitProcess },
    { "TerminateProcess",           (void *)w_TerminateProcess },
    { "GetCurrentProcess",          (void *)w_GetCurrentProcess },
    { "GetCurrentProcessId",        (void *)w_GetCurrentProcessId },
    { "GetCommandLineA",            (void *)w_GetCommandLineA },
    { "GetCommandLineW",            (void *)w_GetCommandLineW },
    { "GetLastError",               (void *)w_GetLastError },
    { "SetLastError",               (void *)w_SetLastError },
    { "CreateProcessA",             (void *)w_CreateProcessA },
    { "GetExitCodeProcess",         (void *)w_GetExitCodeProcess },
    { "GetStartupInfoA",            (void *)w_GetStartupInfoA },
    { "GetStartupInfoW",            (void *)w_GetStartupInfoW },

    { "GetProcessHeap",             (void *)w_GetProcessHeap },
    { "HeapCreate",                 (void *)w_HeapCreate },
    { "HeapDestroy",                (void *)w_HeapDestroy },
    { "HeapAlloc",                  (void *)w_HeapAlloc },
    { "HeapFree",                   (void *)w_HeapFree },
    { "HeapReAlloc",                (void *)w_HeapReAlloc },
    { "HeapSize",                   (void *)w_HeapSize },
    { "HeapValidate",               (void *)w_HeapValidate },
    { "HeapSetInformation",         (void *)w_HeapSetInformation },
    { "VirtualAlloc",               (void *)w_VirtualAlloc },
    { "VirtualFree",                (void *)w_VirtualFree },
    { "VirtualProtect",             (void *)w_VirtualProtect },
    { "VirtualQuery",               (void *)w_VirtualQuery },
    { "GlobalAlloc",                (void *)w_GlobalAlloc },
    { "GlobalFree",                 (void *)w_GlobalFree },
    { "GlobalLock",                 (void *)w_GlobalLock },
    { "GlobalUnlock",               (void *)w_GlobalUnlock },
    { "GlobalSize",                 (void *)w_GlobalSize },
    { "GlobalReAlloc",              (void *)w_GlobalReAlloc },
    { "LocalAlloc",                 (void *)w_GlobalAlloc },
    { "LocalFree",                  (void *)w_GlobalFree },
    { "LocalReAlloc",               (void *)w_GlobalReAlloc },

    { "GetModuleHandleA",           (void *)w_GetModuleHandleA },
    { "GetModuleHandleW",           (void *)w_GetModuleHandleW },
    { "GetModuleHandleExA",         (void *)w_GetModuleHandleExA },
    { "GetModuleHandleExW",         (void *)w_GetModuleHandleExA },
    { "GetModuleFileNameA",         (void *)w_GetModuleFileNameA },
    { "GetModuleFileNameW",         (void *)w_GetModuleFileNameW },
    { "LoadLibraryA",               (void *)w_LoadLibraryA },
    { "LoadLibraryW",               (void *)w_LoadLibraryW },
    { "LoadLibraryExA",             (void *)w_LoadLibraryExA },
    { "LoadLibraryExW",             (void *)w_LoadLibraryExW },
    { "FreeLibrary",                (void *)w_FreeLibrary },
    { "GetProcAddress",             (void *)w_GetProcAddress },

    { "GetEnvironmentStrings",      (void *)w_GetEnvironmentStrings },
    { "GetEnvironmentStringsA",     (void *)w_GetEnvironmentStrings },
    { "GetEnvironmentStringsW",     (void *)w_GetEnvironmentStringsW },
    { "FreeEnvironmentStringsA",    (void *)w_FreeEnvironmentStringsA },
    { "FreeEnvironmentStringsW",    (void *)w_FreeEnvironmentStringsA },
    { "GetEnvironmentVariableA",    (void *)w_GetEnvironmentVariableA },
    { "GetEnvironmentVariableW",    (void *)w_GetEnvironmentVariableW },
    { "SetEnvironmentVariableA",    (void *)w_SetEnvironmentVariableA },
    { "ExpandEnvironmentStringsA",  (void *)w_ExpandEnvironmentStringsA },

    { "GetSystemTime",              (void *)w_GetSystemTime },
    { "GetLocalTime",               (void *)w_GetLocalTime },
    { "GetSystemTimeAsFileTime",    (void *)w_GetSystemTimeAsFileTime },
    { "GetSystemTimePreciseAsFileTime", (void *)w_GetSystemTimePreciseAsFileTime },
    { "SystemTimeToFileTime",       (void *)w_SystemTimeToFileTime },
    { "FileTimeToSystemTime",       (void *)w_FileTimeToSystemTime },
    { "FileTimeToLocalFileTime",    (void *)w_FileTimeToLocalFileTime },
    { "LocalFileTimeToFileTime",    (void *)w_LocalFileTimeToFileTime },
    { "GetTimeZoneInformation",     (void *)w_GetTimeZoneInformation },
    { "GetTickCount",               (void *)w_GetTickCount },
    { "GetTickCount64",             (void *)w_GetTickCount64 },
    { "QueryPerformanceCounter",    (void *)w_QueryPerformanceCounter },
    { "QueryPerformanceFrequency",  (void *)w_QueryPerformanceFrequency },

    { "GetSystemInfo",              (void *)w_GetSystemInfo },
    { "GetNativeSystemInfo",        (void *)w_GetNativeSystemInfo },
    { "GetVersion",                 (void *)w_GetVersion },
    { "GetVersionExA",              (void *)w_GetVersionExA },
    { "IsProcessorFeaturePresent",  (void *)w_IsProcessorFeaturePresent },
    { "IsWow64Process",             (void *)w_IsWow64Process },
    { "GetSystemDirectoryA",        (void *)w_GetSystemDirectoryA },
    { "GetWindowsDirectoryA",       (void *)w_GetWindowsDirectoryA },
    { "GetComputerNameA",           (void *)w_GetComputerNameA },
    { "IsDebuggerPresent",          (void *)w_IsDebuggerPresent },
    { "DebugBreak",                 (void *)w_DebugBreak },
    { "OutputDebugStringA",         (void *)w_OutputDebugStringA },
    { "OutputDebugStringW",         (void *)w_OutputDebugStringW },
    { "Beep",                       (void *)w_Beep },

    { "lstrlenA",                   (void *)w_lstrlenA },
    { "lstrlenW",                   (void *)w_lstrlenW },
    { "lstrcpyA",                   (void *)w_lstrcpyA },
    { "lstrcpynA",                  (void *)w_lstrcpynA },
    { "lstrcatA",                   (void *)w_lstrcatA },
    { "lstrcmpA",                   (void *)w_lstrcmpA },
    { "lstrcmpiA",                  (void *)w_lstrcmpiA },
    { "GetACP",                     (void *)w_GetACP },
    { "GetOEMCP",                   (void *)w_GetOEMCP },
    { "GetCPInfo",                  (void *)w_GetCPInfo },
    { "IsDBCSLeadByte",             (void *)w_IsDBCSLeadByte },
    { "MultiByteToWideChar",        (void *)w_MultiByteToWideChar },
    { "WideCharToMultiByte",        (void *)w_WideCharToMultiByte },
    { "CompareStringA",             (void *)w_CompareStringA },
    { "FormatMessageA",             (void *)w_FormatMessageA },
    { NULL, NULL }
};

/* kernel32 is assembled from three files; they are stitched into one table so
 * that a program importing from any of them sees a single library. */
static win_export_t kernel32[512];

static void append_table(int *n, const win_export_t *t) {
    for (const win_export_t *e = t; e->name; e++) {
        if (*n >= (int)(sizeof kernel32 / sizeof kernel32[0]) - 1) return;
        kernel32[(*n)++] = *e;
    }
}

void k32_init(void) {
    int n = 0;
    append_table(&n, kernel32_core);
    append_table(&n, k32_file_exports);
    append_table(&n, k32_thread_exports);
    kernel32[n].name = NULL;
    kernel32[n].fn = NULL;

    win_register("kernel32.dll", kernel32);
    win_register("kernelbase.dll", kernel32);
    win_register("ntdll.dll", kernel32);
}

/* Shared with the file and thread halves. */
DWORD win_error_from_errno(int e) { return error_from_errno(e); }

/* The C runtime asks for environment variables through its own getenv, which
 * has to see the same table this does. */
DWORD win_env_lookup(const char *name, char *out, DWORD cap) {
    const char *v = name ? env_find(name) : NULL;
    if (!v) return 0;
    return (DWORD)strlcpy(out, v, cap);
}
