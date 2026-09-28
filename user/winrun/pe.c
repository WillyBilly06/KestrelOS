/* pe.c - mapping Windows images.
 *
 * A PE file is a header, a list of sections with the addresses they want to
 * live at, a table of fix-ups for when they cannot, and a list of the
 * functions it expects somebody else to provide.  Loading one means laying the
 * sections out, applying the fix-ups, and filling in the function table - and
 * for a program that ships its own DLLs, doing all of that again for each of
 * them before the first line of the program runs.
 *
 * Everything here is bounds-checked against the real length of the file.  An
 * executable is data from outside this system, and a header that claims a
 * section runs to the end of memory is a normal thing to find in the wild.
 */
#include "win.h"

/* ------------------------------------------------------------ PE structures */

#define IMAGE_DOS_SIGNATURE  0x5A4D          /* "MZ" */
#define IMAGE_NT_SIGNATURE   0x00004550      /* "PE\0\0" */
#define PE32PLUS_MAGIC       0x20B
#define PE32_MAGIC           0x10B
#define MACHINE_AMD64        0x8664
#define SUBSYSTEM_CONSOLE    3
#define SUBSYSTEM_GUI        2

#define DIR_EXPORT      0
#define DIR_IMPORT      1
#define DIR_RESOURCE    2
#define DIR_EXCEPTION   3
#define DIR_BASERELOC   5
#define DIR_TLS         9
#define DIR_DELAY      13

#define SCN_UNINIT_DATA 0x00000080

#define DLL_PROCESS_ATTACH 1
#define DLL_THREAD_ATTACH  2
#define DLL_THREAD_DETACH  3
#define DLL_PROCESS_DETACH 0

typedef struct { uint32_t rva, size; } data_dir_t;

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  major_linker, minor_linker;
    uint32_t size_code, size_init, size_uninit;
    uint32_t entry_rva, base_code;
    uint64_t image_base;
    uint32_t section_align, file_align;
    uint16_t major_os, minor_os, major_image, minor_image, major_subsys, minor_subsys;
    uint32_t win32_version, size_image, size_headers, checksum;
    uint16_t subsystem, dll_flags;
    uint64_t stack_reserve, stack_commit, heap_reserve, heap_commit;
    uint32_t loader_flags, dir_count;
    data_dir_t dirs[16];
} opt64_t;

typedef struct __attribute__((packed)) {
    char     name[8];
    uint32_t virtual_size, virtual_address;
    uint32_t raw_size, raw_offset;
    uint32_t reloc_offset, line_offset;
    uint16_t reloc_count, line_count;
    uint32_t characteristics;
} section_t;

typedef struct __attribute__((packed)) {
    uint32_t lookup_rva, timestamp, forwarder, name_rva, thunk_rva;
} import_desc_t;

typedef struct __attribute__((packed)) {
    uint32_t flags, timestamp;
    uint16_t major, minor;
    uint32_t name_rva, ordinal_base;
    uint32_t count, name_count;
    uint32_t function_rva, name_rva_table, ordinal_table;
} export_dir_t;

typedef struct __attribute__((packed)) {
    uint64_t raw_start, raw_end, index_addr, callbacks;
    uint32_t zero_fill, characteristics;
} tls_dir_t;

typedef struct __attribute__((packed)) {
    uint32_t characteristics, timestamp;
    uint16_t major, minor, named_count, id_count;
} res_dir_t;

typedef struct __attribute__((packed)) { uint32_t name, offset; } res_entry_t;
typedef struct __attribute__((packed)) { uint32_t data_rva, size, codepage, reserved; } res_data_t;

/* --------------------------------------------------------------- the table */

static win_module_t *modules;

static void lower(char *s) {
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32;
}

/* Windows names DLLs case-insensitively, and half the world writes
 * "KERNEL32.dll".  Comparisons here always fold case.  The ".dll" is optional
 * because LoadLibrary accepts it either way. */
static bool name_matches(const char *a, const char *b) {
    if (!strcasecmp(a, b)) return true;
    size_t la = strlen(a), lb = strlen(b);
    if (la > 4 && !strcasecmp(a + la - 4, ".dll") && la - 4 == lb)
        return !strncasecmp(a, b, lb);
    if (lb > 4 && !strcasecmp(b + lb - 4, ".dll") && lb - 4 == la)
        return !strncasecmp(a, b, la);
    return false;
}

