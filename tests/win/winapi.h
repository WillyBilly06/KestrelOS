/* winapi.h - just enough of the Windows headers to build the test programs.
 *
 * There is no Windows SDK here, so the declarations the test programs need are
 * written out.  They are deliberately the real shapes and the real spellings:
 * a program built against these and one built against Microsoft's headers
 * produce the same imports and the same calls, which is the whole point of
 * testing with them.
 */
#ifndef KESTREL_TEST_WINAPI_H
#define KESTREL_TEST_WINAPI_H

#ifdef __cplusplus
/* Everything below is the system's own interface, which does not change
 * its name because the program including it happens to be C++. */
extern "C" {
#endif

typedef unsigned char      BYTE;
typedef unsigned short     WORD;
typedef unsigned int       DWORD;
typedef int                LONG;
typedef unsigned int       UINT;
typedef int                BOOL;
typedef void              *HANDLE;
typedef HANDLE             HWND, HDC, HINSTANCE, HMODULE, HBRUSH, HPEN, HFONT, HICON, HCURSOR, HMENU, HKEY, HGDIOBJ, HBITMAP;
typedef unsigned long long UINT_PTR, ULONG_PTR, WPARAM;
typedef long long          LONG_PTR, LPARAM, LRESULT;
typedef unsigned short     WCHAR;
typedef unsigned long long SIZE_T;

#define TRUE  1
#define FALSE 0
#define NULL  ((void *)0)
#define WINAPI __stdcall
#define CALLBACK __stdcall

typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;
typedef struct { LONG cx, cy; } SIZE;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;

/* ---------------------------------------------------------------- kernel32 */

#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE  ((DWORD)-12)

#define GENERIC_READ   0x80000000u
#define GENERIC_WRITE  0x40000000u
#define CREATE_NEW     1
#define CREATE_ALWAYS  2
#define OPEN_EXISTING  3
#define OPEN_ALWAYS    4
#define FILE_ATTRIBUTE_NORMAL    0x80
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define INVALID_HANDLE_VALUE ((HANDLE)(long long)-1)
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define FILE_BEGIN 0
#define FILE_END   2

#define INFINITE       0xFFFFFFFFu
#define WAIT_OBJECT_0  0
#define WAIT_TIMEOUT   0x102

#define ERROR_SUCCESS       0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_ALREADY_EXISTS 183

void   WINAPI ExitProcess(UINT code);
HANDLE WINAPI GetStdHandle(DWORD which);
BOOL   WINAPI WriteFile(HANDLE h, const void *buf, DWORD len, DWORD *put, void *ov);
BOOL   WINAPI ReadFile(HANDLE h, void *buf, DWORD len, DWORD *got, void *ov);
BOOL   WINAPI CloseHandle(HANDLE h);
DWORD  WINAPI GetLastError(void);
void   WINAPI SetLastError(DWORD e);
char * WINAPI GetCommandLineA(void);
DWORD  WINAPI GetTickCount(void);
void   WINAPI Sleep(DWORD ms);
HANDLE WINAPI GetProcessHeap(void);
void * WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T n);
BOOL   WINAPI HeapFree(HANDLE heap, DWORD flags, void *p);
SIZE_T WINAPI HeapSize(HANDLE heap, DWORD flags, void *p);
void * WINAPI VirtualAlloc(void *addr, SIZE_T len, DWORD type, DWORD protect);
BOOL   WINAPI VirtualFree(void *addr, SIZE_T len, DWORD type);
HMODULE WINAPI GetModuleHandleA(const char *name);
HMODULE WINAPI LoadLibraryA(const char *name);
BOOL   WINAPI FreeLibrary(HMODULE m);
void * WINAPI GetProcAddress(HMODULE m, const char *name);
DWORD  WINAPI GetModuleFileNameA(HMODULE m, char *out, DWORD cap);
DWORD  WINAPI GetEnvironmentVariableA(const char *name, char *out, DWORD cap);
BOOL   WINAPI QueryPerformanceCounter(long long *out);
BOOL   WINAPI QueryPerformanceFrequency(long long *out);
void   WINAPI GetSystemTime(SYSTEMTIME *st);
void   WINAPI GetSystemTimeAsFileTime(FILETIME *ft);
BOOL   WINAPI SystemTimeToFileTime(const SYSTEMTIME *st, FILETIME *ft);
BOOL   WINAPI FileTimeToSystemTime(const FILETIME *ft, SYSTEMTIME *st);

HANDLE WINAPI CreateFileA(const char *name, DWORD access, DWORD share, void *sa,
                          DWORD disposition, DWORD attrs, HANDLE templ);
