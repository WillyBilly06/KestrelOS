/* testlib - a real DLL, to prove that a program can ship its own.
 *
 * It has state of its own, initialised in DllMain, so that a caller can tell
 * the difference between a DLL that was loaded properly and one whose exports
 * merely happened to resolve. */
#define WIN_NO_STARTUP
#include "winapi.h"

/* A DLL is entered through DllMain, not through a startup routine. */
int _fltused = 0x9875;

#define DLL_PROCESS_ATTACH 1

__declspec(dllexport) int   library_ready;
__declspec(dllexport) int   call_count;

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved) {
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) library_ready = 0xC0FFEE;
    return TRUE;
}

__declspec(dllexport) int add_up(int a, int b) {
    call_count++;
    return a + b;
}

__declspec(dllexport) const char *library_name(void) {
    call_count++;
    return "testlib";
}

__declspec(dllexport) int was_initialised(void) {
    return library_ready == 0xC0FFEE;
}

__declspec(dllexport) int calls_so_far(void) {
    return call_count;
}

/* A function that calls back into the program, which is how a plug-in works
 * and the thing most likely to go wrong across a module boundary. */
__declspec(dllexport) int apply(int (*fn)(int), int value) {
    call_count++;
    return fn(value);
}
