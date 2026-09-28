/* smbios.c - what the firmware knows about the machine it is running on.
 *
 * A processor can be asked about itself and answers precisely.  Almost nothing
 * else can: the memory modules cannot say how fast they are clocked or which
 * slot they sit in, and the board cannot say what it is called.  That
 * information exists only because the firmware wrote it down at power-on, into
 * a set of tables this file reads.
 *
 * The tables are reached through the firmware's own configuration table, whose
 * address the loader already passes across for other reasons.  Nothing new is
 * asked of the loader, which matters more than it sounds: on a machine with
 * Secure Boot the loader's contents are what the firmware has been told to
 * trust, so a change there costs the owner a trip through the enrolment tool,
 * and a change here costs nothing.
 *
 * Everything below treats the tables as data from somewhere else, because they
 * are.  Firmware tables are frequently wrong and occasionally truncated, so
 * every length is checked against the end of the table before it is trusted,
 * and a field that does not survive that check is left empty rather than
 * guessed at.
 */
#include "kernel.h"
#include "klog.h"
#include "cpu.h"
#include "smbios.h"

smbios_info_t g_smbios;

/* ------------------------------------------------------------- the anchor */

/* The two forms of entry point.  The 64-bit one came with version 3 and is
 * what any machine new enough to boot this will have; the older one is still
 * read because it costs six lines and virtual machines still emit it. */
typedef struct __attribute__((packed)) {
    char     anchor[5];        /* "_SM3_"                                   */
    u8       checksum;
    u8       length;
    u8       major, minor, docrev, revision;
    u8       reserved;
    u32      max_size;
    u64      table_address;
} smbios3_entry_t;

typedef struct __attribute__((packed)) {
    char     anchor[4];        /* "_SM_"                                    */
    u8       checksum;
    u8       length;
    u8       major, minor;
    u16      max_struct_size;
    u8       revision;
    u8       formatted[5];
    char     dmi_anchor[5];    /* "_DMI_"                                   */
    u8       dmi_checksum;
    u16      table_length;
    u32      table_address;
    u16      struct_count;
    u8       bcd_revision;
} smbios_entry_t;

typedef struct __attribute__((packed)) {
    u8  type;
    u8  length;
    u16 handle;
} smbios_header_t;

/* --------------------------------------------------------------- strings */

/* Strings are not stored in the structure.  They follow it, one after another,
 * each ending in a zero, the run ending in a second zero - and the structure
 * refers to them by position, counting from one, with zero meaning "no
 * string".  Reading one is therefore a walk, and the walk has to be bounded
 * because the run is only terminated if the table is well formed. */
static const char *smb_string(const smbios_header_t *h, const u8 *end, u8 index) {
    if (!index) return "";

    const char *p = (const char *)h + h->length;
    while (index > 1 && (const u8 *)p < end) {
        while ((const u8 *)p < end && *p) p++;
        if ((const u8 *)p >= end) return "";
        p++;                                  /* past the terminator        */
        if ((const u8 *)p < end && !*p) return "";   /* end of the run      */
        index--;
    }
    return (const u8 *)p < end ? p : "";
}

static void copy_string(char *dst, size_t cap, const char *src) {
    /* Firmware pads these with spaces surprisingly often, and a product name
     * that renders as "ASUS      " looks like a bug in the display rather than
     * in the table it came from. */
    while (*src == ' ') src++;
    strlcpy(dst, src, cap);
    size_t n = strlen(dst);
    while (n && dst[n - 1] == ' ') dst[--n] = 0;
}

/* ------------------------------------------------------------ descriptions */

static const char *memory_form_factor(u8 v) {
    /* SMBIOS 3.x, section 7.18.1.  Every value, in order, because the previous
     * table listed a scattered subset and each entry from 0x02 to 0x0D sat one
     * place too low - 0x09 is a DIMM and it was labelled TSOP, which is 0x0A.
     * The user's own desktop memory was reported as TSOP for that reason.
     *
     * The two entries that were right, 0x0F and 0x10, are why it was never
     * obvious: a table that is wrong in the middle and right at the end looks
     * like a table somebody checked. */
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
    default:   return "";
    }
}

