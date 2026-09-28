/* nv_fwimage.h - reading the packaging of NVIDIA's co-processor firmware.
 *
 * The files NVIDIA publishes for a GSP card come in two shapes: an ELF image
 * (the FMC, and the GSP resident manager itself) whose named sections the boot
 * ROM is told where to find, and a plain container (the bootloader) with a
 * small header in front of a payload.  Reading those headers is pure byte
 * work with no card involved, so it lives here on its own and is checked on
 * the host against the real firmware (tools/nv_firmware_host_test.c) - a
 * truncated download or an image of the wrong class is caught there in a
 * millisecond rather than as a co-processor that will not start and no way to
 * tell why.  See nv_gsp.c for the boot sequence that uses these.
 */
#ifndef KESTREL_NV_FWIMAGE_H
#define KESTREL_NV_FWIMAGE_H

#if defined(NV_FWIMAGE_HOST_TEST)
#include <stddef.h>          /* the host test provides u8/u16/u32/u64 itself */
#else
#include "kernel.h"
#endif

/* True if the image begins with the ELF magic.  Class-agnostic: both the
 * 32-bit FMC and the 64-bit resident manager answer yes. */
bool nv_fw_is_elf(const u8 *image, size_t bytes);

/* Find one ELF section by name, for ELF32 (the FMC) and ELF64 (the GSP
 * resident image - it is 64-bit, which the first cut of this did not handle).
 * Everything is bounds-checked against the file, because these headers are the
 * file's own account of itself and a truncated file describes one larger than
 * arrived.  `out_size` is 32-bit: a firmware section never approaches 4 GiB,
 * and an ELF64 section claiming to is rejected as out of bounds first. */
bool nv_fw_elf_section(const u8 *image, size_t bytes, const char *want,
                       const u8 **out, u32 *out_size);

/* The payload inside an NVIDIA container (the bootloader image), or false with
 * a reason it is not there. */
bool nv_fw_container_payload(const u8 *image, size_t bytes,
                             const u8 **payload, u32 *payload_size);

#endif /* KESTREL_NV_FWIMAGE_H */
