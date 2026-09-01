/*
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details: <https://www.gnu.org/licenses/>.
 *
 * kernel_main.c - Kernel entry point
 * Copyright (c) 2026 OpSys Project
 * Called from boot.asm after switching to 64-bit long mode.
 * Orchestrates initialization of all kernel subsystems, creates the
 * init process, and transitions to user mode.
 *
 * ------------------------------------------------------------------
 * Structure (boot flow):
 *
 *   boot.asm (multiboot2, long mode, paging, 64-bit GDT)
 *     -> KernelMain()
 *     -> [serial/rng -> gdt/idt/pit -> pmm/vmm -> sched/thread ->
 *         cap/ipc/mutex -> syscall -> process(init)]
 *     -> load init ELF + map user stack + TSS.RSP0/IST stacks
 *     -> enter_user_mode() (ring 3)
 *
 * How it works:
 *   boot.asm validates multiboot2 and long mode, sets up paging and a
 *   64-bit GDT, then calls KernelMain, which initializes every
 *   subsystem in a fixed order, creates PID 1 (init) with a minimal
 *   capability set, loads the embedded init ELF into its address
 *   space and finally IRETQ-jumps to ring 3 via enter_user_mode().
 * Purpose:
 *   One linear, dependency-ordered boot sequence: no subsystem is
 *   used before it is initialized, and init starts with exactly the
 *   privileges it is granted at boot.
 * Caveats:
 *   KernelMain never returns; fatal errors goto halt (hlt loop).
 *   The PIT and all PIC IRQs are disabled before the ring-3
 *   transition and are re-enabled by the first syscall
 *   (EnableSchedulerOnce() in syscall.c).
 * ------------------------------------------------------------------
 */

#include <kernel/blob.h>
#include <kernel/cap.h>
#include <kernel/elf_boot.h>
#include <kernel/framebuffer.h>
#include <kernel/gdt.h>
#include <kernel/idt.h>
#include <kernel/io.h>
#include <kernel/ipc.h>
#include <kernel/mutex.h>
#include <kernel/pmm.h>
#include <kernel/process.h>
#include <kernel/rng.h>
#include <kernel/sched.h>
#include <kernel/serial.h>
#include <kernel/syscall.h>
#include <kernel/thread.h>
#include <kernel/types.h>
#include <kernel/vmm.h>
#include <multiboot2.h>

/* Kernel virtual addresses of end symbols from linker script */
extern char _kernel_end[];
extern char __kernel_phys_end[];

/* Stack-protector canary (defined in kernel/arch/x86_64/stack_chk.c);
 * randomized at boot by StackChkRandomize() for ASLR (⑭). */
extern unsigned long __stack_chk_guard;

/* Randomize the stack-protector canary from the boot PRNG
 * (defined in kernel/arch/x86_64/stack_chk.c). */
void StackChkRandomize(void);

/**
 * kernel_main - Kernel entry point
 * @mboot_info_phys: Physical address of the multiboot2 info structure
 * @kernel_phys_start: Physical base address of the loaded kernel (0x100000)
 *
 * Called from boot.asm after switching to 64-bit long mode.
 * Initializes all subsystems, creates the init process, and jumps to user mode.
 */
