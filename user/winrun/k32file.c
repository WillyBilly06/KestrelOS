/* k32file.c - kernel32: files, directories and the console.
 *
 * Windows and this system agree on what a file is and disagree on nearly
 * everything around it: the spelling of a path, the shape of a handle, whether
 * a directory walk returns "." first, and what a program is told when it asks
 * for something that is not there.  All of that is translated here.
 */
#include "win.h"

DWORD win_error_from_errno(int e);

/* ------------------------------------------------------------------ flags */

#define GENERIC_READ            0x80000000
#define GENERIC_WRITE           0x40000000

#define CREATE_NEW              1
#define CREATE_ALWAYS           2
#define OPEN_EXISTING           3
#define OPEN_ALWAYS             4
#define TRUNCATE_EXISTING       5

#define FILE_ATTRIBUTE_READONLY   0x0001
#define FILE_ATTRIBUTE_DIRECTORY  0x0010
#define FILE_ATTRIBUTE_ARCHIVE    0x0020
#define FILE_ATTRIBUTE_NORMAL     0x0080
#define INVALID_FILE_ATTRIBUTES   0xFFFFFFFF
#define INVALID_FILE_SIZE         0xFFFFFFFF
#define INVALID_SET_FILE_POINTER  0xFFFFFFFF

#define FILE_BEGIN   0
#define FILE_CURRENT 1
#define FILE_END     2

/* --------------------------------------------------------------- handles */

/* The three standard streams are answered without a table entry, so that a
 * program which writes to handle 0x11 directly still reaches the console. */
/* The bit bucket.  A program that writes its log there means it, and sending
 * it to the console instead would fill the screen with what the program asked
 * to have thrown away. */
#define H_NULL ((HANDLE)(uintptr_t)0x13)

static int fd_of(HANDLE h) {
    if (h == H_STDIN)  return STDIN_FD;
    if (h == H_STDOUT) return STDOUT_FD;
    if (h == H_STDERR) return STDERR_FD;
    object_t *o = obj_get(h, OBJ_FILE);
    return o ? o->fd : -1;
}

static HANDLE WINAPI w_GetStdHandle(DWORD which) {
    switch (which) {
    case (DWORD)-10: return H_STDIN;
    case (DWORD)-11: return H_STDOUT;
    case (DWORD)-12: return H_STDERR;
    default: return H_INVALID;
    }
}
static BOOL WINAPI w_SetStdHandle(DWORD which, HANDLE h) { (void)which; (void)h; return WIN_TRUE; }

static BOOL WINAPI w_CloseHandle(HANDLE h) {
    if (h == H_STDIN || h == H_STDOUT || h == H_STDERR || h == H_NULL) return WIN_TRUE;
    if (!obj_any(h)) { win_set_error(ERROR_INVALID_HANDLE); return WIN_FALSE; }
    obj_release(h);
    return WIN_TRUE;
}

static BOOL WINAPI w_DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *out,
                                     DWORD access, BOOL inherit, DWORD options) {
    (void)sp; (void)tp; (void)access; (void)inherit; (void)options;
    object_t *o = obj_any(src);
    if (!o) {
        /* A standard stream duplicates to itself, which is all a program that
         * duplicates one ever wanted. */
        if (src == H_STDIN || src == H_STDOUT || src == H_STDERR) { if (out) *out = src; return WIN_TRUE; }
        win_set_error(ERROR_INVALID_HANDLE);
        return WIN_FALSE;
    }
    o->refs++;
    if (out) *out = src;
    return WIN_TRUE;
}

/* ------------------------------------------------------------------ files */

