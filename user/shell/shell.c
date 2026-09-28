/* shell - the KestrelOS command interpreter.
 *
 * Builtins cover everything the system needs to be usable without extra
 * binaries; anything else is looked up in /bin, and a name ending in .exe is
 * handed to winrun so Windows console programs can be launched the same way.
 */
#include "kestrel.h"

#define MAX_ARGS 24
#define LINE_MAX 512

static char cwd[512] = "/";

/* ------------------------------------------------------------------ helpers */

static void update_cwd(void) {
    if (!getcwd(cwd, sizeof cwd)) strlcpy(cwd, "/", sizeof cwd);
}

static const char *type_name(uint32_t type) {
    switch (type) {
    case FT_DIR: return "dir";
    case FT_CHR: return "char";
    case FT_BLK: return "block";
    default:     return "file";
    }
}

/* Resolve a possibly relative path for display and for passing on. */
static void join_path(const char *in, char *out, size_t cap) {
    if (!in || !*in) { strlcpy(out, cwd, cap); return; }
    if (in[0] == '/') { strlcpy(out, in, cap); return; }
    if (!strcmp(cwd, "/")) snprintf(out, cap, "/%s", in);
    else snprintf(out, cap, "%s/%s", cwd, in);
}

/* ------------------------------------------------------------------ builtins */

static int cmd_help(int argc, char **argv);

static int cmd_ls(int argc, char **argv) {
    const char *target = argc > 1 ? argv[1] : ".";
    bool long_form = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) { long_form = true; if (target == argv[i]) target = "."; }
        else target = argv[i];
    }

    char path[512];
    join_path(target, path, sizeof path);

    kstat_t st;
    if (stat(path, &st) < 0) {
        printf("ls: %s: %s\n", target, strerror(errno));
        return 1;
    }
    if (st.type != FT_DIR) {
        printf("%s\n", target);
        return 0;
    }

    DIR *d = opendir(path);
    if (!d) { printf("ls: %s: %s\n", target, strerror(errno)); return 1; }

    kdirent_t e;
    int count = 0, col = 0;
    int cols, rows;
    console_size(&cols, &rows);

    while (readdir(d, &e) == 0) {
        count++;
        if (long_form) {
            char size[24];
            format_size(size, sizeof size, e.size);
            char full[600];
            join_path(target, full, sizeof full);
            strlcat(full, "/", sizeof full);
            strlcat(full, e.name, sizeof full);

            kstat_t es;
            char when[32] = "";
            if (stat(full, &es) == 0 && es.mtime) format_time(when, sizeof when, es.mtime);

            printf("%-5s %10s  %-19s %s%s%s\n", type_name(e.type), size, when,
                   e.type == FT_DIR ? A_CYAN : "", e.name, e.type == FT_DIR ? A_RESET : "");
        } else {
            int width = 20;
            if (e.type == FT_DIR) printf(A_CYAN "%-*s" A_RESET, width, e.name);
            else printf("%-*s", width, e.name);
            col += width;
            if (col + width > cols) { printf("\n"); col = 0; }
        }
    }
    if (!long_form && col) printf("\n");
    closedir(d);
    if (!count) printf(A_GREY "(empty)" A_RESET "\n");
    return 0;
}

static int cmd_cat(int argc, char **argv) {
    if (argc < 2) { printf("usage: cat <file>...\n"); return 1; }

    char *buf = malloc(8192);
    if (!buf) { printf("cat: out of memory\n"); return 1; }

    int result = 0;
    for (int i = 1; i < argc; i++) {
        char path[512];
        join_path(argv[i], path, sizeof path);

        int fd = open(path, O_RDONLY);
        if (fd < 0) { printf("cat: %s: %s\n", argv[i], strerror(errno)); result = 1; continue; }

        for (;;) {
            ssize_t n = read(fd, buf, 8192);
            if (n <= 0) break;
            write(STDOUT_FD, buf, (size_t)n);
        }
        close(fd);
    }
    free(buf);
    return result;
}

