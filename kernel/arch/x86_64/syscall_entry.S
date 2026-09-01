; SPDX-License-Identifier: GPL-3.0-or-later
;
; This program is free software: you can redistribute it and/or modify
; it under the terms of the GNU General Public License as published by
; the Free Software Foundation, either version 3 of the License, or
; (at your option) any later version.
;
; This program is distributed in the hope that it will be useful, but
; WITHOUT ANY WARRANTY; without even the implied warranty of
; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
; General Public License for more details: <https://www.gnu.org/licenses/>.
;
;
; syscall_entry.S - System call entry/exit for x86_64
; Copyright (c) 2026 OpSys Project
;
; NASM syntax, Intel mode.
;
; Two entries converge on one common body:
;
;   syscall_entry_fast - the SYSCALL instruction (v0.7): the CPU loads
;       RIP = LSTAR, CS/SS from STAR, masks RFLAGS with SFMASK, saves
;       user RIP in RCX and user RFLAGS in R11, and does NOT touch RSP
;       or the TSS.  The scheduler keeps MSR_KERNEL_GS_BASE = the
;       running thread, so the entry does `swapgs` and then synthesizes
;       the identical 5-qword frame [RIP][CS][RFLAGS][RSP][SS] at
;       kstack_top-40 via GS-relative stores — no scratch register is
;       needed (every GPR is live at syscall entry).
;
;   syscall_entry_stub - the legacy INT 0x80 path (kept as a fallback /
;       A-B switch): the CPU pushes the 5-qword frame on TSS.RSP0; the
;       stub copies it to the per-thread stack.  It also does `swapgs`
;       on entry so both paths share one swapgs+sysretq exit.
;
; Common body (syscall_common): save all 15 GPRs, call
; syscall_dispatch, run the signal checkpoint, restore the GS state
; EXPLICITLY (wrmsr: GS.base = 0, MSR_KERNEL_GS_BASE = the running
; thread — a mid-syscall yield can cross the swapgs pairing, so the
; exit must not rely on it), restore GPRs, then IRETQ to user mode.
;
; NOTE (sysretq): the SYSRETQ exit was implemented first but QEMU TCG
; raises #GP (error 0x28, TSS selector) on it in this configuration —
; an unresolved hypervisor/CPU interaction, documented in the exit
; path.  The entry-side savings are identical either way.
;
; On fast-path entry:
;   RAX = syscall number
;   RDI = arg1, RSI = arg2, RDX = arg3, R10 = arg4, R8 = arg5
;   RCX = user RIP, R11 = user RFLAGS, RSP = user RSP
;
; On INT 0x80 entry:
;   RAX = syscall number, RDI/RSI/RDX/R10/R8 = args
;   CPU has pushed: SS, RSP, RFLAGS, CS, RIP (40 bytes on TSS.RSP0)
;   RCX and R11 are trashed by INT (free to use as temporaries)
;
; SECURITY INVARIANT (TOCTOU): both paths run with IF=0 for the whole
; handler — INT via the interrupt gate, SYSCALL via SFMASK (bit 1).
; The PIT tick cannot fire between a handler's vmm_validate_user_ptr()
; check and its memcpy of the same range, so validate-then-copy is
; race-free by construction (see the vmm.c comment).
;
; Exports:
;   syscall_entry_fast - LSTAR target for the SYSCALL instruction
;   syscall_entry_stub - placed at IDT vector 0x80
;   syscall_entry_iretq - alternate return point (unused by the kernel)
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

; ------------------------------------------------------------------
; Fast-path offsets relative to the THREAD (GS base after swapgs):
;   KSTACK_TOP_OFFSET (104) - the kernel stack top (frame goes at
;                             kstack_top-40, which is NOT thread+64 —
;                             the thread struct and its kernel stack
;                             are separate allocations!)
;   SYSCALL_SAVE_RSP (112)  - the entry stashes the USER RSP here
;                             before switching stacks (no scratch reg
;                             is free at syscall entry).
; ------------------------------------------------------------------
%define SYSCALL_SAVE_RSP    112     ; thread_t.syscall_save_rsp

section .text

; ------------------------------------------------------------------
; syscall_entry_fast
;
; SYSCALL instruction entry (IA32_LSTAR target).
;
; On entry:
;   RCX = user RIP, R11 = user RFLAGS, RSP = user RSP
;   RAX = syscall number, RDI/RSI/RDX/R10/R8 = args
;   IF/TF/DF masked by SFMASK (set up in idt.c)
;
; The CPU did NOT switch stacks.  swapgs turns GS into the running
; thread (MSR_KERNEL_GS_BASE, maintained by the scheduler), which lets
; us write the frame with GS-relative stores and load the kernel stack
; pointer — all without touching any register that still holds a live
; argument.
; ------------------------------------------------------------------
global syscall_entry_fast
syscall_entry_fast:
        swapgs                          ; GS = thread (kernel GS base)

        ; The CPU did NOT switch stacks: save the user RSP in the thread
        ; (GS-relative, no scratch register), then load kstack_top.
        mov  [gs:SYSCALL_SAVE_RSP], rsp ; thread.syscall_save_rsp = user RSP
        mov  rsp, [gs:KSTACK_TOP_OFFSET] ; rsp = kernel stack top
        sub  rsp, 40                     ; rsp = frame base (kstack_top-40)

        ; Synthesize the 5-qword frame.  RCX (user RIP) and R11 (user
        ; RFLAGS) survived the stack switch untouched; the user RSP comes
        ; back from the thread save slot (RCX is a free scratch now).
        mov  [rsp + 0], rcx              ; frame.RIP    <- user RIP (RCX)
        mov  [rsp + 8], dword 0x1B       ; frame.CS     <- user code selector
        mov  [rsp + 16], r11             ; frame.RFLAGS <- user RFLAGS (R11)
        mov  rcx, [gs:SYSCALL_SAVE_RSP]  ; rcx = user RSP
        mov  [rsp + 24], rcx             ; frame.RSP    <- user RSP
        mov  [rsp + 32], dword 0x23      ; frame.SS     <- user data selector
        jmp  syscall_common

