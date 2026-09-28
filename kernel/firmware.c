/* firmware.c - loading vendor firmware the user supplies.
 *
 * The whole of it: look in a couple of directories, read the file, hand it to
 * the driver.  What makes it worth its own file is the bookkeeping around the
 * failure case - a driver that cannot find its firmware should leave behind a
 * precise statement of what was missing, because "your Wi-Fi does not work" and
 * "your Wi-Fi needs iwlwifi-so-a0-gf-a0-83.ucode, which is in the linux-firmware
 * package" are very different messages to be given.
 */
#include "kernel.h"
#include "mm.h"
#include "vfs.h"
#include "klog.h"
#include "firmware.h"

#define MAX_NEEDS 16
/* The largest firmware image this will load.
 *
 * Eight megabytes was chosen when the only firmware in question was a wireless
 * card's, which is a few hundred kilobytes.  A modern graphics card's is a
 * different order of thing entirely: the resource manager that runs on an
 * NVIDIA card's own processor from Ampere onwards is tens of megabytes, and at
 * the old limit it was refused as "not a firmware image" - so the one file
 * that would let the card be driven could not be loaded even when somebody had
 * gone and fetched it.
 *
 * Ninety-six is chosen against the largest of those images with room to spare,
 * and is still small enough that a corrupt length is caught rather than turned
 * into an allocation that empties memory. */
#define MAX_FIRMWARE_BYTES (96 * 1024 * 1024)

static firmware_need_t needs[MAX_NEEDS];
static int need_count;

/* --------------------------------------------------------------- declaring */

void firmware_declare(const char *name, const char *source) {
    for (int i = 0; i < need_count; i++)
        if (!strcmp(needs[i].name, name)) return;      /* already known */

    if (need_count >= MAX_NEEDS) return;

    firmware_need_t *n = &needs[need_count++];
    memset(n, 0, sizeof *n);
    strlcpy(n->name, name, sizeof n->name);
    strlcpy(n->source, source, sizeof n->source);
    n->present = firmware_present(name, &n->size);
}

int firmware_snapshot(firmware_need_t *out, int max) {
    int n = 0;
    for (int i = 0; i < need_count && n < max; i++) {
        out[n] = needs[i];
        /* Re-check: a volume may have been mounted since the driver asked. */
        out[n].present = firmware_present(out[n].name, &out[n].size);
        n++;
    }
    return n;
}

/* ----------------------------------------------------------------- looking */

static void build_path(char *buf, size_t cap, const char *dir, const char *name) {
    snprintf(buf, cap, "%s/%s", dir, name);
}

bool firmware_present(const char *name, u64 *size_out) {
    /* Initrd (RAM) FIRST, then the FAT boot volume, and the NTFS data volume
     * LAST.  The data volume is where the user drops big firmware, but on a
     * real Windows-formatted stick this kernel's NTFS reader is unreliable and
     * a FAILED large read there desyncs the USB mass-storage stick outright
     * (usbmsc "out of step"), taking down all USB I/O - logging included.  So
     * anything that is also in the initrd is read from RAM and the data volume
     * is only touched for firmware that lives nowhere else. */
    static const char *dirs[] = { FIRMWARE_PATH_INITRD, FIRMWARE_PATH_BOOT,
                                  FIRMWARE_PATH_DATA };

    for (size_t i = 0; i < ARRAY_LEN(dirs); i++) {
        char path[192];
        build_path(path, sizeof path, dirs[i], name);

        vstat_t st;
        if (vfs_stat(path, &st) < 0) continue;

        if (size_out) *size_out = st.size;
        return true;
    }
    if (size_out) *size_out = 0;
    return false;
}

/* Resolve a firmware file's full path across the search directories, without
 * reading it.  For a caller that wants to stream a large image straight into
 * memory of its own - the GSP-RM image is sixty megabytes and does not belong
 * in a single heap allocation - rather than take firmware_load's buffer. */
bool firmware_resolve(const char *name, char *path_out, size_t cap) {
    if (!name || !path_out) return false;
    /* Initrd (RAM) FIRST, then the FAT boot volume, and the NTFS data volume
     * LAST.  The data volume is where the user drops big firmware, but on a
     * real Windows-formatted stick this kernel's NTFS reader is unreliable and
     * a FAILED large read there desyncs the USB mass-storage stick outright
     * (usbmsc "out of step"), taking down all USB I/O - logging included.  So
     * anything that is also in the initrd is read from RAM and the data volume
     * is only touched for firmware that lives nowhere else. */
    static const char *dirs[] = { FIRMWARE_PATH_INITRD, FIRMWARE_PATH_BOOT,
                                  FIRMWARE_PATH_DATA };
    for (size_t i = 0; i < ARRAY_LEN(dirs); i++) {
        build_path(path_out, cap, dirs[i], name);
        vstat_t st;
        if (vfs_stat(path_out, &st) == 0) return true;
    }
    path_out[0] = 0;
    return false;
}

bool firmware_load(const char *name, firmware_t *out) {
    if (!name || !out) return false;
    memset(out, 0, sizeof *out);

    /* Initrd (RAM) FIRST, then the FAT boot volume, and the NTFS data volume
     * LAST.  The data volume is where the user drops big firmware, but on a
     * real Windows-formatted stick this kernel's NTFS reader is unreliable and
     * a FAILED large read there desyncs the USB mass-storage stick outright
     * (usbmsc "out of step"), taking down all USB I/O - logging included.  So
     * anything that is also in the initrd is read from RAM and the data volume
     * is only touched for firmware that lives nowhere else. */
    static const char *dirs[] = { FIRMWARE_PATH_INITRD, FIRMWARE_PATH_BOOT,
                                  FIRMWARE_PATH_DATA };

    for (size_t i = 0; i < ARRAY_LEN(dirs); i++) {
        char path[192];
        build_path(path, sizeof path, dirs[i], name);

        vstat_t st;
        if (vfs_stat(path, &st) < 0) continue;

        u64 size = st.size;
        if (!size || size > MAX_FIRMWARE_BYTES) {
            kwarn("firmware", "%s is %lu bytes, which is not a firmware image",
                  name, size);
            return false;
        }

        u8 *buffer = kmalloc((size_t)size);
        if (!buffer) {
            kerr("firmware", "out of memory reading %s (%lu bytes)", name, size);
            return false;
        }

        s64 got = vfs_read_file(path, buffer, (size_t)size);
        if (got != (s64)size) {
            kerr("firmware", "%s: read %ld of %lu bytes", name, (long)got, size);
            kfree(buffer);
            return false;
        }

        out->data = buffer;
        out->size = (size_t)size;
        strlcpy(out->name, name, sizeof out->name);
        kinfo("firmware", "loaded %s (%lu bytes) from %s", name, size, dirs[i]);
        return true;
    }

    kwarn("firmware", "%s is not present; the hardware that needs it will not "
                      "start (put it in %s)", name, FIRMWARE_PATH_DATA);
    return false;
}

void firmware_free(firmware_t *fw) {
    if (!fw || !fw->data) return;
    kfree(fw->data);
    fw->data = NULL;
    fw->size = 0;
}

/* -------------------------------------------------------------------- init */

void firmware_init(void) {
    /* Make the directory so the tool that imports blobs has somewhere to put
     * them, and so its absence is never the reason an import fails. */
    if (vfs_mkdir(FIRMWARE_PATH_DATA) < 0) {
        /* Already there, or the volume is not mounted; neither is an error
         * worth a warning at start-up. */
    }
}
