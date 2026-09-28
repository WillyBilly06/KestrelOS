/* runarm - run a program built for ARM.
 *
 *   runarm --selftest        check the interpreter against a real compiler
 *   runarm <file>            load an AArch64 executable and run it
 *
 * This machine is x86-64.  A program built for ARM is not a different version
 * of the same thing - it is a different instruction set, and the only way to
 * run one is to read its instructions and do what they say, which is what
 * user/libarm does.
 *
 * This is the rest of what makes that a program rather than a calculation:
 * reading an executable file, laying its parts out in memory where it expects
 * to find them, giving it a stack, and answering the requests it makes of the
 * system while it runs.  From inside, it is running on ARM.
 */
#include "kestrel.h"
#include "arm64.h"

/* The parts of an executable that matter here.  An ELF file begins with a
 * header saying what kind of thing it is and where the table of loadable
 * pieces starts; each entry in that table says where a piece lives in the file
 * and where it wants to be in memory. */
#define ELF_MAGIC     "\177ELF"
#define ELF_64BIT     2
#define ELF_LITTLE    1
#define ELF_EXECUTABLE 2
#define ELF_AARCH64   183
#define SEGMENT_LOAD  1

typedef struct {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf_header_t;

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} elf_segment_t;

/* Room above the program for a stack, and a little beyond that so a program
 * that asks for more memory has somewhere for it to come from. */
#define STACK_BYTES   (256 * 1024)
#define SPARE_BYTES   (256 * 1024)

/* How the program finished, kept because once it has gone there is nothing
 * left to ask. */
static struct {
    int  status;
    bool finished;
    int  refused;
    long last_refused;
    int  wrote;                 /* requests to write that were carried out */
    int  out_of_range;          /* and ones pointing outside the program   */
    int  opened;                /* files it asked for and got              */
} outcome;

/* Where the program's own memory ends and the space it can ask for begins.
 * A program with no library underneath it still wants somewhere to put things
 * that outlive a function, and this is where that comes from: a line that
 * moves up when the program asks for more and never comes back down, which is
 * the oldest arrangement there is and the one a small program expects. */
static struct {
    uint64_t current;
    uint64_t limit;
} heap;

/* The system underneath the program.
 *
 * A program built for this architecture by an ordinary toolchain makes its
 * requests the way Linux does - the number in x8, the arguments in x0 onwards,
 * the answer back in x0 - because that is what the toolchain builds it to
 * expect.  Nothing here pretends to be Linux.  It answers the requests a
 * program cannot do without and tells the program plainly when it is asked for
 * something else, which is what lets a program fail in its own way rather than
 * running into nothing.
 */
