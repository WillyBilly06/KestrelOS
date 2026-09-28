/* ntfs_host_test.c - run the NTFS reader and writer against a real filesystem.
 *
 * Neither the loader nor the kernel is a convenient place to find out that a
 * run list is misread, and a writer that is wrong there damages a volume
 * Windows also has to mount.  This runs exactly the same code on the machine
 * doing the building, against an image that Windows formatted and Windows
 * wrote the files into - so what it proves is that the format is understood,
 * not that this agrees with an idea of the format written at the same time as
 * itself.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/ntfs_core.h"

extern void (*ntfs_trace)(const char *, unsigned long long, unsigned long long);
static void trace(const char *w, unsigned long long a, unsigned long long b) {
    printf("    [%s %llu %llu]\n", w, a, b);
}

static FILE *image;
static unsigned long long base_offset;

static int seek_to(uint64_t lba) {
#if defined(_WIN32)
    return _fseeki64(image, (long long)(base_offset + lba * 512ull), SEEK_SET);
#else
    return fseeko(image, (off_t)(base_offset + lba * 512ull), SEEK_SET);
#endif
}

static bool image_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx;
    if (seek_to(lba) != 0) return false;
    return fread(buf, 512, count, image) == count;
}

static bool image_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    (void)ctx;
    if (seek_to(lba) != 0) return false;
    return fwrite(buf, 512, count, image) == count;
}

static void dump(ntfs_volume_t *v, const char *path, int depth) {
    ntfs_file_t dir;
    if (!ntfs_lookup(v, path, &dir) || !dir.directory) return;

    for (uint32_t i = 0; i < 4096; i++) {
        char name[512];
        ntfs_file_t e;
        if (!ntfs_readdir(v, &dir, i, name, sizeof name, &e)) break;
        if (name[0] == '$') continue;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;

        for (int d = 0; d < depth; d++) printf("  ");
        if (e.directory) {
            printf("%s/\n", name);
            char sub[1024];
            snprintf(sub, sizeof sub, "%s%s%s", path,
                     path[strlen(path) - 1] == '/' ? "" : "/", name);
            if (depth < 3) dump(v, sub, depth + 1);
        } else {
            printf("%-40s %10llu bytes%s%s\n", name,
                   (unsigned long long)e.size,
                   e.resident ? "  (inside its record)" : "",
                   e.unreadable ? "  (compressed or encrypted)" : "");
        }
    }
}

/* The pattern the 3 MB file was filled with when the image was made. */
static unsigned char expected_byte(unsigned long long at) {
    return (unsigned char)(at % 251);
}

static int run_write_test(ntfs_volume_t *v, const char *path) {
    ntfs_file_t f;
    printf("\nwriting into %s\n", path);
    if (!ntfs_lookup(v, path, &f)) { printf("  not found\n"); return 1; }
    if (f.resident) {
        printf("  this file lives inside its own record, which is not written\n");
        return 1;
    }

    enum { AT = 4096, N = 8192 };
    static unsigned char out[N], back[N], edge[256];
    for (int i = 0; i < N; i++) out[i] = (unsigned char)(0xA0 + (i % 7));

    long put = ntfs_write_file(v, &f, AT, out, N);
    printf("  wrote %ld byte(s) at offset %d\n", put, AT);
    if (put != N) return 1;

    /* Read it straight back, which catches a write that reported success and
     * went somewhere else entirely. */
    long got = ntfs_read_file(v, &f, AT, back, N);
    int same = (got == N) && !memcmp(out, back, N);
    printf("  read back %ld byte(s): %s\n", got, same ? "identical" : "DIFFERENT");

    /* And the bytes on either side, which is what a write that is off by a
     * cluster damages while still looking correct where it was aimed. */
    int before_ok = 1, after_ok = 1;
    ntfs_read_file(v, &f, AT - (long)sizeof edge, edge, (uint32_t)sizeof edge);
    for (size_t i = 0; i < sizeof edge; i++)
        if (edge[i] != expected_byte(AT - sizeof edge + i)) before_ok = 0;

    ntfs_read_file(v, &f, AT + N, edge, (uint32_t)sizeof edge);
    for (size_t i = 0; i < sizeof edge; i++)
        if (edge[i] != expected_byte(AT + N + i)) after_ok = 0;

    printf("  neighbouring bytes before: %s, after: %s\n",
           before_ok ? "untouched" : "DAMAGED",
           after_ok ? "untouched" : "DAMAGED");

    /* A write that is not aligned to a cluster, which has to read the cluster,
     * change part of it and put it back.  Getting that wrong overwrites the
     * bytes around it with whatever happened to be in the buffer. */
    static unsigned char small[100];
    for (int i = 0; i < 100; i++) small[i] = (unsigned char)(0x5A + i);

    long put2 = ntfs_write_file(v, &f, 70000, small, (uint32_t)sizeof small);
    long got2 = ntfs_read_file(v, &f, 70000, back, (uint32_t)sizeof small);
    int small_ok = (put2 == (long)sizeof small) && (got2 == (long)sizeof small) &&
                   !memcmp(small, back, sizeof small);

    int around_ok = 1;
    ntfs_read_file(v, &f, 70000 - 64, edge, 64);
    for (int i = 0; i < 64; i++)
        if (edge[i] != expected_byte(70000 - 64 + i)) around_ok = 0;
    ntfs_read_file(v, &f, 70000 + sizeof small, edge, 64);
    for (int i = 0; i < 64; i++)
        if (edge[i] != expected_byte(70000 + sizeof small + i)) around_ok = 0;

    printf("  a 100-byte write inside one cluster: %s, its neighbours %s\n",
           small_ok ? "correct" : "WRONG", around_ok ? "untouched" : "DAMAGED");

    fflush(image);
    return (same && before_ok && after_ok && small_ok && around_ok) ? 0 : 1;
}


