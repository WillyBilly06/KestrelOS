/* xfs_host_test.c - run the XFS reader against a volume.
 *
 * The same arrangement as the NTFS, exFAT and ext4 host tests.  What this one
 * has to cover that they do not is that XFS is big-endian and its inode
 * numbers are three packed fields rather than an index - two ways of being
 * subtly wrong that read plausible numbers rather than failing.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/xfs.h"

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
    check(xfs_selftest() == 0,
          "extents unpack correctly, including across the two halves");

    xfs_volume_t v;
    if (!xfs_mount(&v, NULL, image_read)) {
        printf("mount failed: %s\n", v.error);
        return 1;
    }

    printf("\nmounted: %u-byte blocks, %u group(s) of %u, %u-byte inodes, "
           "root inode %llu%s%s\n",
           v.block_size, v.ag_count, v.ag_blocks, v.inode_size,
           (unsigned long long)v.root_inode,
           v.version_5 ? ", v5" : ", v4",
           v.has_file_type ? ", entries carry a type" : "");

    printf("\nthe root directory:\n");
    xfs_file_t root;
    check(xfs_lookup(&v, "/", &root) && root.directory, "the root opens");

    for (uint32_t i = 0; ; i++) {
        char name[256];
        xfs_file_t e;
        if (!xfs_readdir(&v, &root, i, name, sizeof name, &e)) break;
        printf("    %-14s inode %-6llu %8llu bytes  %s\n", name,
               (unsigned long long)e.inode, (unsigned long long)e.size,
               e.directory ? "directory" : "file");
    }

    printf("\nreading:\n");
    {
        xfs_file_t f;
        check(xfs_lookup(&v, "/hello.txt", &f), "a file is found by name");
        check(!f.directory && f.size == 100, "it is a file of the right size");

        unsigned char buf[256];
        long got = xfs_read_file(&v, &f, 0, buf, (uint32_t)f.size);
        check(got == (long)f.size, "all of it reads back");

        int same = 1;
        for (long i = 0; i < got; i++)
            if (buf[i] != expected((unsigned long long)i, 5)) { same = 0; break; }
        check(same, "every byte is the byte that was written");
    }

    /* A directory inside a directory, which exercises the inode-number
     * arithmetic a second time from a different starting point. */
    {
        xfs_file_t f;
        check(xfs_lookup(&v, "/sub", &f) && f.directory, "a subdirectory is found");
        check(xfs_lookup(&v, "/sub/inside.txt", &f), "a file inside it is found");
        check(f.size == 60, "and it is the size it should be");

        unsigned char buf[128];
        long got = xfs_read_file(&v, &f, 0, buf, 60);
        int same = got == 60;
        for (long i = 0; i < got && same; i++)
            if (buf[i] != expected((unsigned long long)i, 90)) same = 0;
        check(same, "its bytes are right");
    }

    {
        xfs_file_t f;
        check(!xfs_lookup(&v, "/nosuch", &f), "a name that is not there is not found");
        check(!xfs_lookup(&v, "/HELLO.TXT", &f),
              "a name in the wrong case is not found - this is a Unix filesystem");
        check(!xfs_lookup(&v, "/hello.txt/x", &f),
              "descending into a file rather than a directory is refused");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
