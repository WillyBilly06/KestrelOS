/* isr.S - interrupt entry stubs, the syscall entry point and the context switch.
 *
 * Every vector funnels into isr_common, which builds the `regs` frame that
 * interrupt.c and the scheduler both read.  The frame layout, from low address
 * to high, is exactly the field order of struct regs in cpu.h.
 */
.intel_syntax noprefix
.text

/* ------------------------------------------------------------------------- */
/* interrupt stubs                                                           */
/* ------------------------------------------------------------------------- */

.macro ISR_NOERR vec
.global isr\vec
.align 16
isr\vec:
    push 0                  /* dummy error code, so every frame is the same */
    push \vec
    jmp isr_common
.endm

.macro ISR_ERR vec
.global isr\vec
.align 16
isr\vec:
    /* the CPU already pushed an error code */
    push \vec
    jmp isr_common
.endm

/* CPU exceptions: 8, 10-14, 17, 21, 29 and 30 arrive with an error code. */
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

/* Everything from 32 up is a device or software vector with no error code. */
.altmacro
.set vecnum, 32
.rept 224
    ISR_NOERR %vecnum
    .set vecnum, vecnum + 1
.endr
.noaltmacro

/* Table the C side installs from. */
.macro ISR_SLOT vec
    .quad isr\vec
.endm

.section .rodata
.global isr_table
.align 8
isr_table:
.altmacro
.set vecnum, 0
.rept 256
    ISR_SLOT %vecnum
    .set vecnum, vecnum + 1
.endr
.noaltmacro

.text

/* ------------------------------------------------------------------------- */
/* common entry                                                              */
/* ------------------------------------------------------------------------- */

.global isr_common
.align 16
isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    /* Reload the kernel data selectors: a fault from user mode leaves the user
     * segments loaded, and the kernel must not run on them. */
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    cld
    mov rdi, rsp            /* struct regs * */
    call interrupt_dispatch
    /* dispatch may return a different frame (a context switch) */
    mov rsp, rax

.global isr_return
isr_return:
    /* Restore the user data selectors when returning to ring 3. */
    mov rax, [rsp + 18*8]               /* saved CS */
    and rax, 3
    jz 1f
    mov ax, 0x1b
    mov ds, ax
    mov es, ax
1:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16             /* discard vector and error code */
    iretq

/* ------------------------------------------------------------------------- */
/* syscall entry                                                             */
/*                                                                           */
/* SYSCALL leaves the caller's RSP untouched and stashes RIP in RCX and        */
/* RFLAGS in R11. KERNEL_GS_BASE names this CPU's private entry scratch.       */
/* Maskable interrupts are off via SFMASK. No GPR or user stack is clobbered. */
/* SWAPGS is paired here, not in the shared interrupt/IRET path: ordinary    */
/* kernel C retains user GS. NMI/MCE entry uses private IST stacks and never */
/* reads or changes GS, including when interrupting this short swap window. */
/* ------------------------------------------------------------------------- */

#include "cpu_entry_offsets.h"

.global syscall_entry
.align 16
syscall_entry:
    swapgs
    lfence
    mov qword ptr gs:[CPU_ENTRY_USER_RSP], rsp
    mov rsp, qword ptr gs:[CPU_ENTRY_KERNEL_RSP]

    /* Build the same frame shape an interrupt would, so a syscall can block
     * and be resumed by the scheduler like any other trap. */
    push 0x1b                               /* SS   */
    push qword ptr gs:[CPU_ENTRY_USER_RSP]    /* RSP  */
    swapgs                                  /* restore user GS before C */
    push r11                                /* RFLAGS */
    push 0x23                               /* CS   */
    push rcx                                /* RIP  */
    push 0                                  /* error code */
    push 0x80                               /* vector */

    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov ax, 0x10
    mov ds, ax
    mov es, ax

    cld
    mov rdi, rsp
    call syscall_dispatch
    mov rsp, rax

    /* Returning through iretq rather than sysret keeps one exit path for both
     * the fast case and the case where the scheduler moved us to another
     * thread's frame. */
    jmp isr_return

.text

/* ------------------------------------------------------------------------- */
/* kernel context switch                                                     */
/*                                                                           */
/*   context_switch(u64 *save_rsp, u64 new_rsp)                              */
/*                                                                           */
/* Saves the callee-saved registers and RFLAGS on the outgoing thread's       */
/* kernel stack, records its stack pointer, then adopts the incoming thread's */
/* stack and unwinds the mirror image.  A thread that blocks resumes at the   */
/* instruction after its own call, which is what lets a system call wait in   */
/* the middle of its work instead of having to be restarted.                  */
/* ------------------------------------------------------------------------- */

.global context_switch
.align 16
context_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    pushfq
    mov [rdi], rsp
    mov rsp, rsi
    popfq
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

/* ------------------------------------------------------------------------- */
/* thread bootstrap                                                          */
/*                                                                           */
/* A brand new thread is given a stack whose "return address" is this label,  */
/* with a regs frame laid out immediately above it.  Falling into isr_return  */
/* enters the thread in whichever ring its frame describes.                   */
/* ------------------------------------------------------------------------- */

.global thread_bootstrap
.align 16
thread_bootstrap:
    /* No sti here: the frame's own RFLAGS re-enables interrupts at iretq,
     * so the window between the switch and entry stays closed. */
    /* RSP is the 16-byte-aligned regs_t frame; the C call uses memory below
     * it, and isr_return restores the thread's architectural registers. */
    call sched_switch_complete
    call proc_thread_first_entry
    jmp isr_return

/* ------------------------------------------------------------------------- */
/* FPU state                                                                 */
/* ------------------------------------------------------------------------- */

.global fpu_save
fpu_save:
    fxsave64 [rdi]
    ret

.global fpu_restore
fpu_restore:
    fxrstor64 [rdi]
    ret

.global fpu_init_state
fpu_init_state:
    fninit
    ret

/* Load a page table and switch stacks in one step; used when the scheduler
 * moves between address spaces. */
.global switch_address_space
switch_address_space:
    mov cr3, rdi
    ret

/* switch_stack_and_call(u64 stack_top, void (*fn)(void))
 *
 * Move onto a different stack and continue in `fn`, which never returns.  The
 * kernel uses this once during start-up to leave the loader's stack: that stack
 * lives in the identity map, which is not shared with user address spaces, so
 * the first switch to a user process would pull it out from under us. */
.global switch_stack_and_call
switch_stack_and_call:
    mov rsp, rdi
    xor rbp, rbp
    push 0                  /* a null return address to stop unwinders */
    jmp rsi

.section .note.GNU-stack,"",@progbits
