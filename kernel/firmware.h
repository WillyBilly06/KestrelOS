/* firmware.h - loading vendor firmware the user supplies.
 *
 * A great deal of hardware will not do anything until a blob of the vendor's
 * own code has been pushed into it: nearly every Wi-Fi card, and every NVIDIA
 * GPU from Turing onward.  The vendors distribute those blobs but do not permit
 * anyone else to redistribute them, which is why no operating system ships
 * them - Linux does not either.  What Linux has is a driver and a place to look
 * for the file, and the user puts the file there.
 *
 * This is that place.  A driver asks for a firmware file by name; if the user
 * has imported it, the driver gets it and the hardware works.  If not, the
 * driver says exactly which file is missing and where to get it, rather than
 * failing in a way that looks like the hardware being unsupported.
 */
#ifndef KESTREL_FIRMWARE_H
#define KESTREL_FIRMWARE_H

#include "kernel.h"

/* Where imported firmware lives, in search order.  The data volume is the one
 * that survives a reboot; the initrd copy exists so a build can carry a blob
 * deliberately placed there. */
#define FIRMWARE_PATH_DATA   "/data/firmware"
/* The volume this was booted from, which on a stick is the only writable one
 * there is - and a stick is exactly where somebody adds a firmware file,
 * because it is the machine they are carrying to hardware it does not know. */
#define FIRMWARE_PATH_BOOT   "/boot/KESTREL/firmware"
#define FIRMWARE_PATH_INITRD "/lib/firmware"

typedef struct {
    u8    *data;
    size_t size;
    char   name[64];
} firmware_t;

/* Look for a firmware file and read it into memory.  Returns false when it is
 * not there, having logged which file was wanted. */
bool firmware_load(const char *name, firmware_t *out);
void firmware_free(firmware_t *fw);

/* Whether a file is present, without reading it - for reporting what the
 * machine would need before anything tries to use it. */
bool firmware_present(const char *name, u64 *size_out);

/* Resolve a firmware file's full path (across the search dirs) without
 * reading it, for a caller that streams a large image itself. */
bool firmware_resolve(const char *name, char *path_out, size_t cap);

/* What one piece of hardware needs, so a driver's requirement can be reported
 * whether or not the driver ever runs. */
typedef struct {
    char name[64];         /* the file, as the vendor names it     */
    char source[96];       /* where the user can get it            */
    bool present;
    u64  size;
} firmware_need_t;

/* Drivers register what they would need, whether or not they got it.  The
 * `firmware` tool and the System app read this back, so a machine with an
 * unsupported card can be told precisely what is missing. */
void firmware_declare(const char *name, const char *source);
int  firmware_snapshot(firmware_need_t *out, int max);

void firmware_init(void);

#endif
