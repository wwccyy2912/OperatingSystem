; =============================================================================
; boot.asm - Multiboot2 entry point with long mode switch
; NASM Intel syntax, x86_64 target
; Copyright (c) 2026 OpSys Project
;
; Boot sequence:
;   1. 32-bit protected mode (from GRUB2)
;   2. Validate multiboot2, detect CPUID + long mode
;   3. Build identity + higher-half page tables (2MB huge pages, 4GB;
;      1GB huge pages from 4GB to 128GB in the shared PDP)
;   4. Enable PAE, EFER.LME, paging -> IA-32e compatibility mode
;   5. Load 64-bit GDT, far jump to 64-bit code
;   6. Switch to higher-half, call kernel_main()
; =============================================================================

%define MULTIBOOT2_MAGIC         0xe85250d6
%define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289

; Multiboot2 information request tag types
%define MB2_INFO_MMAP           6
%define MB2_INFO_ELF            9
%define MB2_INFO_FRAMEBUFFER    8       ; framebuffer info is boot-info tag 8 (5 = BIOS boot device)

; Page table entry flags
%define PTE_PRESENT     (1 << 0)
%define PTE_WRITABLE    (1 << 1)
%define PTE_HUGE        (1 << 7)

; GDT selectors
%define GDT64_CODE      0x08
%define GDT64_DATA      0x10

; CPUID bits
%define CPUID_FEAT_EDX_LONG_MODE (1 << 29)

; CR bits
%define CR0_PE          (1 << 0)
%define CR0_PG          (1 << 31)
%define CR4_PAE         (1 << 5)
%define EFER_LME        (1 << 8)
%define EFER_NXE        (1 << 11)

; MSR
%define MSR_EFER        0xC0000080

; =============================================================================
; Multiboot2 header - MUST be in first 32KB of the binary
; =============================================================================
section .multiboot2
align 16

mb_header_start:
    dd MULTIBOOT2_MAGIC                                    ; magic
    dd 0                                                    ; architecture: i386 (0)
    dd mb_header_end - mb_header_start                     ; header length
    dd -(MULTIBOOT2_MAGIC + 0 + (mb_header_end - mb_header_start)) ; checksum

    ; --- Information request tag: memory map, ELF sections, framebuffer ---
    dd 1                                                    ; type: INFO_REQUEST
    dd 24                                                   ; size: 12 + 3*4
    dd 0                                                    ; reserved
    dd MB2_INFO_MMAP                                       ; tag: memory map
    dd MB2_INFO_ELF                                        ; tag: ELF sections
    dd MB2_INFO_FRAMEBUFFER                                ; tag: framebuffer

    ; --- Framebuffer request tag (spec 3.1.10): request linear graphics mode ---
    ;     type=5, flags=0, size=20, then width/height/depth (no reserved field)
    dd 5                                                    ; type: FRAMEBUFFER (u16) + flags=0 (u16)
    dd 20                                                   ; size: 8 header + 12 payload
    dd 1024                                                 ; width (pixels)
    dd 768                                                  ; height (pixels)
    dd 32                                                   ; depth (bpp)

    ; --- Pad to 8-byte boundary so the end tag starts 8-aligned ---
    dd 0

    ; --- End tag ---
    dd 0                                                    ; type: END
    dd 8                                                    ; size
mb_header_end:


; =============================================================================
; 32-bit bootstrap code
; =============================================================================
section .text
bits 32

global _start
extern kernel_main

; Linker symbols (physical addresses, defined in linker.ld)
extern __kernel_phys_start

_start:
    ; -------------------------------------------------------------------------
    ; CRITICAL: Save EAX (multiboot2 bootloader magic) IMMEDIATELY before
    ; any code that might clobber it.
    ; -------------------------------------------------------------------------
    mov [saved_eax], eax

    ; -------------------------------------------------------------------------
    ; DEBUG: Emit 'A' to COM1 to confirm _start is reached
    ; -------------------------------------------------------------------------
    mov al, 'A'
    mov dx, 0x3F8
    out dx, al

    ; -------------------------------------------------------------------------
    ; DEBUG: Dump EAX value as hex to COM1 (8 hex digits)
    ; -------------------------------------------------------------------------
    mov eax, [saved_eax]
    
    mov al, 'E'
    mov dx, 0x3F8
    out dx, al
    mov al, 'A'
    out dx, al
    mov al, 'X'
    out dx, al
    mov al, '='
    out dx, al
    
    mov eax, [saved_eax]   ; reload from saved variable
    
    ; Dump EAX byte by byte (big-endian hex)
    mov ecx, 8             ; 8 hex digits
    mov edx, 0x3F8        ; COM1 port
