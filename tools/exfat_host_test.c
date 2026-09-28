/* exfat_host_test.c - run the exFAT reader against a real volume.
 *
 * The same arrangement as ntfs_host_test.c: the driver runs unchanged on the
 * machine doing the building, against an image on disk, so a misread field is
 * found here rather than on a machine with no way to report it.
 *
 * The image the build makes is written from the on-disk layout rather than
 * from this reader, so the two are independent.  Point it at a volume Windows
 * formatted instead and the same checks apply - that is the stronger test and
 * the one to run when there is an image to run it on.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/exfat.h"

static FILE *image;
static unsigned long long base_offset;

static bool image_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx;
#if defined(_WIN32)
    if (_fseeki64(image, (long long)(base_offset + lba * 512ull), SEEK_SET)) return false;
#else
    if (fseeko(image, (off_t)(base_offset + lba * 512ull), SEEK_SET)) return false;
#endif
    return fread(buf, 512, count, image) == count;
}

static int failures;

static void check(int ok, const char *what) {
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

/* The patterns the builder filled the two files with. */
static unsigned char expected(unsigned long long at, int seed) {
    return (unsigned char)((at * 7 + seed) & 0xFF);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <image> [byte-offset]\n", argv[0]);
        return 2;
    }
    image = fopen(argv[1], "rb");
    if (!image) { perror(argv[1]); return 1; }
    base_offset = (argc > 2) ? strtoull(argv[2], NULL, 0) : 0;

    printf("arithmetic that needs no disk:\n");
    int arith = exfat_selftest();
    check(arith == 0, "the checksums and the name hash are right");

    exfat_volume_t v;
    if (!exfat_mount(&v, NULL, image_read)) {
        printf("mount failed: %s\n", v.error);
        return 1;
    }

    printf("\nmounted: %u bytes/sector, %u sectors/cluster (%u bytes), "
           "%u clusters, root at %u\n",
           v.bytes_per_sector, v.sectors_per_cluster, v.bytes_per_cluster,
           v.cluster_count, v.root_cluster);

    printf("\nthe root directory:\n");
    exfat_file_t root;
    check(exfat_lookup(&v, "/", &root) && root.directory, "the root opens");

    for (uint32_t i = 0; ; i++) {
        char name[512];
        exfat_file_t e;
        if (!exfat_readdir(&v, &root, i, name, sizeof name, &e)) break;
        printf("    %-24s %10llu bytes  %s%s\n", name,
               (unsigned long long)e.size,
               e.directory ? "directory" : "file",
               e.contiguous ? ", contiguous" : ", chained");
    }

    printf("\nreading:\n");

    /* The chained file, whose clusters are 5 -> 8 -> 6.  A reader that
     * assumes they run on gets the second and third the wrong way round, and
     * only the middle of the file is wrong - which is why this is checked
     * byte for byte rather than by length. */
    {
        exfat_file_t f;
        check(exfat_lookup(&v, "/CHAINED.BIN", &f), "a chained file is found");
        check(!f.contiguous, "it is marked as following the allocation table");

        static unsigned char buf[64 * 1024];
        long got = exfat_read_file(&v, &f, 0, buf, (uint32_t)f.size);
        check(got == (long)f.size, "all of it reads back");

        int same = 1;
        for (long i = 0; i < got; i++)
            if (buf[i] != expected((unsigned long long)i, 3)) { same = 0; break; }
        check(same, "every byte is the byte that was written, across three clusters");
    }

    /* The contiguous one, which the allocation table says nothing about. */
    {
        exfat_file_t f;
        check(exfat_lookup(&v, "/contiguous file.txt", &f),
              "a contiguous file with spaces in its name is found");
        check(f.contiguous, "it is marked as not needing the allocation table");

        static unsigned char buf[64 * 1024];
        long got = exfat_read_file(&v, &f, 0, buf, (uint32_t)f.size);
        check(got == (long)f.size, "all of it reads back");

        int same = 1;
        for (long i = 0; i < got; i++)
            if (buf[i] != expected((unsigned long long)i, 11)) { same = 0; break; }
        check(same, "every byte of it is right too");
    }

    /* Case folding, and a file inside a subdirectory. */
    {
        exfat_file_t f;
        check(exfat_lookup(&v, "/chained.bin", &f),
              "a name typed in the wrong case still finds the file");
        check(exfat_lookup(&v, "/SUB/INSIDE.TXT", &f),
              "a file inside a subdirectory is found");
        check(f.size == 40, "and it is the size it should be");

        unsigned char buf[64];
        long got = exfat_read_file(&v, &f, 0, buf, 40);
        int same = got == 40;
        for (long i = 0; i < got && same; i++)
            if (buf[i] != expected((unsigned long long)i, 200)) same = 0;
        check(same, "its bytes are right");
    }

    /* And the refusals. */
    {
        exfat_file_t f;
        check(!exfat_lookup(&v, "/NOSUCH.TXT", &f),
              "a name that is not there is not found");
        check(!exfat_lookup(&v, "/CHAINED.BIN/x", &f),
              "descending into a file rather than a directory is refused");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
