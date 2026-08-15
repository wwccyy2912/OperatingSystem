; setjmp.S - Non-local jump implementation for x86_64
; OpSys x86_64 Microkernel
;
; jmp_buf layout (8 * 8 = 64 bytes):
;   [0] rbx    [1] rbp    [2] r12   [3] r13
;   [4] r14    [5] r15    [6] rsp    [7] rip
;
; setjmp returns 0 on direct call, val on longjmp return.
; longjmp never returns; it transfers control to setjmp's call site.

bits 64
section .text

global setjmp
global longjmp

; int setjmp(jmp_buf env)  -- rdi = env
setjmp:
        mov     [rdi + 0],  rbx
        mov     [rdi + 8],  rbp
        mov     [rdi + 16], r12
        mov     [rdi + 24], r13
        mov     [rdi + 32], r14
        mov     [rdi + 40], r15
        ; rsp: return address was pushed by call, so current rsp+8 is the
        ; value to restore (the stack pointer before the call).
        lea     rax, [rsp + 8]
        mov     [rdi + 48], rax
        ; rip: the return address at [rsp].
        mov     rax, [rsp]
        mov     [rdi + 56], rax
        ; Return 0 (direct call).
        xor     eax, eax
        ret

; void longjmp(jmp_buf env, int val)  -- rdi = env, esi = val
longjmp:
        ; val == 0 → return 1 (per C11 §7.13.2.1).
        xor     eax, eax
        cmp     esi, 0
        cmove   esi, eax
        mov     eax, esi            ; eax = return value

        ; Restore callee-saved registers.
        mov     rbx, [rdi + 0]
        mov     rbp, [rdi + 8]
        mov     r12, [rdi + 16]
        mov     r13, [rdi + 24]
        mov     r14, [rdi + 32]
        mov     r15, [rdi + 40]
        mov     rsp, [rdi + 48]

        ; Jump to saved rip.  Using jmp (not ret) because we've already
        ; loaded rsp directly and there's no matching return address
        ; on the (new) stack.
        mov     rdx, [rdi + 56]
        jmp     rdx
