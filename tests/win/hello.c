/* hello.c - a Windows console program, built for Windows, run under KestrelOS.
 *
 * Deliberately written against the Win32 API rather than a C runtime: it is a
 * genuine PE32+ console executable with an import table, base relocations and
 * a real entry point, which is exactly what the loader has to cope with.
 */

typedef unsigned int   DWORD;
typedef int            BOOL;
typedef void          *HANDLE;
typedef const char    *LPCSTR;
typedef unsigned long long ULONGLONG;

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_INPUT_HANDLE  ((DWORD)-10)

__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD nStdHandle);
__declspec(dllimport) BOOL   __stdcall WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) BOOL   __stdcall ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
__declspec(dllimport) void   __stdcall ExitProcess(DWORD);
__declspec(dllimport) LPCSTR __stdcall GetCommandLineA(void);
__declspec(dllimport) DWORD  __stdcall GetTickCount(void);
__declspec(dllimport) HANDLE __stdcall GetProcessHeap(void);
__declspec(dllimport) void  *__stdcall HeapAlloc(HANDLE, DWORD, unsigned long long);
__declspec(dllimport) BOOL   __stdcall HeapFree(HANDLE, DWORD, void *);

static DWORD str_len(const char *s) {
    DWORD n = 0;
    while (s[n]) n++;
    return n;
}

static void out(const char *s) {
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, str_len(s), &written, 0);
}

static void out_num(unsigned long long v) {
    char buf[24];
    int n = 0;
    if (!v) buf[n++] = '0';
    while (v) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }

    char rev[25];
    int j = 0;
    while (n) rev[j++] = buf[--n];
    rev[j] = 0;
    out(rev);
}

/* A relocatable pointer into initialised data: if base relocations were not
 * applied correctly, reading through this would fault or print rubbish. */
static const char *const banner = "Hello from a Windows program.";
static const char *const table[3] = { "one", "two", "three" };

void mainCRTStartup(void) {
    out("\r\n");
    out(banner);
    out("\r\n\r\n");

    out("  This is a PE32+ console executable built for Windows.\r\n");
    out("  It is running under KestrelOS through the winrun loader.\r\n\r\n");

    out("  Command line : ");
    out(GetCommandLineA());
    out("\r\n");

    out("  Uptime       : ");
    out_num(GetTickCount());
    out(" ms\r\n");

    /* Exercise the heap shim. */
    HANDLE heap = GetProcessHeap();
    char *buf = (char *)HeapAlloc(heap, 8 /* zero */, 64);
    if (buf) {
        const char *src = "  Heap         : allocated and written\r\n";
        DWORD i = 0;
        while (src[i] && i < 63) { buf[i] = src[i]; i++; }
        buf[i] = 0;
        out(buf);
        HeapFree(heap, 0, buf);
    } else {
        out("  Heap         : allocation failed\r\n");
    }

    /* Read through a relocated pointer array. */
    out("  Relocations  : ");
    for (int i = 0; i < 3; i++) {
        out(table[i]);
        if (i < 2) out(", ");
    }
    out("\r\n\r\n");

    ExitProcess(0);
}
