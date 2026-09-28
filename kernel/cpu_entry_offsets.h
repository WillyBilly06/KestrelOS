#ifndef KESTREL_CPU_ENTRY_OFFSETS_H
#define KESTREL_CPU_ENTRY_OFFSETS_H
/* Shared with syscall_entry in isr.S; C layout assertions live in cpu.c. */
#define CPU_ENTRY_KERNEL_RSP 0
#define CPU_ENTRY_USER_RSP   8
#endif