.dump_loop:
    rol eax, 4             ; rotate left 4 bits
    push eax
    and al, 0x0F           ; mask low nibble
    cmp al, 10
    jl .digit
    add al, 'A' - 10       ; convert to A-F
    jmp .emit
.digit:
    add al, '0'            ; convert to 0-9
.emit:
    out dx, al
    pop eax
    dec ecx
    jnz .dump_loop
    
    mov al, 0x0D           ; carriage return
    out dx, al
    mov al, 0x0A           ; newline
    out dx, al

    ; -------------------------------------------------------------------------
    ; Step 1: Set up temporary 8KB stack in .bss
    ; In32-bit mode, symbol addresses truncate to physical (lower 32 bits)
    ; -------------------------------------------------------------------------
    mov esp, boot_stack_top

    ; -------------------------------------------------------------------------
    ; Step 2: Save multiboot2 info physical address (EBX is 32-bit)
    ; -------------------------------------------------------------------------
    mov [mboot_info], ebx           ; 32-bit physical address, upper bits unused

    ; -------------------------------------------------------------------------
    ; DEBUG: Emit 'B' after stack + mboot save
    ; -------------------------------------------------------------------------
    mov al, 'B'
    mov dx, 0x3F8
    out dx, al

    ; -------------------------------------------------------------------------
    ; Step 3: Validate multiboot2 magic in EAX (from saved copy)
    ; -------------------------------------------------------------------------
    mov eax, [saved_eax]
    cmp eax, MULTIBOOT2_BOOTLOADER_MAGIC
    jne .halt

    ; -------------------------------------------------------------------------
    ; DEBUG: Emit 'C' after magic check
    ; -------------------------------------------------------------------------
    mov al, 'C'
    mov dx, 0x3F8
    out dx, al

    ; -------------------------------------------------------------------------
    ; Step 4: Detect CPUID support (flip EFLAGS bit 21)
    ; -------------------------------------------------------------------------
    pushfd
    pop eax
    mov ecx, eax
    xor eax, (1 << 21)             ; flip ID bit
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax, ecx
    jz .halt                        ; CPUID not supported

    ; -------------------------------------------------------------------------
    ; Step 5: Check long mode via CPUID extended function 0x80000001
    ; -------------------------------------------------------------------------
    mov eax, 0x80000001
    cpuid
    test edx, CPUID_FEAT_EDX_LONG_MODE
    jz .halt                        ; long mode not supported

    ; -------------------------------------------------------------------------
    ; DEBUG: Emit 'D' after CPUID/long mode check
    ; -------------------------------------------------------------------------
    mov al, 'D'
    mov dx, 0x3F8
    out dx, al

    ; -------------------------------------------------------------------------
    ; Step 6: Build page tables
    ;   - Identity map first 4GB (PML4[0])
    ;   - Higher-half map first 4GB (PML4[256])
    ;   - Uses 2MB huge pages: PML4 -> PDP -> PD (huge)
    ;   - Above 4GB: 1GB huge pages directly in the PDP (entries 4..127),
    ;     covering physical 4GB - 128GB in BOTH the identity map and the
    ;     higher-half map (PML4[0] and PML4[256] share this PDP).
    ; -------------------------------------------------------------------------
    ; Zero all page table memory: PML4 + PDP + 4xPD = 6 pages = 24576 bytes
    cld                              ; ensure DF=0 for forward stosd
    mov edi, pml4
    xor eax, eax
    mov ecx, (4096 * 6) / 4
    rep stosd

    ; PML4[0] -> PDP  (identity: virtual 0x0000000000000000 -> phys 0)
    mov eax, pdp
    or eax, PTE_PRESENT | PTE_WRITABLE
    mov [pml4], eax
    mov dword [pml4 + 4], 0

    ; PML4[256] -> same PDP  (higher-half: virtual 0xFFFF800000000000 -> phys 0)
    ; 0xFFFF800000000000: bits 47:39 = 100000000 = 256
    mov [pml4 + 256 * 8], eax
    mov dword [pml4 + 256 * 8 + 4], 0

    ; PDP[0] -> PD page 0  (covers 0 - 512MB)
    mov eax, pd
    or eax, PTE_PRESENT | PTE_WRITABLE
    mov [pdp], eax
    mov dword [pdp + 4], 0

    ; PDP[1] -> PD page 1  (covers 512MB - 1GB)
    mov eax, pd
    add eax, 4096
    or eax, PTE_PRESENT | PTE_WRITABLE
    mov [pdp + 8], eax
    mov dword [pdp + 12], 0

    ; PDP[2] -> PD page 2  (covers 1GB - 1.5GB)
    mov eax, pd
    add eax, 8192
    or eax, PTE_PRESENT | PTE_WRITABLE
    mov [pdp + 16], eax
    mov dword [pdp + 20], 0

    ; PDP[3] -> PD page 3  (covers 1.5GB - 2GB)
    mov eax, pd
    add eax, 12288
    or eax, PTE_PRESENT | PTE_WRITABLE
    mov [pdp + 24], eax
    mov dword [pdp + 28], 0

    ; PDP[4..7] = 0 (not present) - already zeroed by rep stosd

    ; Fill all 2048 PD entries (4 pages x 512 entries) with 2MB huge pages
    ; Entry i maps physical address i * 2MB with PRESENT | WRITABLE | HUGE
    mov edi, pd
    xor ecx, ecx