static int cmd_cd(int argc, char **argv) {
    const char *target = argc > 1 ? argv[1] : "/";
    if (chdir(target) < 0) {
        printf("cd: %s: %s\n", target, strerror(errno));
        return 1;
    }
    update_cwd();
    return 0;
}

static int cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("%s\n", cwd);
    return 0;
}

static int cmd_mkdir(int argc, char **argv) {
    if (argc < 2) { printf("usage: mkdir <directory>...\n"); return 1; }
    int result = 0;
    for (int i = 1; i < argc; i++) {
        char path[512];
        join_path(argv[i], path, sizeof path);
        if (mkdir(path) < 0) { printf("mkdir: %s: %s\n", argv[i], strerror(errno)); result = 1; }
    }
    return result;
}

static int remove_tree(const char *path) {
    kstat_t st;
    if (stat(path, &st) < 0) return -1;

    if (st.type == FT_DIR) {
        /* Directory entries shift as things are removed, so restart the scan
         * after each deletion rather than trusting the index. */
        for (;;) {
            DIR *d = opendir(path);
            if (!d) return -1;
            kdirent_t e;
            if (readdir(d, &e) != 0) { closedir(d); break; }
            closedir(d);

            char child[512];
            snprintf(child, sizeof child, "%s/%s", path, e.name);
            if (remove_tree(child) < 0) return -1;
        }
    }
    return unlink(path);
}

static int cmd_rm(int argc, char **argv) {
    if (argc < 2) { printf("usage: rm [-r] <path>...\n"); return 1; }

    bool recursive = false;
    int result = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "-rf")) { recursive = true; continue; }

        char path[512];
        join_path(argv[i], path, sizeof path);

        int r = recursive ? remove_tree(path) : unlink(path);
        if (r < 0) { printf("rm: %s: %s\n", argv[i], strerror(errno)); result = 1; }
    }
    return result;
}

