/* realwork - a Windows program shaped like an application.
 *
 * The other tests here each prove one thing: that an image loads, that imports
 * resolve, that a thread runs.  This one does what a program actually does -
 * maps a file into memory and reads it through the mapping, asks for pages and
 * gives up the right to write to them, starts several threads and waits for
 * all of them at once, and starts another program and waits to see how it
 * finished.
 *
 * Nothing here is unusual, which is the point.  These are the things a program
 * does without thinking about them, and a subsystem that handles each one on
 * its own can still fall over when they are used together.
 */
#include "winapi.h"

/* Not declared in the shared header, because nothing here needed them before. */
#define PAGE_READONLY_F   0x02
#define PAGE_READWRITE_F  0x04
#define MEM_COMMIT_F      0x1000
#define MEM_RESERVE_F     0x2000
#define MEM_RELEASE_F     0x8000
#define FILE_MAP_READ_F   0x0004
#define GENERIC_READ_F    0x80000000u
#define GENERIC_WRITE_F   0x40000000u
#define CREATE_ALWAYS_F   2
#define STILL_RUNNING     259

typedef struct {
    DWORD cb;
    char *reserved, *desktop, *title;
    DWORD x, y, x_size, y_size, x_chars, y_chars, fill, flags;
    unsigned short show, reserved2;
    unsigned char *reserved3;
    HANDLE in, out, err;
} STARTUPINFOA_T;

typedef struct {
    HANDLE process, thread;
    DWORD  process_id, thread_id;
} PROCESS_INFORMATION_T;

__declspec(dllimport) HANDLE __stdcall CreateFileMappingA(HANDLE, void *, DWORD,
                                                          DWORD, DWORD,
                                                          const char *);
__declspec(dllimport) void * __stdcall MapViewOfFile(HANDLE, DWORD, DWORD,
                                                     DWORD, SIZE_T);
__declspec(dllimport) BOOL   __stdcall UnmapViewOfFile(const void *);
__declspec(dllimport) BOOL   __stdcall VirtualProtect(void *, SIZE_T, DWORD,
                                                      DWORD *);
__declspec(dllimport) BOOL   __stdcall CreateProcessA(const char *, char *,
                                                      void *, void *, BOOL,
                                                      DWORD, void *,
                                                      const char *, void *,
                                                      void *);
__declspec(dllimport) BOOL   __stdcall GetExitCodeProcess(HANDLE, DWORD *);

static volatile LONG shared_total;
static HANDLE ready[4];

static DWORD __stdcall adder(void *arg) {
    int which = (int)(long long)arg;
    for (int i = 0; i < 1000; i++) InterlockedIncrement(&shared_total);
    SetEvent(ready[which]);
    return (DWORD)which;
}

int main(void) {
    /* ---- a file, read through a mapping rather than a handle ---------- */
    {
        const char *path = "realwork.tmp";
        const char *content = "mapped rather than read";
        DWORD length = 0;
        while (content[length]) length++;

        HANDLE f = CreateFileA(path, GENERIC_READ_F | GENERIC_WRITE_F, 0, NULL,
                               CREATE_ALWAYS_F, 0, NULL);
        check(f != INVALID_HANDLE_VALUE, "a file is made");

        if (f != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            WriteFile(f, content, length, &wrote, NULL);
            check(wrote == length, "and written to");

            HANDLE m = CreateFileMappingA(f, NULL, PAGE_READONLY_F, 0, 0, NULL);
            check(m != NULL, "a mapping of it is made");
            if (m) {
                const char *view = (const char *)MapViewOfFile(
                    m, FILE_MAP_READ_F, 0, 0, 0);
                check(view != NULL, "and a view onto that mapping");
                if (view) {
                    int same = 1;
                    for (DWORD i = 0; i < length; i++)
                        if (view[i] != content[i]) same = 0;
                    check(same, "what the view shows is what was written");
                    UnmapViewOfFile(view);
                }
                CloseHandle(m);
            }
            CloseHandle(f);
        }
        DeleteFileA(path);
    }

    /* ---- pages asked for, and the right to write given up ------------- */
    {
        SIZE_T size = 4096;
        unsigned char *pages = (unsigned char *)VirtualAlloc(
            NULL, size, MEM_COMMIT_F | MEM_RESERVE_F, PAGE_READWRITE_F);
        check(pages != NULL, "pages are given when asked for");
        if (pages) {
            for (SIZE_T i = 0; i < size; i++)
                pages[i] = (unsigned char)(i & 0xFF);
            int held = 1;
            for (SIZE_T i = 0; i < size; i++)
                if (pages[i] != (unsigned char)(i & 0xFF)) held = 0;
            check(held, "and hold what is put in them");

            DWORD was = 0;
            check(VirtualProtect(pages, size, PAGE_READONLY_F, &was) != 0,
                  "the right to write can be given up");
            VirtualFree(pages, 0, MEM_RELEASE_F);
        }
    }

    /* ---- several threads, waited for together ------------------------- */
    {
        HANDLE threads[4];
        int made = 1;
        shared_total = 0;
        for (int i = 0; i < 4; i++) {
            ready[i] = CreateEventA(NULL, TRUE, FALSE, NULL);
            if (!ready[i]) made = 0;
        }
        check(made, "an event for each thread to signal");

        for (int i = 0; i < 4; i++) {
            threads[i] = CreateThread(NULL, 0, adder, (void *)(long long)i, 0,
                                      NULL);
            if (!threads[i]) made = 0;
        }
        check(made, "four threads start");

        DWORD waited = WaitForMultipleObjects(4, ready, TRUE, 10000);
        check(waited == WAIT_OBJECT_0, "and all four are waited for at once");
        check(shared_total == 4000, "what they counted together is right");

        for (int i = 0; i < 4; i++) {
            CloseHandle(threads[i]);
            CloseHandle(ready[i]);
        }
    }

    /* ---- another program, started and waited for ---------------------- */
    {
        STARTUPINFOA_T si;
        PROCESS_INFORMATION_T pi;
        for (unsigned i = 0; i < sizeof si; i++) ((char *)&si)[i] = 0;
        for (unsigned i = 0; i < sizeof pi; i++) ((char *)&pi)[i] = 0;
        si.cb = sizeof si;

        char command[32] = "hello.exe";
        BOOL started = CreateProcessA(NULL, command, NULL, NULL, FALSE, 0,
                                      NULL, NULL, &si, &pi);
        check(started != 0, "another program starts");
        if (started) {
            check(WaitForSingleObject(pi.process, 10000) == WAIT_OBJECT_0,
                  "and is waited for");
            DWORD code = STILL_RUNNING;
            GetExitCodeProcess(pi.process, &code);
            check(code == 0, "and how it finished comes back");
            CloseHandle(pi.process);
        }
    }

    return report("realwork");
}