win_module_t *pe_find_module(const char *name) {
    for (win_module_t *m = modules; m; m = m->next)
        if (name_matches(m->name, name)) return m;

    /* The API sets - "api-ms-win-core-file-l1-2-0.dll" and its several hundred
     * relatives - are forwarders into kernel32 on a real system.  Treating any
     * name beginning that way as kernel32 is what they resolve to in
     * practice. */
    if (!strncasecmp(name, "api-ms-win-", 11) || !strncasecmp(name, "ext-ms-win-", 11))
        for (win_module_t *m = modules; m; m = m->next)
            if (!strcmp(m->name, "kernel32.dll")) return m;
    return NULL;
}

void win_register(const char *dll, const win_export_t *table) {
    win_module_t *m = calloc(1, sizeof *m);
    if (!m) win_fail("out of memory registering %s", dll);
    strlcpy(m->name, dll, sizeof m->name);
    lower(m->name);
    m->table = table;
    m->refs = 1;
    m->initialised = true;
    m->next = modules;
    modules = m;
}

/* ------------------------------------------------------------ small readers */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* True when an RVA and length lie inside the mapped image.  Every walk over
 * loaded data goes through this, because the directories point at each other
 * by offset and a corrupt one otherwise walks off the end. */
static bool in_image(win_module_t *m, uint32_t rva, uint32_t len) {
    if (!m->base) return false;
    if (rva > m->size) return false;
    if ((uint64_t)rva + len > m->size) return false;
    return true;
}

static void *at(win_module_t *m, uint32_t rva) { return m->base + rva; }

/* A NUL-terminated string inside the image, or NULL if it runs off the end. */
static const char *str_at(win_module_t *m, uint32_t rva) {
    if (rva >= m->size) return NULL;
    const char *s = (const char *)m->base + rva;
    for (uint32_t i = rva; i < m->size; i++)
        if (m->base[i] == 0) return s;
    return NULL;
}

/* --------------------------------------------------------------- exports */

/* Where a DLL says its function lives.  An export can also be a forwarder - a
 * string naming another DLL and function - which is how Windows moves code
 * between libraries without breaking anything that imported it. */
void *pe_export_of(win_module_t *m, const char *name, uint16_t ordinal) {
    if (m->table) {                       /* a built-in library */
        if (!name) return NULL;
        for (const win_export_t *e = m->table; e->name; e++)
            if (!strcmp(e->name, name)) return e->fn;
        return NULL;
    }
    if (!m->base || !m->export_size) return NULL;
    if (!in_image(m, m->export_rva, sizeof(export_dir_t))) return NULL;

    const export_dir_t *ed = at(m, m->export_rva);
    if (!in_image(m, ed->function_rva, ed->count * 4)) return NULL;
    const uint32_t *functions = at(m, ed->function_rva);

    uint32_t index = 0xFFFFFFFF;
    if (name) {
        if (!in_image(m, ed->name_rva_table, ed->name_count * 4)) return NULL;
        if (!in_image(m, ed->ordinal_table, ed->name_count * 2)) return NULL;
        const uint32_t *names = at(m, ed->name_rva_table);
        const uint16_t *ords = at(m, ed->ordinal_table);
        for (uint32_t i = 0; i < ed->name_count; i++) {
            const char *n = str_at(m, names[i]);
            if (n && !strcmp(n, name)) { index = ords[i]; break; }
        }
    } else {
        if (ordinal < ed->ordinal_base) return NULL;
        index = ordinal - ed->ordinal_base;
    }
    if (index >= ed->count) return NULL;

    uint32_t rva = functions[index];
    if (!rva) return NULL;

    /* Inside the export directory means a forwarder, not code. */
    if (rva >= m->export_rva && rva < m->export_rva + m->export_size) {
        const char *fwd = str_at(m, rva);
        if (!fwd) return NULL;
        char dll[64], fn[128];
        const char *dot = strrchr(fwd, '.');
        if (!dot) return NULL;
        size_t dl = (size_t)(dot - fwd);
        if (dl >= sizeof dll) return NULL;
        memcpy(dll, fwd, dl); dll[dl] = 0;
        strlcpy(fn, dot + 1, sizeof fn);
        win_trace("%s forwards %s to %s.%s", m->name, name ? name : "(ordinal)", dll, fn);
        return pe_resolve(dll, fn, 0);
    }
    return m->base + rva;
}

