/* misc.c - the smaller libraries.
 *
 * shell32, ole32, comdlg32, shlwapi, comctl32 and version.  A program reaches
 * for these for one or two things each - a path helper, a folder location, a
 * file dialog - and refusing to start because one of them is absent would be a
 * poor trade for how little of each is actually used.
 *
 * Where something cannot be done here it says so rather than returning a
 * success the caller would then act on.  A file dialog that silently returns
 * "no file chosen" is honest; one that returns a path to a file that does not
 * exist is not.
 */
#include "win.h"
#include "gui.h"

/* ------------------------------------------------------------------ shell32 */

#define CSIDL_DESKTOP       0x0000
#define CSIDL_PROGRAMS      0x0002
#define CSIDL_PERSONAL      0x0005
#define CSIDL_APPDATA       0x001A
#define CSIDL_LOCAL_APPDATA 0x001C
#define CSIDL_WINDOWS       0x0024
#define CSIDL_SYSTEM        0x0025
#define CSIDL_PROGRAM_FILES 0x0026

static const char *known_folder(int id) {
    switch (id & 0xFF) {
    case CSIDL_DESKTOP:       return "C:\\users\\user\\desktop";
    case CSIDL_PROGRAMS:      return "C:\\users\\user\\programs";
    case CSIDL_PERSONAL:      return "C:\\users\\user\\documents";
    case CSIDL_APPDATA:       return "C:\\users\\user\\appdata";
    case CSIDL_LOCAL_APPDATA: return "C:\\users\\user\\appdata\\local";
    case CSIDL_WINDOWS:       return "C:\\windows";
    case CSIDL_SYSTEM:        return "C:\\windows\\system32";
    case CSIDL_PROGRAM_FILES: return "C:\\program files";
    default:                  return NULL;
    }
}

static HRESULT WINAPI w_SHGetFolderPathA(HANDLE owner, int folder, HANDLE token,
                                         DWORD flags, char *out) {
    (void)owner; (void)token; (void)flags;
    const char *path = known_folder(folder);
    if (!path || !out) return (HRESULT)0x80070057;        /* E_INVALIDARG */
    strlcpy(out, path, 260);

    /* A program asking where to put its files expects the answer to exist. */
    char host[512];
    win_path_to_host(path, host, sizeof host);
    if (!file_exists(host)) {
        /* Make each level in turn; mkdir of an existing one is harmless. */
        char build[512];
        size_t n = 0;
        for (const char *p = host; ; p++) {
            if (*p == '/' || !*p) {
                if (n) { build[n] = 0; mkdir(build); }
            }
            if (!*p) break;
            if (n < sizeof build - 1) build[n++] = *p;
        }
    }
    return 0;
}

static BOOL WINAPI w_SHGetSpecialFolderPathA(HANDLE owner, char *out, int folder, BOOL create) {
    (void)create;
    return w_SHGetFolderPathA(owner, folder, NULL, 0, out) == 0 ? WIN_TRUE : WIN_FALSE;
}

static HANDLE WINAPI w_ShellExecuteA(HANDLE owner, const char *verb, const char *file,
                                     const char *params, const char *dir, int show) {
    (void)owner; (void)verb; (void)params; (void)dir; (void)show;
    win_trace("ShellExecute was asked to open \"%s\"", file ? file : "");
    /* Nothing here starts other programs on a guest's behalf; the return
     * value below is the documented "no association" answer. */
    return (HANDLE)(uintptr_t)31;
}

static int WINAPI w_SHCreateDirectoryExA(HANDLE owner, const char *path, void *sa) {
    (void)owner; (void)sa;
    char host[512], build[512];
    win_path_to_host(path, host, sizeof host);
    size_t n = 0;
    for (const char *p = host; ; p++) {
        if ((*p == '/' || !*p) && n) { build[n] = 0; mkdir(build); }
        if (!*p) break;
        if (n < sizeof build - 1) build[n++] = *p;
    }
    return file_exists(host) ? 0 : (int)ERROR_PATH_NOT_FOUND;
}

/* ------------------------------------------------------------------ shlwapi */

static char *WINAPI w_PathFindFileNameA(const char *path) {
    if (!path) return NULL;
    const char *last = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') last = p + 1;
    return (char *)last;
}

static char *WINAPI w_PathFindExtensionA(const char *path) {
    if (!path) return NULL;
    const char *name = w_PathFindFileNameA(path);
    const char *dot = strrchr(name, '.');
    return (char *)(dot ? dot : name + strlen(name));
}

static BOOL WINAPI w_PathFileExistsA(const char *path) {
    char host[512];
    win_path_to_host(path, host, sizeof host);
    return file_exists(host) ? WIN_TRUE : WIN_FALSE;
}

static BOOL WINAPI w_PathRemoveFileSpecA(char *path) {
    if (!path) return WIN_FALSE;
    char *name = w_PathFindFileNameA(path);
    if (name == path) return WIN_FALSE;
    name[-1] = 0;
    return WIN_TRUE;
}

static BOOL WINAPI w_PathAppendA(char *path, const char *more) {
    if (!path || !more) return WIN_FALSE;
    size_t n = strlen(path);
    if (n && path[n - 1] != '\\') { path[n++] = '\\'; path[n] = 0; }
    while (*more == '\\') more++;
    strlcat(path, more, 260);
    return WIN_TRUE;
}

