/* hwprobe.c - run the kernel's own hardware reasoning on the real machine.
 *
 * Everything in the operating system is tested either against a model or
 * inside a virtual machine, and both lie in the same direction: a model agrees
 * with the code because the same person wrote both, and a hypervisor presents
 * a simplified, well-behaved version of hardware that real silicon is not.
 *
 * Two of the things the kernel works out can be checked without any of that,
 * because they need no driver and no privilege: what the processor says about
 * itself, and what the firmware wrote down about the machine.  CPUID is an
 * ordinary instruction, and Windows will hand over the raw firmware tables to
 * any program that asks.
 *
 * So this runs the same derivations the kernel runs - the same CPUID leaves in
 * the same order, the same table walk - on the actual machine, and prints what
 * they produce.  Where that disagrees with what Windows reports about the same
 * machine, the kernel is wrong, and this is the cheapest possible place to
 * find that out.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <intrin.h>

static void cpuid_raw(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b,
                      uint32_t *c, uint32_t *d) {
    int r[4];
    __cpuidex(r, (int)leaf, (int)sub);
    *a = (uint32_t)r[0]; *b = (uint32_t)r[1];
    *c = (uint32_t)r[2]; *d = (uint32_t)r[3];
}

/* ------------------------------------------------------------- topology
 *
 * The same walk as cputopo.c: each subleaf names a level and says how far to
 * shift an identifier right to get past it.
 */
static int read_level_widths(uint32_t leaf, uint32_t *smt_shift,
                             uint32_t *core_shift, uint32_t *threads_per_core) {
    uint32_t a, b, c, d;
    int found = 0;
    *smt_shift = *core_shift = 0;
    *threads_per_core = 0;

    for (uint32_t sub = 0; sub < 16; sub++) {
        cpuid_raw(leaf, sub, &a, &b, &c, &d);
        uint32_t type = (c >> 8) & 0xFF;
        if (!type) break;

        uint32_t shift = a & 0x1F;
        uint32_t count = b & 0xFFFF;

        if (type == 1) { *smt_shift = shift; *threads_per_core = count ? count : 1; found = 1; }
        else if (type == 2) { *core_shift = shift; found = 1; }
        else if (shift > *core_shift) *core_shift = shift;
    }
    if (!*threads_per_core) *threads_per_core = 1;
    return found;
}

static void caches(void) {
    uint32_t a, b, c, d;
    printf("\ncaches, as the processor describes them:\n");
    for (uint32_t sub = 0; sub < 8; sub++) {
        cpuid_raw(4, sub, &a, &b, &c, &d);
        uint32_t type = a & 0x1F;
        if (!type) break;
        if (type > 3) continue;

        uint32_t level = (a >> 5) & 7;
        uint32_t sharing = ((a >> 14) & 0xFFF) + 1;
        uint32_t line = (b & 0xFFF) + 1;
        uint32_t parts = ((b >> 12) & 0x3FF) + 1;
        uint32_t ways = ((b >> 22) & 0x3FF) + 1;
        uint32_t sets = c + 1;
        unsigned long long bytes = (unsigned long long)ways * parts * line * sets;

        const char *kind = type == 1 ? "data" : type == 2 ? "instruction" : "unified";
        printf("  L%u %-12s %6llu KiB  %2u-way  %u-byte lines  shared by %u\n",
               level, kind, bytes / 1024, ways, line, sharing);
    }
}

/* --------------------------------------------------------------- SMBIOS */

#pragma pack(push, 1)
typedef struct { uint8_t type, length; uint16_t handle; } smb_hdr;
#pragma pack(pop)

static const char *smb_string(const smb_hdr *h, const uint8_t *end, uint8_t index) {
    if (!index) return "";
    const char *p = (const char *)h + h->length;
    while (index > 1 && (const uint8_t *)p < end) {
        while ((const uint8_t *)p < end && *p) p++;
        if ((const uint8_t *)p >= end) return "";
        p++;
        if ((const uint8_t *)p < end && !*p) return "";
        index--;
    }
    return (const uint8_t *)p < end ? p : "";
}

/* SMBIOS 3.x, section 7.18.1.  Kept identical to kernel/smbios.c so that this
 * probe checks the kernel's table rather than a second guess at it - which is
 * the entire point of running it here. */
static const char *form_factor(uint8_t v) {
    switch (v) {
    case 0x01: return "other";
    case 0x02: return "unknown";
    case 0x03: return "SIMM";
    case 0x04: return "SIP";
    case 0x05: return "chip";
    case 0x06: return "DIP";
    case 0x07: return "ZIP";
    case 0x08: return "proprietary card";
    case 0x09: return "DIMM";
    case 0x0A: return "TSOP";
    case 0x0B: return "row of chips";
    case 0x0C: return "RIMM";
    case 0x0D: return "SODIMM";
    case 0x0E: return "SRIMM";
    case 0x0F: return "FB-DIMM";
    case 0x10: return "die";
    default:   return "?";
    }
}

