/* installer - put KestrelOS onto a disk.
 *
 * Two modes:
 *   * take the whole disk, which erases everything on it
 *   * use unallocated space, leaving existing partitions alone - which is what
 *     pairs with shrinking a Windows volume beforehand
 *
 * Nothing is written until the summary screen has been confirmed by typing the
 * word YES, and the summary names every partition that will be destroyed.
 */
#include "kestrel.h"
#include "disk.h"

#define MAX_DISKS 16

typedef struct {
    char     name[24];
    char     model[48];
    uint64_t size;
    bool     is_boot_disk;
    int      partitions;
} disk_entry_t;

static disk_entry_t disks[MAX_DISKS];
static int disk_count;
static char boot_disk[24];

/* ------------------------------------------------------------------ screen */

static int cols = 80, rows = 25;

static void screen_setup(void) {
    console_size(&cols, &rows);
    if (cols < 60) cols = 60;
    if (rows < 20) rows = 20;
}

static void hline(int n) { for (int i = 0; i < n; i++) printf("\xC4"); }

static void header(const char *title) {
    console_clear();
    printf("\n  " A_CYAN A_BOLD "KestrelOS installer" A_RESET);
    printf("   " A_GREY "%s" A_RESET "\n  ", title);
    hline(cols - 4);
    printf("\n\n");
}

static void footer(const char *hint) {
    printf("\n  ");
    hline(cols - 4);
    printf("\n  " A_GREY "%s" A_RESET "\n", hint);
}

static void pause_for_key(void) {
    printf("\n  Press Enter to continue. ");
    char buf[8];
    readline(buf, sizeof buf);
}

/* Ask a yes/no question; the default applies when the line is empty. */
static bool ask_yes(const char *question, bool fallback) {
    char buf[16];
    for (;;) {
        printf("  %s %s ", question, fallback ? "[Y/n]" : "[y/N]");
        if (readline(buf, sizeof buf) < 0) return fallback;
        if (!buf[0]) return fallback;
        if (buf[0] == 'y' || buf[0] == 'Y') return true;
        if (buf[0] == 'n' || buf[0] == 'N') return false;
        printf("  Please answer y or n.\n");
    }
}

/* -------------------------------------------------------------- enumeration */

static void find_boot_disk(void) {
    /* The volume mounted at /data is on the disk we booted from; if there is
     * none, fall back to leaving the field blank. */
    boot_disk[0] = 0;
    for (uint32_t i = 0;; i++) {
        kblockinfo_t b;
        if (enum_block(i, &b) < 0) break;
        if (!b.is_partition) continue;
        if (strcasecmp(b.label, "KESTREL") && strcasecmp(b.label, "ESP")) continue;

        /* "disk0p2" -> "disk0" */
        const char *p = strrchr(b.name, 'p');
        if (!p) continue;
        size_t n = (size_t)(p - b.name);
        if (n >= sizeof boot_disk) continue;
        memcpy(boot_disk, b.name, n);
        boot_disk[n] = 0;
        return;
    }
}

static void scan_disks(void) {
    disk_count = 0;
    for (uint32_t i = 0;; i++) {
        kblockinfo_t b;
        if (enum_block(i, &b) < 0) break;
        if (b.is_partition) continue;
        if (disk_count >= MAX_DISKS) break;

        disk_entry_t *d = &disks[disk_count++];
        strlcpy(d->name, b.name, sizeof d->name);
        strlcpy(d->model, b.model, sizeof d->model);
        d->size = b.size;
        d->is_boot_disk = boot_disk[0] && !strcmp(b.name, boot_disk);
        d->partitions = 0;
    }

    /* Count partitions per disk for the listing. */
    for (uint32_t i = 0;; i++) {
        kblockinfo_t b;
        if (enum_block(i, &b) < 0) break;
        if (!b.is_partition) continue;
        for (int j = 0; j < disk_count; j++) {
            size_t n = strlen(disks[j].name);
            if (!strncmp(b.name, disks[j].name, n) && b.name[n] == 'p') disks[j].partitions++;
        }
    }
}

/* ------------------------------------------------------------------ screens */