static int cmd_cp(int argc, char **argv) {
    if (argc < 3) { printf("usage: cp <source> <destination>\n"); return 1; }

    char from[512], to[512];
    join_path(argv[1], from, sizeof from);
    join_path(argv[2], to, sizeof to);

    /* Copying onto a directory means "into" it. */
    kstat_t st;
    if (stat(to, &st) == 0 && st.type == FT_DIR) {
        const char *leaf = strrchr(from, '/');
        leaf = leaf ? leaf + 1 : from;
        strlcat(to, "/", sizeof to);
        strlcat(to, leaf, sizeof to);
    }

    if (copy_file(from, to) < 0) {
        printf("cp: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}

static int cmd_mv(int argc, char **argv) {
    if (argc < 3) { printf("usage: mv <source> <destination>\n"); return 1; }

    char from[512], to[512];
    join_path(argv[1], from, sizeof from);
    join_path(argv[2], to, sizeof to);

    kstat_t st;
    if (stat(to, &st) == 0 && st.type == FT_DIR) {
        const char *leaf = strrchr(from, '/');
        leaf = leaf ? leaf + 1 : from;
        strlcat(to, "/", sizeof to);
        strlcat(to, leaf, sizeof to);
    }

    if (rename(from, to) == 0) return 0;

    /* A rename across directories or filesystems is not supported by the
     * kernel, so fall back to copying and removing. */
    if (errno == EXDEV) {
        if (copy_file(from, to) == 0 && unlink(from) == 0) return 0;
    }
    printf("mv: %s: %s\n", argv[1], strerror(errno));
    return 1;
}

static int cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) printf("%s%s", argv[i], i + 1 < argc ? " " : "");
    printf("\n");
    return 0;
}

static int cmd_write(int argc, char **argv) {
    if (argc < 3) { printf("usage: write <file> <text>...\n"); return 1; }

    char path[512];
    join_path(argv[1], path, sizeof path);

    char text[1024];
    size_t n = 0;
    for (int i = 2; i < argc; i++) {
        n += strlcpy(text + n, argv[i], sizeof text - n);
        if (n < sizeof text - 2 && i + 1 < argc) { text[n++] = ' '; text[n] = 0; }
    }
    if (n < sizeof text - 1) { text[n++] = '\n'; text[n] = 0; }

    if (write_file(path, text, n) < 0) {
        printf("write: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}

static int cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    console_clear();
    return 0;
}

static int cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    static const char *states[] = { "unused", "starting", "ready", "running", "sleeping", "blocked", "zombie" };

    printf(A_BOLD "%5s %5s %-16s %-9s %8s" A_RESET "\n", "PID", "PPID", "NAME", "STATE", "CPU(ms)");
    for (uint32_t i = 0;; i++) {
        kprocinfo_t p;
        if (proclist(i, &p) < 0) break;
        printf("%5d %5d %-16s %-9s %8llu\n", p.pid, p.parent, p.name,
               p.state < 7 ? states[p.state] : "?", (unsigned long long)p.cpu_ms);
    }
    return 0;
}

static int cmd_kill(int argc, char **argv) {
    if (argc < 2) { printf("usage: kill <pid>\n"); return 1; }
    int pid = atoi(argv[1]);
    if (kill(pid) < 0) { printf("kill: %d: %s\n", pid, strerror(errno)); return 1; }
    return 0;
}

static int cmd_meminfo(int argc, char **argv) {
    (void)argc; (void)argv;
    uint64_t m[6];
    if (meminfo(m) < 0) { printf("meminfo: %s\n", strerror(errno)); return 1; }

    char a[24], b[24], c[24], d[24], e[24], f[24];
    format_size(a, sizeof a, m[0]);
    format_size(b, sizeof b, m[1]);
    format_size(c, sizeof c, m[2]);
    format_size(d, sizeof d, m[3]);
    format_size(e, sizeof e, m[4]);
    format_size(f, sizeof f, m[5]);

    printf("Physical memory   %10s total, %10s free, %10s in use\n", a, b, c);
    printf("Kernel heap       %10s total, %10s in use\n", d, e);
    printf("This process heap %10s\n", f);
    return 0;
}

static int cmd_lspci(int argc, char **argv) {
    (void)argc; (void)argv;
    for (uint32_t i = 0;; i++) {
        kpciinfo_t p;
        if (enum_pci(i, &p) < 0) break;
        printf("%02x:%02x.%u  %s\n", p.bus, p.slot, p.func, p.description);
    }
    return 0;
}

static int cmd_lsblk(int argc, char **argv) {
    (void)argc; (void)argv;
    printf(A_BOLD "%-10s %12s  %-8s %s" A_RESET "\n", "NAME", "SIZE", "TYPE", "MODEL / LABEL");
    for (uint32_t i = 0;; i++) {
        kblockinfo_t b;
        if (enum_block(i, &b) < 0) break;
        char size[24];
        format_size(size, sizeof size, b.size);
        printf("%-10s %12s  %-8s %s\n", b.name, size,
               b.is_partition ? "part" : "disk",
               b.label[0] ? b.label : b.model);
    }
    return 0;
}

static int cmd_mount(int argc, char **argv) {
    if (argc == 1) {
        char buf[1024];
        if (mountlist(buf, sizeof buf) < 0) { printf("mount: %s\n", strerror(errno)); return 1; }
        printf("%s", buf);
        return 0;
    }
    if (argc < 3) { printf("usage: mount <device> <path>   (no arguments lists mounts)\n"); return 1; }
    if (mount(argv[1], argv[2]) < 0) {
        printf("mount: %s on %s: %s\n", argv[1], argv[2], strerror(errno));
        return 1;
    }
    return 0;
}

static int cmd_umount(int argc, char **argv) {
    if (argc < 2) { printf("usage: umount <path>\n"); return 1; }
    if (unmount(argv[1]) < 0) { printf("umount: %s: %s\n", argv[1], strerror(errno)); return 1; }
    return 0;
}

static int cmd_date(int argc, char **argv) {
    (void)argc; (void)argv;
    char when[32];
    format_time(when, sizeof when, time_now());
    printf("%s UTC\n", when);
    return 0;
}

static int cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    uint64_t ms = uptime_ms();
    printf("up %llu:%02llu:%02llu.%03llu\n",
           (unsigned long long)(ms / 3600000),
           (unsigned long long)((ms / 60000) % 60),
           (unsigned long long)((ms / 1000) % 60),
           (unsigned long long)(ms % 1000));
    return 0;
}

static int cmd_sysinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    ksysinfo_t info;
    if (sysinfo(&info) < 0) { printf("sysinfo: %s\n", strerror(errno)); return 1; }

    char total[24], freemem[24];
    format_size(total, sizeof total, info.mem_total);
    format_size(freemem, sizeof freemem, info.mem_free);

    printf("Kernel      %s\n", info.kernel);
    printf("CPU         %s (%u logical)\n", info.cpu, info.cpu_count);
    printf("Memory      %s total, %s free\n", total, freemem);
    printf("Display     %ux%u\n", info.fb_width, info.fb_height);
    printf("Devices     %u PCI, %u block\n", info.pci_count, info.block_count);
    printf("Processes   %u\n", info.proc_count);
    return 0;
}