void *pe_resolve(const char *dll, const char *name, uint16_t ordinal) {
    win_module_t *m = pe_find_module(dll);
    if (!m) m = pe_load(dll, true);
    if (!m) return NULL;
    return pe_export_of(m, name, ordinal);
}

/* -------------------------------------------------------------- resources */

/* The resource directory is three levels of the same node type: by type, then
 * by identifier, then by language.  Only the first language is looked at,
 * which is what a program with one language in it will find anyway. */
static const res_entry_t *res_child(win_module_t *m, uint32_t dir_rva, uint16_t id, bool by_id) {
    if (!in_image(m, dir_rva, sizeof(res_dir_t))) return NULL;
    const res_dir_t *d = at(m, dir_rva);
    uint32_t total = (uint32_t)d->named_count + d->id_count;
    if (!in_image(m, dir_rva + sizeof(res_dir_t), total * sizeof(res_entry_t))) return NULL;
    const res_entry_t *e = (const res_entry_t *)((const uint8_t *)d + sizeof(res_dir_t));

    if (!by_id) return total ? &e[0] : NULL;          /* first of whatever there is */
    for (uint32_t i = d->named_count; i < total; i++)
        if ((e[i].name & 0x7FFFFFFF) == id) return &e[i];
    return NULL;
}

uint32_t pe_resource_rva(win_module_t *m);

const void *pe_find_resource(win_module_t *m, uint16_t type, uint16_t id, uint32_t *size_out) {
    if (!m || !m->base) return NULL;
    uint32_t rva = pe_resource_rva(m);
    if (!rva) return NULL;

    const res_entry_t *t = res_child(m, rva, type, true);
    if (!t || !(t->offset & 0x80000000)) return NULL;
    const res_entry_t *i = res_child(m, rva + (t->offset & 0x7FFFFFFF), id, true);
    if (!i || !(i->offset & 0x80000000)) return NULL;
    const res_entry_t *l = res_child(m, rva + (i->offset & 0x7FFFFFFF), 0, false);
    if (!l || (l->offset & 0x80000000)) return NULL;

    uint32_t data_rva = rva + l->offset;
    if (!in_image(m, data_rva, sizeof(res_data_t))) return NULL;
    const res_data_t *d = at(m, data_rva);
    if (!in_image(m, d->data_rva, d->size)) return NULL;
    if (size_out) *size_out = d->size;
    return at(m, d->data_rva);
}

/* Kept beside the module so pe_find_resource does not have to re-read the
 * header, which may have been overlaid by a section. */
static uint32_t resource_rva[32];
static win_module_t *resource_owner[32];
static int resource_count;

uint32_t pe_resource_rva(win_module_t *m) {
    for (int i = 0; i < resource_count; i++)
        if (resource_owner[i] == m) return resource_rva[i];
    return 0;
}

static void remember_resources(win_module_t *m, uint32_t rva) {
    if (!rva || resource_count >= (int)(sizeof resource_rva / sizeof resource_rva[0])) return;
    resource_owner[resource_count] = m;
    resource_rva[resource_count++] = rva;
}

/* ------------------------------------------------------------------- TLS */

/* Thread-local storage in a PE image is a block of initialised data that each
 * thread gets a private copy of, plus a list of callbacks run when a thread
 * starts or ends.  A single-threaded program uses the block and never the
 * callbacks; a threaded one uses both. */
static tls_dir_t *module_tls[32];
static win_module_t *tls_owner[32];
static int tls_count;

