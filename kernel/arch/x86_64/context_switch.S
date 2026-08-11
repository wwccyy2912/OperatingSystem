;
; context_switch.S - Thread context switch for x86_64
; Copyright (c) 2026 OpSys Project
;
; NASM syntax, Intel mode.
; void context_switch(thread_t *prev, thread_t *next)
; void context_switch_to_user(u64 rip, u64 rsp, u64 rflags, u64 cs, u64 ss)
;

bits 64
default rel

; ------------------------------------------------------------------
; thread_t struct offsets (must match kernel/include/kernel/thread.h)
; ------------------------------------------------------------------
%define TID_OFFSET          0   ; i32
%define PID_OFFSET          4   ; i32
%define STATE_OFFSET        8   ; i32 (enum)
%define PRIORITY_OFFSET     12  ; int
%define RSP_OFFSET          16  ; u64
%define RBX_OFFSET          24  ; u64
%define RBP_OFFSET          32  ; u64
%define R12_OFFSET          40  ; u64
%define R13_OFFSET          48  ; u64
%define R14_OFFSET          56  ; u64
%define R15_OFFSET          64  ; u64
%define RFLAGS_OFFSET       72  ; u64
%define RIP_OFFSET          80  ; u64
%define ADDR_SPACE_OFFSET   88  ; addr_space_t*

section .text

section .data
    global s_enter_user_temp_rsp
    s_enter_user_temp_rsp: dq 0

section .text