static HANDLE WINAPI w_CreateFileA(const char *name, DWORD access, DWORD share, void *sa,
                                   DWORD disposition, DWORD attrs, HANDLE templ) {
    (void)share; (void)sa; (void)attrs; (void)templ;
    if (!name) { win_set_error(ERROR_INVALID_PARAMETER); return H_INVALID; }

    /* The console appears as a file called CON or CONOUT$ in old programs. */
    if (!strcasecmp(name, "CON") || !strcasecmp(name, "CONOUT$")) return H_STDOUT;
    if (!strcasecmp(name, "CONIN$")) return H_STDIN;
    if (!strcasecmp(name, "NUL")) return H_NULL;

    char host[512];
    win_path_to_host(name, host, sizeof host);

    bool want_read = (access & GENERIC_READ) != 0;
    bool want_write = (access & GENERIC_WRITE) != 0;
    if (!want_read && !want_write) want_read = true;

    bool exists = file_exists(host);
    int flags = want_write ? (want_read ? O_RDWR : O_WRONLY) : O_RDONLY;

    switch (disposition) {
    case CREATE_NEW:
        if (exists) { win_set_error(ERROR_FILE_EXISTS); return H_INVALID; }
        flags |= O_CREAT;
        break;
    case CREATE_ALWAYS:
        flags |= O_CREAT | O_TRUNC;
        break;
    case OPEN_EXISTING:
        if (!exists) { win_set_error(ERROR_FILE_NOT_FOUND); return H_INVALID; }
        break;
    case OPEN_ALWAYS:
        flags |= O_CREAT;
        break;
    case TRUNCATE_EXISTING:
        if (!exists) { win_set_error(ERROR_FILE_NOT_FOUND); return H_INVALID; }
        flags |= O_TRUNC;
        break;
    default:
        win_set_error(ERROR_INVALID_PARAMETER);
        return H_INVALID;
    }

    int fd = open(host, flags);
    if (fd < 0) { win_set_error(win_error_from_errno(errno)); return H_INVALID; }

    object_t *o;
    HANDLE h = obj_alloc(OBJ_FILE, &o);
    if (!h) { close(fd); win_set_error(ERROR_NOT_ENOUGH_MEMORY); return H_INVALID; }
    o->fd = fd;

    /* OPEN_ALWAYS and CREATE_ALWAYS both report whether the file was already
     * there, which is how a program tells "created" from "reopened". */
    win_set_error((disposition == OPEN_ALWAYS || disposition == CREATE_ALWAYS) && exists
                  ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
    return h;
}

static HANDLE WINAPI w_CreateFileW(const WCHAR *name, DWORD access, DWORD share, void *sa,
                                   DWORD disposition, DWORD attrs, HANDLE templ) {
    char buf[512];
    win_wide_to_utf8(name, buf, sizeof buf);
    return w_CreateFileA(buf, access, share, sa, disposition, attrs, templ);
}

static BOOL WINAPI w_ReadFile(HANDLE h, void *buf, DWORD len, DWORD *got, void *overlapped) {
    (void)overlapped;
    if (h == H_NULL) { if (got) *got = 0; return WIN_TRUE; }   /* always at the end */
    int fd = fd_of(h);
    if (fd < 0) { win_set_error(ERROR_INVALID_HANDLE); return WIN_FALSE; }
    ssize_t n = read(fd, buf, len);
    if (n < 0) { win_set_error(win_error_from_errno(errno)); if (got) *got = 0; return WIN_FALSE; }
    if (got) *got = (DWORD)n;
    return WIN_TRUE;
}

static BOOL WINAPI w_WriteFile(HANDLE h, const void *buf, DWORD len, DWORD *put, void *overlapped) {
    (void)overlapped;
    if (h == H_NULL) { if (put) *put = len; return WIN_TRUE; }
    int fd = fd_of(h);
    if (fd < 0) { win_set_error(ERROR_INVALID_HANDLE); return WIN_FALSE; }
    ssize_t n = write(fd, buf, len);
    if (n < 0) { win_set_error(win_error_from_errno(errno)); if (put) *put = 0; return WIN_FALSE; }
    if (put) *put = (DWORD)n;
    return WIN_TRUE;
}

static DWORD WINAPI w_SetFilePointer(HANDLE h, LONG low, LONG *high, DWORD whence) {
    int fd = fd_of(h);
    if (fd < 0) { win_set_error(ERROR_INVALID_HANDLE); return INVALID_SET_FILE_POINTER; }
    int64_t off = high ? (((int64_t)*high << 32) | (uint32_t)low) : low;
    off_t r = lseek(fd, off, (int)whence);
    if (r < 0) { win_set_error(win_error_from_errno(errno)); return INVALID_SET_FILE_POINTER; }
    if (high) *high = (LONG)((uint64_t)r >> 32);
    return (DWORD)r;
}

static BOOL WINAPI w_SetFilePointerEx(HANDLE h, int64_t off, int64_t *out, DWORD whence) {
    int fd = fd_of(h);
    if (fd < 0) { win_set_error(ERROR_INVALID_HANDLE); return WIN_FALSE; }
    off_t r = lseek(fd, off, (int)whence);
    if (r < 0) { win_set_error(win_error_from_errno(errno)); return WIN_FALSE; }
    if (out) *out = r;
    return WIN_TRUE;
}

static DWORD WINAPI w_GetFileSize(HANDLE h, DWORD *high) {
    int fd = fd_of(h);
    if (fd < 0) { win_set_error(ERROR_INVALID_HANDLE); return INVALID_FILE_SIZE; }
    off_t here = lseek(fd, 0, SEEK_CUR);
    off_t end = lseek(fd, 0, SEEK_END);
    lseek(fd, here, SEEK_SET);
    if (end < 0) { win_set_error(win_error_from_errno(errno)); return INVALID_FILE_SIZE; }
    if (high) *high = (DWORD)((uint64_t)end >> 32);
    return (DWORD)end;
}

static BOOL WINAPI w_GetFileSizeEx(HANDLE h, int64_t *out) {
    DWORD high = 0;
    DWORD low = w_GetFileSize(h, &high);
    if (low == INVALID_FILE_SIZE && win_get_error() != ERROR_SUCCESS) return WIN_FALSE;
    if (out) *out = ((int64_t)high << 32) | low;
    return WIN_TRUE;
}

static BOOL WINAPI w_SetEndOfFile(HANDLE h) {
    int fd = fd_of(h);
    if (fd < 0) { win_set_error(ERROR_INVALID_HANDLE); return WIN_FALSE; }
    off_t here = lseek(fd, 0, SEEK_CUR);
    if (here < 0 || ftruncate(fd, (uint64_t)here) < 0) {
        win_set_error(win_error_from_errno(errno));
        return WIN_FALSE;
    }
    return WIN_TRUE;
}

static BOOL WINAPI w_FlushFileBuffers(HANDLE h) { (void)h; sync(); return WIN_TRUE; }

/* 1 is a disk file, 2 a character device.  A program checks this to decide
 * whether its output is going somewhere it can seek. */
static DWORD WINAPI w_GetFileType(HANDLE h) {
    if (h == H_STDIN || h == H_STDOUT || h == H_STDERR) return 2;
    return obj_get(h, OBJ_FILE) ? 1 : 0;
}

static BOOL WINAPI w_GetFileTime(HANDLE h, FILETIME *created, FILETIME *accessed, FILETIME *written) {
    (void)h;
    /* The file system here does not keep timestamps, and inventing plausible
     * ones would be worse than admitting it: a program that sorts by date
     * would get a confident wrong answer. */
    FILETIME zero = { 0, 0 };
    if (created) *created = zero;
    if (accessed) *accessed = zero;
    if (written) *written = zero;
    return WIN_TRUE;
}

static BOOL WINAPI w_DeleteFileA(const char *name) {
    char host[512];
    win_path_to_host(name, host, sizeof host);
    if (unlink(host) < 0) { win_set_error(win_error_from_errno(errno)); return WIN_FALSE; }
    return WIN_TRUE;
}
static BOOL WINAPI w_DeleteFileW(const WCHAR *name) {
    char buf[512]; win_wide_to_utf8(name, buf, sizeof buf); return w_DeleteFileA(buf);
}

static BOOL WINAPI w_MoveFileA(const char *from, const char *to) {
    char a[512], b[512];
    win_path_to_host(from, a, sizeof a);
    win_path_to_host(to, b, sizeof b);
    if (rename(a, b) < 0) { win_set_error(win_error_from_errno(errno)); return WIN_FALSE; }
    return WIN_TRUE;
}
static BOOL WINAPI w_MoveFileExA(const char *from, const char *to, DWORD flags) {
    (void)flags; return w_MoveFileA(from, to);
}

static BOOL WINAPI w_CopyFileA(const char *from, const char *to, BOOL fail_if_exists) {
    char a[512], b[512];
    win_path_to_host(from, a, sizeof a);
    win_path_to_host(to, b, sizeof b);
    if (fail_if_exists && file_exists(b)) { win_set_error(ERROR_FILE_EXISTS); return WIN_FALSE; }
    if (copy_file(a, b) < 0) { win_set_error(win_error_from_errno(errno)); return WIN_FALSE; }
    return WIN_TRUE;
}

static BOOL WINAPI w_CreateDirectoryA(const char *name, void *sa) {
    (void)sa;
    char host[512];
    win_path_to_host(name, host, sizeof host);
    if (mkdir(host) < 0) { win_set_error(win_error_from_errno(errno)); return WIN_FALSE; }
    return WIN_TRUE;
}
static BOOL WINAPI w_CreateDirectoryW(const WCHAR *name, void *sa) {
    char buf[512]; win_wide_to_utf8(name, buf, sizeof buf); return w_CreateDirectoryA(buf, sa);
}
static BOOL WINAPI w_RemoveDirectoryA(const char *name) {
    char host[512];
    win_path_to_host(name, host, sizeof host);
    if (rmdir(host) < 0) { win_set_error(win_error_from_errno(errno)); return WIN_FALSE; }
    return WIN_TRUE;
}

static DWORD WINAPI w_GetFileAttributesA(const char *name) {
    char host[512];
    win_path_to_host(name, host, sizeof host);
    kstat_t st;
    if (stat(host, &st) < 0) { win_set_error(win_error_from_errno(errno)); return INVALID_FILE_ATTRIBUTES; }
    return st.type == FT_DIR ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
}
static DWORD WINAPI w_GetFileAttributesW(const WCHAR *name) {
    char buf[512]; win_wide_to_utf8(name, buf, sizeof buf); return w_GetFileAttributesA(buf);
}
static BOOL WINAPI w_SetFileAttributesA(const char *name, DWORD attrs) {
    (void)name; (void)attrs; return WIN_TRUE;      /* nothing here to set */
}

typedef struct {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD    nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;

static BOOL WINAPI w_GetFileAttributesExA(const char *name, int level,
                                          WIN32_FILE_ATTRIBUTE_DATA *out) {
    (void)level;
    if (!out) return WIN_FALSE;
    char host[512];
    win_path_to_host(name, host, sizeof host);
    kstat_t st;
    if (stat(host, &st) < 0) { win_set_error(win_error_from_errno(errno)); return WIN_FALSE; }
    memset(out, 0, sizeof *out);
    out->dwFileAttributes = st.type == FT_DIR ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    out->nFileSizeLow = (DWORD)st.size;
    out->nFileSizeHigh = (DWORD)(st.size >> 32);
    return WIN_TRUE;
}

/* ------------------------------------------------------- the directory walk */

typedef struct {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD    nFileSizeHigh, nFileSizeLow;
    DWORD    dwReserved0, dwReserved1;
    char     cFileName[260];
    char     cAlternateFileName[14];
} WIN32_FIND_DATAA;

typedef struct {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD    nFileSizeHigh, nFileSizeLow;
    DWORD    dwReserved0, dwReserved1;
    WCHAR    cFileName[260];
    WCHAR    cAlternateFileName[14];
} WIN32_FIND_DATAW;

/* The wildcards a Windows program uses are '*' for any run of characters and
 * '?' for exactly one.  Matching is case-insensitive, as it is on Windows. */
static bool wildcard_match(const char *pattern, const char *name) {
    const char *star = NULL, *retry = NULL;
    while (*name) {
        char p = *pattern, n = *name;
        if (p >= 'A' && p <= 'Z') p += 32;
        char nl = (n >= 'A' && n <= 'Z') ? (char)(n + 32) : n;
        if (*pattern && (p == nl || p == '?')) { pattern++; name++; continue; }
        if (*pattern == '*') { star = pattern++; retry = name; continue; }
        if (star) { pattern = star + 1; name = ++retry; continue; }
        return false;
    }
    while (*pattern == '*') pattern++;
    return !*pattern;
}

static void fill_find(object_t *o, const kdirent_t *e, WIN32_FIND_DATAA *out) {
    memset(out, 0, sizeof *out);
    out->dwFileAttributes = e->type == FT_DIR ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    char full[600];
    snprintf(full, sizeof full, "%s/%s", o->dirpath, e->name);
    kstat_t st;
    if (stat(full, &st) == 0) {
        out->nFileSizeLow = (DWORD)st.size;
        out->nFileSizeHigh = (DWORD)(st.size >> 32);
    }
    strlcpy(out->cFileName, e->name, sizeof out->cFileName);
}

static bool find_next_entry(object_t *o, WIN32_FIND_DATAA *out) {
    kdirent_t e;
    while (readdir(o->dir, &e) == 0) {
        if (!strcmp(e.name, ".") || !strcmp(e.name, "..")) continue;
        if (!wildcard_match(o->pattern, e.name)) continue;
        fill_find(o, &e, out);
        return true;
    }
    return false;
}

static HANDLE WINAPI w_FindFirstFileA(const char *spec, WIN32_FIND_DATAA *out) {
    if (!spec || !out) { win_set_error(ERROR_INVALID_PARAMETER); return H_INVALID; }

    char host[512];
    win_path_to_host(spec, host, sizeof host);

    /* The last component is the pattern; everything before it is the
     * directory to walk. */
    char dirpath[512], pattern[128];
    char *slash = strrchr(host, '/');
    if (slash) {
        size_t dl = (size_t)(slash - host);
        if (!dl) dl = 1;                       /* "/x" means the root */
        memcpy(dirpath, host, dl);
        dirpath[dl] = 0;
        strlcpy(pattern, slash + 1, sizeof pattern);
    } else {
        strlcpy(dirpath, ".", sizeof dirpath);
        strlcpy(pattern, host, sizeof pattern);
    }
    if (!pattern[0]) strlcpy(pattern, "*", sizeof pattern);

    DIR *d = opendir(dirpath);
    if (!d) { win_set_error(ERROR_PATH_NOT_FOUND); return H_INVALID; }

    object_t *o;
    HANDLE h = obj_alloc(OBJ_FIND, &o);
    if (!h) { closedir(d); win_set_error(ERROR_NOT_ENOUGH_MEMORY); return H_INVALID; }
    o->dir = d;
    strlcpy(o->pattern, pattern, sizeof o->pattern);
    strlcpy(o->dirpath, dirpath, sizeof o->dirpath);

    if (!find_next_entry(o, out)) {
        obj_release(h);
        win_set_error(ERROR_NO_MORE_FILES);
        return H_INVALID;
    }
    return h;
}

static BOOL WINAPI w_FindNextFileA(HANDLE h, WIN32_FIND_DATAA *out) {
    object_t *o = obj_get(h, OBJ_FIND);
    if (!o || !out) { win_set_error(ERROR_INVALID_HANDLE); return WIN_FALSE; }
    if (!find_next_entry(o, out)) { win_set_error(ERROR_NO_MORE_FILES); return WIN_FALSE; }
    return WIN_TRUE;
}

static HANDLE WINAPI w_FindFirstFileW(const WCHAR *spec, WIN32_FIND_DATAW *out) {
    char buf[512];
    WIN32_FIND_DATAA a;
    win_wide_to_utf8(spec, buf, sizeof buf);
    HANDLE h = w_FindFirstFileA(buf, &a);
    if (h != H_INVALID && out) {
        memcpy(out, &a, offsetof(WIN32_FIND_DATAA, cFileName));
        win_utf8_to_wide(a.cFileName, out->cFileName, 260);
        out->cAlternateFileName[0] = 0;
    }
    return h;
}

static BOOL WINAPI w_FindNextFileW(HANDLE h, WIN32_FIND_DATAW *out) {
    WIN32_FIND_DATAA a;
    if (!w_FindNextFileA(h, &a)) return WIN_FALSE;
    if (out) {
        memcpy(out, &a, offsetof(WIN32_FIND_DATAA, cFileName));
        win_utf8_to_wide(a.cFileName, out->cFileName, 260);
        out->cAlternateFileName[0] = 0;
    }
    return WIN_TRUE;
}

static BOOL WINAPI w_FindClose(HANDLE h) {
    if (!obj_get(h, OBJ_FIND)) { win_set_error(ERROR_INVALID_HANDLE); return WIN_FALSE; }
    obj_release(h);
    return WIN_TRUE;
}

/* ------------------------------------------------------------- directories */

static DWORD WINAPI w_GetCurrentDirectoryA(DWORD cap, char *out) {
    char host[512], win[512];
    if (!getcwd(host, sizeof host)) { win_set_error(ERROR_ACCESS_DENIED); return 0; }
    win_path_from_host(host, win, sizeof win);
    size_t n = strlen(win);
    if (n + 1 > cap) return (DWORD)(n + 1);
    strlcpy(out, win, cap);
    return (DWORD)n;
}
static DWORD WINAPI w_GetCurrentDirectoryW(DWORD cap, WCHAR *out) {
    char buf[512];
    DWORD n = w_GetCurrentDirectoryA(sizeof buf, buf);
    if (n && n < sizeof buf) win_utf8_to_wide(buf, out, cap);
    return n;
}
static BOOL WINAPI w_SetCurrentDirectoryA(const char *name) {
    char host[512];
    win_path_to_host(name, host, sizeof host);
    if (chdir(host) < 0) { win_set_error(win_error_from_errno(errno)); return WIN_FALSE; }
    return WIN_TRUE;
}

static DWORD WINAPI w_GetTempPathA(DWORD cap, char *out) {
    return (DWORD)strlcpy(out, "C:\\tmp\\", cap);
}

static UINT WINAPI w_GetTempFileNameA(const char *dir, const char *prefix, UINT unique, char *out) {
    static UINT counter;
    if (!unique) unique = (UINT)(uptime_ms() & 0xFFFF) + ++counter;
    snprintf(out, 260, "%s\\%.3s%04x.tmp", dir ? dir : "C:\\tmp", prefix ? prefix : "tmp", unique & 0xFFFF);
    return unique;
}

static DWORD WINAPI w_GetFullPathNameA(const char *name, DWORD cap, char *out, char **file_part) {
    char host[512], full[512], win[512];
    win_path_to_host(name, host, sizeof host);
    if (host[0] == '/') {
        strlcpy(full, host, sizeof full);
    } else {
        char cwd[512];
        if (!getcwd(cwd, sizeof cwd)) strlcpy(cwd, "/", sizeof cwd);
        snprintf(full, sizeof full, "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", host);
    }
    win_path_from_host(full, win, sizeof win);
    size_t n = strlen(win);
    if (n + 1 > cap) return (DWORD)(n + 1);
    strlcpy(out, win, cap);
    if (file_part) {
        char *slash = strrchr(out, '\\');
        *file_part = slash ? slash + 1 : out;
    }
    return (DWORD)n;
}

/* --------------------------------------------------------------- the console */

static DWORD console_mode_in = 0x0007;     /* processed input, line input, echo */
static DWORD console_mode_out = 0x0003;

static BOOL WINAPI w_WriteConsoleA(HANDLE h, const void *buf, DWORD len, DWORD *put, void *reserved) {
    (void)reserved;
    return w_WriteFile(h, buf, len, put, NULL);
}

static BOOL WINAPI w_WriteConsoleW(HANDLE h, const WCHAR *buf, DWORD len, DWORD *put, void *reserved) {
    (void)reserved;
    char narrow[1024];
    DWORD done = 0;
    while (done < len) {
        DWORD chunk = len - done;
        if (chunk > sizeof narrow - 1) chunk = sizeof narrow - 1;
        for (DWORD i = 0; i < chunk; i++) {
            WCHAR c = buf[done + i];
            narrow[i] = (char)(c < 0x80 ? c : '?');
        }
        DWORD n = 0;
        if (!w_WriteFile(h, narrow, chunk, &n, NULL)) return WIN_FALSE;
        done += chunk;
    }
    if (put) *put = len;
    return WIN_TRUE;
}

static BOOL WINAPI w_ReadConsoleA(HANDLE h, void *buf, DWORD len, DWORD *got, void *ctl) {
    (void)ctl;
    return w_ReadFile(h, buf, len, got, NULL);
}

static BOOL WINAPI w_ReadConsoleW(HANDLE h, WCHAR *buf, DWORD len, DWORD *got, void *ctl) {
    (void)ctl;
    char narrow[512];
    DWORD n = 0;
    DWORD want = len < sizeof narrow ? len : (DWORD)sizeof narrow;
    if (!w_ReadFile(h, narrow, want, &n, NULL)) return WIN_FALSE;
    for (DWORD i = 0; i < n; i++) buf[i] = (WCHAR)(unsigned char)narrow[i];
    if (got) *got = n;
    return WIN_TRUE;
}

static BOOL WINAPI w_GetConsoleMode(HANDLE h, DWORD *out) {
    if (!out) return WIN_FALSE;
    if (h != H_STDIN && h != H_STDOUT && h != H_STDERR) { win_set_error(ERROR_INVALID_HANDLE); return WIN_FALSE; }
    *out = (h == H_STDIN) ? console_mode_in : console_mode_out;
    return WIN_TRUE;
}

static BOOL WINAPI w_SetConsoleMode(HANDLE h, DWORD mode) {
    if (h == H_STDIN) {
        console_mode_in = mode;
        /* Clearing line input and echo is a program asking for keys as they
         * are pressed, which the console here can do. */
        console_raw(!(mode & 0x0002));
    } else {
        console_mode_out = mode;
    }
    return WIN_TRUE;
}

typedef struct {
    WORD  X, Y;
} COORD;
typedef struct { WORD Left, Top, Right, Bottom; } SMALL_RECT;
typedef struct {
    COORD dwSize, dwCursorPosition;
    WORD  wAttributes;
    SMALL_RECT srWindow;
    COORD dwMaximumWindowSize;
} CONSOLE_SCREEN_BUFFER_INFO;

static BOOL WINAPI w_GetConsoleScreenBufferInfo(HANDLE h, CONSOLE_SCREEN_BUFFER_INFO *info) {
    (void)h;
    if (!info) return WIN_FALSE;
    int cols = 80, rows = 25, col = 0, row = 0;
    console_size(&cols, &rows);
    console_where(&col, &row);
    memset(info, 0, sizeof *info);
    info->dwSize.X = (WORD)cols;
    info->dwSize.Y = (WORD)rows;
    info->dwCursorPosition.X = (WORD)col;
    info->dwCursorPosition.Y = (WORD)row;
    info->wAttributes = 0x07;
    info->srWindow.Right = (WORD)(cols - 1);
    info->srWindow.Bottom = (WORD)(rows - 1);
    info->dwMaximumWindowSize = info->dwSize;
    return WIN_TRUE;
}

/* COORD is two shorts, so it arrives in a single register rather than by
 * reference - which is the one place the Microsoft convention surprises
 * anybody hand-writing these. */
static BOOL WINAPI w_SetConsoleCursorPosition(HANDLE h, COORD pos) {
    (void)h;
    console_goto(pos.X, pos.Y);
    return WIN_TRUE;
}

static BOOL WINAPI w_SetConsoleTextAttribute(HANDLE h, WORD attr) {
    (void)h;
    /* The low three bits are the foreground in the order blue, green, red;
     * ANSI orders them red, green, blue, so they swap. */
    int fg = ((attr & 1) << 2) | (attr & 2) | ((attr & 4) >> 2);
    printf("\x1b[%dm", (attr & 8) ? 90 + fg : 30 + fg);
    return WIN_TRUE;
}

static BOOL WINAPI w_SetConsoleTitleA(const char *title) { (void)title; return WIN_TRUE; }
static BOOL WINAPI w_SetConsoleTitleW(const WCHAR *title) { (void)title; return WIN_TRUE; }
static UINT WINAPI w_GetConsoleCP(void) { return 65001; }
static UINT WINAPI w_GetConsoleOutputCP(void) { return 65001; }
static BOOL WINAPI w_SetConsoleCP(UINT cp) { (void)cp; return WIN_TRUE; }
static BOOL WINAPI w_SetConsoleOutputCP(UINT cp) { (void)cp; return WIN_TRUE; }
static BOOL WINAPI w_AllocConsole(void) { return WIN_TRUE; }
static BOOL WINAPI w_FreeConsole(void) { return WIN_TRUE; }
static HANDLE WINAPI w_GetConsoleWindow(void) { return NULL; }

static BOOL WINAPI w_FillConsoleOutputCharacterA(HANDLE h, char c, DWORD len, COORD at, DWORD *put) {
    (void)h;
    console_goto(at.X, at.Y);
    for (DWORD i = 0; i < len; i++) putchar(c);
    if (put) *put = len;
    return WIN_TRUE;
}
static BOOL WINAPI w_FillConsoleOutputAttribute(HANDLE h, WORD attr, DWORD len, COORD at, DWORD *put) {
    (void)h; (void)attr; (void)at;
    if (put) *put = len;
    return WIN_TRUE;
}

/* ------------------------------------------------------------ file mapping */

static HANDLE WINAPI w_CreateFileMappingA(HANDLE file, void *sa, DWORD protect,
                                          DWORD max_high, DWORD max_low, const char *name) {
    (void)sa; (void)protect; (void)name;
    object_t *o;
    HANDLE h = obj_alloc(OBJ_MAPPING, &o);
    if (!h) { win_set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    o->fd = fd_of(file);
    o->map_size = ((size_t)max_high << 32) | max_low;
    if (!o->map_size && o->fd >= 0) {
        off_t here = lseek(o->fd, 0, SEEK_CUR);
        off_t end = lseek(o->fd, 0, SEEK_END);
        lseek(o->fd, here, SEEK_SET);
        o->map_size = end > 0 ? (size_t)end : 0;
    }
    return h;
}

/* There is no demand paging from a file here, so a view is read in whole.
 * Writes to it do not travel back to the file, and a program that maps for
 * writing is told so rather than losing its changes silently. */
static void *WINAPI w_MapViewOfFile(HANDLE h, DWORD access, DWORD off_high, DWORD off_low, size_t len) {
    object_t *o = obj_get(h, OBJ_MAPPING);
    if (!o) { win_set_error(ERROR_INVALID_HANDLE); return NULL; }
    if (access & 0x0002 /* FILE_MAP_WRITE */) {
        win_trace("a writable file mapping was asked for; changes will not reach the file");
    }
    size_t want = len ? len : o->map_size;
    if (!want) { win_set_error(ERROR_INVALID_PARAMETER); return NULL; }

    void *mem = malloc(want);
    if (!mem) { win_set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    memset(mem, 0, want);
    if (o->fd >= 0) {
        off_t at = ((off_t)off_high << 32) | off_low;
        off_t here = lseek(o->fd, 0, SEEK_CUR);
        lseek(o->fd, at, SEEK_SET);
        read(o->fd, mem, want);
        lseek(o->fd, here, SEEK_SET);
    }
    o->map_base = mem;
    return mem;
}

static BOOL WINAPI w_UnmapViewOfFile(void *addr) { free(addr); return WIN_TRUE; }
static BOOL WINAPI w_FlushViewOfFile(void *addr, size_t len) { (void)addr; (void)len; return WIN_TRUE; }

/* ---------------------------------------------------------------- the table */

const win_export_t k32_file_exports[] = {
    { "GetStdHandle",                 (void *)w_GetStdHandle },
    { "SetStdHandle",                 (void *)w_SetStdHandle },
    { "CloseHandle",                  (void *)w_CloseHandle },
    { "DuplicateHandle",              (void *)w_DuplicateHandle },

    { "CreateFileA",                  (void *)w_CreateFileA },
    { "CreateFileW",                  (void *)w_CreateFileW },
    { "ReadFile",                     (void *)w_ReadFile },
    { "WriteFile",                    (void *)w_WriteFile },
    { "SetFilePointer",               (void *)w_SetFilePointer },
    { "SetFilePointerEx",             (void *)w_SetFilePointerEx },
    { "GetFileSize",                  (void *)w_GetFileSize },
    { "GetFileSizeEx",                (void *)w_GetFileSizeEx },
    { "SetEndOfFile",                 (void *)w_SetEndOfFile },
    { "FlushFileBuffers",             (void *)w_FlushFileBuffers },
    { "GetFileTime",                  (void *)w_GetFileTime },
    { "GetFileType",                  (void *)w_GetFileType },

    { "DeleteFileA",                  (void *)w_DeleteFileA },
    { "DeleteFileW",                  (void *)w_DeleteFileW },
    { "MoveFileA",                    (void *)w_MoveFileA },
    { "MoveFileExA",                  (void *)w_MoveFileExA },
    { "CopyFileA",                    (void *)w_CopyFileA },
    { "CreateDirectoryA",             (void *)w_CreateDirectoryA },
    { "CreateDirectoryW",             (void *)w_CreateDirectoryW },
    { "RemoveDirectoryA",             (void *)w_RemoveDirectoryA },
    { "GetFileAttributesA",           (void *)w_GetFileAttributesA },
    { "GetFileAttributesW",           (void *)w_GetFileAttributesW },
    { "GetFileAttributesExA",         (void *)w_GetFileAttributesExA },
    { "SetFileAttributesA",           (void *)w_SetFileAttributesA },

    { "FindFirstFileA",               (void *)w_FindFirstFileA },
    { "FindNextFileA",                (void *)w_FindNextFileA },
    { "FindFirstFileW",               (void *)w_FindFirstFileW },
    { "FindNextFileW",                (void *)w_FindNextFileW },
    { "FindClose",                    (void *)w_FindClose },

    { "GetCurrentDirectoryA",         (void *)w_GetCurrentDirectoryA },
    { "GetCurrentDirectoryW",         (void *)w_GetCurrentDirectoryW },
    { "SetCurrentDirectoryA",         (void *)w_SetCurrentDirectoryA },
    { "GetTempPathA",                 (void *)w_GetTempPathA },
    { "GetTempFileNameA",             (void *)w_GetTempFileNameA },
    { "GetFullPathNameA",             (void *)w_GetFullPathNameA },

    { "WriteConsoleA",                (void *)w_WriteConsoleA },
    { "WriteConsoleW",                (void *)w_WriteConsoleW },
    { "ReadConsoleA",                 (void *)w_ReadConsoleA },
    { "ReadConsoleW",                 (void *)w_ReadConsoleW },
    { "GetConsoleMode",               (void *)w_GetConsoleMode },
    { "SetConsoleMode",               (void *)w_SetConsoleMode },
    { "GetConsoleScreenBufferInfo",   (void *)w_GetConsoleScreenBufferInfo },
    { "SetConsoleCursorPosition",     (void *)w_SetConsoleCursorPosition },
    { "SetConsoleTextAttribute",      (void *)w_SetConsoleTextAttribute },
    { "SetConsoleTitleA",             (void *)w_SetConsoleTitleA },
    { "SetConsoleTitleW",             (void *)w_SetConsoleTitleW },
    { "GetConsoleCP",                 (void *)w_GetConsoleCP },
    { "GetConsoleOutputCP",           (void *)w_GetConsoleOutputCP },
    { "SetConsoleCP",                 (void *)w_SetConsoleCP },
    { "SetConsoleOutputCP",           (void *)w_SetConsoleOutputCP },
    { "AllocConsole",                 (void *)w_AllocConsole },
    { "FreeConsole",                  (void *)w_FreeConsole },
    { "GetConsoleWindow",             (void *)w_GetConsoleWindow },
    { "FillConsoleOutputCharacterA",  (void *)w_FillConsoleOutputCharacterA },
    { "FillConsoleOutputAttribute",   (void *)w_FillConsoleOutputAttribute },

    { "CreateFileMappingA",           (void *)w_CreateFileMappingA },
    { "MapViewOfFile",                (void *)w_MapViewOfFile },
    { "UnmapViewOfFile",              (void *)w_UnmapViewOfFile },
    { "FlushViewOfFile",              (void *)w_FlushViewOfFile },
    { NULL, NULL }
};