static const char *memory_type(u8 v) {
    switch (v) {
    case 0x0F: return "SDRAM";
    case 0x12: return "DDR";
    case 0x13: return "DDR2";
    case 0x18: return "DDR3";
    case 0x1A: return "DDR4";
    case 0x1E: return "LPDDR4";
    case 0x22: return "DDR5";
    case 0x23: return "LPDDR5";
    default:   return "";
    }
}

/* ------------------------------------------------------------ the entries */

static void take_bios(const smbios_header_t *h, const u8 *end) {
    const u8 *f = (const u8 *)h;
    if (h->length < 0x18) return;
    copy_string(g_smbios.bios_vendor, sizeof g_smbios.bios_vendor, smb_string(h, end, f[4]));
    copy_string(g_smbios.bios_version, sizeof g_smbios.bios_version, smb_string(h, end, f[5]));
    copy_string(g_smbios.bios_date, sizeof g_smbios.bios_date, smb_string(h, end, f[8]));
}

static void take_system(const smbios_header_t *h, const u8 *end) {
    const u8 *f = (const u8 *)h;
    if (h->length < 0x08) return;
    copy_string(g_smbios.system_maker, sizeof g_smbios.system_maker, smb_string(h, end, f[4]));
    copy_string(g_smbios.system_product, sizeof g_smbios.system_product, smb_string(h, end, f[5]));
}

static void take_board(const smbios_header_t *h, const u8 *end) {
    const u8 *f = (const u8 *)h;
    if (h->length < 0x08) return;
    copy_string(g_smbios.board_maker, sizeof g_smbios.board_maker, smb_string(h, end, f[4]));
    copy_string(g_smbios.board_product, sizeof g_smbios.board_product, smb_string(h, end, f[5]));
}

static void take_processor(const smbios_header_t *h, const u8 *end) {
    const u8 *f = (const u8 *)h;
    if (h->length < 0x1A) return;
    if (!g_smbios.cpu_socket[0])
        copy_string(g_smbios.cpu_socket, sizeof g_smbios.cpu_socket, smb_string(h, end, f[4]));

    /* The processor's own view of its clocks is better than this table's, so
     * this is only kept for the machines where CPUID declines to answer.
     *
     * Which of the two figures to take is not obvious and the wrong one is
     * absurd.  The table holds a maximum at 0x14 and a current speed at 0x16,
     * and firmware routinely writes a placeholder into the maximum - a
     * Gigabyte Z890 board reports 12000 there, against a current speed of
     * 3900, and a 265K does not run at twelve gigahertz.  Windows shows the
     * current speed for the same reason.
     *
     * So the current speed is preferred, and the maximum is used only when
     * there is no current speed and the maximum is a number a processor could
     * actually have.  Six gigahertz is comfortably above anything shipping and
     * comfortably below the placeholders.
     */
    if (!g_smbios.cpu_max_mhz) {
        u16 current = (h->length >= 0x18) ? *(const u16 *)(f + 0x16) : 0;
        u16 maximum = (h->length >= 0x16) ? *(const u16 *)(f + 0x14) : 0;

        if (current) g_smbios.cpu_max_mhz = current;
        else if (maximum && maximum <= 6000) g_smbios.cpu_max_mhz = maximum;
    }
}

static void take_memory_array(const smbios_header_t *h, const u8 *end) {
    (void)end;
    const u8 *f = (const u8 *)h;
    if (h->length < 0x0F) return;
    g_smbios.memory_slots_total = *(const u16 *)(f + 0x0D);
}