.fill_pd_loop:
    mov eax, ecx
    shl eax, 21                        ; phys = index * 0x200000 (2MB)
    or eax, PTE_PRESENT | PTE_WRITABLE | PTE_HUGE
    mov [edi + ecx * 8], eax           ; low dword of PTE
    mov dword [edi + ecx * 8 + 4], 0   ; high dword (phys < 4GB)
    inc ecx
    cmp ecx, 2048
    jb .fill_pd_loop

    ; Fill PDP[4..127] with 1GB huge pages (physical 4GB - 128GB).
    ; Entry i maps physical address i * 1GB with PRESENT | WRITABLE | HUGE.
    ; PML4[0] (identity) and PML4[256] (higher-half) share this PDP, so
    ; phys_to_virt() stays valid for the whole mapped range.  The PDP is
    ; one 4KB page (512 entries); entries 128..511 stay zero (absent).
    mov edi, pdp
    mov ecx, 4

.fill_pdp_loop:
    mov eax, ecx
    shl eax, 30                        ; low dword: (i << 30) & 0xFFFFFFFF
    or eax, PTE_PRESENT | PTE_WRITABLE | PTE_HUGE
    mov [edi + ecx * 8], eax           ; low dword of PDPTE
    mov eax, ecx
    shr eax, 2                         ; high dword: i >> 2 (i * 1GB >> 32)
    mov [edi + ecx * 8 + 4], eax       ; high dword of PDPTE
    inc ecx
    cmp ecx, 128
    jb .fill_pdp_loop

    ; -------------------------------------------------------------------------
    ; DEBUG: Dump PML4[0] and PML4[256] before loading CR3
    ; -------------------------------------------------------------------------
    ; Line 1: PML4[0] low + high dwords
    mov dx, 0x3F8
    mov al, '0'
    out dx, al
    mov al, ':'
    out dx, al
    mov eax, [pml4]
    call .print_eax_hex
    mov al, ' '
    out dx, al
    mov eax, [pml4 + 4]
    call .print_eax_hex
    mov al, 0x0D
    out dx, al
    mov al, 0x0A
    out dx, al
    ; Line 2: PML4[256] low + high dwords (higher-half entry)
    mov al, 'B'
    out dx, al
    mov al, ':'
    out dx, al
    mov eax, [pml4 + 256 * 8]
    call .print_eax_hex
    mov al, ' '
    out dx, al
    mov eax, [pml4 + 256 * 8 + 4]
    call .print_eax_hex
    mov al, 0x0D
    out dx, al
    mov al, 0x0A
    out dx, al
    ; Line 3: PDP[0] low + high dwords
    mov al, 'P'
    out dx, al
    mov al, ':'
    out dx, al
    mov eax, [pdp]
    call .print_eax_hex
    mov al, ' '
    out dx, al
    mov eax, [pdp + 4]
    call .print_eax_hex
    mov al, 0x0D
    out dx, al
    mov al, 0x0A
    out dx, al
    ; Line 4: PD[0] low + high dwords
    mov al, 'D'
    out dx, al
    mov al, ':'
    out dx, al
    mov eax, [pd]
    call .print_eax_hex
    mov al, ' '
    out dx, al
    mov eax, [pd + 4]
    call .print_eax_hex
    mov al, 0x0D
    out dx, al
    mov al, 0x0A
    out dx, al

    ; -------------------------------------------------------------------------
    ; Step 7: Load PML4 physical address into CR3
    ; -------------------------------------------------------------------------
    mov eax, pml4
    mov cr3, eax

    ; -------------------------------------------------------------------------
    ; Step 8: Enable PAE (CR4 bit 5)
    ; -------------------------------------------------------------------------
    mov eax, cr4
    or eax, CR4_PAE
    mov cr4, eax

    ; -------------------------------------------------------------------------
    ; Step 9: Enable long mode (EFER.LME, MSR 0xC0000080 bit 8)
    ; -------------------------------------------------------------------------
    mov ecx, MSR_EFER
    rdmsr
    or eax, EFER_LME | EFER_NXE
    wrmsr

    ; -------------------------------------------------------------------------
    ; Step 10: Enable paging (CR0.PG) + protected mode (CR0.PE) simultaneously
    ; After this, CPU is in IA-32e compatibility mode (32-bit with 64-bit paging)
    ; -------------------------------------------------------------------------
    mov eax, cr0
    or eax, CR0_PG | CR0_PE
    mov cr0, eax

    ; -------------------------------------------------------------------------
    ; DEBUG: Emit 'E' after paging enabled
    ; -------------------------------------------------------------------------
    mov al, 'E'
    mov dx, 0x3F8
    out dx, al

    ; -------------------------------------------------------------------------
    ; Step 11: Load our own 64-bit GDT
    ; Build the 6-byte LGDT operand on the stack (cannot use dd for linker
    ; symbols in ELF64 due to R_X86_64_32 relocation size limits)
    ; -------------------------------------------------------------------------
    sub esp, 6
    mov word [esp], gdt64_end - gdt64 - 1   ; limit (size - 1)
    mov eax, gdt64                           ; base (lower 32 bits = physical)
    mov [esp + 2], eax
    lgdt [esp]
    add esp, 6

    ; -------------------------------------------------------------------------
    ; Step 12: Far jump to 64-bit code segment
    ; The offset is the lower 32 bits of start64's VMA = physical address.
    ; After jumping to a 64-bit code segment (L=1), CPU enters full long mode.
    ; RIP = zero-extend(physical address) = still physical.
    ; -------------------------------------------------------------------------
    jmp GDT64_CODE:start64


