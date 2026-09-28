/* events - read the system event log.
 *
 * By default it shows warnings and worse, which is what someone looking for a
 * problem actually wants; -a widens it to everything the kernel recorded.
 */
#include "kestrel.h"

static const char *level_colour(int level) {
    switch (level) {
    case 0: return A_GREY;      /* debug */
    case 1: return "";          /* info  */
    case 2: return A_YELLOW;    /* warn  */
    case 3: return A_RED;       /* error */
    case 4: return A_BOLD A_RED;/* crit  */
    default: return "";
    }
}

static void print_record(const klog_record_t *r) {
    printf("%s[%5llu.%03llu] %-5s %-9s %s" A_RESET "\n",
           level_colour((int)r->level),
           (unsigned long long)(r->time_ms / 1000),
           (unsigned long long)(r->time_ms % 1000),
           log_level_name((int)r->level),
           r->subsys,
           r->msg);
}

/* Emit one entry at every level.  This is how the persistence path gets
 * exercised on a machine that is behaving itself and producing no warnings of
 * its own. */
static int emit_test_events(void) {
    static const char *text[5] = {
        "test entry at debug level",
        "test entry at info level",
        "test entry at warning level",
        "test entry at error level",
        "test entry at critical level",
    };
    for (int level = 0; level < 5; level++) {
        if (log_write(level, "events", text[level]) < 0) {
            printf("events: could not write a log entry: %s\n", strerror(errno));
            return 1;
        }
    }
    printf("Wrote one entry at each level.\n");

    int r = log_flush();
    if (r < 0) { printf("events: flush failed: %s\n", strerror(errno)); return 1; }
    if (r) printf("Warnings and above were written to /data/logs/events.log.\n");
    else   printf(A_GREY "No data volume is mounted, so nothing was written to disk.\n" A_RESET);
    return 0;
}

static void usage(void) {
    printf("usage: events [options]\n\n");
    printf("  -a            show every entry, not just warnings and errors\n");
    printf("  -l <level>    minimum level: debug, info, warn, error, crit\n");
    printf("  -n <count>    show only the last <count> matching entries\n");
    printf("  -f            keep running and print new entries as they arrive\n");
    printf("  -s            summarise counts by level\n");
    printf("  -p            show the persistent log from /data/logs/events.log\n");
    printf("  --clear       discard the in-memory log\n");
    printf("  --flush       force the persistent log to be written now\n\n");
}

static int level_from_name(const char *s) {
    if (!strcasecmp(s, "debug")) return 0;
    if (!strcasecmp(s, "info"))  return 1;
    if (!strcasecmp(s, "warn") || !strcasecmp(s, "warning")) return 2;
    if (!strcasecmp(s, "error") || !strcasecmp(s, "err")) return 3;
    if (!strcasecmp(s, "crit") || !strcasecmp(s, "critical")) return 4;
    return -1;
}

static int show_summary(void) {
    uint32_t counts[5];
    if (log_counts(counts) < 0) { printf("events: %s\n", strerror(errno)); return 1; }

    static const char *names[5] = { "debug", "info", "warnings", "errors", "critical" };
    printf("\n" A_BOLD "Event log summary" A_RESET "\n");
    for (int i = 0; i < 5; i++)
        printf("  %-10s %s%6u" A_RESET "\n", names[i], level_colour(i), counts[i]);

    uint64_t dropped = log_dropped();
    if (dropped) printf("  %-10s %6llu " A_GREY "(the ring buffer wrapped)" A_RESET "\n",
                        "dropped", (unsigned long long)dropped);

    printf("\n  next sequence number: %llu\n", (unsigned long long)log_next_seq());
    /* Where the log actually is, asked rather than assumed.
     *
     * This named /data/logs/events.log and, failing to find it, reported
     * that logging was "not active (no data volume mounted)".  On a stick
     * both halves were wrong: the log was active and it was on the volume
     * the machine was booted from, because there is no data volume there
     * to use.  So the one command that answers "is this being recorded"
     * answered no, while it was being recorded. */
    {
        ksysinfo_t si;
        if (sysinfo(&si) == 0 && si.logging && si.log_path[0]) {
            kstat_t st;
            if (stat(si.log_path, &st) == 0) {
                char size[24];
                format_size(size, sizeof size, st.size);
                printf("  persistent log:       %s (%s)\n", si.log_path, size);
            } else {
                printf("  persistent log:       %s\n", si.log_path);
            }
        } else {
            printf("  persistent log:       " A_GREY "not being written" A_RESET "\n");
        }
    }
    printf("\n");
    return 0;
}

