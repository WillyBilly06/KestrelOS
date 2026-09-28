/* nv_firmware_host_test.c - read the REAL NVIDIA firmware with the real parser.
 *
 * KestrelOS cannot redistribute NVIDIA's co-processor firmware, so its GSP boot
 * path is never exercised in the VM (the files are absent) and never on a build
 * machine (no card).  That left the packaging parser - the code that finds the
 * signed image inside the files before handing it to the card - checked only
 * against tiny hand-built inputs.  This runs the UNMODIFIED nv_fwimage.c against
 * whatever real firmware is on disk (the FMC, the bootloader container, and the
 * 63 MB GSP resident manager), so a real file it cannot read is a failure here
 * rather than a co-processor that will not start with no way to tell why.
 *
 * It caught one: the GSP resident image is a 64-bit ELF, and the first cut of
 * the section finder was 32-bit only, so the file the whole boot exists to load
 * would have been rejected.  Every named section the parser returns is
 * cross-checked against an independent walk of the section table here.
 *
 * Firmware that is absent is SKIPPED, not failed - the point is that what is
 * present parses.  Build + run (from the repo root):
 *   clang -std=c11 -Wall -Wextra -Wno-unused-function -DNV_FWIMAGE_HOST_TEST \
 *         -I kernel tools/nv_firmware_host_test.c kernel/nv_fwimage.c \
 *         -o nv_firmware_test
 *   ./nv_firmware_test
 */
#ifndef NV_FWIMAGE_HOST_TEST
#define NV_FWIMAGE_HOST_TEST
#endif
#define _CRT_SECURE_NO_WARNINGS   /* fopen is fine here */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#include "nv_fwimage.h"

static int fails = 0, total = 0, skipped = 0;
static void ck(const char *what, int cond) {
    total++;
    if (!cond) { fails++; printf("    FAIL: %s\n", what); }
    else        printf("    ok:   %s\n", what);
}

static u8 *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = malloc(n);
    if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *len = (size_t)n;
    return buf;
}

/* Independent ELF section lookup (little-endian), for the cross-check. */
static bool indep_section(const u8 *d, size_t n, const char *want,
                          u64 *off, u64 *size) {
    if (n < 6 || memcmp(d, "\x7f""ELF", 4)) return false;
    int cls = d[4];
    u64 shoff; u16 entsize, num, strndx;
    if (cls == 1) {
        shoff  = *(const u32 *)(d + 0x20);
        entsize= *(const u16 *)(d + 0x2E);
        num    = *(const u16 *)(d + 0x30);
        strndx = *(const u16 *)(d + 0x32);
    } else if (cls == 2) {
        shoff  = *(const u64 *)(d + 0x28);
        entsize= *(const u16 *)(d + 0x3A);
        num    = *(const u16 *)(d + 0x3C);
        strndx = *(const u16 *)(d + 0x3E);
    } else return false;
    /* name at +0 (u32); offset/size at +16/+20 (ELF32) or +24/+32 (ELF64). */
    #define RD(base, disp, w) ((w)==4 ? (u64)*(const u32*)((base)+(disp)) : *(const u64*)((base)+(disp)))
    const u8 *strsh = d + shoff + (u64)strndx * entsize;
    u64 str_off = cls==1 ? RD(strsh,16,4) : RD(strsh,24,8);
    const char *names = (const char *)(d + str_off);
    for (u16 i = 0; i < num; i++) {
        const u8 *sh = d + shoff + (u64)i * entsize;
        u32 nameoff = *(const u32 *)sh;
        if (strcmp(names + nameoff, want)) continue;
        *off  = cls==1 ? RD(sh,16,4) : RD(sh,24,8);
        *size = cls==1 ? RD(sh,20,4) : RD(sh,32,8);
        return true;
    }
    #undef RD
    return false;
}

/* Assert nv_fw_elf_section agrees with the independent walk for one name. */
static void check_section(const u8 *d, size_t n, const char *name) {
    char label[128];
    const u8 *sec = NULL; u32 sz = 0;
    bool got = nv_fw_elf_section(d, n, name, &sec, &sz);
    snprintf(label, sizeof label, "section \"%s\" found", name);
    ck(label, got);
    if (!got) return;
    u64 ioff = 0, isz = 0;
    bool ind = indep_section(d, n, name, &ioff, &isz);
    snprintf(label, sizeof label, "section \"%s\" offset matches independent walk", name);
    ck(label, ind && (size_t)(sec - d) == ioff);
    snprintf(label, sizeof label, "section \"%s\" size matches independent walk", name);
    ck(label, ind && sz == isz);
    printf("          %s: off=%#zx size=%u\n", name, (size_t)(sec - d), sz);
}

static void test_elf(const char *path, const char *const *sections, int nsec) {
    size_t n = 0;
    u8 *d = slurp(path, &n);
    if (!d) { printf("  [skip] %s (absent)\n", path); skipped++; return; }
    printf("  [%s]  %zu bytes, ELF%s\n", path, n, d[4]==2 ? "64" : d[4]==1 ? "32" : "?");
    ck("is an ELF image", nv_fw_is_elf(d, n));
    for (int i = 0; i < nsec; i++) check_section(d, n, sections[i]);
    free(d);
}

static void test_container(const char *path) {
    size_t n = 0;
    u8 *d = slurp(path, &n);
    if (!d) { printf("  [skip] %s (absent)\n", path); skipped++; return; }
    printf("  [%s]  %zu bytes\n", path, n);
    const u8 *pay = NULL; u32 psz = 0;
    bool ok = nv_fw_container_payload(d, n, &pay, &psz);
    ck("parses as an NVIDIA container", ok);
    if (ok) {
        ck("payload lies within the file", pay >= d && pay + psz <= d + n);
        printf("          payload: off=%#zx size=%u (file %zu)\n",
               (size_t)(pay - d), psz, n);
    }
    free(d);
}

int main(void) {
    /* The FMC: a 32-bit ELF whose four named sections the boot ROM is handed. */
    static const char *fmc_secs[] = { "image", "signature", "publickey", "hash" };
    test_elf("firmware/nvidia/gb202/gsp/fmc-570.144.bin", fmc_secs, 4);

    /* The bootloader: an NVIDIA container. */
    test_container("firmware/nvidia/gb202/gsp/bootloader-570.144.bin");

    /* The GSP resident manager: a 64-bit ELF.  This is the file the boot exists
     * to load; the sections a Blackwell (GB20x) card needs are the image, its
     * version, and the signature for its own family. */
    static const char *gsp_secs[] = { ".fwimage", ".fwversion", ".fwsignature_gb20x" };
    test_elf("staging/firmware/nvidia/gb202/gsp/gsp-570.144.bin", gsp_secs, 3);

    /* The 595.99.02 GSP-RM extracted from the user's NVIDIA .run (gsp_ga10x.bin):
     * a newer resident image, same FMC-scheme ELF, carrying the gb20x signature
     * the 5070 Ti needs.  KestrelOS's parser must read it exactly as it does the
     * 570.144 one - the format is stable across releases. */
    test_elf("staging/firmware/nvidia/gb202/gsp/gsp-595.99.02.bin", gsp_secs, 3);

    printf("\n%d/%d checks passed", total - fails, total);
    if (skipped) printf(" (%d file(s) skipped as absent)", skipped);
    printf("%s\n", fails ? "  <<< FAILURE" : "  ALL GOOD");
    return fails ? 1 : 0;
}