static bool answer(arm64_cpu_t *cpu, uint32_t what, void *ctx) {
    (void)what; (void)ctx;
    long number = (long)cpu->x[8];

    /* An address the program gives has to be inside the memory it was given;
     * a program is entitled to get this wrong and must not be able to reach
     * anything of ours when it does. */
    #define IN_PROGRAM(at, len) \
        ((at) >= cpu->memory_base && (len) >= 0 && \
         (at) + (uint64_t)(len) <= cpu->memory_base + cpu->memory_size)
    #define POINTER(at) ((char *)cpu->memory + ((at) - cpu->memory_base))

    switch (number) {
    case 64: {                                  /* write */
        uint64_t at = cpu->x[1];
        long len = (long)cpu->x[2];
        if (!IN_PROGRAM(at, len)) {
            outcome.out_of_range++;
            cpu->x[0] = (uint64_t)-14;
            return true;
        }
        ssize_t n = write((int)cpu->x[0], POINTER(at), (size_t)len);
        outcome.wrote++;
        cpu->x[0] = (uint64_t)(long)n;
        return true;
    }
    case 63: {                                  /* read */
        uint64_t at = cpu->x[1];
        long len = (long)cpu->x[2];
        if (!IN_PROGRAM(at, len)) { cpu->x[0] = (uint64_t)-14; return true; }
        ssize_t n = read((int)cpu->x[0], POINTER(at), (size_t)len);
        cpu->x[0] = (uint64_t)(long)n;
        return true;
    }
    case 56: {                                  /* open, relative to nothing */
        uint64_t at = cpu->x[1];
        if (!IN_PROGRAM(at, 1)) { cpu->x[0] = (uint64_t)-14; return true; }
        /* The name is in the program's memory and might not end where it
         * should, so it is copied out with a limit rather than trusted. */
        char path[256];
        const char *from = POINTER(at);
        uint64_t room = cpu->memory_base + cpu->memory_size - at;
        size_t n = 0;
        while (n < sizeof path - 1 && n < room && from[n]) { path[n] = from[n]; n++; }
        path[n] = 0;
        if (n && from[n]) { cpu->x[0] = (uint64_t)-36; return true; }

        /* Only the read-only forms.  A program that wants to write is asking
         * for something this has not been thought through for. */
        long flags = (long)cpu->x[2];
        if (flags & ~0ll & 3) { cpu->x[0] = (uint64_t)-13; return true; }
        int fd = open(path, O_RDONLY);
        if (fd >= 0) outcome.opened++;
        cpu->x[0] = (uint64_t)(long)(fd < 0 ? -2 : fd);
        return true;
    }
    case 57:                                    /* close */
        cpu->x[0] = (uint64_t)(long)close((int)cpu->x[0]);
        return true;

    case 214: {                                 /* move the line */
        uint64_t asked = cpu->x[0];
        if (asked == 0) { cpu->x[0] = heap.current; return true; }
        if (asked < heap.current || asked > heap.limit) {
            cpu->x[0] = heap.current;           /* refused, and says so by
                                                 * not having moved */
            return true;
        }
        heap.current = asked;
        cpu->x[0] = heap.current;
        return true;
    }

    case 93:                                    /* exit          */
    case 94:                                    /* exit_group    */
        outcome.status = (int)(cpu->x[0] & 0xFF);
        outcome.finished = true;
        return false;                           /* the program has ended */

    default:
        outcome.refused++;
        outcome.last_refused = number;
        cpu->x[0] = (uint64_t)-38;              /* no such request */
        return true;
    }
    #undef IN_PROGRAM
    #undef POINTER
}

