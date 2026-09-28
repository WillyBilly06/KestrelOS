/* nv_fwimage.c - see nv_fwimage.h.  Pure byte parsing, no card. */
#if defined(NV_FWIMAGE_HOST_TEST)
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#else
#include "kernel.h"
#endif
#include "nv_fwimage.h"

/* --------------------------------------------------------------- container
 *
 * An NVIDIA container: a small header (magic 0x10de) in front of a payload,
 * with the payload's offset and size stated in the header. */
typedef struct __attribute__((packed)) {
    u32 magic;            /* 0x10de                                        */
    u32 version;
    u32 total_size;       /* padded, so larger than the file               */
    u32 header_offset;
    u32 payload_offset;
    u32 payload_size;
} nv_container_t;

#define NV_CONTAINER_MAGIC 0x10DE

bool nv_fw_container_payload(const u8 *image, size_t bytes,
                             const u8 **payload, u32 *payload_size) {
    if (bytes < sizeof(nv_container_t)) return false;

    const nv_container_t *h = (const nv_container_t *)image;
    if (h->magic != NV_CONTAINER_MAGIC) return false;

    /* The payload has to be inside the file.  A container whose header says
     * otherwise is either not this format or was truncated in transit, and
     * both are reasons to stop rather than to read past the end. */
    if ((u64)h->payload_offset + h->payload_size > bytes) return false;
    if (!h->payload_size) return false;

    *payload = image + h->payload_offset;
    *payload_size = h->payload_size;
    return true;
}

bool nv_fw_is_elf(const u8 *image, size_t bytes) {
    return bytes > 4 && image[0] == 0x7F && image[1] == 'E' &&
           image[2] == 'L' && image[3] == 'F';
}

/* --------------------------------------------------------------------- ELF
 *
 * Section headers, 32- and 64-bit.  The two differ only in that the 64-bit
 * form widens the address/offset/size fields to 8 bytes and pushes the
 * section-header table's location later in the file header. */
typedef struct __attribute__((packed)) {
    u32 name_offset;
    u32 type;
    u32 flags;
    u32 addr;
    u32 offset;
    u32 size;
    u32 link, info, align, entry_size;
} elf32_section_t;

typedef struct __attribute__((packed)) {
    u32 name_offset;
    u32 type;
    u64 flags;
    u64 addr;
    u64 offset;
    u64 size;
    u32 link, info;
    u64 align, entry_size;
} elf64_section_t;

/* The ELF class byte and where each class keeps the section-header table. */
#define ELF_CLASS32  1
#define ELF_CLASS64  2

static bool elf32_section(const u8 *image, size_t bytes, const char *want,
                          const u8 **out, u32 *out_size) {
    if (bytes < 0x34) return false;

    u32 shoff     = *(const u32 *)(image + 0x20);
    u16 shentsize = *(const u16 *)(image + 0x2E);
    u16 shnum     = *(const u16 *)(image + 0x30);
    u16 shstrndx  = *(const u16 *)(image + 0x32);

    if (shentsize < sizeof(elf32_section_t) || !shnum || shstrndx >= shnum)
        return false;
    if ((u64)shoff + (u64)shnum * shentsize > bytes) return false;

    const elf32_section_t *strtab =
        (const elf32_section_t *)(image + shoff + (u32)shstrndx * shentsize);
    if ((u64)strtab->offset + strtab->size > bytes) return false;
    const char *names = (const char *)(image + strtab->offset);

    for (u16 i = 0; i < shnum; i++) {
        const elf32_section_t *sh =
            (const elf32_section_t *)(image + shoff + (u32)i * shentsize);

        if (sh->name_offset >= strtab->size) continue;
        if ((u64)sh->offset + sh->size > bytes) continue;
        if (strcmp(names + sh->name_offset, want)) continue;

        *out = image + sh->offset;
        *out_size = sh->size;
        return true;
    }
    return false;
}

static bool elf64_section(const u8 *image, size_t bytes, const char *want,
                          const u8 **out, u32 *out_size) {
    if (bytes < 0x40) return false;

    u64 shoff     = *(const u64 *)(image + 0x28);
    u16 shentsize = *(const u16 *)(image + 0x3A);
    u16 shnum     = *(const u16 *)(image + 0x3C);
    u16 shstrndx  = *(const u16 *)(image + 0x3E);

    if (shentsize < sizeof(elf64_section_t) || !shnum || shstrndx >= shnum)
        return false;
    if (shoff + (u64)shnum * shentsize > bytes) return false;

    const elf64_section_t *strtab =
        (const elf64_section_t *)(image + shoff + (u64)shstrndx * shentsize);
    if (strtab->offset + strtab->size > bytes) return false;
    const char *names = (const char *)(image + strtab->offset);
    u64 str_size = strtab->size;

    for (u16 i = 0; i < shnum; i++) {
        const elf64_section_t *sh =
            (const elf64_section_t *)(image + shoff + (u64)i * shentsize);

        if (sh->name_offset >= str_size) continue;
        if (sh->offset + sh->size > bytes) continue;
        if (strcmp(names + sh->name_offset, want)) continue;

        /* A firmware section never approaches 4 GiB; a 64-bit size that does
         * has already failed the bounds check above against a <4 GiB file. */
        *out = image + sh->offset;
        *out_size = (u32)sh->size;
        return true;
    }
    return false;
}

bool nv_fw_elf_section(const u8 *image, size_t bytes, const char *want,
                       const u8 **out, u32 *out_size) {
    if (!nv_fw_is_elf(image, bytes) || bytes < 6) return false;
    if (image[4] == ELF_CLASS32) return elf32_section(image, bytes, want, out, out_size);
    if (image[4] == ELF_CLASS64) return elf64_section(image, bytes, want, out, out_size);
    return false;
}