DWORD  WINAPI SetFilePointer(HANDLE h, LONG low, LONG *high, DWORD whence);
DWORD  WINAPI GetFileSize(HANDLE h, DWORD *high);
BOOL   WINAPI DeleteFileA(const char *name);
BOOL   WINAPI CreateDirectoryA(const char *name, void *sa);
BOOL   WINAPI RemoveDirectoryA(const char *name);
DWORD  WINAPI GetFileAttributesA(const char *name);
BOOL   WINAPI MoveFileA(const char *from, const char *to);
DWORD  WINAPI GetCurrentDirectoryA(DWORD cap, char *out);
BOOL   WINAPI FlushFileBuffers(HANDLE h);
BOOL   WINAPI SetEndOfFile(HANDLE h);

typedef struct {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD    nFileSizeHigh, nFileSizeLow;
    DWORD    dwReserved0, dwReserved1;
    char     cFileName[260];
    char     cAlternateFileName[14];
} WIN32_FIND_DATAA;

HANDLE WINAPI FindFirstFileA(const char *spec, WIN32_FIND_DATAA *out);
BOOL   WINAPI FindNextFileA(HANDLE h, WIN32_FIND_DATAA *out);
BOOL   WINAPI FindClose(HANDLE h);

HANDLE WINAPI CreateThread(void *sa, SIZE_T stack, void *start, void *param,
                           DWORD flags, DWORD *tid);
void   WINAPI ExitThread(DWORD code);
DWORD  WINAPI GetCurrentThreadId(void);
BOOL   WINAPI GetExitCodeThread(HANDLE h, DWORD *out);
DWORD  WINAPI WaitForSingleObject(HANDLE h, DWORD timeout);
DWORD  WINAPI WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL all, DWORD timeout);

HANDLE WINAPI CreateEventA(void *sa, BOOL manual, BOOL initial, const char *name);
BOOL   WINAPI SetEvent(HANDLE h);
BOOL   WINAPI ResetEvent(HANDLE h);
HANDLE WINAPI CreateMutexA(void *sa, BOOL own, const char *name);
BOOL   WINAPI ReleaseMutex(HANDLE h);
HANDLE WINAPI CreateSemaphoreA(void *sa, LONG initial, LONG max, const char *name);
BOOL   WINAPI ReleaseSemaphore(HANDLE h, LONG release, LONG *previous);

typedef struct {
    void    *DebugInfo;
    LONG     LockCount, RecursionCount;
    HANDLE   OwningThread, LockSemaphore;
    ULONG_PTR SpinCount;
} CRITICAL_SECTION;

void WINAPI InitializeCriticalSection(CRITICAL_SECTION *cs);
void WINAPI EnterCriticalSection(CRITICAL_SECTION *cs);
void WINAPI LeaveCriticalSection(CRITICAL_SECTION *cs);
void WINAPI DeleteCriticalSection(CRITICAL_SECTION *cs);

LONG WINAPI InterlockedIncrement(volatile LONG *p);
LONG WINAPI InterlockedDecrement(volatile LONG *p);
LONG WINAPI InterlockedExchangeAdd(volatile LONG *p, LONG v);
LONG WINAPI InterlockedCompareExchange(volatile LONG *p, LONG want, LONG expect);

DWORD  WINAPI TlsAlloc(void);
BOOL   WINAPI TlsFree(DWORD index);
void * WINAPI TlsGetValue(DWORD index);
BOOL   WINAPI TlsSetValue(DWORD index, void *value);

/* ------------------------------------------------------------------ user32 */

#define WM_CREATE      0x0001
#define WM_DESTROY     0x0002
#define WM_SIZE        0x0005
#define WM_PAINT       0x000F
#define WM_CLOSE       0x0010
#define WM_QUIT        0x0012
#define WM_KEYDOWN     0x0100
#define WM_CHAR        0x0102
#define WM_COMMAND     0x0111
#define WM_TIMER       0x0113
#define WM_MOUSEMOVE   0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP   0x0202

#define WS_OVERLAPPEDWINDOW 0x00CF0000
#define WS_VISIBLE          0x10000000
#define WS_CHILD            0x40000000
#define CW_USEDEFAULT       ((int)0x80000000)
#define SW_SHOW             5
#define PM_REMOVE           1

#define MB_OK     0x0000
#define MB_YESNO  0x0004
#define IDOK      1
#define IDYES     6

typedef LRESULT (CALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);