static void show_welcome(void) {
    header("welcome");
    printf("  This will install KestrelOS onto a disk in this computer.\n\n");
    printf("  You will be asked to choose a disk, then whether to use the whole\n");
    printf("  disk or only its unallocated space.  Nothing is written until you\n");
    printf("  have seen a summary and confirmed it.\n\n");
    printf(A_YELLOW "  Installing to a whole disk erases everything on that disk.\n" A_RESET);
    printf(A_GREY   "  Installing into free space leaves existing partitions untouched.\n" A_RESET);
    footer("Enter to continue, or type quit to leave the installer.");

    printf("\n  > ");
    char buf[16];
    if (readline(buf, sizeof buf) >= 0 && (!strcasecmp(buf, "quit") || !strcasecmp(buf, "q")))
        exit(0);
}

static int choose_disk(void) {
    for (;;) {
        header("choose a disk");
        if (!disk_count) {
            printf(A_RED "  No disks were found.\n\n" A_RESET);
            printf("  The kernel did not detect any storage controller it can drive.\n");
            printf("  Run " A_BOLD "lspci" A_RESET " from the shell to see what hardware is present,\n");
            printf("  and " A_BOLD "events" A_RESET " to see what the storage drivers reported.\n");
            footer("Press Enter to return to the shell.");
            pause_for_key();
            exit(1);
        }

        printf("   %-4s %-10s %12s  %-6s %s\n", "#", "DEVICE", "SIZE", "PARTS", "MODEL");
        printf("   ");
        hline(cols - 6);
        printf("\n");

        for (int i = 0; i < disk_count; i++) {
            char size[24];
            format_size(size, sizeof size, disks[i].size);
            printf("   %-4d %-10s %12s  %-6d %s%s\n", i + 1, disks[i].name, size,
                   disks[i].partitions, disks[i].model,
                   disks[i].is_boot_disk ? A_YELLOW "  (this is the disk you booted from)" A_RESET : "");
        }

        footer("Type a number to choose a disk, or q to quit.");
        printf("\n  > ");

        char buf[16];
        if (readline(buf, sizeof buf) < 0) continue;
        if (!strcasecmp(buf, "q") || !strcasecmp(buf, "quit")) exit(0);

        int choice = atoi(buf);
        if (choice >= 1 && choice <= disk_count) return choice - 1;
        printf("\n  " A_RED "Please enter a number between 1 and %d." A_RESET "\n", disk_count);
        pause_for_key();
    }
}

/* Show what is currently on the disk, and return whether it holds anything. */
static bool show_layout(gpt_disk_t *g, const char *device) {
    printf("  Current contents of " A_BOLD "%s" A_RESET ":\n\n", device);

    if (!g->has_gpt) {
        printf("    " A_GREY "no GUID partition table - the disk appears to be empty or\n");
        printf("    uses a partitioning scheme this installer does not read." A_RESET "\n\n");
        return false;
    }

    bool any = false;
    printf("    %-6s %-22s %12s  %s\n", "SLOT", "TYPE", "SIZE", "NAME");
    for (int i = 0; i < GPT_ENTRIES; i++) {
        gpt_part_t *p = &g->parts[i];
        if (!p->used) continue;
        any = true;

        const char *type = "data";
        if (guid_same(p->type_guid, GUID_ESP)) type = "EFI system partition";
        else if (guid_same(p->type_guid, GUID_KESTREL)) type = "KestrelOS";
        else if (guid_same(p->type_guid, GUID_MSDATA)) type = "Windows/basic data";
        else if (guid_same(p->type_guid, GUID_MSRESERVED)) type = "Microsoft reserved";

        char size[24];
        format_size(size, sizeof size, (p->last_lba - p->first_lba + 1) * SECTOR_SIZE);
        printf("    %-6d %-22s %12s  %s\n", p->index, type, size, p->name);
    }
    if (!any) printf("    " A_GREY "(the table is empty)" A_RESET "\n");
    printf("\n");
    return any;
}

/* ------------------------------------------------------------------- layout */

typedef struct {
    gpt_disk_t *disk;
    bool        whole_disk;
    bool        reuse_esp;
    int         esp_index;
    uint64_t    esp_first, esp_last;
    uint64_t    data_first, data_last;
} plan_t;

#define ESP_SECTORS   (128ull * 1024 * 1024 / SECTOR_SIZE)   /* 128 MiB */
#define MIN_DATA_MB   256