void KernelMain(u64 mboot_info_phys, u64 kernel_phys_start) {
    /* ==================================================================
     * Stage 1: Early console
     * ================================================================== */
    SerialInit();
    SerialPuts("OpSys kernel starting...\n");

    /* ==================================================================
     * Stage 1b: Boot-time PRNG + canary randomization (ASLR, ⑭)
     * ==================================================================
     * Seed the PRNG from hardware entropy immediately, then randomize
     * the stack-protector canary.  Safe here: SerialInit() has already
     * returned (no instrumented frame stays live across the guard
     * change) and kernel_main itself never returns, so its own frame is
     * never canary-checked.
     */
    RngInit();
    StackChkRandomize();
    SerialPrintf("  ASLR: RNG seeded, canary=0x%x\n", (u32)__stack_chk_guard);

    /* ==================================================================
     * Stage 2: Acknowledge multiboot2 information
     * ================================================================== */
    SerialPrintf("  multiboot2 info at physical 0x%x\n", mboot_info_phys);
    SerialPrintf("  kernel phys base: 0x%x\n", kernel_phys_start);

    /* ==================================================================
     * Stage 3: Core infrastructure (GDT, IDT)
     * ================================================================== */
    GdtInit();
    SerialPuts("  GDT initialized\n");

    IdtInit();
    SerialPuts("  IDT initialized\n");

    /* Initialize PIT timer at 100 Hz for scheduler ticks */
    PitInit(100);

    /* ==================================================================
     * Stage 4: Memory management (physical + virtual)
     * ================================================================== */
    PmmInit(mboot_info_phys, (u64)(uptr)__kernel_phys_end);
    SerialPuts("  PMM initialized\n");

    VmmInit();
    SerialPuts("  VMM initialized\n");

    /* Try to initialize framebuffer (non-fatal if unavailable) */
    FbInit(mboot_info_phys);

    /* Framebuffer is owned by the user-space term service (P0: kernel
     * drawing removed — see docs/kernel_roadmap.md). */

    /* ==================================================================
     * Stage 5: Threading and scheduling
     * ================================================================== */
    SchedInit();
    SerialPuts("  Scheduler initialized\n");

    ThreadInit();
    SerialPuts("  Threads initialized\n");

    /* ==================================================================
     * Stage 6: Capability system and IPC
     * ================================================================== */
    CapInit();
    SerialPuts("  Capabilities initialized\n");

    IpcInit();
    SerialPuts("  IPC initialized\n");

    MutexInit();
    SerialPuts("  Mutexes initialized\n");

    /* ==================================================================
     * Stage 7: System call infrastructure
     * ================================================================== */
    SyscallInit();
    SerialPuts("  Syscalls initialized\n");

    /* ==================================================================
     * Stage 8: Create init process with minimal capabilities
     * ================================================================== */
    SerialPuts("  Creating init process...\n");

    /* ProcessInit() creates PID 0 (kernel) and PID 1 (init).
     * PID 1 gets its own address space and capability table. */
    ProcessInit();
    SerialPuts("  Process subsystem initialized\n");

    process_t *init_proc = process_get_init();
    if (!init_proc) {
        SerialPuts("  FATAL: Failed to get init process!\n");
        goto halt;
    }
    SerialPrintf("  Init process created (PID %d), heap_base=0x%x\n",
                  init_proc->pid,
                  (u32)init_proc->heap_base);

    /* ==================================================================
     * Stage 9: Load init ELF into the process address space
     * ================================================================== */
    SerialPuts("  Loading init ELF...\n");

    /* Register embedded ELF blobs (init, hello, ...) before use.
     * blobs.c owns the linker symbols; nothing else may touch them. */
    BlobInit();

    /* Load the embedded init image with the bootstrap-only ELF loader
     * (kernel/mm/elf_boot.c).  Init is a trusted kernel-embedded blob;
     * all other processes are created via SYS_PROCESS_CREATE with
     * user-space-parsed descriptors (roadmap P1). */
    u64 entry_addr = 0;
    {
        const void *init_data;
        u64         init_size;
        int         err = BlobGet("init", &init_data, &init_size);
        if (err != OK) {
            SerialPrintf("  FATAL: blob 'init' not found: %d\n", err);
            goto halt;
        }
        err = ElfBootLoad(init_proc->addr_space, init_data, init_size, &entry_addr);
        if (err != OK) {
            SerialPrintf("  FATAL: elf_boot_load failed: %d\n", err);
            goto halt;
        }
        SerialPrintf("  ELF loaded, entry=0x%x\n", (u32)entry_addr);
    }

    /* Create the main thread for init in its address space.
     * The entry address comes from the ELF header, not a hardcoded value. */
    tid_t init_tid = ThreadCreateUser(entry_addr, 0, init_proc->addr_space, 10);
    if (init_tid < 0) {
        SerialPuts("  FATAL: Failed to create init thread!\n");
        goto halt;
    }
    init_proc->main_tid     = init_tid;
    init_proc->thread_count = 1;
    /* Main thread is enqueued and runnable: same READY transition as
     * process_create(), so ps shows the live root process as READY
     * instead of leaving it in CREATED forever. */
    init_proc->state = PROC_STATE_READY;

    /* Back-link the thread to this process */
    thread_t *init_thread = thread_get(init_tid);
    if (init_thread)
        init_thread->pid = init_proc->pid;

    /* Grant init a minimal set of capabilities.
     * In a full implementation we would grant IPC ports, memory regions,
     * and device capabilities here. For now, just a thread self-cap. */
    if (init_proc->cap_table) {
        CapCreateInTable(
            init_proc->cap_table, CAP_TYPE_THREAD, RIGHT_READ | RIGHT_WRITE, (u64)init_tid, 0);
    }

    /* Seed the management atoms (docs/ops_format.md §6): init is the
     * device owner, so its cap_create_atom/cap_grant_to_subject/
     * cap_revoke_by_atom self-tests (P0/P1/P2V regression) and perm's
     * decision_encode path (which signs atom caps via cap_grant_to_subject)
     * keep working under the new syscall gates.  ATOM_SYS_DEBUG is the
     * developer-mode atom gating SYS_PANIC (and future debug hooks). */
    if (init_proc->cap_table) {
        cap_t h = CAP_NULL;
        CapCreateAtom(init_proc->cap_table,
                        init_proc->subject_id,
                        ATOM_CAP_GRANT_SELF,
                        RIGHT_ALL,
                        0,
                        0,
                        0,
                        &h);
        CapCreateAtom(init_proc->cap_table,
                        init_proc->subject_id,
                        ATOM_SERVICE_MANAGE,
                        RIGHT_ALL,
                        0,
                        0,
                        0,
                        &h);
        CapCreateAtom(init_proc->cap_table,
                        init_proc->subject_id,
                        ATOM_SYS_DEBUG,
                        RIGHT_ALL,
                        0,
                        0,
                        0,
                        &h);
    }

    /* ==================================================================
     * Stage 10: Map user stack for the init process
     * ================================================================== */
    SerialPuts("  Mapping user stack...\n");

    /* User stack page at a randomized virtual address (ASLR, ⑭).
     * RSP starts at the top of the page (grows downward).  The address
     * comes from AslrBootStack() (region [0x4000000, 0x10000000),
     * clear of the ELF image, the fixed test mappings and the thread
     * stack region). */
    u64 user_stack_phys = PmmAllocPage();
    if (!user_stack_phys) {
        SerialPuts("  FATAL: Cannot allocate user stack page\n");
        goto halt;
    }
    u64 boot_stack_virt = AslrBootStack();
    {
        error_t err = VmmMap(init_proc->addr_space,
                              boot_stack_virt,
                              user_stack_phys,
                              PTE_PRESENT | PTE_WRITABLE | PTE_USER);
        if (err != OK) {
            SerialPrintf("  FATAL: VmmMap(0x%x) failed: %d\n", (u32)boot_stack_virt, err);
            goto halt;
        }
        SerialPrintf(
            "  VmmMap(0x%x) -> phys 0x%x OK\n", (u32)boot_stack_virt, (u32)user_stack_phys);
    }

    /* ==================================================================
     * Stage 10: Set up TSS RSP0 for ring 3 → ring 0 transitions
     * ================================================================== */

    /* Allocate a dedicated kernel stack for INT 0x80 (syscalls).
     * This lives at a higher-half address so it remains accessible
     * after switching to the user address space. */
    u64 syscall_stack_phys = PmmAllocPage();
    if (!syscall_stack_phys) {
        SerialPuts("  FATAL: Cannot allocate syscall stack\n");
        goto halt;
    }
    u64 syscall_stack_top = syscall_stack_phys + KERNEL_VIRT_BASE + PAGE_SIZE;
    GdtSetTssRsp0(syscall_stack_top);

    SerialPrintf("  TSS.RSP0 = %p (syscall kernel stack)\n", syscall_stack_top);

    /* ==================================================================
     * Stage 10b: IST stacks for critical exceptions (#DF, NMI)
     * ================================================================== */
    u64 df_stack_phys = PmmAllocPage();
    if (!df_stack_phys) {
        SerialPuts("  FATAL: Cannot allocate DF IST stack\n");
        goto halt;
    }
    GdtSetTssIst(1, df_stack_phys + KERNEL_VIRT_BASE + PAGE_SIZE);

    u64 nmi_stack_phys = PmmAllocPage();
    if (!nmi_stack_phys) {
        SerialPuts("  FATAL: Cannot allocate NMI IST stack\n");
        goto halt;
    }
    GdtSetTssIst(2, nmi_stack_phys + KERNEL_VIRT_BASE + PAGE_SIZE);

    /* ==================================================================
     * Stage 11: Transition to user mode
     * ================================================================== */

    /* Disable PIT timer BEFORE entering user mode.
     * The PIT fires at 100 Hz and its IRQ handler calls SchedTick(),
     * which may try to context-switch. We haven't set up any scheduled
     * thread yet, so a timer interrupt during the CR3/IRETQ transition
     * would be dangerous. We'll re-enable it once the init process is
     * running and the scheduler is properly wired. */
    IoOutb(0x43, 0x30); /* Channel 0, lobyte/hibyte, mode 0 (interrupt on
                            terminal count) */
    IoOutb(0x40, 0x00); /* Low byte of count = 0 */
    IoOutb(0x40, 0x00); /* High byte of count = 0 → counter stops */
    SerialPuts("  PIT disabled for user transition\n");

    /* Mask all PIC IRQs to prevent any hardware interrupt during transition */
    IoOutb(0x21, 0xFF); /* Mask all master PIC IRQs */
    IoOutb(0xA1, 0xFF); /* Mask all slave PIC IRQs */
    SerialPuts("  All IRQs masked\n");

    /* Disable interrupts (IF=0) for the final transition */
    IdtDisableInterrupts();

    /* Screen is handed to user mode already cleared; the term service
     * redraws it (P0: kernel drawing removed). */

    u64 user_cr3 = init_proc->addr_space->pml4_phys;
    u64 temp_rsp = syscall_stack_top - 256; /* safe margin below top */

    /*
     * Tell the scheduler that init_thread is the "current" thread.
     * The IRETQ path below bypasses the scheduler entirely, so without
     * this call, s_current[0] would still point to the idle thread.
     * When the first timer interrupt fires, SchedTick() would try to
     * context_switch(idle→init) which would switch CR3 to the user
     * page tables mid-kernel, causing a page fault at low addresses.
     *
     * We use SchedSetCurrent() (not sched_switch_to) because the
     * latter calls context_switch() which would switch CR3 while we
     * are still running on the boot stack (identity-mapped, PML4[0]).
     *
     * CRITICAL: The idle thread (tid=0) was created by ThreadInit() but
     * is deliberately NOT placed in the ready tree. SchedSetCurrent()
     * replaces it as the running thread, orphaning idle (it stays in
     * THREAD_STATE_RUNNING, never enqueued).
     *
     * Why idle must stay OUT of the ready tree: idle has priority 0, the
     * lowest CFS weight (cfs_weights[0] = 15), so each reschedule that
     * accounts it costs vruntime delta = 1024/15 ≈ 68, vs ≈ 8 for a
     * priority-10 thread (weight 120). Enqueued, idle wins the leftmost
     * (lowest-vruntime) pick roughly once every 8.5 switches and then
     * runs `sti; hlt` until the next PIT tick — a 10 ms stall every
     * time. This was measured as 1000 ticks per 1000 solo yields and
     * ~1.18 ms per IPC round-trip (idle stealing ~1 in 8.5 picks).
     *
     * Instead, the scheduler switches to idle DIRECTLY (see Reschedule()
     * in sched.c) only when the ready tree is empty AND the current
     * thread is BLOCKED/FINISHED and cannot continue. A voluntary yield
     * with no competitors keeps running the current thread — it must not
     * hand the CPU to idle, which would halt until the next tick.
     */
    SchedSetCurrent(init_thread);

    SerialPuts("  Scheduler knows init_thread is current\n");

    SerialPuts("  Transitioning to ring 3...\n");

    /*
     * Enter ring 3 via enter_user_mode().
     *
     * This is a dedicated asm function (NOT inline asm) that receives all
     * values in registers via the SysV ABI.  It reads temp_rsp from a
     * global variable (s_enter_user_temp_rsp), switches to it, loads CR3,
     * builds the IRETQ frame, and far-jumps to ring 3.  It never returns.
     *
     * We use a global instead of a 7th register argument because Clang
     * doesn't guarantee `register ... asm("r10")` will place the value
     * in R10 at the call site.
     */
    extern u64 s_enter_user_temp_rsp;
    s_enter_user_temp_rsp = temp_rsp;

    enter_user_mode(entry_addr,                  /* RDI: RIP (user entry) */
                    boot_stack_virt + PAGE_SIZE, /* RSI: user RSP (stack top) */
                    0x202ULL,                    /* RDX: RFLAGS (IF=1, bit1=1) */
                    0x1BULL,                     /* RCX: CS (GDT[3] | RPL=3) */
                    0x23ULL,                     /* R8:  SS (GDT[4] | RPL=3) */
                    user_cr3);                   /* R9:  PML4 physical addr */

    /* Unreachable */
    __builtin_unreachable();

halt:
    SerialPuts("OpSys kernel halting.\n");
    for (;;) {
        __asm__ volatile("hlt");
    }
}
