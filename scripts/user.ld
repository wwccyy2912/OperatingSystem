/* =============================================================================
 * user.ld - Linker script for user-space programs
 * OpSys x86_64 Microkernel
 *
 * All user programs are linked at the standard ELF load address 0x400000.
 * The kernel maps user pages at this virtual address for each process.
 * ============================================================================= */

ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text : ALIGN(4096)
    {
        *(.text)
        *(.text.*)
    }

    .rodata : ALIGN(4096)
    {
        *(.rodata)
        *(.rodata.*)
    }

    .data : ALIGN(4096)
    {
        *(.data)
        *(.data.*)
    }

    .init_array : ALIGN(8)
    {
        __init_array_start = .;
        *(.init_array)
        *(.init_array.*)
        __init_array_end = .;
    }

    .fini_array : ALIGN(8)
    {
        __fini_array_start = .;
        *(.fini_array)
        *(.fini_array.*)
        __fini_array_end = .;
    }

    .bss : ALIGN(4096)
    {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        _bss_end = .;
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.gnu.build-id)
    }
}