static bool plan_whole_disk(plan_t *plan, gpt_disk_t *g) {
    gpt_init_empty(g);

    if (g->sectors < (ESP_SECTORS + (MIN_DATA_MB * 1024ull * 1024 / SECTOR_SIZE) + 4 * ALIGN_SECTORS)) {
        printf(A_RED "  This disk is too small: KestrelOS needs about %llu MiB.\n" A_RESET,
               (unsigned long long)((ESP_SECTORS * SECTOR_SIZE) / (1024 * 1024) + MIN_DATA_MB));
        return false;
    }

    plan->whole_disk = true;
    plan->reuse_esp = false;
    plan->esp_first = ALIGN_SECTORS;
    plan->esp_last = plan->esp_first + ESP_SECTORS - 1;

    uint64_t data_first = (plan->esp_last + 1 + ALIGN_SECTORS - 1) & ~(uint64_t)(ALIGN_SECTORS - 1);
    plan->data_first = data_first;
    plan->data_last = g->last_usable;
    return true;
}

static bool plan_free_space(plan_t *plan, gpt_disk_t *g) {
    uint64_t gap_start = 0;
    uint64_t gap = gpt_largest_gap(g, &gap_start);

    /* An existing EFI partition is reused rather than duplicated: firmware
     * expects one per disk, and Windows' loader lives in it. */
    plan->reuse_esp = false;
    plan->esp_index = -1;
    for (int i = 0; i < GPT_ENTRIES; i++) {
        if (!g->parts[i].used) continue;
        if (!guid_same(g->parts[i].type_guid, GUID_ESP)) continue;
        uint64_t esp_bytes = (g->parts[i].last_lba - g->parts[i].first_lba + 1) * SECTOR_SIZE;
        if (esp_bytes < 32ull * 1024 * 1024) {
            printf(A_YELLOW "  The existing EFI partition is only %llu MiB, which may be too\n"
                   "  small to hold another operating system's loader.\n" A_RESET,
                   (unsigned long long)(esp_bytes / (1024 * 1024)));
        }
        plan->reuse_esp = true;
        plan->esp_index = g->parts[i].index;
        plan->esp_first = g->parts[i].first_lba;
        plan->esp_last = g->parts[i].last_lba;
        break;
    }

    uint64_t need = (uint64_t)MIN_DATA_MB * 1024 * 1024 / SECTOR_SIZE;
    if (!plan->reuse_esp) need += ESP_SECTORS + ALIGN_SECTORS;

    if (gap < need) {
        char have[24], want[24];
        format_size(have, sizeof have, gap * SECTOR_SIZE);
        format_size(want, sizeof want, need * SECTOR_SIZE);
        printf(A_RED "  There is not enough unallocated space on this disk.\n" A_RESET);
        printf("    largest free area: %s\n", have);
        printf("    needed:            %s\n\n", want);
        printf("  Shrink an existing partition first.  From Windows this is\n");
        printf("  Disk Management, or the installer supplied for Windows will do\n");
        printf("  it for you before rebooting into this program.\n");
        return false;
    }

    /* How much of the gap should the installation take? */
    char have[24];
    format_size(have, sizeof have, gap * SECTOR_SIZE);
    printf("  Unallocated space available: " A_BOLD "%s" A_RESET "\n", have);
    if (plan->reuse_esp)
        printf("  " A_GREY "An existing EFI system partition (slot %d) will be reused." A_RESET "\n",
               plan->esp_index);
    printf("\n");

    uint64_t max_mb = (gap * SECTOR_SIZE) / (1024 * 1024);
    if (!plan->reuse_esp) max_mb -= (ESP_SECTORS * SECTOR_SIZE) / (1024 * 1024) + 1;

    uint64_t want_mb = max_mb;
    for (;;) {
        printf("  How much space should KestrelOS use, in GB?\n");
        printf("  " A_GREY "Between %llu and %llu GB; press Enter to use all of it." A_RESET "\n",
               (unsigned long long)((MIN_DATA_MB + 1023) / 1024),
               (unsigned long long)(max_mb / 1024));
        printf("\n  > ");

        char buf[32];
        if (readline(buf, sizeof buf) < 0) continue;
        if (!buf[0]) { want_mb = max_mb; break; }

        char *end = NULL;
        long gb = strtol(buf, &end, 10);
        if (end == buf || gb <= 0) { printf("\n  " A_RED "Please enter a whole number of GB." A_RESET "\n\n"); continue; }

        uint64_t mb = (uint64_t)gb * 1024;
        if (mb > max_mb) {
            printf("\n  " A_RED "Only %llu GB is available." A_RESET "\n\n", (unsigned long long)(max_mb / 1024));
            continue;
        }
        if (mb < MIN_DATA_MB) {
            printf("\n  " A_RED "At least %d MiB is needed." A_RESET "\n\n", MIN_DATA_MB);
            continue;
        }
        want_mb = mb;
        break;
    }

    uint64_t cursor = gap_start;
    if (!plan->reuse_esp) {
        plan->esp_first = cursor;
        plan->esp_last = cursor + ESP_SECTORS - 1;
        cursor = (plan->esp_last + 1 + ALIGN_SECTORS - 1) & ~(uint64_t)(ALIGN_SECTORS - 1);
    }

    uint64_t data_sectors = want_mb * 1024 * 1024 / SECTOR_SIZE;
    if (cursor + data_sectors > gap_start + gap) data_sectors = gap_start + gap - cursor;

    plan->whole_disk = false;
    plan->data_first = cursor;
    plan->data_last = cursor + data_sectors - 1;
    return true;
}

