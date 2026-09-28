/* btrfs_host_test.c - run the Btrfs reader against a volume.
 *
 * The same arrangement as the other four host tests.  What this one has to
 * cover that they do not is the bootstrap: the map from logical addresses to
 * real ones lives inside the filesystem it describes, so the superblock
 * carries just enough of it to reach the rest.  A reader that gets that wrong
 * does not fail - it reads a plausible wrong part of the disk.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/btrfs.h"

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
    check(btrfs_selftest() == 0,
          "logical addresses map to real ones, and outside a chunk is refused");

    btrfs_volume_t v;
    if (!btrfs_mount(&v, NULL, image_read)) {
        printf("mount failed: %s\n", v.error);
        return 1;
    }

    printf("\nmounted: %u-byte sectors, %u-byte nodes, %d chunk(s) mapped\n",
           v.sector_size, v.node_size, v.chunk_count);
    printf("         root tree %#llx, chunk tree %#llx, filesystem tree %#llx\n",
           (unsigned long long)v.root_tree, (unsigned long long)v.chunk_tree,
           (unsigned long long)v.fs_tree);

    check(v.fs_tree != 0, "the filesystem tree was found through the root tree");

    printf("\nthe root directory:\n");
    btrfs_file_t root;
    check(btrfs_lookup(&v, "/", &root) && root.directory, "the root opens");

    for (uint32_t i = 0; ; i++) {
        char name[256];
        btrfs_file_t e;
        if (!btrfs_readdir(&v, &root, i, name, sizeof name, &e)) break;
        printf("    %-14s inode %-6llu %8llu bytes  %s\n", name,
               (unsigned long long)e.objectid, (unsigned long long)e.size,
               e.directory ? "directory" : "file");
    }

    printf("\nreading:\n");
    {
        btrfs_file_t f;
        check(btrfs_lookup(&v, "/hello.txt", &f), "a file is found by name");
        check(!f.directory && f.size == 5000, "it is a file of the right size");

        static unsigned char buf[8192];
        long got = btrfs_read_file(&v, &f, 0, buf, (uint32_t)f.size);
        check(got == (long)f.size, "all of it reads back");

        /* Longer than one sector on purpose: the extent gives a logical
         * address, and every sector of it has to be mapped again. */
        int same = 1;
        for (long i = 0; i < got; i++)
            if (buf[i] != expected((unsigned long long)i, 5)) { same = 0; break; }
        check(same, "every byte is right, across more than one sector");
    }

    {
        btrfs_file_t f;
        check(btrfs_lookup(&v, "/sub", &f) && f.directory, "a subdirectory is found");
        check(btrfs_lookup(&v, "/sub/inside.txt", &f), "a file inside it is found");
        check(f.size == 40, "and it is the size it should be");

        unsigned char buf[64];
        long got = btrfs_read_file(&v, &f, 0, buf, 40);
        int same = got == 40;
        for (long i = 0; i < got && same; i++)
            if (buf[i] != expected((unsigned long long)i, 77)) same = 0;
        check(same, "its bytes are right");
    }

    {
        btrfs_file_t f;
        check(!btrfs_lookup(&v, "/nosuch", &f), "a name that is not there is not found");
        check(!btrfs_lookup(&v, "/HELLO.TXT", &f),
              "a name in the wrong case is not found");
        check(!btrfs_lookup(&v, "/hello.txt/x", &f),
              "descending into a file rather than a directory is refused");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
