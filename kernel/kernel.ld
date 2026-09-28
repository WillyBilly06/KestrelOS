/* kernel.ld - KestrelOS kernel image layout.
 *
 * The kernel is linked at the top of the address space (-2 GiB) so that
 * -mcmodel=kernel's signed 32-bit relocations reach every symbol.  The loader
 * copies the whole span to a contiguous physical block and maps it here, so the
 * sections only need page alignment, not separate load addresses.
 */
ENTRY(kmain)

KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS
{
    . = KERNEL_VMA;
    __kernel_start = .;

    .text ALIGN(4K) : {
        __text_start = .;
        *(.text.entry)
        *(.text .text.*)
        __text_end = .;
    }

    .rodata ALIGN(4K) : {
        __rodata_start = .;
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        __rodata_end = .;
    }

    .data ALIGN(4K) : {
        __data_start = .;
        *(.data .data.*)
        *(.sdata .sdata.*)
        __data_end = .;
    }

    .bss ALIGN(4K) : {
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(4K);
        __bss_end = .;
    }

    . = ALIGN(4K);
    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.interp)
        *(.dynsym .dynstr .hash .gnu.hash)
    }
}