static int run_file(const char *path, bool quiet) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(STDERR_FD, "runarm: cannot open %s: %s\n", path,
                strerror(errno));
        return 2;
    }

    static uint8_t file[512 * 1024];
    ssize_t got = read(fd, file, sizeof file);
    close(fd);
    if (got < (ssize_t)sizeof(elf_header_t)) {
        fprintf(STDERR_FD, "runarm: %s is too small to be a program\n", path);
        return 2;
    }

    const elf_header_t *head = (const elf_header_t *)file;
    if (memcmp(head->ident, ELF_MAGIC, 4) != 0) {
        fprintf(STDERR_FD, "runarm: %s is not an executable\n", path);
        return 2;
    }
    if (head->ident[4] != ELF_64BIT || head->ident[5] != ELF_LITTLE) {
        fprintf(STDERR_FD, "runarm: %s is not a 64-bit little-endian "
                           "executable\n", path);
        return 2;
    }
    if (head->machine != ELF_AARCH64) {
        fprintf(STDERR_FD, "runarm: %s is built for machine %u, not ARM - "
                           "this runs AArch64 and nothing else\n",
                path, head->machine);
        return 2;
    }
    if (head->type != ELF_EXECUTABLE) {
        fprintf(STDERR_FD, "runarm: %s is not a complete program; only ones "
                           "that need nothing loaded alongside them run "
                           "here\n", path);
        return 2;
    }

    /* Where the program wants to be: the lowest address any of its pieces asks
     * for, and the highest any of them reaches. */
    uint64_t low = ~0ull, high = 0;
    for (int i = 0; i < head->phnum; i++) {
        uint64_t at = head->phoff + (uint64_t)i * head->phentsize;
        if (at + sizeof(elf_segment_t) > (uint64_t)got) break;
        const elf_segment_t *seg = (const elf_segment_t *)(file + at);
        if (seg->type != SEGMENT_LOAD || !seg->memsz) continue;
        if (seg->vaddr < low) low = seg->vaddr;
        if (seg->vaddr + seg->memsz > high) high = seg->vaddr + seg->memsz;
    }
    if (low == ~0ull) {
        fprintf(STDERR_FD, "runarm: %s has nothing to load\n", path);
        return 2;
    }

    low &= ~0xFFFull;
    uint64_t span = ((high - low) + 0xFFF) & ~0xFFFull;
    uint64_t total = span + STACK_BYTES + SPARE_BYTES;

    uint8_t *memory = malloc((size_t)total);
    if (!memory) {
        fprintf(STDERR_FD, "runarm: no room for a %llu-byte program\n",
                (unsigned long long)total);
        return 2;
    }
    memset(memory, 0, (size_t)total);

    /* Each piece where it asked to be.  What is in the file is copied; what is
     * beyond it was already zero, which is what a program expects of the space
     * it declared and did not fill. */
    int loaded = 0;
    for (int i = 0; i < head->phnum; i++) {
        uint64_t at = head->phoff + (uint64_t)i * head->phentsize;
        const elf_segment_t *seg = (const elf_segment_t *)(file + at);
        if (seg->type != SEGMENT_LOAD || !seg->memsz) continue;
        if (seg->offset + seg->filesz > (uint64_t)got) {
            fprintf(STDERR_FD, "runarm: %s claims more than it contains\n",
                    path);
            free(memory);
            return 2;
        }
        memcpy(memory + (seg->vaddr - low), file + seg->offset,
               (size_t)seg->filesz);
        loaded++;
    }

    arm64_cpu_t cpu;
    arm64_init(&cpu, memory, low, total);
    cpu.pc = head->entry;
    /* The stack grows down from the top, kept sixteen-byte aligned because
     * this architecture faults on a stack pointer that is not. */
    cpu.sp = (low + total - 16) & ~0xFull;
    cpu.on_call = answer;

    outcome.status = -1;
    outcome.finished = false;
    outcome.refused = 0;
    outcome.wrote = 0;
    outcome.out_of_range = 0;
    outcome.opened = 0;

    /* The space the program can ask for sits above what was loaded and below
     * the stack, so the two grow towards each other and neither starts inside
     * the other. */
    heap.current = low + span;
    heap.limit = low + span + SPARE_BYTES;

    if (!quiet)
        printf("runarm: %s, %d piece(s), entry %llx\n", path, loaded,
               (unsigned long long)head->entry);

    while (!cpu.stopped) {
        if (!arm64_step(&cpu)) break;
        if (cpu.executed > 200000000ull) {
            fprintf(STDERR_FD, "runarm: the program never finished\n");
            free(memory);
            return 3;
        }
    }

    int result;
    if (cpu.fault) {
        fprintf(STDERR_FD, "runarm: the program stopped: %s (at %llx)\n",
                cpu.fault, (unsigned long long)cpu.fault_addr);
        result = 3;
    } else if (!outcome.finished) {
        fprintf(STDERR_FD, "runarm: the program stopped without finishing\n");
        result = 3;
    } else {
        if (!quiet)
            printf("runarm: finished with status %d after %llu "
                   "instruction(s), %d write(s)%s%s\n", outcome.status,
                   (unsigned long long)cpu.executed, outcome.wrote,
                   outcome.out_of_range ? ", some pointing outside itself" : "",
                   outcome.refused ? ", having asked for something this system "
                                     "does not answer" : "");
        result = outcome.status;
    }

    free(memory);
    return result;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return arm64_selftest() ? 1 : 0;

    if (argc > 1 && argv[1][0] != '-')
        return run_file(argv[1], false);

    printf("usage: runarm --selftest\n");
    printf("       runarm <file>\n");
    printf("\n");
    printf("Runs real AArch64 machine code - compiled by the ordinary\n");
    printf("toolchain for a processor this machine does not have - one\n");
    printf("instruction at a time. With a file, it loads an ARM executable\n");
    printf("and runs it, answering the requests it makes of the system.\n");
    return 1;
}