typedef struct {
    UINT     cbSize, style;
    WNDPROC  lpfnWndProc;
    int      cbClsExtra, cbWndExtra;
    HINSTANCE hInstance;
    HICON    hIcon;
    HCURSOR  hCursor;
    HBRUSH   hbrBackground;
    const char *lpszMenuName, *lpszClassName;
    HICON    hIconSm;
} WNDCLASSEXA;

typedef struct { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG;
typedef struct { HDC hdc; BOOL fErase; RECT rcPaint; BOOL fRestore, fIncUpdate; BYTE rgb[32]; } PAINTSTRUCT;

WORD    WINAPI RegisterClassExA(const WNDCLASSEXA *cls);
HWND    WINAPI CreateWindowExA(DWORD ex, const char *cls, const char *title, DWORD style,
                               int x, int y, int w, int h, HWND parent, HMENU menu,
                               HINSTANCE inst, void *param);
BOOL    WINAPI DestroyWindow(HWND h);
BOOL    WINAPI ShowWindow(HWND h, int how);
BOOL    WINAPI UpdateWindow(HWND h);
BOOL    WINAPI InvalidateRect(HWND h, const RECT *r, BOOL erase);
BOOL    WINAPI GetClientRect(HWND h, RECT *out);
BOOL    WINAPI GetMessageA(MSG *msg, HWND filter, UINT first, UINT last);
BOOL    WINAPI PeekMessageA(MSG *msg, HWND filter, UINT first, UINT last, UINT remove);
BOOL    WINAPI TranslateMessage(const MSG *msg);
LRESULT WINAPI DispatchMessageA(const MSG *msg);
LRESULT WINAPI DefWindowProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp);
void    WINAPI PostQuitMessage(int code);
BOOL    WINAPI PostMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp);
LRESULT WINAPI SendMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp);
HDC     WINAPI BeginPaint(HWND h, PAINTSTRUCT *ps);
BOOL    WINAPI EndPaint(HWND h, const PAINTSTRUCT *ps);
UINT_PTR WINAPI SetTimer(HWND h, UINT_PTR id, UINT period, void *fn);
BOOL    WINAPI KillTimer(HWND h, UINT_PTR id);
int     WINAPI MessageBoxA(HWND owner, const char *text, const char *title, UINT type);
HCURSOR WINAPI LoadCursorA(HINSTANCE inst, const char *name);
int     WINAPI GetSystemMetrics(int index);
BOOL    WINAPI SetWindowTextA(HWND h, const char *text);
int     WINAPI GetWindowTextA(HWND h, char *out, int cap);

/* ------------------------------------------------------------------- gdi32 */

#define RGB(r, g, b) ((DWORD)((BYTE)(r) | ((BYTE)(g) << 8) | ((BYTE)(b) << 16)))
#define TRANSPARENT 1
#define OPAQUE      2
#define SRCCOPY     0x00CC0020
#define DT_CENTER   0x0001
#define DT_VCENTER  0x0004
#define DT_SINGLELINE 0x0020
#define WHITE_BRUSH 0
#define BLACK_PEN   7

HBRUSH  WINAPI CreateSolidBrush(DWORD colour);
HPEN    WINAPI CreatePen(int style, int width, DWORD colour);
BOOL    WINAPI DeleteObject(HGDIOBJ o);
HGDIOBJ WINAPI GetStockObject(int which);
HGDIOBJ WINAPI SelectObject(HDC dc, HGDIOBJ o);
DWORD   WINAPI SetTextColor(HDC dc, DWORD colour);
int     WINAPI SetBkMode(HDC dc, int mode);
BOOL    WINAPI TextOutA(HDC dc, int x, int y, const char *text, int len);
int     WINAPI DrawTextA(HDC dc, const char *text, int len, RECT *r, UINT format);
BOOL    WINAPI Rectangle(HDC dc, int l, int t, int r, int b);
BOOL    WINAPI Ellipse(HDC dc, int l, int t, int r, int b);
BOOL    WINAPI MoveToEx(HDC dc, int x, int y, POINT *old);
BOOL    WINAPI LineTo(HDC dc, int x, int y);
int     WINAPI FillRect(HDC dc, const RECT *r, HBRUSH brush);
DWORD   WINAPI SetPixel(HDC dc, int x, int y, DWORD colour);
DWORD   WINAPI GetPixel(HDC dc, int x, int y);
BOOL    WINAPI GetTextExtentPoint32A(HDC dc, const char *text, int len, SIZE *out);

/* ---------------------------------------------------------------- advapi32 */

