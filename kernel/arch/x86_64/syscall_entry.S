;
; syscall_entry.S - System call entry/exit for x86_64
; Copyright (c) 2026 OpSys Project
;
; NASM syntax, Intel mode.
;
; Provides the entry point for INT 0x80 system calls.
;
; On entry (via INT 0x80):
;   RAX = syscall number
;   RDI = arg1
;   RSI = arg2
;   RDX = arg3
;   R10 = arg4
;   R8  = arg5
;   CPU has pushed: SS, RSP, RFLAGS, CS, RIP  (40 bytes on TSS.RSP0)
;
; IMPORTANT: We immediately switch to the per-thread kernel stack
; (from thread_t.kstack_top) before saving any GPRs.  This avoids
; corrupting another thread's saved state when TSS.RSP0 is shared.
;
; Exports:
;   syscall_entry_stub - placed at IDT vector 0x80
;   syscall_entry_iretq - return to user mode after syscall dispatch
;

bits 64
default rel

; ------------------------------------------------------------------
; External symbols
; ------------------------------------------------------------------
extern syscall_dispatch
extern g_current_thread
extern signal_check_syscall

; ------------------------------------------------------------------
; Thread struct offsets (must match kernel/include/kernel/thread.h)
; ------------------------------------------------------------------
%define KSTACK_TOP_OFFSET    104     ; thread_t.kstack_top

; ------------------------------------------------------------------
; Syscall dispatch helper struct offsets
; (matches the stack layout we create below)
;
; Push order: rax, rbx, rcx, rdx, rsi, rdi, rbp, r8, r9, r10,
;             r11, r12, r13, r14, r15
;
; After all pushes, r15 is on TOP (offset 0), rax is DEEPEST (offset 112).
; Then CPU frame: RIP(120), CS(128), RFLAGS(136), RSP(144), SS(152).
; ------------------------------------------------------------------
%define SF_R15      0
%define SF_R14      8
%define SF_R13      16
%define SF_R12      24
%define SF_R11      32
%define SF_R10      40
%define SF_R9       48
%define SF_R8       56
%define SF_RBP      64
%define SF_RDI      72
%define SF_RSI      80
%define SF_RDX      88
%define SF_RCX      96
%define SF_RBX      104
%define SF_RAX      112
; Below the GPRs: the frame pushed by CPU (5 qwords = 40 bytes)
%define SF_RIP      120
%define SF_CS       128
%define SF_RFLAGS   136
%define SF_RSP      144
%define SF_SS       152
%define SF_SIZE     160     ; total frame size (120 GPRs + 40 CPU frame)

section .text