.halt:
    hlt
    jmp .halt


;-----------------------------------------------------------------------------
; print_eax_hex - Print EAX as 8 hex digits to COM1
;   Input:  EAX = value to print
;   Clobbers: ECX, EDX. EAX preserved.
;-----------------------------------------------------------------------------
.print_eax_hex:
    mov ecx, 8
    mov edx, 0x3F8
.ph_loop:
    rol eax, 4
    push eax
    and al, 0x0F
    cmp al, 10
    jl .ph_digit
    add al, 'A' - 10
    jmp .ph_emit
.ph_digit:
    add al, '0'
.ph_emit:
    out dx, al
    pop eax
    dec ecx
    jnz .ph_loop
    ret


; =============================================================================
; 64-bit long mode code — reached via far jump from 32-bit mode.
; Placed in .boot.text (physical VMA) because the 32-bit far jump needs
; a 32-bit offset that fits below 4GB.  Paging is already enabled at
; this point, so identity-mapped physical addresses are accessible.
; =============================================================================
section .boot.text
bits 64

start64:
    ; -------------------------------------------------------------------------
    ; DEBUG: Emit 'F' in 64-bit mode (via identity-mapped COM1)
    ; -------------------------------------------------------------------------
    mov al, 'F'
    mov dx, 0x3F8
    out dx, al

    ; -------------------------------------------------------------------------
    ; At this point: RIP = physical address (from 32-bit far jump).
    ; Page tables are active: identity-mapped first 4GB + higher-half.
    ; -------------------------------------------------------------------------

    ; Reload all data segment registers to 64-bit data selector
    mov ax, GDT64_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; DEBUG: Emit '1' after segment register reload
    mov al, '1'
    mov dx, 0x3F8
    out dx, al

    ; Load higher-half kernel stack (absolute address, works at physical RIP)
    mov rsp, kernel_stack_top

    ; DEBUG: Emit '2' after stack setup
    mov al, '2'
    mov dx, 0x3F8
    out dx, al

    ; -------------------------------------------------------------------------
    ; Flush TLB: reload CR3 to ensure fresh page walks for higher-half
    ; -------------------------------------------------------------------------
    mov rax, cr3
    mov cr3, rax

    ; -------------------------------------------------------------------------
    ; Jump to higher-half: after this, RIP = VMA and all code works normally.
    ; We use an absolute indirect jump because `jmp label` is RIP-relative.
    ; -------------------------------------------------------------------------
    mov rax, start64_higher_half
    jmp rax