static void take_memory_device(const smbios_header_t *h, const u8 *end) {
    const u8 *f = (const u8 *)h;
    if (h->length < 0x15) return;
    if (g_smbios.memory_count >= SMBIOS_MAX_MEMORY) return;

    smbios_memory_t *m = &g_smbios.memory[g_smbios.memory_count];

    /* Size 0 means the slot is empty.  An empty slot is still worth recording,
     * because "two of four slots filled" is a different machine from "two of
     * two", and it is the kind of thing somebody opens a system information
     * window to find out. */
    u16 size = *(const u16 *)(f + 0x0C);
    copy_string(m->locator, sizeof m->locator, smb_string(h, end, f[0x10]));

    if (size == 0) {
        m->bytes = 0;
    } else if (size == 0x7FFF && h->length >= 0x20) {
        m->bytes = (u64)(*(const u32 *)(f + 0x1C)) * 1024 * 1024;   /* extended */
    } else if (size & 0x8000) {
        m->bytes = (u64)(size & 0x7FFF) * 1024;                     /* in KiB   */
    } else {
        m->bytes = (u64)size * 1024 * 1024;                         /* in MiB   */
    }

    strlcpy(m->form_factor, memory_form_factor(f[0x0E]), sizeof m->form_factor);
    if (h->length >= 0x13) strlcpy(m->type, memory_type(f[0x12]), sizeof m->type);
    if (h->length >= 0x17) m->speed_mts = *(const u16 *)(f + 0x15);
    if (h->length >= 0x1C) {
        copy_string(m->maker, sizeof m->maker, smb_string(h, end, f[0x17]));
        copy_string(m->part, sizeof m->part, smb_string(h, end, f[0x1A]));
    }
    /* The speed it was actually configured to run at, which on a machine with
     * a memory profile enabled is not the speed printed on the module. */
    if (h->length >= 0x22) {
        u16 configured = *(const u16 *)(f + 0x20);
        if (configured) m->speed_mts = configured;
    }

    if (m->bytes) {
        g_smbios.memory_slots_used++;
        g_smbios.memory_bytes += m->bytes;
        if (m->speed_mts > g_smbios.memory_speed_mts)
            g_smbios.memory_speed_mts = m->speed_mts;
        if (!g_smbios.memory_kind[0] && m->type[0])
            strlcpy(g_smbios.memory_kind, m->type, sizeof g_smbios.memory_kind);
        if (!g_smbios.memory_form[0] && m->form_factor[0])
            strlcpy(g_smbios.memory_form, m->form_factor, sizeof g_smbios.memory_form);
    }

    g_smbios.memory_count++;
}

/* -------------------------------------------------------------- the walk */

static void walk(u64 table_phys, u32 table_len) {
    if (!table_phys || !table_len || table_len > (4u << 20)) return;

    const u8 *base = phys_to_virt(table_phys);
    const u8 *end  = base + table_len;
    const u8 *p    = base;

    for (int guard = 0; guard < 2048 && p + sizeof(smbios_header_t) <= end; guard++) {
        const smbios_header_t *h = (const smbios_header_t *)p;

        if (h->type == 127) break;                    /* end of table       */
        if (h->length < sizeof *h) break;             /* malformed          */
        if (p + h->length > end) break;

        switch (h->type) {
        case 0:  take_bios(h, end);          break;
        case 1:  take_system(h, end);        break;
        case 2:  take_board(h, end);         break;
        case 4:  take_processor(h, end);     break;
        case 16: take_memory_array(h, end);  break;
        case 17: take_memory_device(h, end); break;
        default: break;
        }

        /* Step over the structure and the run of strings that follows it. */
        p += h->length;
        while (p + 1 < end && !(p[0] == 0 && p[1] == 0)) p++;
        p += 2;
    }
}

/* ---------------------------------------------------------------- finding */

/* The EFI configuration table, walked for the SMBIOS entry point.
 *
 * Reading the firmware's own structures after it has shut down is only safe
 * because of where they live: the system table and its configuration array are
 * required to be in memory the firmware keeps for its runtime services, which
 * the loader marks reserved and this kernel's allocator will not hand out. */
typedef struct __attribute__((packed)) {
    u8  guid[16];
    u64 table;
} efi_config_entry_t;

/* {f2fd1544-9794-4a2c-992e-e5bbcf20e394}, laid out as a GUID is stored: the
 * first three fields little-endian, the last eight bytes in order. */
static const u8 GUID_SMBIOS3[16] = {
    0x44, 0x15, 0xFD, 0xF2, 0x94, 0x97, 0x2C, 0x4A,
    0x99, 0x2E, 0xE5, 0xBB, 0xCF, 0x20, 0xE3, 0x94
};
/* {eb9d2d31-2d88-11d3-9a16-0090273fc14d} */
static const u8 GUID_SMBIOS[16] = {
    0x31, 0x2D, 0x9D, 0xEB, 0x88, 0x2D, 0xD3, 0x11,
    0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D
};