#define HKEY_CURRENT_USER  ((HKEY)(ULONG_PTR)0x80000001ull)
#define HKEY_LOCAL_MACHINE ((HKEY)(ULONG_PTR)0x80000002ull)
#define KEY_ALL_ACCESS     0xF003F
#define REG_SZ             1
#define REG_DWORD          4

LONG WINAPI RegCreateKeyExA(HKEY parent, const char *sub, DWORD reserved, char *cls,
                            DWORD options, DWORD access, void *sa, HKEY *out, DWORD *disposition);
LONG WINAPI RegOpenKeyExA(HKEY parent, const char *sub, DWORD options, DWORD access, HKEY *out);
LONG WINAPI RegSetValueExA(HKEY key, const char *name, DWORD reserved, DWORD type,
                           const BYTE *data, DWORD len);
LONG WINAPI RegQueryValueExA(HKEY key, const char *name, DWORD *reserved, DWORD *type,
                             BYTE *data, DWORD *len);
LONG WINAPI RegEnumValueA(HKEY key, DWORD index, char *name, DWORD *name_len, DWORD *reserved,
                          DWORD *type, BYTE *data, DWORD *len);
LONG WINAPI RegDeleteValueA(HKEY key, const char *name);
LONG WINAPI RegDeleteKeyA(HKEY parent, const char *sub);
LONG WINAPI RegCloseKey(HKEY key);

/* ------------------------------------------------------------------ msvcrt */

int    __cdecl printf(const char *fmt, ...);
int    __cdecl sprintf(char *buf, const char *fmt, ...);
int    __cdecl puts(const char *s);
void  *__cdecl malloc(SIZE_T n);
void   __cdecl free(void *p);
void  *__cdecl memset(void *d, int c, SIZE_T n);
void  *__cdecl memcpy(void *d, const void *s, SIZE_T n);
int    __cdecl memcmp(const void *a, const void *b, SIZE_T n);
SIZE_T __cdecl strlen(const char *s);
int    __cdecl strcmp(const char *a, const char *b);
char  *__cdecl strcpy(char *d, const char *s);
char  *__cdecl strcat(char *d, const char *s);
char  *__cdecl strstr(const char *h, const char *n);
int    __cdecl atoi(const char *s);
double __cdecl sqrt(double v);
double __cdecl pow(double a, double b);
double __cdecl sin(double v);
double __cdecl cos(double v);
double __cdecl fabs(double v);
void   __cdecl qsort(void *base, SIZE_T count, SIZE_T size,
                     int (__cdecl *cmp)(const void *, const void *));
void   __cdecl exit(int code);
int    __cdecl rand(void);
void   __cdecl srand(unsigned seed);

/* ------------------------------------------------------------- the reporting
 *
 * Every test prints one line per check so that a failure names itself, and
 * counts them so the exit status says whether the whole program passed. */
static int checks_run, checks_failed;

static void check(int ok, const char *what) {
    checks_run++;
    if (!ok) checks_failed++;
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

static int report(const char *name) {
    printf("%s: %d checks, %d failed\n", name, checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}

/* ------------------------------------------------------------- the entry point
 *
 * There is no C runtime object file to link against, so the startup the linker
 * expects to find is written out here.  It does what the real one does at the
 * point that matters: call main, and turn its return value into the process's
 * exit code rather than returning to nowhere. */
#ifndef WIN_NO_STARTUP
int main(void);

/* Objects that exist before the program does.
 *
 * A C++ program can have things built before main runs - a constructor for
 * every object at file scope.  The compiler does not emit calls to them; it
 * puts a pointer to each one in a section with a sorted name, and relies on
 * the linker gathering all of those together in order.  Running them is the
 * startup's job, and a startup that only calls main leaves every one of those
 * objects unbuilt, which the program then uses without noticing.
 *
 * The two markers below are what the walk runs between: everything the
 * compiler contributes sorts after the first and before the second. */
typedef void (__cdecl *win_initialiser)(void);

#pragma section(".CRT$XCA", long, read)
#pragma section(".CRT$XCZ", long, read)
__declspec(allocate(".CRT$XCA")) win_initialiser win_ctors_first[] = { 0 };
__declspec(allocate(".CRT$XCZ")) win_initialiser win_ctors_last[] = { 0 };

void mainCRTStartup(void) {
    for (win_initialiser *at = win_ctors_first; at < win_ctors_last; at++)
        if (*at) (*at)();

    ExitProcess((UINT)main());
}

/* The compiler emits a reference to this when a translation unit uses
 * floating point; on a real system it comes from the runtime library. */
int _fltused = 0x9875;
#endif

#ifdef __cplusplus
}
#endif

#endif
