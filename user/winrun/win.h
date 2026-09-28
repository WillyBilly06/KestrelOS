/* win.h - the shared vocabulary of the Windows subsystem.
 *
 * A Windows program is a PE image that expects a particular set of libraries
 * to be there when it starts: kernel32 for processes, memory and files, user32
 * for windows and messages, gdi32 for drawing, a C runtime, and a handful of
 * others.  None of those exist here, so this subsystem supplies them - not by
 * emulating the instruction set, which is already the right one, but by
 * standing in for the libraries at the point where the program calls them.
 *
 * The pieces:
 *   pe.c      mapping images and DLLs, relocations, imports, exports
 *   k32.c     kernel32: processes, memory, files, threads, synchronisation
 *   u32.c     user32: windows, the message loop, controls
 *   gdi.c     gdi32: device contexts and drawing
 *   crt.c     msvcrt and ucrtbase
 *   advapi.c  the registry
 *   sock.c    winsock, over the system's own TCP
 *   seh.c     structured exception handling
 *   misc.c    shell32, ole32, comdlg32, shlwapi, comctl32, version
 */
#ifndef KESTREL_WIN_H
#define KESTREL_WIN_H

#include "kestrel.h"

/* Guest code is compiled for the Microsoft calling convention, which differs
 * from this system's in which registers carry arguments and in the 32 bytes of
 * shadow space above the return address.  Every function the guest can reach
 * has to be declared with it. */
#define WINAPI __attribute__((ms_abi))

/* ---------------------------------------------------------------- the types */

typedef uint8_t   BYTE;
typedef uint16_t  WORD;
typedef uint32_t  DWORD;
typedef int32_t   LONG;
typedef uint32_t  UINT;
typedef uint32_t  ULONG;
typedef uintptr_t SIZE_T;
typedef int32_t   INT;
typedef int32_t   BOOL;
typedef uint64_t  ULONGLONG;
typedef int64_t   LONGLONG;
typedef uint64_t  UINT_PTR;
typedef int64_t   LONG_PTR;
typedef uint64_t  WPARAM;
typedef int64_t   LPARAM;
typedef int64_t   LRESULT;
typedef int32_t   HRESULT;
typedef void     *HANDLE;
typedef uint16_t  WCHAR;

typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;
typedef struct { LONG cx, cy; } SIZE;

typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;

#define WIN_TRUE  1
#define WIN_FALSE 0

/* Error numbers the guest will recognise. */
#define ERROR_SUCCESS               0
#define ERROR_FILE_NOT_FOUND        2
#define ERROR_PATH_NOT_FOUND        3
#define ERROR_ACCESS_DENIED         5
#define ERROR_INVALID_HANDLE        6
#define ERROR_NOT_ENOUGH_MEMORY     8
#define ERROR_NO_MORE_FILES        18
#define ERROR_HANDLE_EOF           38
#define ERROR_FILE_EXISTS          80
#define ERROR_INVALID_PARAMETER    87
#define ERROR_ALREADY_EXISTS      183
#define ERROR_MOD_NOT_FOUND       126
#define ERROR_PROC_NOT_FOUND      127
#define ERROR_IO_PENDING          997

/* --------------------------------------------------------------- handles */

/* Every kernel object a guest can hold - a file, a thread, an event, a
 * registry key - is one of these.  Handles are indices dressed up as pointers,
 * so a stray pointer used as a handle is caught rather than dereferenced. */
typedef enum {
    OBJ_FREE = 0,
    OBJ_FILE,
    OBJ_CONSOLE,
    OBJ_THREAD,
    OBJ_EVENT,
    OBJ_MUTEX,
    OBJ_SEMAPHORE,
    OBJ_PROCESS,
    OBJ_MODULE,
    OBJ_FIND,           /* a directory walk in progress */
    OBJ_REGKEY,
    OBJ_SOCKET,
    OBJ_MAPPING,        /* a file mapped into memory */
} objkind_t;

typedef struct object {
    objkind_t kind;
    int       refs;

    /* OBJ_FILE / OBJ_CONSOLE */
    int       fd;
    bool      is_console_in;
    /* OBJ_THREAD */
    int       tid;
    uint32_t  finished;         /* the word a joiner sleeps on */
    DWORD     exit_code;
    void     *param;
    void     *start;
    /* OBJ_EVENT / OBJ_MUTEX / OBJ_SEMAPHORE */
    uint32_t  signalled;        /* also the futex word */
    bool      manual_reset;
    int       count, max_count;
    int       owner_tid;
    int       recursion;
    /* OBJ_FIND */
    DIR      *dir;
    char      pattern[128];
    char      dirpath[512];
    /* OBJ_REGKEY */
    int       regkey;
    /* OBJ_SOCKET */
    int       sock;
    /* OBJ_MAPPING */
    void     *map_base;
    size_t    map_size;
    /* OBJ_MODULE */
    void     *module;
} object_t;