void smbios_init(void) {
    memset(&g_smbios, 0, sizeof g_smbios);

    if (!g_boot.efi_system_table) {
        kinfo("smbios", "no EFI system table, so the firmware's description of "
                        "this machine cannot be read");
        return;
    }

    /* The system table's layout: a 24-byte header, then several pointers, then
     * the count and address of the configuration table.  Only the last two
     * matter here, so they are read by offset rather than by declaring the
     * whole structure. */
    const u8 *st = phys_to_virt(g_boot.efi_system_table);
    u64 signature = *(const u64 *)st;
    if (signature != 0x5453595320494249ULL) {          /* "IBI SYST"        */
        kwarn("smbios", "the EFI system table does not look like one");
        return;
    }

    /* The header is 24 bytes, then vendor and revision, then six console
     * pointers, then the two runtime table pointers - which puts the count of
     * configuration entries at 104 and the array itself at 112. */
    u64 count = *(const u64 *)(st + 104);
    u64 array = *(const u64 *)(st + 112);
    if (!count || !array || count > 256) {
        kwarn("smbios", "the EFI configuration table is not readable "
                        "(%llu entries at %#llx)",
              (unsigned long long)count, (unsigned long long)array);
        return;
    }

    const efi_config_entry_t *cfg = phys_to_virt(array);
    u64 entry_phys = 0;
    bool v3 = false;

    for (u64 i = 0; i < count; i++) {
        if (!memcmp(cfg[i].guid, GUID_SMBIOS3, 16)) { entry_phys = cfg[i].table; v3 = true; break; }
        if (!memcmp(cfg[i].guid, GUID_SMBIOS, 16))  { entry_phys = cfg[i].table; }
    }
    if (!entry_phys) {
        kinfo("smbios", "the firmware published no description of this machine");
        return;
    }

    if (v3) {
        const smbios3_entry_t *e = phys_to_virt(entry_phys);
        if (memcmp(e->anchor, "_SM3_", 5)) {
            kwarn("smbios", "the entry point does not carry its anchor");
            return;
        }
        g_smbios.major = e->major;
        g_smbios.minor = e->minor;
        walk(e->table_address, e->max_size);
    } else {
        const smbios_entry_t *e = phys_to_virt(entry_phys);
        if (memcmp(e->anchor, "_SM_", 4)) {
            kwarn("smbios", "the entry point does not carry its anchor");
            return;
        }
        g_smbios.major = e->major;
        g_smbios.minor = e->minor;
        walk(e->table_address, e->table_length);
    }

    g_smbios.present = true;

    /* If the processor would not name its own clock, the firmware's table
     * usually will.  It is the maximum rather than the base, and is labelled
     * as such where it is shown, because those are different numbers and only
     * one of them is what the processor idles at. */
    if (!g_topo.clocks_from_cpuid && g_smbios.cpu_max_mhz) {
        g_topo.max_mhz = g_smbios.cpu_max_mhz;
        kinfo("smbios", "the processor does not report its clocks, so the "
                        "firmware's figure of %u MHz is used", g_topo.max_mhz);
    }

    kinfo("smbios", "version %u.%u: %s %s, board %s %s",
          g_smbios.major, g_smbios.minor,
          g_smbios.system_maker, g_smbios.system_product,
          g_smbios.board_maker, g_smbios.board_product);

    if (g_smbios.memory_count)
        kinfo("smbios", "memory: %u of %u slot(s) filled, %llu MiB of %s %s at %u MT/s",
              g_smbios.memory_slots_used,
              g_smbios.memory_slots_total ? g_smbios.memory_slots_total
                                          : g_smbios.memory_count,
              (unsigned long long)(g_smbios.memory_bytes / (1024 * 1024)),
              g_smbios.memory_kind[0] ? g_smbios.memory_kind : "memory",
              g_smbios.memory_form[0] ? g_smbios.memory_form : "",
              g_smbios.memory_speed_mts);

    for (u32 i = 0; i < g_smbios.memory_count; i++) {
        smbios_memory_t *m = &g_smbios.memory[i];
        if (!m->bytes) {
            kdebug("smbios", "%s: empty", m->locator);
            continue;
        }
        kdebug("smbios", "%s: %llu MiB %s %s at %u MT/s, %s %s",
               m->locator, (unsigned long long)(m->bytes / (1024 * 1024)),
               m->type, m->form_factor, m->speed_mts, m->maker, m->part);
    }
}
