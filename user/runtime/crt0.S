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
        ; Align the stack to a 16-byte boundary before making any calls.
        ; The kernel enters user code either at the raw stack top (%16==0,
        ; boot path) or at top-8 ("as if called", %16==8 — worker threads
        ; via SYS_THREAD_CREATE, whose entry points are plain C functions
        ; compiled for the SysV ABI).  `and $-16, %rsp` normalizes both to
        ; %16==0 so that the `call` below pushes the return address and
        ; the callee correctly sees RSP%16==8.
        and rsp, -16

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
