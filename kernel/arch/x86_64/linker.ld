/* =============================================================================
 * linker.ld - Kernel linker script for x86_64 higher-half kernel
 * Copyright (c) 2026 OpSys Project
 *
 * VMA (virtual)  = __kernel_virt_base + physical offset
 * LMA (physical) = 0x100000 + physical offset  (where GRUB loads the ELF)
 *
 * GRUB2 reads ELF PT_LOAD segments and loads them at p_paddr (LMA).
 * The CPU uses p_vaddr (VMA) after page tables are enabled.
 *
 * Boot-critical sections (.boot.bss / .boot.data) are placed AFTER all
 * higher-half sections so their physical addresses sit above the kernel
 * image.  Their VMA equals the physical address (identity-mapped) so
 * they are reachable in 32-bit mode before paging is enabled.
 * ============================================================================= */

OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

/* ---- Kernel address constants ---- */
__kernel_phys_start = 0x100000;                        /* 1MB, standard GRUB load */
__kernel_virt_base  = 0xFFFF800000000000;              /* higher-half base */
__kernel_virt_start = __kernel_virt_base + __kernel_phys_start;

SECTIONS
{
    /*
     * Place all higher-half sections at VMA = __kernel_virt_base + phys.
     * Use AT() to set LMA so GRUB loads segments at physical locations.
     *
     * The pattern for each section after .multiboot2:
     *   AT(ADDR(.section) - __kernel_virt_base)
     *     = VMA - 0xFFFF800000000000
     *     = physical address
     */
    . = __kernel_virt_start;

    /* --- Multiboot2 header: MUST be within first 32KB, aligned to 16 --- */
    .multiboot2 : AT(__kernel_phys_start) ALIGN(16)
    {
        *(.multiboot2)
    }

    /* --- Code --- */
    .text : AT(ADDR(.text) - __kernel_virt_base) ALIGN(4096)
    {
        _text_start = .;
        *(.text)
        *(.text64)
        _text_end = .;
    }

    /* --- Read-only data (constants, strings, vtables) --- */
    .rodata : AT(ADDR(.rodata) - __kernel_virt_base) ALIGN(4096)
    {
        _rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        *(.lrodata)                     /* TLS read-only (local exec) */
        *(.lrodata.*)
        _rodata_end = .;
    }

    /* --- Initialized data --- */
    .data : AT(ADDR(.data) - __kernel_virt_base) ALIGN(4096)
    {
        _data_start = .;
        *(.data)
        *(.data.*)
        *(.ltdata)                      /* TLS initialized (local exec) */
        *(.ltdata.*)
        _data_end = .;
    }

    /* --- Uninitialized data (BSS, zeroed by loader) --- */
    .bss : AT(ADDR(.bss) - __kernel_virt_base) ALIGN(4096)
    {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        *(.lbss)                        /* TLS BSS (local exec) */
        *(.lbss.*)
        _bss_end = .;
    }

    /* --- Discard unnecessary sections --- */
    /DISCARD/ :
    {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.gnu.build-id)
    }

    /* --- End-of-kernel symbol (virtual address) --- */
    _kernel_end = .;

    /* --- Physical end of main kernel image (before boot sections) --- */
    _kernel_image_phys_end = _kernel_end - __kernel_virt_base;

    /* =================================================================
     * Boot-critical sections at physical addresses (below 4GB).
     *
     * These are referenced in 32-bit mode BEFORE paging is enabled, so
     * VMA MUST equal LMA (physical address). We align the location
     * counter to 0x1000 to match the ELF segment alignment, then use
     * AT(ADDR(.section)) to force LMA = VMA for each section.
     *
     * Placed AFTER the kernel image so they never overlap with any
     * higher-half section's LMA (including TLS / .lbss sections).
     * ================================================================= */
    . = ALIGN(_kernel_image_phys_end, 0x1000);

    .boot.bss (NOLOAD) : AT(ADDR(.boot.bss))
    {
        *(.boot.bss)
    }

    .boot.data : AT(ADDR(.boot.data))
    {
        *(.boot.data)
    }

    .boot.text : AT(ADDR(.boot.text))
    {
        *(.boot.text)
    }

    /* --- Physical end of ENTIRE kernel (including boot sections) --- */
    /* This is what PMM needs to know so it doesn't place its bitmap
     * on top of the page tables, stacks, and GDT in .boot.* sections. */
    __kernel_phys_end = .;
}