static const char *memory_type(uint8_t v) {
    switch (v) {
    case 0x1A: return "DDR4";
    case 0x22: return "DDR5";
    case 0x23: return "LPDDR5";
    case 0x1E: return "LPDDR4";
    case 0x18: return "DDR3";
    default:   return "?";
    }
}

/* What the firmware's table says the processor's top speed is.
 *
 * Kept because the processor itself will not always say: leaf 0x16 exists on
 * a Core Ultra, answers with zeroes, and the kernel falls back to this - so a
 * tool that prints only the leaf reports no clock at all on exactly the
 * machines where the fallback matters, and reads as the system not knowing
 * rather than the processor not saying.  It made this look like a bug in the
 * operating system when the bug was here.
 */
static unsigned firmware_max_mhz;

/* Read just the processor table, before anything is printed.
 *
 * The full walk below prints as it goes and runs after the processor section,
 * so it cannot supply a value that section needs.  Reading the one field twice
 * is cheaper than reordering the output into something less readable. */
/* What every core says about its own cache, not just the one this happens to
 * be running on.
 *
 * CPUID answers for the processor that executes it, and on a machine that
 * mixes performance and efficiency cores those answers differ - different
 * sizes, shared between different numbers of cores.  A single call describes
 * one kind and says nothing about the other, so a total built from it is
 * confidently wrong.
 *
 * Windows will pin a thread to a chosen processor, which is the whole trick:
 * ask each one in turn and add up what each owns a share of.  The kernel does
 * the same thing by running the same instruction on every core it started, so
 * this is a check of that arithmetic against the machine it will run on.
 */
static void per_core_caches(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    unsigned n = si.dwNumberOfProcessors;
    if (n > 64) n = 64;

    printf("\ncache as each processor describes it:\n");

    unsigned long long l1_total = 0, l2_total = 0, l3_total = 0;
    unsigned kinds_seen = 0;
    unsigned seen_l1[8] = { 0 }, seen_l2[8] = { 0 };

    for (unsigned cpu = 0; cpu < n; cpu++) {
        DWORD_PTR mask = (DWORD_PTR)1 << cpu;
        if (!SetThreadAffinityMask(GetCurrentThread(), mask)) continue;
        Sleep(0);                      /* let the scheduler move us */

        unsigned l1 = 0, l2 = 0, l3 = 0;
        unsigned l1_share = 1, l2_share = 1, l3_share = 1;

        for (uint32_t sub = 0; sub < 8; sub++) {
            uint32_t a, b, c, d;
            cpuid_raw(4, sub, &a, &b, &c, &d);
            uint32_t type = a & 0x1F;
            if (!type) break;
            if (type > 3) continue;

            uint32_t level = (a >> 5) & 7;
            uint32_t shared = ((a >> 14) & 0xFFF) + 1;
            uint32_t line = (b & 0xFFF) + 1;
            uint32_t parts = ((b >> 12) & 0x3FF) + 1;
            uint32_t ways = ((b >> 22) & 0x3FF) + 1;
            uint32_t sets = c + 1;
            unsigned bytes = ways * parts * line * sets;

            if (level == 1) { l1 += bytes; l1_share = shared; }
            else if (level == 2) { l2 = bytes; l2_share = shared; }
            else if (level >= 3) { l3 = bytes; l3_share = shared; }
        }

        /* One line per kind of core, and each kind counted once.
         *
         * Printing whenever the size differs from the previous processor
         * walks a machine that alternates between two kinds and reports
         * five: it was counting changes rather than kinds.  Remembering
         * what has already been seen is the difference between "this one
         * differs from the last" and "this is a kind of processor". */
        unsigned k;
        for (k = 0; k < kinds_seen; k++)
            if (seen_l1[k] == l1 && seen_l2[k] == l2) break;

        if (k == kinds_seen && kinds_seen < 8) {
            seen_l1[kinds_seen] = l1;
            seen_l2[kinds_seen] = l2;
            kinds_seen++;
            printf("  processor %2u: L1 %u KiB (shared by %u), L2 %u KiB "
                   "(shared by %u), L3 %u KiB (shared by %u)\n",
                   cpu, l1 / 1024, l1_share, l2 / 1024, l2_share,
                   l3 / 1024, l3_share);
        }

        /* A processor says how many *could* share a cache, not how many do -
         * this one claims its last-level cache is shared by 128 on a part with
         * twenty processors.  Nothing can be shared by more than exist. */
        if (l1_share > n) l1_share = n;
        if (l2_share > n) l2_share = n;
        if (l3_share > n) l3_share = n;

        l1_total += l1 / (l1_share ? l1_share : 1);
        l2_total += l2 / (l2_share ? l2_share : 1);
        l3_total += l3 / (l3_share ? l3_share : 1);
    }

    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)-1);

    printf("  %u kind(s) of core\n", kinds_seen);
    printf("  totals, each core counted for its share: L1 %llu KiB, "
           "L2 %llu KiB, L3 %llu KiB\n",
           l1_total / 1024, l2_total / 1024, l3_total / 1024);
}