void pe_run_tls_callbacks(win_module_t *m, DWORD reason) {
    for (int i = 0; i < tls_count; i++) {
        if (m && tls_owner[i] != m) continue;
        tls_dir_t *t = module_tls[i];
        if (!t || !t->callbacks) continue;
        uint64_t *cb = (uint64_t *)(uintptr_t)t->callbacks;
        for (int k = 0; cb[k]; k++) {
            typedef void WINAPI (*tls_cb)(void *, DWORD, void *);
            ((tls_cb)(uintptr_t)cb[k])(tls_owner[i]->base, reason, NULL);
        }
    }
}

/* --------------------------------------------------------------- loading */

static const char *search_dirs[] = { NULL, ".", "/lib/win", "/bin", NULL };

static bool find_file(const char *name, char *out, size_t cap) {
    if (strchr(name, '/') || strchr(name, '\\')) {
        win_path_to_host(name, out, cap);
        return file_exists(out);
    }
    /* The directory the program itself came from is searched first, which is
     * where a program that ships its own DLLs keeps them. */
    static char exedir[512];
    strlcpy(exedir, win_exe_path, sizeof exedir);
    char *slash = strrchr(exedir, '/');
    if (slash) *slash = 0; else strlcpy(exedir, ".", sizeof exedir);
    search_dirs[0] = exedir;

    for (int i = 0; i < (int)(sizeof search_dirs / sizeof search_dirs[0]); i++) {
        if (!search_dirs[i]) continue;
        snprintf(out, cap, "%s/%s", search_dirs[i], name);
        if (file_exists(out)) return true;
        /* LoadLibrary("foo") means foo.dll. */
        size_t n = strlen(name);
        if (n < 4 || strcasecmp(name + n - 4, ".dll")) {
            snprintf(out, cap, "%s/%s.dll", search_dirs[i], name);
            if (file_exists(out)) return true;
        }
    }
    return false;
}

static void apply_relocations(win_module_t *m, const opt64_t *opt, int64_t delta) {
    if (!delta || !opt->dirs[DIR_BASERELOC].size) return;
    uint32_t rva = opt->dirs[DIR_BASERELOC].rva;
    uint32_t end = rva + opt->dirs[DIR_BASERELOC].size;
    if (!in_image(m, rva, opt->dirs[DIR_BASERELOC].size)) {
        win_fail("%s: the relocation directory lies outside the image", m->name);
    }
    int applied = 0;
    while (rva < end) {
        uint32_t page = rd32(m->base + rva);
        uint32_t block = rd32(m->base + rva + 4);
        if (block < 8 || rva + block > end) break;
        uint32_t count = (block - 8) / 2;
        for (uint32_t i = 0; i < count; i++) {
            uint16_t entry = rd16(m->base + rva + 8 + i * 2);
            uint32_t type = entry >> 12, offset = entry & 0x0FFF;
            if (type == 0) continue;                          /* padding */
            if (type != 10) win_fail("%s: relocation type %u is not supported", m->name, type);
            if (!in_image(m, page + offset, 8)) continue;
            uint64_t *slot = (uint64_t *)(m->base + page + offset);
            *slot = (uint64_t)((int64_t)*slot + delta);
            applied++;
        }
        rva += block;
    }
    win_trace("%s: applied %d relocations (delta %+lld)", m->name, applied, (long long)delta);
}

/* A stub that names the function a program reached for, rather than faulting
 * at an address that means nothing.  Each unresolved import gets its own, so
 * the name is right even when several are missing. */
typedef struct { const char *dll, *name; } stub_info_t;
static stub_info_t stubs[512];
static int stub_count;

static void WINAPI report_missing(unsigned index);

/* One trampoline per unresolved import: it loads its own index and jumps to
 * the reporter.  Writing them out as bytes avoids needing one C function per
 * possible import. */
static uint8_t *stub_code;
/* Five bytes to load the index, ten to load the address, two to jump:
 * seventeen, rounded up so each stub starts on a sensible boundary. */
#define STUB_BYTES 24

