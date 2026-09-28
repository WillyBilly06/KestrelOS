/* smp_trampoline.S - the few hundred bytes an application processor runs
 * between being woken and reaching C.
 *
 * A processor that has just been sent a startup message is not in the state
 * the rest of the kernel runs in.  It begins in sixteen-bit real mode, with no
 * paging, no long mode, and a code segment of whatever the startup message
 * named - so none of the kernel's own code is reachable from it, and none of
 * the kernel's addresses mean anything to it yet.  This is the bridge.
 *
 * It runs at a fixed low physical address because the startup message names a
 * page number rather than an address, and only the first megabyte can be named
 * that way.  TRAMP_BASE below must match SMP_TRAMPOLINE_PHYS in smp.c, and the
 * page is reserved before anything else can be handed it.
 *
 * Addresses here are absolute rather than relative, which is unusual for this
 * codebase and is the point: the assembler cannot know where this will be
 * copied to, so nothing may depend on where it was linked.  Everything the
 * boot processor has to tell each waking core - which page tables to use, the
 * stack it owns, where to go next - is left in a parameter block at the end of
 * the page and read from a fixed address.
 */
.intel_syntax noprefix

.set TRAMP_BASE,   0x8000

/* The parameter block, filled in by smp.c before each core is woken. */
.set PARAM_PML4,   TRAMP_BASE + 0xF00
.set PARAM_STACK,  TRAMP_BASE + 0xF08
.set PARAM_ENTRY,  TRAMP_BASE + 0xF10
.set PARAM_ALIVE,  TRAMP_BASE + 0xF18

/* Progress marks.  A core that does not arrive says nothing about how far it
 * got, and the three stages fail for completely different reasons - so each
 * one writes a byte before going on. */
.set MARK_PROT32,  TRAMP_BASE + 0xF20
.set MARK_LONG64,  TRAMP_BASE + 0xF21

/* Absolute addresses of the labels below.  Computed once here rather than
 * written into operands: the assembler will not take two symbols in a memory
 * operand, and these are constants because everything they refer to lives in
 * this one blob. */
.set ADDR_PROT32,  TRAMP_BASE + (prot32 - smp_trampoline_start)
.set ADDR_LONG64,  TRAMP_BASE + (long64 - smp_trampoline_start)
.set ADDR_GDTPTR,  TRAMP_BASE + (gdt32_ptr - smp_trampoline_start)
.set ADDR_GDT,     TRAMP_BASE + (gdt32 - smp_trampoline_start)

.section .rodata
.global smp_trampoline_start
.global smp_trampoline_end
.align 4096

smp_trampoline_start:

/* ------------------------------------------------------------------ 16 bit */
.code16
    cli
    cld

    /* Segments to zero, so the absolute addresses below mean what they say. */
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax

    lgdt    [ADDR_GDTPTR]

    /* Protected mode, which is only a step on the way: long mode cannot be
     * entered from real mode directly. */
    mov     eax, cr0
    or      eax, 1
    mov     cr0, eax

    .byte 0x66, 0xEA                    /* ljmp far, 32-bit operand           */
    .long ADDR_PROT32
    .word 0x08

/* ------------------------------------------------------------------ 32 bit */
.code32
prot32:
    mov     byte ptr [MARK_PROT32], 1
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     fs, ax
    mov     gs, ax

    /* Physical address extension, without which there are no 64-bit page
     * tables to point at. */
    mov     eax, cr4
    or      eax, (1 << 5)
    mov     cr4, eax

    /* The boot processor's own page tables.  Sharing them is what makes every
     * kernel address mean the same thing on every core - and it is why the
     * identity mapping of low memory has to still be present at this point,
     * because the very next instructions execute from one. */
    mov     eax, [PARAM_PML4]
    mov     cr3, eax

    /* Long mode enable - and no-execute enable with it.
     *
     * The second is not optional here, however much it looks like a
     * refinement.  The kernel's page tables set bit 63 on everything that is
     * not code, and what that bit means depends entirely on this flag: with it
     * on, the bit forbids execution; with it off, the bit is reserved, and any
     * access at all through an entry that has it set is a fault.
     *
     * So a core that enters long mode without it faults on the first write to
     * its own stack - before there is an interrupt table to catch it, which
     * means a triple fault, which on this hardware means the machine restarts.
     * The symptom is the machine rebooting the instant the startup message is
     * sent, with the waking core never reaching a single instruction of C. */
    mov     ecx, 0xC0000080
    rdmsr
    or      eax, (1 << 8) | (1 << 11)
    wrmsr

    /* Paging on.  Long mode only actually begins here. */
    mov     eax, cr0
    or      eax, 0x80000001
    mov     cr0, eax

    .byte 0xEA                          /* ljmp ptr16:32                      */
    .long ADDR_LONG64
    .word 0x18

/* ------------------------------------------------------------------ 64 bit */
.code64
long64:
    mov     byte ptr [MARK_LONG64], 1
    mov     ax, 0x20
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     fs, ax
    mov     gs, ax

    /* The stack this core was given.  Until this instruction it has none, so
     * nothing above may call anything. */
    mov     rsp, [PARAM_STACK]
    xor     rbp, rbp

    /* Say so, before jumping away.  The boot processor is spinning on this
     * and will give up on a core that never writes it - which is the only way
     * it can tell "still starting" from "did not start". */
    mov     qword ptr [PARAM_ALIVE], 1

    mov     rax, [PARAM_ENTRY]
    mov     rdi, rsp                    /* SysV argument: allocated stack top */
    call    rax

    /* C is not expected to return.  If it does, stop this core rather than
     * letting it run off into whatever follows. */
1:  cli
    hlt
    jmp     1b

/* ---------------------------------------------------------------- the GDT
 *
 * Enough to get from real mode to 64-bit code and no more: the kernel's own
 * table is loaded from C once there is a stack to do it with.
 */
.align 16
gdt32:
    .quad 0x0000000000000000            /* null                               */
    .quad 0x00CF9A000000FFFF            /* 0x08  32-bit code                  */
    .quad 0x00CF92000000FFFF            /* 0x10  32-bit data                  */
    .quad 0x00AF9A000000FFFF            /* 0x18  64-bit code                  */
    .quad 0x00CF92000000FFFF            /* 0x20  64-bit data                  */
gdt32_end:

.align 4
gdt32_ptr:
    .word gdt32_end - gdt32 - 1
    .long ADDR_GDT

.align 4096
smp_trampoline_end:
