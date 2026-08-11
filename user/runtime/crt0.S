; crt0.S - C runtime startup for user-space programs
; OpSys x86_64 Microkernel
;
; _start is the ELF entry point set by the linker (ENTRY(_start) in user.ld).
; It initializes the C runtime and calls main(argc, argv).
; The kernel sets up the user stack before jumping here.

bits 64
section .text

global _start
extern _init
extern main
extern exit

_start:
    ; Call _init() to run global constructors (.init_array)
    call _init

    ; Call main(argc, argv)
    xor edi, edi        ; argc = 0
    xor esi, esi        ; argv = NULL
    call main

    ; Call exit(retval) — noreturn
    mov edi, eax
    call exit

    ; Should never reach here
.halt:
    hlt
    jmp .halt