static void *make_stub(const char *dll, const char *name) {
    if (stub_count >= (int)(sizeof stubs / sizeof stubs[0])) return (void *)report_missing;
    if (!stub_code) {
        long got = syscall6(SYS_MMAP, 0, (long)(sizeof stubs / sizeof stubs[0]) * STUB_BYTES,
                            4 /* executable */, 0, 0, 0);
        if (got < 0) return (void *)report_missing;
        stub_code = (uint8_t *)got;
    }
    int i = stub_count++;
    stubs[i].dll = dll;
    stubs[i].name = name;

    uint8_t *p = stub_code + (size_t)i * STUB_BYTES;
    uint64_t target = (uint64_t)(uintptr_t)report_missing;

    p[0] = 0xB9;                                      /* mov ecx, i        */
    p[1] = (uint8_t)i;
    p[2] = (uint8_t)(i >> 8);
    p[3] = (uint8_t)(i >> 16);
    p[4] = (uint8_t)(i >> 24);

    p[5] = 0x48; p[6] = 0xB8;                         /* movabs rax, target */
    for (int k = 0; k < 8; k++) p[7 + k] = (uint8_t)(target >> (8 * k));

    p[15] = 0xFF; p[16] = 0xE0;                       /* jmp rax            */
    return p;
}

/* The stub's "mov ecx, i" is the first argument under the Microsoft
 * convention, so the index arrives here as an ordinary parameter. */
static void WINAPI report_missing(unsigned index) {
    const char *dll = "a library", *name = "an unsupported function";
    if (index < (unsigned)stub_count) { dll = stubs[index].dll; name = stubs[index].name; }
    fprintf(STDERR_FD,
            "\nwinrun: the program called %s in %s, which is not implemented.\n", name, dll);
    flush_output();
    exit(78);
}

static void resolve_imports(win_module_t *m, const opt64_t *opt) {
    if (!opt->dirs[DIR_IMPORT].size) return;
    uint32_t rva = opt->dirs[DIR_IMPORT].rva;
    if (!in_image(m, rva, sizeof(import_desc_t)))
        win_fail("%s: the import directory lies outside the image", m->name);

    int resolved = 0, stubbed = 0;
    for (const import_desc_t *imp = at(m, rva); imp->name_rva; imp++) {
        if (!in_image(m, (uint32_t)((const uint8_t *)imp - m->base), sizeof *imp)) break;
        const char *dll = str_at(m, imp->name_rva);
        if (!dll) break;

        uint32_t lookup_rva = imp->lookup_rva ? imp->lookup_rva : imp->thunk_rva;
        if (!in_image(m, lookup_rva, 8) || !in_image(m, imp->thunk_rva, 8)) break;

        /* Loading the library it names may pull in more of them; that is the
         * recursion a program with its own DLLs depends on. */
        win_module_t *lib = pe_find_module(dll);
        if (!lib) lib = pe_load(dll, true);
        if (!lib)
            win_fail("%s needs %s, which is neither built in nor on the disk", m->name, dll);

        uint64_t *names = at(m, lookup_rva);
        uint64_t *slots = at(m, imp->thunk_rva);
        for (int i = 0; names[i]; i++) {
            const char *fname = NULL;
            uint16_t ordinal = 0;
            if (names[i] & (1ULL << 63)) {
                ordinal = (uint16_t)(names[i] & 0xFFFF);
            } else {
                uint32_t hint_rva = (uint32_t)(names[i] & 0x7FFFFFFF);
                fname = str_at(m, hint_rva + 2);
                if (!fname) break;
            }

            void *fn = pe_export_of(lib, fname, ordinal);
            if (!fn && fname && lib->table) {
                /* Windows spreads one function across several libraries and
                 * forwards between them; a name missing from the library it
                 * was imported from is looked for in the others. */
                for (win_module_t *other = modules; other && !fn; other = other->next)
                    if (other->table && other != lib) fn = pe_export_of(other, fname, 0);
            }
            if (fn) {
                slots[i] = (uint64_t)(uintptr_t)fn;
                resolved++;
            } else {
                /* Many imports are never called; a stub that reports the name
                 * if it ever is beats refusing to start. */
                static char kept[512][80];
                static int keptn;
                const char *nm = fname;
                if (fname && keptn < 512) {
                    strlcpy(kept[keptn], fname, sizeof kept[0]);
                    nm = kept[keptn++];
                } else if (!fname) {
                    nm = "an ordinal import";
                }
                slots[i] = (uint64_t)(uintptr_t)make_stub(lib->name, nm);
                stubbed++;
                win_trace("unresolved %s:%s", dll, fname ? fname : "(ordinal)");
            }
        }
    }
    win_trace("%s: %d imports resolved, %d stubbed", m->name, resolved, stubbed);
}