static BOOL WINAPI w_PathIsDirectoryA(const char *path) {
    char host[512];
    win_path_to_host(path, host, sizeof host);
    kstat_t st;
    return (stat(host, &st) == 0 && st.type == FT_DIR) ? WIN_TRUE : WIN_FALSE;
}

/* ------------------------------------------------------------------- ole32 */

static HRESULT WINAPI w_CoInitialize(void *reserved) { (void)reserved; return 0; }
static HRESULT WINAPI w_CoInitializeEx(void *reserved, DWORD flags) { (void)reserved; (void)flags; return 0; }
static void    WINAPI w_CoUninitialize(void) { }

/* There is no object model here, so nothing can be created.  Returning the
 * documented "class not registered" is what a program checks for, and it will
 * take its own fallback path. */
static HRESULT WINAPI w_CoCreateInstance(const void *clsid, void *outer, DWORD ctx,
                                         const void *iid, void **out) {
    (void)clsid; (void)outer; (void)ctx; (void)iid;
    if (out) *out = NULL;
    win_trace("CoCreateInstance was called; there is no object model here");
    return (HRESULT)0x80040154;                   /* REGDB_E_CLASSNOTREG */
}
static void *WINAPI w_CoTaskMemAlloc(size_t n) { return malloc(n); }
static void  WINAPI w_CoTaskMemFree(void *p) { free(p); }

/* ----------------------------------------------------------------- comdlg32 */

/* A file dialog needs a file browser to be worth anything, and there is not
 * one inside this subsystem.  Reporting that the user cancelled is exactly
 * what a program is prepared for. */
static BOOL WINAPI w_GetOpenFileNameA(void *ofn) {
    (void)ofn;
    win_trace("a file dialog was opened; reporting that nothing was chosen");
    return WIN_FALSE;
}
static DWORD WINAPI w_CommDlgExtendedError(void) { return 0; }

/* ----------------------------------------------------------------- comctl32 */

static BOOL WINAPI w_InitCommonControlsEx(const void *icc) { (void)icc; return WIN_TRUE; }
static void WINAPI w_InitCommonControls(void) { }

/* ------------------------------------------------------------------ version */

static DWORD WINAPI w_GetFileVersionInfoSizeA(const char *file, DWORD *handle) {
    (void)file;
    if (handle) *handle = 0;
    return 0;                       /* no version resource is read here */
}
static BOOL WINAPI w_GetFileVersionInfoA(const char *file, DWORD handle, DWORD len, void *data) {
    (void)file; (void)handle; (void)len; (void)data;
    return WIN_FALSE;
}

/* ---------------------------------------------------------------- the tables */

static const win_export_t shell32[] = {
    { "SHGetFolderPathA",        (void *)w_SHGetFolderPathA },
    { "SHGetSpecialFolderPathA", (void *)w_SHGetSpecialFolderPathA },
    { "ShellExecuteA",           (void *)w_ShellExecuteA },
    { "SHCreateDirectoryExA",    (void *)w_SHCreateDirectoryExA },
    { NULL, NULL }
};

static const win_export_t shlwapi[] = {
    { "PathFindFileNameA",   (void *)w_PathFindFileNameA },
    { "PathFindExtensionA",  (void *)w_PathFindExtensionA },
    { "PathFileExistsA",     (void *)w_PathFileExistsA },
    { "PathRemoveFileSpecA", (void *)w_PathRemoveFileSpecA },
    { "PathAppendA",         (void *)w_PathAppendA },
    { "PathIsDirectoryA",    (void *)w_PathIsDirectoryA },
    { NULL, NULL }
};

static const win_export_t ole32[] = {
    { "CoInitialize",    (void *)w_CoInitialize },
    { "CoInitializeEx",  (void *)w_CoInitializeEx },
    { "CoUninitialize",  (void *)w_CoUninitialize },
    { "CoCreateInstance",(void *)w_CoCreateInstance },
    { "CoTaskMemAlloc",  (void *)w_CoTaskMemAlloc },
    { "CoTaskMemFree",   (void *)w_CoTaskMemFree },
    { NULL, NULL }
};

static const win_export_t comdlg32[] = {
    { "GetOpenFileNameA",     (void *)w_GetOpenFileNameA },
    { "GetSaveFileNameA",     (void *)w_GetOpenFileNameA },
    { "CommDlgExtendedError", (void *)w_CommDlgExtendedError },
    { NULL, NULL }
};

static const win_export_t comctl32[] = {
    { "InitCommonControlsEx", (void *)w_InitCommonControlsEx },
    { "InitCommonControls",   (void *)w_InitCommonControls },
    { NULL, NULL }
};

static const win_export_t version[] = {
    { "GetFileVersionInfoSizeA", (void *)w_GetFileVersionInfoSizeA },
    { "GetFileVersionInfoA",     (void *)w_GetFileVersionInfoA },
    { NULL, NULL }
};

void misc_init(void) {
    win_register("shell32.dll", shell32);
    win_register("shlwapi.dll", shlwapi);
    win_register("ole32.dll", ole32);
    win_register("oleaut32.dll", ole32);
    win_register("comdlg32.dll", comdlg32);
    win_register("comctl32.dll", comctl32);
    win_register("version.dll", version);
}