; ------------------------------------------------------------------
; syscall_entry_stub
;
; INT 0x80 handler entry point.
;
; On entry:
;   RSP points to TSS.RSP0 with CPU frame: [RIP][CS][RFLAGS][RSP][SS]
;   RAX = syscall number, RDI/RSI/RDX/R8/R10 = args
;   RCX and R11 are trashed by INT (free to use as temporaries)
;
; Strategy:
;   1. Load g_current_thread → get kstack_top
;   2. Copy 40-byte INT frame from TSS.RSP0 to per-thread stack
;   3. Switch RSP to per-thread kernel stack
;   4. Save GPRs and call syscall_dispatch
;   5. Restore GPRs and IRETQ back to user mode
; ------------------------------------------------------------------
global syscall_entry_stub
syscall_entry_stub:
    ; ---------------------------------------------------------------
    ; Step 1: Switch to per-thread kernel stack
    ;
    ; We use RCX and R11 as temporaries (both trashed by INT 0x80).
    ; Load g_current_thread (global pointer to current thread_t*),
    ; then read kstack_top from offset 104.
    ; ---------------------------------------------------------------
    lea  rcx, [rel g_current_thread]
    mov  rcx, [rcx]              ; rcx = thread_t*
    mov  r11, [rcx + KSTACK_TOP_OFFSET]  ; r11 = kstack_top

    ; ---------------------------------------------------------------
    ; Step 2: Copy 40-byte INT frame from TSS.RSP0 to per-thread stack
    ;
    ; Make room on the per-thread stack, then copy each qword.
    ; ---------------------------------------------------------------
    sub  r11, 40                 ; make room for 5 qwords

    mov  rcx, [rsp + 0]         ; RIP
    mov  [r11 + 0],  rcx
    mov  rcx, [rsp + 8]         ; CS
    mov  [r11 + 8],  rcx
    mov  rcx, [rsp + 16]        ; RFLAGS
    mov  [r11 + 16], rcx
    mov  rcx, [rsp + 24]        ; RSP (user)
    mov  [r11 + 24], rcx
    mov  rcx, [rsp + 32]        ; SS
    mov  [r11 + 32], rcx

    ; ---------------------------------------------------------------
    ; Step 3: Switch RSP to per-thread kernel stack
    ;
    ; After this, RSP points to the INT frame on the per-thread stack.
    ; TSS.RSP0 is no longer in use by this thread.
    ; ---------------------------------------------------------------
    mov  rsp, r11

    ; ---------------------------------------------------------------
    ; Step 4: Save all general-purpose registers onto per-thread stack
    ; ---------------------------------------------------------------
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; ---------------------------------------------------------------
    ; Step 5: Set up arguments for syscall_dispatch()
    ;   i64 syscall_dispatch(u64 num, u64 arg1, u64 arg2,
    ;                        u64 arg3, u64 arg4, u64 arg5)
    ;
    ; System V AMD64: RDI, RSI, RDX, RCX, R8, R9
    ;
    ; From saved registers on stack:
    ;   syscall num  = saved RAX  [rsp + SF_RAX]
    ;   arg1         = saved RDI  [rsp + SF_RDI]
    ;   arg2         = saved RSI  [rsp + SF_RSI]
    ;   arg3         = saved RDX  [rsp + SF_RDX]
    ;   arg4         = saved R10  [rsp + SF_R10]
    ;   arg5         = saved R8   [rsp + SF_R8]
    ; ---------------------------------------------------------------

    mov rdi, [rsp + SF_RAX]    ; syscall number
    mov rsi, [rsp + SF_RDI]    ; arg1
    mov rdx, [rsp + SF_RSI]    ; arg2
    mov rcx, [rsp + SF_RDX]    ; arg3
    mov r8,  [rsp + SF_R10]    ; arg4
    mov r9,  [rsp + SF_R8]     ; arg5

    call syscall_dispatch

    ; ---------------------------------------------------------------
    ; Step 6: Store return value in saved RAX slot
    ; ---------------------------------------------------------------
    mov [rsp + SF_RAX], rax

    ; ---------------------------------------------------------------
    ; Step 6.5: Signal delivery checkpoint
    ;
    ; If the current process has a pending signal and this is a user
    ; syscall (CS == 0x1B), build the sigframe on the user stack and
    ; rewrite the saved frame so the handler runs on IRETQ.  The frame
    ; pointer is RSP (the saved RAX slot is frame[0]).
    ;
    ; The checkpoint follows the SysV ABI: caller-saved registers are
    ; all already saved in their frame slots, callee-saved ones
    ; (rbx/rbp/r12-r15) are preserved by the callee, and RSP is
    ; restored by the callee's ret.  If a signal was delivered, the
    ; frame's RAX slot still holds the syscall return value; the
    ; handler's eventual sigreturn restores the original RAX.
    ; ---------------------------------------------------------------
    mov  rdi, rsp                ; frame pointer
    call signal_check_syscall

    ; ---------------------------------------------------------------
    ; Step 7: Restore all general-purpose registers
    ; ---------------------------------------------------------------
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; ---------------------------------------------------------------
    ; Step 8: Return to user mode via IRETQ
    ; CPU pops: RIP, CS, RFLAGS, RSP, SS  from per-thread stack
    ; ---------------------------------------------------------------
    iretq


; ------------------------------------------------------------------
; syscall_entry_iretq
;
; Alternate entry for syscall return. Some code paths may need
; to modify the IRETQ frame before returning. This provides a
; clean return point.
; ------------------------------------------------------------------
global syscall_entry_iretq
syscall_entry_iretq:
    iretq
