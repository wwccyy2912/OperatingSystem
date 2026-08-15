; sigrestore.S - Signal return trampoline for user-space programs
; OpSys x86_64 Microkernel
;
; When the kernel delivers a signal, it rewrites the interrupted user
; context so that execution resumes in the signal handler with:
;     RIP = handler, RDI = signum, RSP = base - 8
; where base = align_down(orig_rsp - SIGFRAME_TOTAL, 16).  The 8-byte
; slot at [base-8, base) holds the address of __restore_rt (it is the
; first thing the handler's prologue sees on the stack).  When the
; handler returns (ret), that slot is popped as the return address, so
; control lands here with RSP pointing exactly at the sigframe base.
; We pass that pointer to the kernel via SYS_SIGRETURN (50), which
; restores the original user context.

bits 64
section .text

global __restore_rt

__restore_rt:
        ; RSP == sigframe base (the slot holding our own address was popped
        ; by the handler's ret).  Pass it as arg1 (RDI) to SYS_SIGRETURN.
        mov rdi, rsp
        mov eax, 50             ; SYS_SIGRETURN
        int 0x80

        ; The kernel must never return from SYS_SIGRETURN.  If it does,
        ; something is badly wrong — hang loudly.
.halt:
        hlt
        jmp .halt