static void smbios_prescan(void) {
    UINT size = GetSystemFirmwareTable('RSMB', 0, NULL, 0);
    if (!size) return;

    static unsigned char raw[512 * 1024];
    if (size > sizeof raw) return;
    GetSystemFirmwareTable('RSMB', 0, raw, size);

    /* Windows wraps them in an eight-byte header of its own. */
    const uint8_t *p = raw + 8, *end = raw + size;
    while (p + sizeof(smb_hdr) <= end) {
        const smb_hdr *h = (const smb_hdr *)p;
        if (h->type == 127) break;
        if (h->length < sizeof *h || p + h->length > end) break;

        if (h->type == 4 && h->length >= 0x16) {
            /* The same choice the kernel makes, for the same reason: the
             * maximum at 0x14 is often a placeholder - this board writes 12000
             * against a current speed of 3900 - so the current speed is
             * preferred and the maximum only used when it is plausible. */
            unsigned current = (h->length >= 0x18)
                                 ? *(const uint16_t *)(p + 0x16) : 0;
            unsigned maximum = *(const uint16_t *)(p + 0x14);

            if (current) firmware_max_mhz = current;
            else if (maximum && maximum <= 6000) firmware_max_mhz = maximum;
        }

        const uint8_t *q = p + h->length;
        while (q + 1 < end && !(q[0] == 0 && q[1] == 0)) q++;
        p = q + 2;
    }
}

static void smbios(void) {
    UINT size = GetSystemFirmwareTable('RSMB', 0, NULL, 0);
    if (!size) { printf("\nthe firmware tables are not readable\n"); return; }

    static unsigned char buf[512 * 1024];
    if (size > sizeof buf) { printf("\nfirmware tables too large\n"); return; }
    GetSystemFirmwareTable('RSMB', 0, buf, size);

    /* Windows wraps them in an eight-byte header of its own. */
    const uint8_t *p = buf + 8;
    const uint8_t *end = buf + size;

    printf("\nwhat the firmware wrote down about this machine:\n");

    int slots_used = 0, slots_total = 0;
    unsigned long long total_bytes = 0;

    while (p + sizeof(smb_hdr) <= end) {
        const smb_hdr *h = (const smb_hdr *)p;
        if (h->type == 127) break;
        if (h->length < sizeof *h || p + h->length > end) break;

        if (h->type == 1 && h->length >= 8)
            printf("  machine   : %s %s\n", smb_string(h, end, p[4]), smb_string(h, end, p[5]));
        else if (h->type == 2 && h->length >= 8)
            printf("  board     : %s %s\n", smb_string(h, end, p[4]), smb_string(h, end, p[5]));
        else if (h->type == 0 && h->length >= 0x18)
            printf("  firmware  : %s %s, %s\n", smb_string(h, end, p[4]),
                   smb_string(h, end, p[5]), smb_string(h, end, p[8]));
        else if (h->type == 4 && h->length >= 0x16)
            firmware_max_mhz = *(const uint16_t *)(p + 0x14);
        else if (h->type == 16 && h->length >= 0x0F)
            slots_total = *(const uint16_t *)(p + 0x0D);
        else if (h->type == 17 && h->length >= 0x15) {
            uint16_t sz = *(const uint16_t *)(p + 0x0C);
            unsigned long long bytes;
            if (!sz) bytes = 0;
            else if (sz == 0x7FFF && h->length >= 0x20)
                bytes = (unsigned long long)(*(const uint32_t *)(p + 0x1C)) * 1024 * 1024;
            else if (sz & 0x8000) bytes = (unsigned long long)(sz & 0x7FFF) * 1024;
            else bytes = (unsigned long long)sz * 1024 * 1024;

            unsigned speed = h->length >= 0x17 ? *(const uint16_t *)(p + 0x15) : 0;
            if (h->length >= 0x22) {
                unsigned configured = *(const uint16_t *)(p + 0x20);
                if (configured) speed = configured;
            }

            if (bytes) {
                slots_used++;
                total_bytes += bytes;
                printf("  %-9s : %llu MiB %s %s at %u MT/s, %s %s\n",
                       smb_string(h, end, p[0x10]), bytes / (1024 * 1024),
                       memory_type(p[0x12]), form_factor(p[0x0E]), speed,
                       h->length >= 0x1C ? smb_string(h, end, p[0x17]) : "",
                       h->length >= 0x1C ? smb_string(h, end, p[0x1A]) : "");
            }
        }

        p += h->length;
        while (p + 1 < end && !(p[0] == 0 && p[1] == 0)) p++;
        p += 2;
    }

    printf("  memory    : %d of %d slot(s) filled, %llu MiB total\n",
           slots_used, slots_total, total_bytes / (1024 * 1024));
}