win_module_t *pe_load(const char *name, bool as_dll) {
    win_module_t *existing = pe_find_module(name);
    if (existing) { existing->refs++; return existing; }

    char path[512];
    if (!find_file(name, path, sizeof path)) {
        win_trace("%s was not found on the disk", name);
        return NULL;
    }

    kstat_t st;
    if (stat(path, &st) < 0) return NULL;
    if (st.size < 0x200 || st.size > (64u << 20)) {
        win_trace("%s is not a plausible image (%llu bytes)", path, (unsigned long long)st.size);
        return NULL;
    }

    uint8_t *file = malloc((size_t)st.size);
    if (!file) win_fail("out of memory reading %s", path);
    if (read_file(path, file, (size_t)st.size) != (ssize_t)st.size) {
        free(file);
        win_trace("%s could not be read in full", path);
        return NULL;
    }

    if (rd16(file) != IMAGE_DOS_SIGNATURE) {
        free(file);
        if (!as_dll) win_fail("%s is not a Windows executable (no MZ header)", path);
        return NULL;
    }
    uint32_t pe_off = rd32(file + 0x3C);
    if ((uint64_t)pe_off + 0x108 > st.size || rd32(file + pe_off) != IMAGE_NT_SIGNATURE) {
        free(file);
        if (!as_dll) win_fail("%s: the PE signature is missing", path);
        return NULL;
    }

    const uint8_t *coff = file + pe_off + 4;
    uint16_t machine = rd16(coff);
    uint16_t sections = rd16(coff + 2);
    uint16_t opt_size = rd16(coff + 16);
    const opt64_t *opt = (const opt64_t *)(coff + 20);

    if (opt->magic == PE32_MAGIC)
        win_fail("%s is a 32-bit image; this loader runs 64-bit code only", path);
    if (opt->magic != PE32PLUS_MAGIC)
        win_fail("%s: unrecognised optional header magic %#x", path, opt->magic);
    if (machine != MACHINE_AMD64)
        win_fail("%s is built for machine type %#x, not x86-64", path, machine);

    size_t image_size = opt->size_image;
    if (!image_size || image_size > (256u << 20))
        win_fail("%s: implausible image size %zu", path, image_size);

    win_module_t *m = calloc(1, sizeof *m);
    if (!m) win_fail("out of memory loading %s", path);
    strlcpy(m->name, strrchr(name, '/') ? strrchr(name, '/') + 1 : name, sizeof m->name);
    lower(m->name);
    if (!strchr(m->name, '.') && as_dll) strlcat(m->name, ".dll", sizeof m->name);
    strlcpy(m->path, path, sizeof m->path);
    m->size = image_size;
    m->refs = 1;

    /* The preferred address first: an image with no relocations can only run
     * where it was linked. */
    long got = syscall6(SYS_MMAP, (long)opt->image_base, (long)image_size, 4, 0, 0, 0);
    bool preferred = (got >= 0 && (uint64_t)got == opt->image_base);
    if (!preferred) {
        if (got >= 0) syscall6(SYS_MUNMAP, got, (long)image_size, 0, 0, 0, 0);
        got = syscall6(SYS_MMAP, 0, (long)image_size, 4, 0, 0, 0);
        if (got < 0) win_fail("could not reserve %zu bytes for %s", image_size, m->name);
        if (!opt->dirs[DIR_BASERELOC].size)
            win_fail("%s must load at %#llx, which is taken, and it has no relocations",
                     m->name, (unsigned long long)opt->image_base);
    }
    m->base = (uint8_t *)got;
    memset(m->base, 0, image_size);

    size_t header_bytes = opt->size_headers < st.size ? opt->size_headers : (size_t)st.size;
    memcpy(m->base, file, header_bytes);

    const section_t *sec = (const section_t *)((const uint8_t *)opt + opt_size);
    for (int i = 0; i < sections; i++) {
        const section_t *s = &sec[i];
        if ((uint64_t)s->virtual_address + s->virtual_size > image_size)
            win_fail("%s: section %.8s lies outside the image", m->name, s->name);
        if (s->raw_size && !(s->characteristics & SCN_UNINIT_DATA)) {
            if ((uint64_t)s->raw_offset + s->raw_size > st.size)
                win_fail("%s: section %.8s runs past the end of the file", m->name, s->name);
            uint32_t copy = s->raw_size < s->virtual_size ? s->raw_size : s->virtual_size;
            memcpy(m->base + s->virtual_address, file + s->raw_offset, copy);
        }
    }

    m->export_rva = opt->dirs[DIR_EXPORT].rva;
    m->export_size = opt->dirs[DIR_EXPORT].size;
    m->entry = opt->entry_rva ? (uint64_t)(uintptr_t)m->base + opt->entry_rva : 0;
    remember_resources(m, opt->dirs[DIR_RESOURCE].rva);

    /* Linked into the table before imports are resolved, so a pair of DLLs
     * that import from each other does not loop forever. */
    m->next = modules;
    modules = m;

    apply_relocations(m, opt, (int64_t)((uint64_t)m->base - opt->image_base));

    /* Thread-local storage: the index the program uses to find its block has
     * to be written before any code runs. */
    if (opt->dirs[DIR_TLS].size && in_image(m, opt->dirs[DIR_TLS].rva, sizeof(tls_dir_t))) {
        tls_dir_t *t = at(m, opt->dirs[DIR_TLS].rva);
        if (tls_count < (int)(sizeof module_tls / sizeof module_tls[0])) {
            tls_owner[tls_count] = m;
            module_tls[tls_count++] = t;
        }
        if (t->index_addr) *(uint32_t *)(uintptr_t)t->index_addr = 0;
    }

    resolve_imports(m, opt);
    free(file);

    win_trace("%s mapped at %p (%zu bytes)%s", m->name, m->base, m->size,
              preferred ? ", at its preferred address" : ", relocated");

    m->initialised = false;
    if (as_dll && m->entry) {
        /* DllMain runs now, which is where a DLL sets itself up.  A DLL that
         * returns false from it has refused to load. */
        typedef BOOL WINAPI (*dllmain_fn)(void *, DWORD, void *);
        pe_run_tls_callbacks(m, DLL_PROCESS_ATTACH);
        BOOL ok = ((dllmain_fn)(uintptr_t)m->entry)(m->base, DLL_PROCESS_ATTACH, NULL);
        if (!ok) win_trace("%s: DllMain refused to initialise", m->name);
    }
    m->initialised = true;
    return m;
}