static int show_persistent(void) {
    /* The same question, and the same answer: the kernel chose the file,
     * so it is the thing to ask. */
    ksysinfo_t si;
    if (sysinfo(&si) != 0 || !si.logging || !si.log_path[0]) {
        printf("events: this boot is not being written to disk.\n");
        printf(A_GREY "The notice above the taskbar says why.\n" A_RESET);
        return 1;
    }

    int fd = open(si.log_path, O_RDONLY);
    if (fd < 0) {
        printf("events: %s: %s\n", si.log_path, strerror(errno));
        return 1;
    }
    printf(A_GREY "from %s\n" A_RESET, si.log_path);
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) break;
        write(STDOUT_FD, buf, (size_t)n);
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    int min_level = 2;          /* warnings and worse by default */
    int limit = 0;
    bool follow = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) min_level = 0;
        else if (!strcmp(argv[i], "-f")) follow = true;
        else if (!strcmp(argv[i], "-s")) return show_summary();
        else if (!strcmp(argv[i], "--test")) return emit_test_events();
        else if (!strcmp(argv[i], "-p")) return show_persistent();
        else if (!strcmp(argv[i], "--clear")) {
            if (log_clear() < 0) { printf("events: %s\n", strerror(errno)); return 1; }
            printf("event log cleared\n");
            return 0;
        }
        else if (!strcmp(argv[i], "--flush")) {
            int r = log_flush();
            if (r < 0) { printf("events: %s\n", strerror(errno)); return 1; }
            printf(r ? "persistent log flushed\n" : "no persistent log is active\n");
            return 0;
        }
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) {
            min_level = level_from_name(argv[++i]);
            if (min_level < 0) { printf("events: unknown level '%s'\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else { printf("events: unrecognised option '%s'\n\n", argv[i]); usage(); return 1; }
    }

    klog_record_t batch[32];

    /* When a limit is given, walk the whole log first to find where to start,
     * so "-n 20" really shows the last twenty matches rather than the first. */
    uint64_t start = 1;
    if (limit > 0) {
        int matched = 0;
        uint64_t seq = 1;
        uint64_t marks[256];
        int mark_count = 0;

        for (;;) {
            int n = log_read(seq, batch, 32);
            if (n <= 0) break;
            for (int i = 0; i < n; i++) {
                if ((int)batch[i].level < min_level) continue;
                marks[mark_count % 256] = batch[i].seq;
                mark_count++;
                matched++;
            }
            seq = batch[n - 1].seq + 1;
        }
        if (matched > limit && mark_count >= limit) {
            int index = (mark_count - limit) % 256;
            start = marks[index];
        }
    }

    uint64_t seq = start;
    int shown = 0;
    for (;;) {
        int n = log_read(seq, batch, 32);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if ((int)batch[i].level < min_level) continue;
            print_record(&batch[i]);
            shown++;
        }
        seq = batch[n - 1].seq + 1;
    }

    if (!shown && !follow) {
        if (min_level >= 2) printf(A_GREEN "No warnings or errors have been recorded." A_RESET "\n");
        else printf("The event log is empty.\n");
    }

    if (follow) {
        printf(A_GREY "(following the log; press Ctrl-C to stop)" A_RESET "\n");
        for (;;) {
            int n = log_read(seq, batch, 32);
            if (n > 0) {
                for (int i = 0; i < n; i++) {
                    if ((int)batch[i].level < min_level) continue;
                    print_record(&batch[i]);
                }
                seq = batch[n - 1].seq + 1;
            } else {
                sleep_ms(200);
            }
        }
    }
    return 0;
}