; =============================================================================
; Higher-half entry: RIP now equals VMA, all addressing is consistent.
; =============================================================================
section .text64
start64_higher_half:

    ; -------------------------------------------------------------------------
    ; DEBUG: Emit 'G' in higher-half (via higher-half-mapped COM1)
    ; -------------------------------------------------------------------------
    mov al, 'G'
    mov dx, 0x3F8
    out dx, al

    ; Pass multiboot2 info physical address as first argument (RDI)
    ; Use EDI (32-bit) to zero-extend — mboot_info is only 4 bytes in BSS.
    ; [abs] keeps absolute addressing: mboot_info sits in .boot.bss at its
    ; physical VMA (below 4GB), which the identity map covers, and is also
    ; valid at the higher-half VMA (PML4[256] shares the same PDP).
    mov edi, [abs mboot_info]

    ; Pass kernel physical base as second argument (RSI)
    mov rsi, __kernel_phys_start

    ; Call kernel_main using absolute indirect call
    mov rax, kernel_main
    call rax

    ; Should never return, but halt just in case
.halt64:
    hlt
    jmp .halt64


; =============================================================================
; Boot-critical data - minimal 64-bit GDT for mode switch
; Placed at physical address (below 4GB) so it's reachable in 32-bit mode
; =============================================================================
section .boot.data
align 4

gdt64:
    ; Entry 0: Null descriptor
    dq 0x0000000000000000

    ; Entry 1 (selector 0x08): 64-bit code segment
    ;   Base  = 0x00000000
    ;   Limit = 0xFFFFF (4GB with G=1)
    ;   Access: P=1 DPL=00 S=1 Type=1010 (code, readable, not conforming) = 0x9A
    ;   Flags:  G=1 D/B=0 L=1 AVL=0 = 0xA  ->  Flags:Limit[19:16] = 0xAF
    dq 0x00AF9A000000FFFF

    ; Entry 2 (selector 0x10): 64-bit data segment
    ;   Access: P=1 DPL=00 S=1 Type=0010 (data, writable) = 0x92
    ;   Flags:  G=1 D/B=1 L=0 AVL=0 = 0xC  ->  Flags:Limit[19:16] = 0xCF
    dq 0x00CF92000000FFFF

gdt64_end:


; =============================================================================
; Boot-critical BSS - temporary stack, page tables, kernel stack
; Placed at physical address 0x200000 (below 4GB) so reachable in 32-bit mode
; =============================================================================
section .boot.bss nobits

; Temporary 8KB stack for 32-bit bootstrap (grows downward)
alignb 16
boot_stack_base:
    resb 8192
boot_stack_top:

; Page tables: PML4 (1) + PDP (1) + PD (4) = 6 pages, each 4096 bytes
alignb 4096
pml4:
    resb 4096
pdp:
    resb 4096
pd:
    resb 4096 * 4                  ; 4 PD pages x 512 entries x 8 bytes = 16384

; Storage for multiboot2 info physical address (passed in EBX)
mboot_info:
    resd 1

; Storage for multiboot2 bootloader magic (passed in EAX)
saved_eax:
    resd 1

; 16KB kernel stack for 64-bit mode (higher-half, used after paging enabled)
alignb 4096
kernel_stack:
    resb 16384
kernel_stack_top:
