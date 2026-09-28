/* init - the first user process.
 *
 * Prints the banner, then keeps a shell running.  If the shell ever exits the
 * machine would otherwise be unusable, so it is restarted; a shell that dies
 * immediately and repeatedly is reported rather than looped on forever.
 */
#include "kestrel.h"

static void banner(void) {
    ksysinfo_t info;
    if (sysinfo(&info) < 0) return;

    char total[24], freemem[24];
    format_size(total, sizeof total, info.mem_total);
    format_size(freemem, sizeof freemem, info.mem_free);

    printf("\n" A_CYAN "  KestrelOS" A_RESET A_GREY "  %s" A_RESET "\n", info.kernel);
    printf(A_GREY "  %s\n", info.cpu);
    printf("  %s memory (%s free), %ux%u display, %u PCI device%s, %u disk%s" A_RESET "\n\n",
           total, freemem, info.fb_width, info.fb_height,
           info.pci_count, info.pci_count == 1 ? "" : "s",
           info.block_count, info.block_count == 1 ? "" : "s");

    /* Surface anything that went wrong during boot straight away, rather than
     * making the user go looking for it. */
    uint32_t counts[5] = { 0 };
    if (log_counts(counts) == 0) {
        uint32_t bad = counts[2] + counts[3] + counts[4];
        if (bad) {
            printf(A_YELLOW "  %u warning%s and %u error%s were recorded during start-up."
                   A_RESET "  Run " A_BOLD "events" A_RESET " to see them.\n\n",
                   counts[2], counts[2] == 1 ? "" : "s",
                   counts[3] + counts[4], (counts[3] + counts[4]) == 1 ? "" : "s");
        }
    }

    char motd[512];
    ssize_t n = read_file("/etc/motd", motd, sizeof motd - 1);
    if (n > 0) {
        motd[n] = 0;
        printf("%s", motd);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    log_write(1, "init", "userland started");
    banner();

    /* The graphical shell is the normal face of the system; the text shell is
     * the fallback when the desktop cannot take the display, and the thing the
     * desktop drops back to when the user asks for a console. */
    /* A desktop needs something to drive it.
     *
     * On a machine where no keyboard and no pointer came up, starting the
     * graphical shell is worse than not starting it: it takes the screen,
     * clears the console, and leaves a picture that cannot be used and cannot
     * be read.  Everything the system knows about why there is no keyboard is
     * in the text it just wiped.
     *
     * So when there is no input at all, the console is left alone and the
     * reason is printed on it.  A machine that cannot be driven should at
     * least be able to explain itself. */
    /* USB takes its time, and asking too early gets the wrong answer.
     *
     * Bringing up a keyboard behind a hub means resetting the hub's port,
     * waiting for it to settle, addressing the device, reading its
     * descriptors and then its report descriptor - and the specification
     * requires real delays at several of those steps, so a device two hubs
     * down can take seconds to appear.  On the machine this was written for,
     * the shell prompt was printed before the mouse had even been addressed.
     *
     * So this waits for input rather than glancing at it once.  It gives up
     * after a few seconds because a machine genuinely without a keyboard must
     * still finish booting, and it stops the moment something appears, so a
     * machine with a keyboard in a socket waits no longer than it takes to
     * enumerate it. */
    ksysinfo_t si;
    bool have_input = false;
    for (int waited = 0; waited < 100; waited++) {          /* up to 10 seconds */
        if (sysinfo(&si) != 0) { have_input = true; break; }  /* cannot tell */
        if (si.keyboards + si.pointers > 0) { have_input = true; break; }
        sleep_ms(100);
    }

    bool graphical = file_exists("/bin/desktop") && !file_exists("/etc/no-desktop");

    if (graphical && !have_input) {
        graphical = false;
        printf("\n" A_YELLOW
               "  No keyboard and no pointer were found, so the desktop has\n"
               "  not been started - it would cover the messages above, which\n"
               "  are the only account of why." A_RESET "\n\n"
               A_GREY
               "  What is on this screen is what the USB drivers saw: which\n"
               "  controllers exist, what is plugged into each of their\n"
               "  ports, and which devices were brought up." A_RESET "\n\n");
    }

    const char *program = graphical ? "/bin/desktop" : "/bin/shell";
    const char *leaf = graphical ? "desktop" : "shell";

    if (graphical) printf(A_GREY "  Starting the desktop...\n" A_RESET);

    int quick_exits = 0;

    for (;;) {
        uint64_t started = uptime_ms();
        const char *child_argv[1] = { leaf };

        int pid = spawn(program, child_argv, 1);
        if (pid < 0) {
            printf(A_RED "init: cannot start %s: %s" A_RESET "\n", program, strerror(errno));
            log_write(3, "init", "cannot start the session program");

            if (graphical) {
                /* Fall back rather than leaving the machine unusable. */
                printf(A_YELLOW "init: falling back to the text shell." A_RESET "\n");
                log_write(2, "init", "the desktop would not start; using the text shell");
                graphical = false;
                program = "/bin/shell";
                leaf = "shell";
                continue;
            }

            sleep_ms(2000);
            if (++quick_exits > 5) {
                printf(A_RED "init: giving up. The system is running but has no shell." A_RESET "\n");
                for (;;) sleep_ms(60000);
            }
            continue;
        }

        int status = 0;
        wait(pid, &status);

        /* Whatever the session did to the console, take it back to a known
         * state before starting the next one. */
        log_console_level(1);
        console_raw(false);
        console_cursor(true);

        uint64_t ran_for = uptime_ms() - started;
        quick_exits = (ran_for < 500) ? quick_exits + 1 : 0;

        if (quick_exits > 4 && graphical) {
            printf(A_YELLOW "\ninit: the desktop keeps exiting immediately; using the text shell." A_RESET "\n");
            log_write(2, "init", "the desktop is exiting immediately; using the text shell");
            graphical = false;
            program = "/bin/shell";
            leaf = "shell";
            quick_exits = 0;
            continue;
        }
        if (quick_exits > 5) {
            printf(A_RED "\ninit: %s keeps exiting immediately (status %d)." A_RESET "\n", leaf, status);
            printf("Run " A_BOLD "events -s" A_RESET " to see where this boot is being recorded.\n");
            log_write(3, "init", "the session program is exiting immediately; not restarting");
            for (;;) sleep_ms(60000);
        }

        printf(A_GREY "\ninit: %s exited (status %d), restarting it." A_RESET "\n", leaf, status);
    }
}