/* ------------------------------------------------------------------ copying */

typedef struct { const char *from; const char *to; const char *what; } copy_job_t;

static bool copy_with_progress(const char *from, const char *to, const char *what) {
    printf("    %-28s ", what);
    flush_output();

    if (copy_file(from, to) < 0) {
        printf(A_RED "failed: %s" A_RESET "\n", strerror(errno));
        /* And to the log as well.  This message used to go only to the
         * screen, so an install that failed here left a serial log that
         * stopped after naming the step and said nothing about why -
         * which is the least useful place for the one fact that matters. */
        char why[160];
        snprintf(why, sizeof why, "could not copy %s to %s: %s",
                 from, to, strerror(errno));
        log_write(3, "install", why);
        return false;
    }

    kstat_t st;
    if (stat(to, &st) == 0) {
        char size[24];
        format_size(size, sizeof size, st.size);
        printf(A_GREEN "ok" A_RESET " " A_GREY "(%s)" A_RESET "\n", size);
    } else {
        printf(A_GREEN "ok" A_RESET "\n");
    }
    return true;
}

/* Copy one directory, one level deep, reporting the whole as a single line.
 *
 * A file that will not copy is named and the rest carry on: a system missing
 * one program is worth having, and stopping on the first failure would leave a
 * volume with an arbitrary fraction of the system on it and no way to tell how
 * far it got.
 */
#define DIRENT_DIR 2      /* VN_DIR, as the kernel reports it */

static void copy_dir(const char *from, const char *to, int depth,
                     int *copied, int *failed) {
    /* Bounded rather than unbounded.  A system tree is two or three deep, and
     * a loop in a filesystem should not be able to turn an install into one
     * that never finishes. */
    if (depth > 4) return;

    mkdir(to);

    DIR *d = opendir(from);
    if (!d) return;

    kdirent_t e;
    while (readdir(d, &e) == 0) {
        if (e.name[0] == '.') continue;

        char src[256], dst[256];
        snprintf(src, sizeof src, "%s/%s", from, e.name);
        snprintf(dst, sizeof dst, "%s/%s", to, e.name);

        /* A directory is descended into rather than reported as a file that
         * would not copy - which is what /etc/ssl looked like, and it made a
         * complete install report a failure it had not had. */
        if (e.type == DIRENT_DIR) {
            copy_dir(src, dst, depth + 1, copied, failed);
            continue;
        }

        if (copy_file(src, dst) < 0) {
            char why[192];
            snprintf(why, sizeof why, "could not copy %s: %s", src, strerror(errno));
            log_write(2, "install", why);
            (*failed)++;
        } else {
            (*copied)++;
        }
    }
    closedir(d);
}

static bool copy_tree(const char *from, const char *to, const char *what) {
    printf("    %-28s ", what);
    flush_output();

    int copied = 0, failed = 0;
    copy_dir(from, to, 0, &copied, &failed);

    if (failed)
        printf(A_YELLOW "%d copied, %d failed" A_RESET "\n", copied, failed);
    else
        printf(A_GREEN "ok" A_RESET " " A_GREY "(%d files)" A_RESET "\n", copied);
    return failed == 0;
}

/* ------------------------------------------------------------------- action */

