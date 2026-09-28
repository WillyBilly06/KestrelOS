/* start.S - the process entry point.
 *
 * The kernel enters a new process with RDI = argc and RSI = argv, RSP already
 * 16-byte aligned, and every other register zero.  All this needs to do is
 * align the frame the way the ABI expects after a call and hand over to C.
 */
.intel_syntax noprefix
.section .text.start, "ax"

.global _start
_start:
    xor rbp, rbp            /* end the frame chain here */
    and rsp, -16            /* the ABI wants 16-byte alignment before the call */
    call libc_start         /* libc_start(argc, argv) never returns */

    /* Should libc_start ever return, exit rather than run off the end. */
    mov rdi, 0
    mov rax, 0              /* SYS_EXIT */
    syscall
1:  hlt
    jmp 1b

/* long syscall6(nr, a0, a1, a2, a3, a4, a5)
 *
 * The kernel takes its fourth argument in R10 because SYSCALL destroys RCX,
 * so the sixth C argument (on the stack) has to be shuffled into place. */
.global syscall6
syscall6:
    mov rax, rdi            /* number   */
    mov rdi, rsi            /* arg 0    */
    mov rsi, rdx            /* arg 1    */
    mov rdx, rcx            /* arg 2    */
    mov r10, r8             /* arg 3    */
    mov r8,  r9             /* arg 4    */
    mov r9,  [rsp + 8]      /* arg 5    */
    syscall
    ret

.section .note.GNU-stack,"",@progbits