/* ---------------------------------------------------------- creating files
 *
 * What this has to prove is not "the call returned true".  It is that the
 * structures written agree with each other well enough that a reader which
 * knows nothing about how they were made can find and read the result - and,
 * separately, that Windows agrees, which is what chkdsk is for afterwards.
 *
 * The order of the cases below is deliberate: each one exercises a path the
 * previous one did not, and the later ones are the ones most likely to be
 * wrong.
 */
static int creates_failed;

static void check(int ok, const char *what) {
    printf("  %-56s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) creates_failed++;
}

static int run_create_test(ntfs_volume_t *v) {
    char name[128];
    ntfs_file_t f;

    printf("\ncreating:\n");

    /* 1. A directory in the root.  The root of this volume has far more
     *    entries than fit in its record, so its index is a tree and this
     *    lands in an allocated block rather than in the record - which is the
     *    path that matters and the one a small test volume never reaches. */
    check(ntfs_create(v, "/KESTRELOS", true, &f), "make a directory in the root");
    check(ntfs_lookup(v, "/KESTRELOS", &f) && f.directory,
          "find it again by name");

    /* 2. A file inside it.  A fresh directory's index IS resident, so this is
     *    the other insertion path - growing a resident attribute inside a
     *    record, which is the one that was written in the wrong order. */
    check(ntfs_create(v, "/KESTRELOS/KERNEL.LOG", false, &f),
          "make a file in that directory");

    /* 3. Grow it past what fits inside a record, which has to allocate
     *    clusters and move the attribute out of the record entirely. */
    static unsigned char pattern[200000];
    for (size_t i = 0; i < sizeof pattern; i++)
        pattern[i] = (unsigned char)(i * 31 + (i >> 11));

    check(ntfs_lookup(v, "/KESTRELOS/KERNEL.LOG", &f), "look it up before growing");
    check(ntfs_resize(v, &f, sizeof pattern), "give it 200000 bytes");
    check(!f.resident, "it is no longer stored inside its record");

    long put = ntfs_write_file(v, &f, 0, pattern, (uint32_t)sizeof pattern);
    check(put == (long)sizeof pattern, "write 200000 bytes into it");

    /* Re-look-up: this is the part that matters.  It goes back through the
     * directory index and the record from scratch, so if either disagrees
     * with what was written, the data does not come back. */
    static unsigned char back[sizeof pattern];
    check(ntfs_lookup(v, "/KESTRELOS/KERNEL.LOG", &f), "find it again after growing");
    check(f.size == sizeof pattern, "it reports the size it was given");
    long got = ntfs_read_file(v, &f, 0, back, (uint32_t)sizeof pattern);
    check(got == (long)sizeof pattern, "read it all back");
    check(memcmp(pattern, back, sizeof pattern) == 0,
          "every byte read back is the byte written");

    /* 4. Enough files to push the new directory's index out of its record.
     *    This is where the driver is expected to refuse rather than split, so
     *    what is checked is that it refuses cleanly and that everything it
     *    did manage to create is still readable afterwards. */
    int made = 0;
    for (int i = 0; i < 2000; i++) {
        snprintf(name, sizeof name, "/KESTRELOS/FILE%03d.TXT", i);
        v->error[0] = 0;
        if (!ntfs_create(v, name, false, &f)) {
            printf("  stopped at %s: %s\n", name, v->error);
            break;
        }
        made++;
        if (v->error[0]) printf("    [%d] %s\n", i, v->error);
    }
    printf("  %-56s %d\n", "files created before the index filled up", made);
    check(made >= 1500, "a directory holds a useful number of files");

    int all_found = 1;
    int missing = 0;
    for (int i = 0; i < made; i++) {
        snprintf(name, sizeof name, "/KESTRELOS/FILE%03d.TXT", i);
        if (!ntfs_lookup(v, name, &f)) {
            all_found = 0;
            if (missing < 12) printf("    missing: %s\n", name);
            missing++;
        }
    }
    if (missing) printf("    %d of %d could not be found again\n", missing, made);
    check(all_found, "every one of them can be found again by name");

    /* 5. And the big file is still intact after all that index churn. */
    check(ntfs_lookup(v, "/KESTRELOS/KERNEL.LOG", &f) && f.size == sizeof pattern,
          "the large file survived the directory filling up");

    /* 6. Refusals that must stay refusals. */
    check(!ntfs_create(v, "/KESTRELOS/KERNEL.LOG", false, &f),
          "creating a name that already exists is refused");
    check(!ntfs_create(v, "/NOSUCHDIR/x.txt", false, &f),
          "creating in a directory that is not there is refused");

    fflush(image);
    printf("\n%d check(s) failed\n", creates_failed);
    return creates_failed ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <image> [byte-offset] [file]\n", argv[0]);
        return 2;
    }

    int writing = getenv("NTFS_WRITE") != NULL;
    image = fopen(argv[1], writing ? "r+b" : "rb");
    if (!image) { perror(argv[1]); return 1; }
    base_offset = (argc > 2) ? strtoull(argv[2], NULL, 0) : 0;

    if (getenv("NTFS_TRACE")) ntfs_trace = trace;

    ntfs_volume_t v;
    bool ok = writing ? ntfs_mount_rw(&v, NULL, image_read, image_write)
                      : ntfs_mount(&v, NULL, image_read);
    if (!ok) {
        printf("mount failed: %s\n", v.error);
        return 1;
    }

    printf("mounted: %u bytes/sector, %u sectors/cluster (%u bytes), "
           "%u-byte records, MFT at cluster %llu%s\n",
           v.bytes_per_sector, v.sectors_per_cluster, v.bytes_per_cluster,
           v.record_bytes, (unsigned long long)v.mft_lcn,
           writing ? ", writable" : "");

    if (getenv("NTFS_CREATE"))
        return run_create_test(&v);

    if (writing)
        return run_write_test(&v, argc > 3 ? argv[3] : "/KESTREL/big.bin");

    printf("\nroot directory:\n");
    dump(&v, "/", 1);

    if (argc > 3) {
        ntfs_file_t f;
        printf("\nreading %s\n", argv[3]);
        if (!ntfs_lookup(&v, argv[3], &f)) { printf("  not found\n"); return 1; }
        printf("  %llu bytes, %s\n", (unsigned long long)f.size,
               f.resident ? "resident" : "in run list");

        static unsigned char buf[1 << 20];
        uint32_t want = f.size < sizeof buf ? (uint32_t)f.size : (uint32_t)sizeof buf;
        long got = ntfs_read_file(&v, &f, 0, buf, want);
        printf("  read %ld byte(s)\n", got);
        if (got > 0) {
            printf("  first bytes: ");
            for (long i = 0; i < (got < 24 ? got : 24); i++) printf("%02x ", buf[i]);
            printf("\n");
        }
    }
    return 0;
}