; ------------------------------------------------------------------
; void context_switch(thread_t *prev, thread_t *next, int resume_if)
;
; System V AMD64 ABI: RDI = prev, RSI = next, EDX = resume_if
;
; Saves callee-saved registers of prev, loads from next.
; Switches CR3 if address spaces differ.
;
; resume_if tells how prev must resume:
;   1  prev switched away voluntarily (yield/sleep/block): it resumes
;      directly at its continuation and must run with IF=1.
;   0  prev was preempted by the PIT ISR: it resumes inside the ISR
;      return chain (reschedule -> sched_tick -> isr_handler -> iretq)
;      and must run with IF=0 until that iretq restores the true
;      interrupted flags.
;
; Interrupt-atomic protocol (fixes the spurious #PF at RIP=0 on resume):
;   1. The caller disables interrupts before the call, and `cli` here is
;      the first instruction, so the ENTIRE save/restore runs with IF=0.
;      The PIT can therefore never observe a half-saved or half-restored
;      thread, and a nested context_switch from sched_tick can never
;      corrupt an in-flight switch.
;   2. The final transfer uses IRETQ.  One non-interruptible instruction
;      pops RIP, CS, RFLAGS, RSP and SS: IF is raised only after the CPU
;      has fully switched to next, so there is no popfq/ret window in
;      which an interrupt could push a frame onto the wrong stack or
;      cause a nested switch to double-pop the return address.
; ------------------------------------------------------------------
global context_switch
context_switch:
    cli

    ; Save callee-saved registers of prev thread
    ;
    ; Save RSP *before* the call pushed the return address.  The `call
    ; context_switch` instruction pushes 8 bytes (the return address) onto
    ; the stack, so rsp at entry here is 8 bytes below the caller's
    ; pre-call rsp.  The IRETQ transfer below loads RSP from the frame
    ; field, so the saved value must be the caller's pre-call rsp
    ; (rsp + 8), matching the caller's frame layout on resume.  If we
    ; saved the post-call rsp, the restored thread's epilogue would pop
    ; from 8 bytes too low, reading a local variable instead of the return
    ; address -> #PF at a user address (the old bug: CR2=0x40223e).
    lea rax, [rsp + 8]
    mov [rdi + RSP_OFFSET], rax
    mov [rdi + RBX_OFFSET], rbx
    mov [rdi + RBP_OFFSET], rbp
    mov [rdi + R12_OFFSET], r12
    mov [rdi + R13_OFFSET], r13
    mov [rdi + R14_OFFSET], r14
    mov [rdi + R15_OFFSET], r15

    ; prev->rflags = (resume_if << 9) | 0x2
    ;   resume_if=0 -> 0x2   (IF=0: resume into the ISR return chain)
    ;   resume_if=1 -> 0x202 (IF=1: resume directly at the continuation)
    ; The other flag bits are irrelevant: C code recomputes them, and a
    ; preempted thread's true flags live in the ISR frame for its iretq.
    mov eax, edx
    shl rax, 9
    or  rax, 0x2
    mov [rdi + RFLAGS_OFFSET], rax

    ; Save the return address (where we'll return to after switch)
    pop rax
    mov [rdi + RIP_OFFSET], rax

    ; ---------------------------------------------------------------
    ; Switch address space if needed
    ; ---------------------------------------------------------------
    mov rax, [rdi + ADDR_SPACE_OFFSET]   ; prev->addr_space
    mov rcx, [rsi + ADDR_SPACE_OFFSET]   ; next->addr_space
    cmp rax, rcx
    je .load_next

    ; If next->addr_space is NULL, skip CR3 load (kernel thread)
    test rcx, rcx
    jz .load_next

    ; Load CR3 from next address space (pml4_phys at offset 0)
    mov rax, [rcx]
    mov cr3, rax

.load_next:
    ; Load callee-saved registers of next thread (IF=0 throughout)
    mov rbx, [rsi + RBX_OFFSET]
    mov rbp, [rsi + RBP_OFFSET]
    mov r12, [rsi + R12_OFFSET]
    mov r13, [rsi + R13_OFFSET]
    mov r14, [rsi + R14_OFFSET]
    mov r15, [rsi + R15_OFFSET]

    ; Final transfer: build an IRETQ frame on the (abandoned) prev stack
    ; and let IRETQ perform the switch atomically.  IRETQ pops, in order:
    ; RIP, CS, RFLAGS, RSP, SS — so push them in reverse order.  The
    ; frame is dead data once IRETQ runs (prev resumes at its saved RSP,
    ; above this frame).  Selectors: GDT_SEL_KCODE=0x08, GDT_SEL_KDATA=0x10.
    mov rax, 0x10               ; kernel SS
    push rax
    mov rax, [rsi + RSP_OFFSET]
    push rax
    mov rax, [rsi + RFLAGS_OFFSET]
    push rax
    mov rax, 0x08               ; kernel CS
    push rax
    mov rax, [rsi + RIP_OFFSET]
    push rax
    iretq

; ------------------------------------------------------------------
; void context_switch_to_user(u64 rip, u64 rsp, u64 rflags,
;                             u64 cs, u64 ss)
;
; System V AMD64 ABI:
;   RDI = rip   (user instruction pointer)
;   RSI = rsp   (user stack pointer)
;   RDX = rflags
;   RCX = cs    (user code segment selector, e.g. 0x2B)
;   R8  = ss    (user stack segment selector, e.g. 0x23)
;
; Builds an IRETQ frame and performs a far return to ring 3.
; ------------------------------------------------------------------
global context_switch_to_user
context_switch_to_user:
    ; Disable interrupts until IRETQ restores RFLAGS
    cli

    ; Build IRETQ frame (pushed in reverse order of IRETQ popping)
    push r8         ; SS   (user stack segment)
    push rsi        ; RSP  (user stack pointer)
    push rdx        ; RFLAGS
    push rcx        ; CS   (user code segment)
    push rdi        ; RIP  (user instruction pointer)

    iretq

; ------------------------------------------------------------------
; void enter_user_mode(u64 rip, u64 user_rsp, u64 rflags,
;                       u64 cs, u64 ss, u64 cr3);
;
; System V AMD64 ABI:
;   RDI = rip     (user instruction pointer)
;   RSI = user_rsp (user stack pointer)
;   RDX = rflags
;   RCX = cs      (user code segment, e.g. 0x1B)
;   R8  = ss      (user stack segment, e.g. 0x23)
;   R9  = cr3     (physical address of user PML4)
;
; Reads temp_rsp from the global s_enter_user_temp_rsp (a higher-half
; stack used during the CR3 switch).  This is a one-way function: it
; switches to the user address space and IRETQs into ring 3.
;
; Why a separate function instead of inline asm?
;   Inline asm that writes to %%rsp breaks the compiler's stack
;   tracking.  The compiler emits the asm template before loading
;   operand registers, so it loads entry_addr from the NEW (wrong)
;   stack offset, producing 0.  This asm function receives everything
;   in registers via the SysV ABI, then reads temp_rsp from a global
;   that was set before the call.
; ------------------------------------------------------------------
extern s_enter_user_temp_rsp

global enter_user_mode
enter_user_mode:
    cli

    ; Load temp_rsp from global (higher-half address, accessible from
    ; both kernel and user page tables since PML4[256..511] is shared).
    lea rax, [rel s_enter_user_temp_rsp]
    mov rsp, [rax]

    ; Switch to user page tables
    mov cr3, r9

    ; Build IRETQ frame (pushed in reverse order of IRETQ popping)
    push r8         ; SS   (user stack segment)
    push rsi        ; RSP  (user stack pointer)
    push rdx        ; RFLAGS
    push rcx        ; CS   (user code segment)
    push rdi        ; RIP  (user instruction pointer)

    iretq