static bool do_install(plan_t *plan, const char *device) {
    gpt_disk_t *g = plan->disk;

    printf("\n  Writing the partition table...\n");
    if (plan->whole_disk) {
        gpt_add(g, GUID_ESP, plan->esp_first, plan->esp_last, "EFI System Partition");
        gpt_add(g, GUID_KESTREL, plan->data_first, plan->data_last, "KestrelOS");
    } else {
        if (!plan->reuse_esp)
            gpt_add(g, GUID_ESP, plan->esp_first, plan->esp_last, "EFI System Partition");
        gpt_add(g, GUID_KESTREL, plan->data_first, plan->data_last, "KestrelOS");
    }

    if (gpt_write(g) < 0) {
        printf(A_RED "    could not write the partition table: %s" A_RESET "\n", strerror(errno));
        return false;
    }
    printf("    " A_GREEN "partition table written" A_RESET "\n");

    /* Format before rescanning, so the kernel sees finished filesystems. */
    if (!plan->reuse_esp) {
        printf("  Formatting the EFI system partition...\n");
        int type = fat_format(g->fd, plan->esp_first, plan->esp_last - plan->esp_first + 1, "ESP");
        if (type < 0) {
            printf(A_RED "    formatting failed: %s" A_RESET "\n", strerror(errno));
            return false;
        }
        printf("    " A_GREEN "FAT%d" A_RESET "\n", type);
    } else {
        printf("  " A_GREY "Keeping the existing EFI system partition." A_RESET "\n");
    }

    printf("  Formatting the KestrelOS partition...\n");
    int type = fat_format(g->fd, plan->data_first, plan->data_last - plan->data_first + 1, "KESTREL");
    if (type < 0) {
        printf(A_RED "    formatting failed: %s" A_RESET "\n", strerror(errno));
        return false;
    }
    printf("    " A_GREEN "FAT%d" A_RESET "\n", type);

    sync();

    /* Let the kernel re-read the table so the new partitions get device nodes. */
    printf("  Re-reading the partition table...\n");
    gpt_close(g);
    int found = blk_rescan(device);
    if (found < 0) {
        printf(A_RED "    the kernel could not re-read it: %s" A_RESET "\n", strerror(errno));
        return false;
    }
    printf("    " A_GREEN "%d partition(s)" A_RESET "\n", found);

    /* Work out which device nodes the two partitions became. */
    char esp_dev[32] = "", data_dev[32] = "";
    for (uint32_t i = 0;; i++) {
        kblockinfo_t b;
        if (enum_block(i, &b) < 0) break;
        if (!b.is_partition) continue;
        size_t n = strlen(device);
        if (strncmp(b.name, device, n) || b.name[n] != 'p') continue;

        uint64_t first_bytes = 0;
        (void)first_bytes;
        if (!strcasecmp(b.label, "KestrelOS") || !strcasecmp(b.label, "KESTREL"))
            strlcpy(data_dev, b.name, sizeof data_dev);
        else if (!strcasecmp(b.label, "EFI System Partition") || !strcasecmp(b.label, "ESP"))
            strlcpy(esp_dev, b.name, sizeof esp_dev);
    }
    if (!esp_dev[0] || !data_dev[0]) {
        printf(A_RED "    could not identify the new partitions" A_RESET "\n");
        return false;
    }

    /* Mount both and copy the system across. */
    mkdir("/mnt");
    mkdir("/mnt/esp");
    mkdir("/mnt/data");

    printf("  Mounting the new partitions...\n");
    if (mount(esp_dev, "/mnt/esp") < 0) {
        printf(A_RED "    %s: %s" A_RESET "\n", esp_dev, strerror(errno));
        return false;
    }
    if (mount(data_dev, "/mnt/data") < 0) {
        printf(A_RED "    %s: %s" A_RESET "\n", data_dev, strerror(errno));
        unmount("/mnt/esp");
        return false;
    }
    printf("    " A_GREEN "%s on /mnt/esp, %s on /mnt/data" A_RESET "\n", esp_dev, data_dev);

    printf("  Copying the system...\n");
    mkdir("/mnt/esp/EFI");
    mkdir("/mnt/esp/EFI/BOOT");
    mkdir("/mnt/esp/EFI/KESTREL");
    mkdir("/mnt/esp/KESTREL");
    mkdir("/mnt/data/logs");

    bool ok = true;
    /* \EFI\BOOT\BOOTX64.EFI is the removable-media path every UEFI
     * implementation will boot without needing an NVRAM entry. */
    ok = copy_with_progress("/lib/boot/BOOTX64.EFI", "/mnt/esp/EFI/BOOT/BOOTX64.EFI", "loader (fallback path)") && ok;
    ok = copy_with_progress("/lib/boot/BOOTX64.EFI", "/mnt/esp/EFI/KESTREL/BOOTX64.EFI", "loader") && ok;
    ok = copy_with_progress("/lib/boot/KERNEL.ELF", "/mnt/esp/KESTREL/KERNEL.ELF", "kernel") && ok;
    ok = copy_with_progress("/dev/initrd", "/mnt/esp/KESTREL/INITRD.KAR", "system image") && ok;
    if (!ok) { unmount("/mnt/data"); unmount("/mnt/esp"); return false; }

    /* And the programs onto the data volume.
     *
     * This is the layout Windows and Linux both use, and for the same reason.
     * The EFI partition carries only what the firmware has to be able to read
     * before there is an operating system - the loader, the kernel, and the
     * small image holding the drivers and firmware needed to reach a disk at
     * all.  Everything after that point lives on the system volume, where
     * there is room for it and where it can be replaced without touching the
     * partition the firmware boots from.
     *
     * The boot image keeps its own copy, so a stick still boots on its own
     * with no installed volume to find.  That is what an initramfs is for, and
     * it is why the firmware cannot simply be moved here: the storage driver
     * that would read this volume is one of the things it needs.
     */
    copy_tree("/bin", "/mnt/data/bin", "programs");
    copy_tree("/etc", "/mnt/data/etc", "configuration");

    /* The loader needs to be told which partition holds the data volume. */
    printf("  Writing the boot configuration...\n");
    char data_guid[40] = "";
    for (uint32_t i = 0;; i++) {
        kblockinfo_t b;
        if (enum_block(i, &b) < 0) break;
        if (!strcmp(b.name, data_dev)) { strlcpy(data_guid, b.part_guid, sizeof data_guid); break; }
    }

    char cfg[512];
    int n = snprintf(cfg, sizeof cfg,
                     "# KestrelOS loader configuration\n"
                     "timeout=3\n"
                     "kernel=\\KESTREL\\KERNEL.ELF\n"
                     "initrd=\\KESTREL\\INITRD.KAR\n"
                     "cmdline=\n"
                     "data=PARTUUID=%s\n", data_guid);
    if (write_file("/mnt/esp/KESTREL/BOOT.CFG", cfg, (size_t)n) < 0) {
        printf(A_RED "    could not write BOOT.CFG: %s" A_RESET "\n", strerror(errno));
        unmount("/mnt/data");
        unmount("/mnt/esp");
        return false;
    }
    printf("    " A_GREEN "ok" A_RESET " " A_GREY "(data volume %s)" A_RESET "\n", data_guid);

    printf("  Flushing everything to disk...\n");
    sync();
    unmount("/mnt/data");
    unmount("/mnt/esp");
    sync();
    printf("    " A_GREEN "done" A_RESET "\n");
    return true;
}