#define MAX_OBJECTS 256

object_t *obj_get(HANDLE h, objkind_t kind);
object_t *obj_any(HANDLE h);
HANDLE    obj_alloc(objkind_t kind, object_t **out);
void      obj_release(HANDLE h);

/* The standard streams are fixed handles so that a program which hard-codes
 * them - and some do - still works. */
#define H_STDIN   ((HANDLE)(uintptr_t)0x10)
#define H_STDOUT  ((HANDLE)(uintptr_t)0x11)
#define H_STDERR  ((HANDLE)(uintptr_t)0x12)
#define H_INVALID ((HANDLE)(uintptr_t)-1)

/* --------------------------------------------------------------- modules */

typedef struct { const char *name; void *fn; } win_export_t;

typedef struct win_module {
    char        name[64];        /* "kernel32.dll", lower case            */
    char        path[512];
    /* Built-in libraries hand out functions from a table; a DLL read from
     * disk hands them out of its own export directory. */
    const win_export_t *table;
    uint8_t    *base;            /* NULL for a built-in                   */
    size_t      size;
    uint32_t    export_rva, export_size;
    uint64_t    entry;
    int         refs;
    bool        initialised;
    struct win_module *next;
} win_module_t;

win_module_t *pe_load(const char *name, bool as_dll);
win_module_t *pe_find_module(const char *name);
void         *pe_resolve(const char *dll, const char *name, uint16_t ordinal);
void         *pe_export_of(win_module_t *m, const char *name, uint16_t ordinal);
uint64_t      pe_entry(win_module_t *m);
bool          pe_is_gui(win_module_t *m);
void          pe_run_tls_callbacks(win_module_t *m, DWORD reason);

/* The resource directory, which is where icons, strings, dialogs and version
 * information live inside the image. */
const void *pe_find_resource(win_module_t *m, uint16_t type, uint16_t id,
                             uint32_t *size_out);

/* Registered by each built-in library at start-up. */
void win_register(const char *dll, const win_export_t *table);
void d3d_init(void);

/* Direct3D 12, which is a different model rather than a newer spelling - see
 * the note at the top of d3d12.c. */
void d3d12_init(void);

/* Called once, before anything else, to register every built-in. */
void win_libraries_init(void);

/* Each library's own registration, called from win_libraries_init. */
void k32_init(void);
void u32_init(void);
void gdi_init(void);
void crt_init(void);
void advapi_init(void);
void sock_init(void);
void misc_init(void);
void seh_init(void);

/* ----------------------------------------------------------------- shared */

extern bool     win_verbose;
extern char     win_exe_path[512];
extern char     win_cmdline[1024];
extern WCHAR    win_cmdline_w[1024];
extern win_module_t *win_main_module;

void  win_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void  win_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void  win_set_error(DWORD err);
DWORD win_get_error(void);

/* Windows paths and this system's paths are not the same shape.  A drive
 * letter maps to a directory, backslashes become slashes, and a bare relative
 * path is left alone. */
void  win_path_to_host(const char *win, char *out, size_t cap);
void  win_path_from_host(const char *host, char *out, size_t cap);

/* UTF-16 is what half the Windows API speaks, so conversion is needed
 * constantly.  Both are bounded and always terminate. */
size_t win_wide_to_utf8(const WCHAR *w, char *out, size_t cap);
size_t win_utf8_to_wide(const char *s, WCHAR *out, size_t cap);

/* Waiting, shared between kernel32's synchronisation objects and the message
 * loop, both of which have to wait without spinning a core flat. */
#define WAIT_OBJECT_0     0x00000000
#define WAIT_ABANDONED_0  0x00000080
#define WAIT_TIMEOUT      0x00000102
#define WAIT_FAILED       0xFFFFFFFF
#define INFINITE          0xFFFFFFFF

DWORD win_wait_one(object_t *o, DWORD timeout_ms);

/* The GUI side, which only comes up if the program actually asks for a
 * window.  Returns false if there is no display to use. */
bool  u32_start_gui(void);
bool  u32_gui_running(void);
void  u32_shutdown(void);

/* Structured exception handling: guest code that faults is given the chance to
 * handle it the way it would on Windows before anything else happens. */
void  seh_install(void);
bool  seh_dispatch_fault(uint32_t code, uint64_t address, uint64_t rip, uint64_t rsp);

#endif