int main(void) {
    uint32_t a, b, c, d;

    char brand[49] = {0};
    cpuid_raw(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        for (int i = 0; i < 3; i++) {
            cpuid_raw(0x80000002 + (uint32_t)i, 0, &a, &b, &c, &d);
            memcpy(brand + i * 16 + 0, &a, 4);
            memcpy(brand + i * 16 + 4, &b, 4);
            memcpy(brand + i * 16 + 8, &c, 4);
            memcpy(brand + i * 16 + 12, &d, 4);
        }
    }
    printf("processor: %s\n", brand);

    cpuid_raw(0, 0, &a, &b, &c, &d);
    uint32_t max_leaf = a;

    uint32_t smt_shift = 0, core_shift = 0, tpc = 1;
    int have = 0;
    if (max_leaf >= 0x1F) have = read_level_widths(0x1F, &smt_shift, &core_shift, &tpc);
    if (!have && max_leaf >= 0x0B) have = read_level_widths(0x0B, &smt_shift, &core_shift, &tpc);

    printf("topology : leaf %s, SMT shift %u, core shift %u, %u thread(s) per core\n",
           have ? (max_leaf >= 0x1F ? "0x1F" : "0x0B") : "none",
           smt_shift, core_shift, tpc);

    /* Decompose every processor's identifier, the way the kernel decomposes
     * the ones the firmware's table lists. */
    DWORD_PTR process_mask = 0, system_mask = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask);

    uint32_t cores[512], packages[512];
    int ncores = 0, npackages = 0, threads = 0;

    for (int i = 0; i < 64; i++) {
        if (!(system_mask & ((DWORD_PTR)1 << i))) continue;

        /* Run CPUID on that processor by moving this thread onto it, which is
         * the only way to read its own identifier. */
        DWORD_PTR one = (DWORD_PTR)1 << i;
        if (!SetThreadAffinityMask(GetCurrentThread(), one)) continue;
        Sleep(0);

        cpuid_raw(0x1F, 0, &a, &b, &c, &d);
        uint32_t apic = d;
        if (!apic && max_leaf >= 0x0B) { cpuid_raw(0x0B, 0, &a, &b, &c, &d); apic = d; }

        threads++;
        uint32_t core_id = apic >> smt_shift;
        uint32_t pkg_id = core_shift ? (apic >> core_shift) : 0;

        int known = 0;
        for (int k = 0; k < ncores; k++) if (cores[k] == core_id) { known = 1; break; }
        if (!known && ncores < 512) cores[ncores++] = core_id;

        known = 0;
        for (int k = 0; k < npackages; k++) if (packages[k] == pkg_id) { known = 1; break; }
        if (!known && npackages < 512) packages[npackages++] = pkg_id;
    }

    printf("derived  : %d socket(s), %d core(s), %d logical processor(s)\n",
           npackages, ncores, threads);

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    printf("Windows  : %u logical processor(s)\n", si.dwNumberOfProcessors);

    smbios_prescan();

    unsigned base = 0, top = 0, bus = 0;
    if (max_leaf >= 0x16) {
        cpuid_raw(0x16, 0, &a, &b, &c, &d);
        base = a & 0xFFFF; top = b & 0xFFFF; bus = c & 0xFFFF;
    }

    if (base || top) {
        printf("clocks   : base %u MHz, maximum %u MHz, bus %u MHz\n",
               base, top, bus);
    } else {
        /* Printed after the firmware table has been walked, so the fallback
         * has a value by now. */
        printf("clocks   : the processor reports none%s",
               max_leaf >= 0x16 ? " (the leaf exists and answers with zeroes)"
                                : " (no such leaf)");
        if (firmware_max_mhz)
            printf(", so the firmware's %u MHz is what this system shows\n",
                   firmware_max_mhz);
        else
            printf(", and the firmware does not say either\n");
    }

    if (max_leaf >= 7) {
        cpuid_raw(7, 0, &a, &b, &c, &d);
        printf("hybrid   : %s\n", ((d >> 15) & 1) ? "yes" : "no");
    }
    cpuid_raw(1, 0, &a, &b, &c, &d);
    printf("VT-x     : %s\n", ((c >> 5) & 1) ? "supported" : "not supported");

    caches();
    per_core_caches();

    smbios();
    return 0;
}
