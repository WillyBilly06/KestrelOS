/* Retpoline targets required by NVIDIA's x86-64 NVKMS core. */
.section .text

.macro NVKMS_THUNK reg
.global __x86_indirect_thunk_\reg
.type __x86_indirect_thunk_\reg,@function
__x86_indirect_thunk_\reg:
    call 2f
1:
    pause
    lfence
    jmp 1b
2:
    mov %\reg, (%rsp)
    ret
.size __x86_indirect_thunk_\reg, .-__x86_indirect_thunk_\reg
.endm

NVKMS_THUNK rax
NVKMS_THUNK rbx
NVKMS_THUNK rcx
NVKMS_THUNK rdx
NVKMS_THUNK rsi
NVKMS_THUNK r8
NVKMS_THUNK r9
NVKMS_THUNK r10
NVKMS_THUNK r11
NVKMS_THUNK r12
NVKMS_THUNK r13
NVKMS_THUNK r14
NVKMS_THUNK r15

.section .note.GNU-stack,"",@progbits
