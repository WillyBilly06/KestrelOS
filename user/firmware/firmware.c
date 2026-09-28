/* firmware - see what firmware the hardware wants, and put it there.
 *
 *   firmware                     what is needed and what is present
 *   firmware import <path> [as]  copy a blob in from a mounted volume
 *   firmware list                what has been imported
 *   firmware remove <name>
 *
 * Most Wi-Fi cards and every recent NVIDIA card need a blob of the vendor's own
 * code before they will do anything.  Vendors distribute those and forbid
 * anyone else from redistributing them, so no operating system ships them -
 * Linux does not either.  What it has is a place to look, and the user puts the
 * file there.  This is that.
 */
#include "kestrel.h"

/* Where an imported file is put.
 *
 * The data volume when there is one, and the volume this was booted from when
 * there is not.  It used to be only the first, which meant that importing a
 * firmware file failed on a stick - and a stick is the case the whole feature
 * exists for: it is what somebody carries to a machine whose hardware needs a
 * file this system does not ship.
 */
#define FIRMWARE_DIR      "/data/firmware"
#define FIRMWARE_DIR_BOOT "/boot/KESTREL/firmware"

/* Which of the two this machine can actually write to, decided once.
 *
 * The data volume when there is one; the volume this was booted from when
 * there is not.  Deciding rather than assuming is the whole point: assuming
 * the first meant that on a stick - the case this feature exists for - every
 * import failed with a path that was never going to work. */
static const char *firmware_dir(void) {
    static const char *chosen;
    if (chosen) return chosen;

    mkdir(FIRMWARE_DIR);
    DIR *d = opendir(FIRMWARE_DIR);
    if (d) { closedir(d); chosen = FIRMWARE_DIR; return chosen; }

    mkdir("/boot/KESTREL");
    mkdir(FIRMWARE_DIR_BOOT);
    d = opendir(FIRMWARE_DIR_BOOT);
    if (d) { closedir(d); chosen = FIRMWARE_DIR_BOOT; return chosen; }

    /* Neither: the caller reports it, rather than writing somewhere that
     * silently goes nowhere. */
    chosen = "";
    return chosen;
}

static void ensure_directory(void) { (void)firmware_dir(); }

static int show_needs(void) {
    kfirmware_t f;
    int count = 0, missing = 0;

    for (uint32_t i = 0; ; i++) {
        if (enum_firmware(i, &f) < 0) break;
        if (!count) printf("%-46s %s\n", "FILE", "STATUS");

        if (f.present) {
            printf("%-46s present, %llu bytes\n", f.name,
                   (unsigned long long)f.size);
        } else {
            printf("%-46s MISSING\n", f.name);
            printf("%-46s   from %s\n", "", f.source);
            missing++;
        }
        count++;
    }

    if (!count) {
        printf("No hardware in this machine has asked for firmware.\n");
        return 0;
    }

    if (missing) {
        printf("\n%d file%s missing.  To supply one:\n", missing,
               missing == 1 ? " is" : "s are");
        printf("  1. Find it on another machine - Linux keeps these in\n");
        printf("     /lib/firmware, and a Windows driver package contains the\n");
        printf("     same images under its own names.\n");
        printf("  2. Put it on a FAT volume this machine can read.\n");
        printf("  3. `mount` that volume, then `firmware import <path>`.\n");
        printf("  4. Reboot.  The driver looks for it at start-up.\n");
    } else {
        printf("\nEverything that was asked for is present.\n");
    }
    return missing ? 1 : 0;
}

static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int do_import(const char *from, const char *as) {
    ensure_directory();

    int in = open(from, O_RDONLY);
    if (in < 0) {
        printf("firmware: cannot read %s (%s)\n", from, strerror(-in));
        printf("          `mount` shows which volumes are available.\n");
        return 1;
    }

    char target[192];
    snprintf(target, sizeof target, "%s/%s", firmware_dir(),
             as ? as : basename_of(from));

    /* A firmware file may name a subdirectory - mediatek/mt7921_wm.bin - and
     * that directory has to exist before the file can be created in it. */
    char *slash = strrchr(target + strlen(firmware_dir()) + 1, '/');
    if (slash) {
        *slash = 0;
        mkdir(target);
        *slash = '/';
    }

    int out = open(target, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        printf("firmware: cannot write %s (%s)\n", target, strerror(-out));
        close(in);
        return 1;
    }

    static uint8_t buffer[16384];
    uint64_t total = 0;
    for (;;) {
        ssize_t n = read(in, buffer, sizeof buffer);
        if (n <= 0) break;
        if (write(out, buffer, (size_t)n) != n) {
            printf("firmware: the write failed part way through; the volume may "
                   "be full\n");
            close(in);
            close(out);
            return 1;
        }
        total += (uint64_t)n;
    }
    close(in);
    close(out);

    printf("firmware: imported %llu bytes as %s\n",
           (unsigned long long)total, target);
    printf("          Reboot for the driver to find it.\n");
    return 0;
}

static int do_list(void) {
    DIR *dir = opendir(firmware_dir());
    if (!dir) {
        printf("Nothing has been imported yet.\n");
        return 0;
    }

    kdirent_t entry;
    int count = 0;
    while (readdir(dir, &entry) == 0) {
        if (!strcmp(entry.name, ".") || !strcmp(entry.name, "..")) continue;
        if (!count) printf("%-46s %s\n", "FILE", "SIZE");
        printf("%-46s %llu\n", entry.name, (unsigned long long)entry.size);
        count++;
    }
    closedir(dir);

    if (!count) printf("Nothing has been imported yet.\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return show_needs();

    if (!strcmp(argv[1], "list")) return do_list();

    if (!strcmp(argv[1], "import")) {
        if (argc < 3) {
            printf("usage: firmware import <path> [name-to-store-it-as]\n");
            return 1;
        }
        return do_import(argv[2], argc > 3 ? argv[3] : NULL);
    }

    if (!strcmp(argv[1], "remove")) {
        if (argc < 3) {
            printf("usage: firmware remove <name>\n");
            return 1;
        }
        char target[192];
        snprintf(target, sizeof target, "%s/%s", firmware_dir(), argv[2]);
        int r = unlink(target);
        if (r < 0) {
            printf("firmware: cannot remove %s (%s)\n", target, strerror(-r));
            return 1;
        }
        printf("firmware: removed %s\n", argv[2]);
        return 0;
    }

    printf("usage: firmware [import <path> [as] | list | remove <name>]\n");
    return 1;
}