/* --------------------------------------------------------------------- main */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    screen_setup();
    log_write(1, "install", "installer started");

    show_welcome();
    find_boot_disk();
    scan_disks();

    int index = choose_disk();
    disk_entry_t *chosen = &disks[index];

    gpt_disk_t g;
    if (gpt_open(&g, chosen->name) < 0) {
        header("error");
        printf(A_RED "  Cannot open /dev/%s: %s" A_RESET "\n", chosen->name, strerror(errno));
        pause_for_key();
        return 1;
    }

    plan_t plan;
    memset(&plan, 0, sizeof plan);
    plan.disk = &g;

    header("what to do with the disk");
    bool occupied = show_layout(&g, chosen->name);

    if (chosen->is_boot_disk) {
        printf(A_YELLOW "  This is the disk you are running from.  Installing to the whole\n");
        printf("  disk would erase the running system.\n" A_RESET "\n");
    }

    printf("   " A_BOLD "1" A_RESET "  Use the whole disk" A_GREY " - erases everything on %s" A_RESET "\n",
           chosen->name);
    printf("   " A_BOLD "2" A_RESET "  Use unallocated space" A_GREY " - keeps existing partitions" A_RESET "\n");
    printf("   " A_BOLD "q" A_RESET "  Go back\n");
    footer("Choose 1, 2 or q.");
    printf("\n  > ");

    char buf[16];
    if (readline(buf, sizeof buf) < 0) { gpt_close(&g); return 1; }
    if (buf[0] == 'q' || buf[0] == 'Q') { gpt_close(&g); return 0; }

    header("how much space");
    bool planned;
    if (buf[0] == '1') {
        if (occupied) {
            printf(A_YELLOW "  Everything currently on %s will be destroyed.\n" A_RESET "\n", chosen->name);
            show_layout(&g, chosen->name);
        }
        planned = plan_whole_disk(&plan, &g);
    } else if (buf[0] == '2') {
        planned = plan_free_space(&plan, &g);
    } else {
        printf("  " A_RED "That was not one of the choices." A_RESET "\n");
        pause_for_key();
        gpt_close(&g);
        return 1;
    }

    if (!planned) {
        pause_for_key();
        gpt_close(&g);
        return 1;
    }

    /* ---- summary and confirmation ---- */
    header("confirm");
    char esp_size[24], data_size[24];
    format_size(esp_size, sizeof esp_size, (plan.esp_last - plan.esp_first + 1) * SECTOR_SIZE);
    format_size(data_size, sizeof data_size, (plan.data_last - plan.data_first + 1) * SECTOR_SIZE);

    printf("  KestrelOS will be installed to " A_BOLD "%s" A_RESET " (%s).\n\n", chosen->name, chosen->model);

    if (plan.whole_disk) {
        printf(A_RED A_BOLD "  The entire disk will be erased.\n" A_RESET);
        printf(A_RED "  Every partition listed on the previous screen will be destroyed.\n" A_RESET "\n");
    } else {
        printf(A_GREEN "  Existing partitions will be left exactly as they are.\n" A_RESET);
        printf("  Only unallocated space will be used.\n\n");
    }

    printf("  What will be created:\n");
    if (plan.reuse_esp)
        printf("    EFI system partition   %12s  " A_GREY "(reusing slot %d)" A_RESET "\n",
               esp_size, plan.esp_index);
    else
        printf("    EFI system partition   %12s  " A_GREY "(new)" A_RESET "\n", esp_size);
    printf("    KestrelOS              %12s  " A_GREY "(new, formatted FAT)" A_RESET "\n\n", data_size);

    footer("Type YES in capitals to go ahead, or anything else to cancel.");
    printf("\n  > ");

    char confirm[16];
    if (readline(confirm, sizeof confirm) < 0 || strcmp(confirm, "YES")) {
        printf("\n  Cancelled.  Nothing has been written.\n");
        pause_for_key();
        gpt_close(&g);
        return 0;
    }

    header("installing");
    log_write(1, "install", "writing the partition table");

    bool ok = do_install(&plan, chosen->name);

    if (!ok) {
        printf("\n" A_RED "  The installation did not finish." A_RESET "\n");
        printf("  Run " A_BOLD "events" A_RESET " from the shell to see what went wrong.\n");
        log_write(3, "install", "installation failed");
        pause_for_key();
        return 1;
    }

    log_write(1, "install", "installation complete");
    header("finished");
    printf(A_GREEN A_BOLD "  KestrelOS has been installed to %s.\n" A_RESET "\n", chosen->name);
    printf("  The loader was written to the EFI system partition at both\n");
    printf("    \\EFI\\BOOT\\BOOTX64.EFI      " A_GREY "(the path firmware boots by default)" A_RESET "\n");
    printf("    \\EFI\\KESTREL\\BOOTX64.EFI\n\n");

    if (!plan.whole_disk) {
        printf("  Your existing operating systems were not touched.  If this\n");
        printf("  computer still starts straight into another one, choose the\n");
        printf("  KestrelOS entry from the firmware boot menu, or set it as the\n");
        printf("  first boot option in firmware setup.\n\n");
    }

    footer("Remove the installation medium before restarting.");

    if (ask_yes("\n  Restart now?", true)) {
        printf("\n  Restarting...\n");
        flush_output();
        sleep_ms(1200);
        reboot();
    }

    printf("\n  Returning to the shell.  Type " A_BOLD "reboot" A_RESET " when you are ready.\n\n");
    return 0;
}