; ------------------------------------------------------------------
; syscall_entry_stub
;
; INT 0x80 handler entry point (IDT vector 0x80).
;
; On entry:
;   RSP points to TSS.RSP0 with CPU frame: [RIP][CS][RFLAGS][RSP][SS]
;   RAX = syscall number, RDI/RSI/RDX/R8/R10 = args
;   RCX and R11 are trashed by INT (free to use as temporaries)
;
; Strategy:
;   1. swapgs (same GS discipline as the fast path, so the shared
;      exit's swapgs is symmetric)
;   2. Load g_current_thread -> get kstack_top
;   3. Copy 40-byte INT frame from TSS.RSP0 to per-thread stack
;   4. Switch RSP to per-thread kernel stack
;   5. Join syscall_common
; ------------------------------------------------------------------
global syscall_entry_stub
syscall_entry_stub:
        swapgs                          ; GS = thread (symmetric with exit)

        ; ---------------------------------------------------------------
        ; Step 2: Switch to per-thread kernel stack
        ;
        ; We use RCX and R11 as temporaries (both trashed by INT 0x80).
        ; Load g_current_thread (global pointer to current thread_t*),
        ; then read kstack_top from offset 104.
        ; ---------------------------------------------------------------
        lea  rcx, [rel g_current_thread]
        mov  rcx, [rcx]              ; rcx = thread_t*
        mov  r11, [rcx + KSTACK_TOP_OFFSET]  ; r11 = kstack_top

        ; ---------------------------------------------------------------
        ; Step 3: Copy 40-byte INT frame from TSS.RSP0 to per-thread stack
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
        ; Step 4: Switch RSP to per-thread kernel stack
        ;
        ; After this, RSP points to the INT frame on the per-thread stack.
        ; TSS.RSP0 is no longer in use by this thread.
        ; ---------------------------------------------------------------
        mov  rsp, r11

; ------------------------------------------------------------------
; syscall_common — shared by both entries.
;
; On entry: RSP = per-thread kernel stack, [RSP..RSP+40) = the frame
; [RIP][CS][RFLAGS][RSP][SS] (identical layout for both paths).
; ------------------------------------------------------------------
syscall_common:
        ; ---------------------------------------------------------------
        ; Save all general-purpose registers onto per-thread stack
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
        ; Set up arguments for syscall_dispatch()
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
        ; Store return value in saved RAX slot
        ; ---------------------------------------------------------------
        mov [rsp + SF_RAX], rax

        ; ---------------------------------------------------------------
        ; Signal delivery checkpoint
        ;
        ; If the current process has a pending signal and this is a user
        ; syscall (CS == 0x1B), build the sigframe on the user stack and
        ; rewrite the saved frame so the handler runs on SYSRETQ.  The
        ; frame pointer is RSP (the saved RAX slot is frame[0]).
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
        ; Restore the GS state BEFORE popping registers: the wrmsr
        ; sequence below clobbers RAX/RCX/RDX, and the pops that follow
        ; restore the user's values (including the syscall return in
        ; RAX) from the frame — so doing the MSR work first is safe.
        ;
        ; GS is restored EXPLICITLY (no swapgs): a blocking syscall can
        ; yield mid-flight, and a context switch normalizes the GS/
        ; MSR102 pairing (sched_set_kernel_gs).  The exit therefore must
        ; not assume the entry swapgs is still paired — it WRITES the
        ; clean state instead: user GS base = 0, and MSR_KERNEL_GS_BASE
        ; = the running thread (so the next entry swapgs sees it).
        ; RCX/RAX/RDX are free here (user ABI clobbers rcx; rax/rdx are
        ; restored from the frame slots by the pops below).
        ; ---------------------------------------------------------------
        mov  ecx, 0xC0000101            ; IA32_GS_BASE
        xor  eax, eax
        xor  edx, edx
        wrmsr                           ; GS.base = 0 (user GS)
        mov  rax, [rel g_current_thread]
        mov  ecx, 0xC0000102            ; MSR_KERNEL_GS_BASE
        mov  rdx, rax
        shr  rdx, 32
        wrmsr                           ; MSR102 = running thread

        ; ---------------------------------------------------------------
        ; Restore all general-purpose registers.  RCX and R11 are popped
        ; for stack alignment but their values are discarded (the user
        ; ABI declares rcx/r11 clobbered by the syscall instruction).
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
        ; Return to user mode via IRETQ.
        ;
        ; The frame [RIP][CS][RFLAGS][RSP][SS] is still at RSP (pushed
        ; by the CPU for the INT path, synthesized by GS-relative stores
        ; for the fast path) — IRETQ pops it directly, so the user's
        ; RSP is restored by the frame, not by a register move.
        ;
        ; SYSRETQ was implemented as the intended exit first, but QEMU
        ; TCG raises #GP (error 0x28, the TSS selector) on it in this
        ; configuration — an unresolved hypervisor/CPU interaction.
        ; IRETQ is fully verified; the entry-side savings (no TSS.RSP0
        ; frame copy, GS-based synthesis) are retained either way.
        ; ---------------------------------------------------------------
        iretq


; ------------------------------------------------------------------
; syscall_entry_iretq
;
; Alternate return point.  Some code paths may need to modify the
; frame before returning; kept for compatibility (not used by the
; kernel — the exit above is SYSRETQ-based).
; ------------------------------------------------------------------
global syscall_entry_iretq
syscall_entry_iretq:
        iretq
