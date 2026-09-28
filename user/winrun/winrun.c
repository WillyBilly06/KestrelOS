/* winrun - run a Windows program.
 *
 * The program is a PE image built for this same processor, so nothing is
 * emulated: the instructions run directly.  What it needs, and does not have
 * here, is Windows underneath it - the libraries it calls, the shape of a
 * handle, a window that can be painted, a registry to read.  That is what this
 * subsystem provides, and this file is the part that starts it.
 *
 *     winrun program.exe [arguments]
 *     winrun -v program.exe            with a running commentary
 *
 * A console program writes to the terminal it was started from.  A graphical
 * one takes over the screen and gets a real window, painted through the same
 * window manager the desktop uses.
 */
#include "win.h"

bool     win_verbose;
char     win_exe_path[512];
char     win_cmdline[1024];
WCHAR    win_cmdline_w[1024];
win_module_t *win_main_module;

/* --------------------------------------------------------------- reporting */

void win_fail(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    u32_shutdown();
    fprintf(STDERR_FD, "winrun: %s\n", buf);
    flush_output();
    exit(1);
}

void win_trace(const char *fmt, ...) {
    if (!win_verbose) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(STDERR_FD, "winrun: %s\n", buf);
}

/* ---------------------------------------------------- what the guest asks for */

/* The command line split the way a C program expects it, quotes and all. */
int win_build_argv(char ***out) {
    static char storage[1024];
    static char *argv[64];
    int argc = 0;

    strlcpy(storage, win_cmdline, sizeof storage);
    char *p = storage;
    while (*p && argc < 63) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *start;
        if (*p == '"') {
            start = ++p;
            while (*p && *p != '"') p++;
        } else {
            start = p;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        if (*p) *p++ = 0;
        argv[argc++] = start;
    }
    argv[argc] = NULL;
    *out = argv;
    return argc;
}

/* The runtime asks for environment variables through its own door; kernel32
 * has the table, so it answers for both. */
DWORD win_getenv(const char *name, char *out, DWORD cap) {
    extern DWORD win_env_lookup(const char *name, char *out, DWORD cap);
    return win_env_lookup(name, out, cap);
}

/* The C runtime starts threads through its own entry points, which have to end
 * up in the same place kernel32's do. */
HANDLE win_create_thread(void *start, void *arg, unsigned flags, unsigned *tid) {
    extern HANDLE win_thread_create(void *start, void *arg, unsigned flags, unsigned *tid);
    return win_thread_create(start, arg, flags, tid);
}
void win_exit_thread(unsigned code) {
    extern void win_thread_exit(unsigned code);
    win_thread_exit(code);
}

/* ------------------------------------------------------------------ start-up */

void win_libraries_init(void) {
    /* Order matters only in that kernel32 has to exist before anything can
     * import from it; registration itself is independent. */
    k32_init();
    crt_init();
    u32_init();
    gdi_init();
    advapi_init();
    sock_init();
    d3d_init();
    d3d12_init();
    misc_init();
    seh_init();
}

void k32_thread_start_main(uint64_t stack_base, uint64_t stack_limit);

int main(int argc, char **argv) {
    int first = 1;
    while (first < argc && argv[first][0] == '-') {
        if (!strcmp(argv[first], "-v")) { win_verbose = true; first++; continue; }
        break;
    }

    if (argc <= first) {
        printf("usage: winrun [-v] <program.exe> [arguments]\n\n");
        printf("Runs a Windows x86-64 program: console or graphical, with or\n");
        printf("without DLLs of its own.  A function that is not implemented is\n");
        printf("reported by name if the program ever calls it.\n");
        return 1;
    }

    strlcpy(win_exe_path, argv[first], sizeof win_exe_path);

    /* If the name was given the Windows way, translate it before looking. */
    if (strchr(win_exe_path, '\\')) {
        char host[512];
        win_path_to_host(win_exe_path, host, sizeof host);
        strlcpy(win_exe_path, host, sizeof win_exe_path);
    }
    if (!file_exists(win_exe_path)) {
        /* A bare name is looked for beside the other programs, so "winrun
         * hello.exe" works from anywhere. */
        char guess[512];
        snprintf(guess, sizeof guess, "/bin/%s", win_exe_path);
        if (file_exists(guess)) strlcpy(win_exe_path, guess, sizeof win_exe_path);
    }

    size_t n = 0;
    n += strlcpy(win_cmdline + n, win_exe_path, sizeof win_cmdline - n);
    for (int i = first + 1; i < argc && n < sizeof win_cmdline - 2; i++) {
        win_cmdline[n++] = ' ';
        n += strlcpy(win_cmdline + n, argv[i], sizeof win_cmdline - n);
    }
    win_cmdline[n < sizeof win_cmdline ? n : sizeof win_cmdline - 1] = 0;
    win_utf8_to_wide(win_cmdline, win_cmdline_w, sizeof win_cmdline_w / sizeof win_cmdline_w[0]);

    win_libraries_init();

    win_main_module = pe_load(win_exe_path, false);
    if (!win_main_module) win_fail("%s could not be loaded", win_exe_path);
    if (!win_main_module->entry) win_fail("%s has no entry point", win_exe_path);

    /* The thread block, which guest code reads through GS.  The stack bounds
     * come from this thread's actual stack. */
    uint64_t here = (uint64_t)(uintptr_t)&argc;
    k32_thread_start_main((here + 0xFFFF) & ~0xFFFFULL, (here - 0x30000) & ~0xFFFULL);

    seh_install();

    bool gui = pe_is_gui(win_main_module);
    if (gui) {
        if (!u32_start_gui()) {
            if (errno == EBUSY)
                win_fail("%s is a graphical program and another program has the "
                         "screen; leave the desktop with Ctrl+Alt+C and run it "
                         "from there", win_exe_path);
            win_fail("%s is a graphical program and there is no display to run "
                     "it on", win_exe_path);
        }
        win_trace("%s is a graphical program; the display is now its own", win_exe_path);
    }

    pe_run_tls_callbacks(win_main_module, 1 /* DLL_PROCESS_ATTACH */);

    win_trace("entering %s at %p", win_main_module->name, (void *)(uintptr_t)win_main_module->entry);
    flush_output();

    typedef int WINAPI (*entry_fn)(void);
    int code = ((entry_fn)(uintptr_t)win_main_module->entry)();

    u32_shutdown();
    flush_output();
    return code;
}