static int cmd_sync(int argc, char **argv) {
    (void)argc; (void)argv;
    if (sync() < 0) { printf("sync: %s\n", strerror(errno)); return 1; }
    printf("written to disk\n");
    return 0;
}

static int cmd_reboot(int argc, char **argv) { (void)argc; (void)argv; reboot(); return 0; }
static int cmd_poweroff(int argc, char **argv) { (void)argc; (void)argv; poweroff(); return 0; }

/* ------------------------------------------------------------- dispatch table */

typedef struct {
    const char *name;
    int (*fn)(int, char **);
    const char *help;
} builtin_t;

static const builtin_t builtins[] = {
    { "help",     cmd_help,     "list these commands" },
    { "ls",       cmd_ls,       "list a directory (-l for details)" },
    { "cd",       cmd_cd,       "change directory" },
    { "pwd",      cmd_pwd,      "print the working directory" },
    { "cat",      cmd_cat,      "print a file" },
    { "write",    cmd_write,    "write text to a file" },
    { "mkdir",    cmd_mkdir,    "create a directory" },
    { "rm",       cmd_rm,       "remove a file or directory (-r)" },
    { "cp",       cmd_cp,       "copy a file" },
    { "mv",       cmd_mv,       "move or rename a file" },
    { "echo",     cmd_echo,     "print its arguments" },
    { "clear",    cmd_clear,    "clear the screen" },
    { "ps",       cmd_ps,       "list processes" },
    { "kill",     cmd_kill,     "terminate a process" },
    { "meminfo",  cmd_meminfo,  "show memory use" },
    { "lspci",    cmd_lspci,    "list PCI devices" },
    { "lsblk",    cmd_lsblk,    "list disks and partitions" },
    { "mount",    cmd_mount,    "list or attach filesystems" },
    { "umount",   cmd_umount,   "detach a filesystem" },
    { "date",     cmd_date,     "show the date and time" },
    { "uptime",   cmd_uptime,   "show how long the system has run" },
    { "sysinfo",  cmd_sysinfo,  "summarise the machine" },
    { "sync",     cmd_sync,     "flush pending writes to disk" },
    { "reboot",   cmd_reboot,   "restart the machine" },
    { "poweroff", cmd_poweroff, "switch the machine off" },
};

