/*
 * canarytest.c - User stack-canary self-test service
 * Copyright (c) 2026 OpSys Project
 *
 * Exists ONLY to prove the Ring 3 stack protector (v0.6.2) fires:
 * it deliberately overflows a stack buffer.  GCC -fstack-protector-
 * strong instruments main() (it has a local array): the epilogue
 * compares the canary and calls __stack_chk_fail(), which logs
 * "STACK SMASHING DETECTED" to the debug log and exits with
 * 128+SIGABRT = 134.  The init selftest spawns this blob and asserts
 * the exit code; the smoke suite asserts the serial message.
 *
 * Deliberately given no caps and no IRQ bindings.
 */

#include <libos/syscalls.h>

int main(void) {
    volatile char buf[8];
    volatile int  i;
    /* Delay before overflowing: the init selftest spawns us and then
     * blocks in process_wait().  We must not die before its waiting_tid
     * is registered, or the kernel reaps us as an orphan and the wait
     * fails with ERR_NOENT.  ~0.5 s at the 100 Hz PIT tick. */
    sleep(50);
    /* Overrun the 8-byte buffer by 24 bytes: the compiler places the
     * canary slot right after the locals (rsp+0x18 here), so writing
     * past buf[] by ~16-24 bytes clobbers it.  Keep the total well
     * inside the frame — a bigger overrun would cross the 1-page user
     * stack top and #PF (SIGSEGV) before the canary check fires. */
    for (i = 0; i < 32; i++)
        buf[i] = (char)i;
    (void)buf;
    return 0; /* unreachable: __stack_chk_fail exits first */
}