uint64_t pe_entry(win_module_t *m) { return m ? m->entry : 0; }

/* The subsystem field says whether the program expects a console or a window,
 * which decides whether the GUI has to be brought up before it starts. */
bool pe_is_gui(win_module_t *m) {
    if (!m || !m->base) return false;
    uint32_t pe_off = rd32(m->base + 0x3C);
    const opt64_t *opt = (const opt64_t *)(m->base + pe_off + 4 + 20);
    return opt->subsystem == SUBSYSTEM_GUI;
}

/* The exception directory, which structured exception handling walks to find
 * the handler for a faulting address. */
bool pe_exception_range(win_module_t *m, uint32_t *rva, uint32_t *size) {
    if (!m || !m->base) return false;
    uint32_t pe_off = rd32(m->base + 0x3C);
    const opt64_t *opt = (const opt64_t *)(m->base + pe_off + 4 + 20);
    if (!opt->dirs[DIR_EXCEPTION].size) return false;
    *rva = opt->dirs[DIR_EXCEPTION].rva;
    *size = opt->dirs[DIR_EXCEPTION].size;
    return true;
}

/* Which module an address belongs to, which is the first question every
 * exception and every GetModuleHandle-from-a-pointer has to answer. */
win_module_t *pe_module_for(uint64_t address) {
    for (win_module_t *m = modules; m; m = m->next) {
        if (!m->base) continue;
        uint64_t lo = (uint64_t)(uintptr_t)m->base;
        if (address >= lo && address < lo + m->size) return m;
    }
    return NULL;
}

win_module_t *pe_module_list(void) { return modules; }