static int cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("\n" A_BOLD "Built-in commands" A_RESET "\n");
    for (size_t i = 0; i < sizeof builtins / sizeof builtins[0]; i++)
        printf("  %-10s %s\n", builtins[i].name, builtins[i].help);

    printf("\n" A_BOLD "Programs in /bin" A_RESET "\n");
    DIR *d = opendir("/bin");
    if (d) {
        kdirent_t e;
        int col = 0, cols, rows;
        console_size(&cols, &rows);
        printf("  ");
        while (readdir(d, &e) == 0) {
            printf("%-14s", e.name);
            col += 14;
            if (col + 14 > cols - 2) { printf("\n  "); col = 0; }
        }
        printf("\n");
        closedir(d);
    }
    printf("\nA name ending in " A_BOLD ".exe" A_RESET " is run through the Windows program loader.\n\n");
    return 0;
}

/* ------------------------------------------------------------------ external */

static int run_external(int argc, char **argv) {
    char path[512];

    /* An explicit path is used as given; a bare name is looked up in /bin. */
    if (strchr(argv[0], '/')) join_path(argv[0], path, sizeof path);
    else snprintf(path, sizeof path, "/bin/%s", argv[0]);

    size_t len = strlen(path);
    bool is_exe = len > 4 && !strcasecmp(path + len - 4, ".exe");

    if (!file_exists(path) && !is_exe) {
        /* Maybe they meant a Windows program. */
        char with_exe[512];
        snprintf(with_exe, sizeof with_exe, "%s.exe", path);
        if (file_exists(with_exe)) { strlcpy(path, with_exe, sizeof path); is_exe = true; }
    }

    if (!file_exists(path)) {
        printf("%s: command not found\n", argv[0]);
        return 127;
    }

    const char *child[MAX_ARGS + 2];
    int child_argc = 0;

    if (is_exe) {
        if (!file_exists("/bin/winrun")) {
            printf("%s: the Windows program loader (/bin/winrun) is not installed\n", argv[0]);
            return 127;
        }
        child[child_argc++] = "winrun";
        child[child_argc++] = path;
        for (int i = 1; i < argc && child_argc < MAX_ARGS; i++) child[child_argc++] = argv[i];

        int status = run("/bin/winrun", child, child_argc);
        if (status < 0) { printf("%s: %s\n", argv[0], strerror(errno)); return 1; }
        return status;
    }

    for (int i = 0; i < argc && child_argc < MAX_ARGS; i++) child[child_argc++] = argv[i];

    int status = run(path, child, child_argc);
    if (status < 0) { printf("%s: %s\n", argv[0], strerror(errno)); return 1; }
    return status;
}

/* ------------------------------------------------------------------- parsing */

/* Split a line into arguments, honouring double quotes so paths with spaces
 * survive. */
static int split(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;

    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = 0;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = 0;
        }
    }
    return argc;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    update_cwd();

    char line[LINE_MAX];
    char *args[MAX_ARGS];
    int last_status = 0;

    for (;;) {
        if (last_status) printf(A_GREEN "kestrel" A_RESET ":" A_BLUE "%s" A_RESET A_RED " [%d]" A_RESET "$ ", cwd, last_status);
        else printf(A_GREEN "kestrel" A_RESET ":" A_BLUE "%s" A_RESET "$ ", cwd);

        int n = readline(line, sizeof line);
        if (n < 0) {
            /* End of input: on a console this means Ctrl-D. */
            printf("\n");
            continue;
        }
        if (!n) continue;

        int count = split(line, args, MAX_ARGS);
        if (!count) continue;

        if (!strcmp(args[0], "exit")) {
            int code = count > 1 ? atoi(args[1]) : 0;
            return code;
        }

        bool handled = false;
        for (size_t i = 0; i < sizeof builtins / sizeof builtins[0]; i++) {
            if (strcmp(args[0], builtins[i].name)) continue;
            last_status = builtins[i].fn(count, args);
            handled = true;
            break;
        }
        if (!handled) last_status = run_external(count, args);

        update_cwd();
    }
}
