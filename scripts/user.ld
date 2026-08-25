/* =============================================================================
 * user.ld - Linker script for user-space programs
 * OpSys x86_64 Microkernel
 *
 * All user programs are linked at the standard ELF load address 0x400000.
 * The kernel maps user pages at this virtual address for each process.
 *
 * Explicit PHDRS enforce strict W^X: .text is R+X only, .rodata R only,
 * .data/.bss R+W only.  Without them, ld may coalesce a small program's
 * sections (e.g. a 0x240-byte .text plus .init_array) into one RWX LOAD
 * segment, and the kernel maps segments verbatim from p_flags.
 * ============================================================================= */

ENTRY(_start)

PHDRS
{
        text   PT_LOAD FLAGS(5); /* R+X */
        rodata PT_LOAD FLAGS(4); /* R   */
        data   PT_LOAD FLAGS(6); /* R+W */
}

SECTIONS
{
        . = 0x400000;

        .text : ALIGN(4096)
        {
                *(.text)
                *(.text.*)
        } :text

        .rodata : ALIGN(4096)
        {
                *(.rodata)
                *(.rodata.*)
        } :rodata

        .init_array : ALIGN(4096)
        {
                __init_array_start = .;
                KEEP(*(.init_array))
                KEEP(*(.init_array.*))
                __init_array_end = .;
        } :rodata

        .fini_array : ALIGN(8)
        {
                __fini_array_start = .;
                KEEP(*(.fini_array))
                KEEP(*(.fini_array.*))
                __fini_array_end = .;
        } :rodata

        .data : ALIGN(4096)
        {
                *(.data)
                *(.data.*)
        } :data

        .bss : ALIGN(4096)
        {
                _bss_start = .;
                *(.bss)
                *(.bss.*)
                *(COMMON)
                _bss_end = .;
        } :data

        /DISCARD/ :
        {
                *(.comment)
                *(.note.*)
                *(.eh_frame)
                *(.eh_frame_hdr)
                *(.gnu.build-id)
        }
}
