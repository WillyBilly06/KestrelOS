/* ext4_host_test.c - run the ext2/3/4 reader against a real volume.
 *
 * The same arrangement as the NTFS and exFAT host tests.  What this one has to
 * cover that they do not is that ext has two entirely different ways of
 * finding a file's blocks - the ext4 extent tree and the older indirect chain
 * - and a reader can easily have one right and the other wrong.  So the image
 * contains one file of each kind and both are checked byte for byte.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/ext4.h"

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

static int whole_file_matches(ext4_volume_t *v, ext4_file_t *f, int seed) {
    static unsigned char buf[64 * 1024];
    if (f->size > sizeof buf) return 0;
    long got = ext4_read_file(v, f, 0, buf, (uint32_t)f->size);
    if (got != (long)f->size) return 0;
    for (long i = 0; i < got; i++)
        if (buf[i] != expected((unsigned long long)i, seed)) return 0;
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <image> [byte-offset]\n", argv[0]);
        return 2;
    }
    image = fopen(argv[1], "rb");
    if (!image) { perror(argv[1]); return 1; }
    base_offset = (argc > 2) ? strtoull(argv[2], NULL, 0) : 0;

    ext4_volume_t v;
    if (!ext4_mount(&v, NULL, image_read)) {
        printf("mount failed: %s\n", v.error);
        return 1;
    }

    printf("mounted: %u-byte blocks, %llu of them, %u inodes per group, "
           "%u-byte inodes%s\n",
           v.block_size, (unsigned long long)v.block_count,
           v.inodes_per_group, v.inode_size,
           v.sixty_four_bit ? ", 64-bit" : "");

    printf("\nthe root directory:\n");
    ext4_file_t root;
    check(ext4_lookup(&v, "/", &root) && root.directory, "the root opens");

    for (uint32_t i = 0; ; i++) {
        char name[256];
        ext4_file_t e;
        if (!ext4_readdir(&v, &root, i, name, sizeof name, &e)) break;
        printf("    %-16s %8llu bytes  inode %-4u %s%s\n", name,
               (unsigned long long)e.size, e.inode,
               e.directory ? "directory" : "file",
               e.extents ? ", extent tree" : ", indirect");
    }

    printf("\nreading:\n");

    /* The ext4 way: a small B-tree in the inode mapping one range of the file
     * onto one range of the disk. */
    {
        ext4_file_t f;
        check(ext4_lookup(&v, "/extents.bin", &f), "a file with an extent tree is found");
        check(f.extents, "it is marked as using extents");
        check(whole_file_matches(&v, &f, 5),
              "every byte of it reads back, across three blocks");
    }

    /* The older way: twelve blocks in the inode and then a block of pointers.
     * The thirteenth block is the one that catches a reader that stops at the
     * direct blocks - the first twelve would be right and only the tail
     * wrong. */
    {
        ext4_file_t f;
        check(ext4_lookup(&v, "/indirect.bin", &f), "a file with an indirect chain is found");
        check(!f.extents, "it is marked as not using extents");
        check(f.size > 12 * v.block_size,
              "it is longer than the twelve blocks the inode holds directly");
        check(whole_file_matches(&v, &f, 9),
              "every byte of it reads back, including past the indirect block");
    }

    /* Subdirectories, and case sensitivity - which ext has and the other two
     * filesystems here do not. */
    {
        ext4_file_t f;
        check(ext4_lookup(&v, "/sub", &f) && f.directory, "a subdirectory is found");
        check(ext4_lookup(&v, "/sub/inside.txt", &f), "a file inside it is found");
        check(f.size == 500, "and it is the size it should be");
        check(whole_file_matches(&v, &f, 200), "its bytes are right");

        check(!ext4_lookup(&v, "/SUB/INSIDE.TXT", &f),
              "a name in the wrong case is NOT found - this is a Unix filesystem");
    }

    /* Refusals. */
    {
        ext4_file_t f;
        check(!ext4_lookup(&v, "/nosuch", &f), "a name that is not there is not found");
        check(!ext4_lookup(&v, "/extents.bin/x", &f),
              "descending into a file rather than a directory is refused");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
