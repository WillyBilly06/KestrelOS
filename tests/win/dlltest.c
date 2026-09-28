/* dlltest - a program that ships a DLL and also loads one by hand.
 *
 * Two paths have to work and they are not the same: an import resolved by the
 * loader before the program starts, and one looked up by name while it is
 * running.  A program with plug-ins uses the second; almost everything else
 * uses the first. */
#include "winapi.h"

/* Resolved by the loader, from the import table, before main runs. */
__declspec(dllimport) int add_up(int a, int b);
__declspec(dllimport) const char *library_name(void);
__declspec(dllimport) int was_initialised(void);
__declspec(dllimport) int calls_so_far(void);
__declspec(dllimport) int apply(int (*fn)(int), int value);

static int doubler(int v) { return v * 2; }

int main(void) {
    printf("dlltest: shipping a DLL\n");

    printf(" imports resolved before the program started\n");
    check(add_up(2, 3) == 5, "a function in the DLL was called");
    check(!strcmp(library_name(), "testlib"), "and one returning a string");
    check(was_initialised(), "DllMain ran when the DLL was loaded");

    int before = calls_so_far();
    add_up(1, 1);
    check(calls_so_far() == before + 1, "the DLL keeps state between calls");

    printf(" calling back into the program\n");
    check(apply(doubler, 21) == 42, "the DLL called a function in the program");

    printf(" loading one by hand\n");
    HMODULE m = LoadLibraryA("testlib.dll");
    check(m != NULL, "LoadLibraryA found it");
    check(m == GetModuleHandleA("testlib.dll"), "and it is the copy already loaded");

    typedef int (*add_fn)(int, int);
    add_fn found = (add_fn)GetProcAddress(m, "add_up");
    check(found != NULL, "GetProcAddress found add_up by name");
    check(found == add_up, "at the same address the loader used");
    check(found && found(20, 22) == 42, "and calling it works");

    check(GetProcAddress(m, "no_such_function") == NULL, "a name that is not there is refused");

    check(FreeLibrary(m), "FreeLibrary accepted it");

    printf(" the program's own module\n");
    HMODULE self = GetModuleHandleA(NULL);
    check(self != NULL, "GetModuleHandleA(NULL) names the program");
    char path[512];
    DWORD n = GetModuleFileNameA(self, path, sizeof path);
    check(n > 0 && strstr(path, "dlltest") != NULL, "and GetModuleFileNameA gives its path");
    if (n) printf("       %s\n", path);

    return report("dlltest");
}
