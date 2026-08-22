/*
 * main.c - Init process (PID 1)
 * Copyright (c) 2026 OpSys Project
 *
 * The first user-space process. Exercises syscalls to validate
 * the ring 3 -> ring 0 -> ring 3 round trip, then performs the
 * init protocol (query kernel state, spawn worker thread).
 */

#include "../lib/libc/stdio.h"
#include "../lib/libc/stdlib.h"
#include "../lib/libos/syscalls.h"
/* P1 地基: perm.h pulls in vfs.h — both protocol structs for the
 * live vfs_server + perm-manager tests below.  Include perm.h only
 * (it includes vfs.h) so the u8/u32/i32/u64 typedefs are not doubled. */
#include "../perm/perm.h"
#include "../lib/libfs/fs.h"

/* ---- Test framework ---- */

static int tests_run  = 0;
static int tests_pass = 0;

#define TEST(name)                       \
    do {                                 \
        printf("  TEST: %s ... ", name); \
        tests_run++;                     \
    } while (0)

#define PASS()            \
    do {                  \
        tests_pass++;     \
        printf("PASS\n"); \
    } while (0)

#define FAIL(msg)                  \
    do {                           \
        printf("FAIL: %s\n", msg); \
    } while (0)

#define ASSERT(cond, msg) \
    do {                  \
        if (!(cond)) {    \
            FAIL(msg);    \
            return;       \
        }                 \
    } while (0)

/* ---- Cycle-accurate timer (bypasses the ambiguous PIT tick rate) ---- */

static inline unsigned long long rdtsc64(void) {
    unsigned int lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

/* ---- Worker thread ---- */

static void worker_thread(void *arg) {
    int id = (int)(long)arg;
    printf("  worker[%d]: started, PID=%d\n", id, get_pid());
    printf("  worker[%d]: free pages=%d\n", id, get_free_pages());
    printf("  worker[%d]: done\n", id);
    thread_exit(0);
}

/* ---- IPC receiver worker (for send/recv test) ---- */

static int  g_ipc_recv_port;
static char g_ipc_recv_buf[64];
static int  g_ipc_recv_len;
static int  g_ipc_recv_ret;

static void ipc_recv_worker(void *arg) {
    (void)arg;
    int tok        = 0;
    g_ipc_recv_len = sizeof(g_ipc_recv_buf);
    g_ipc_recv_ret = ipc_recv(g_ipc_recv_port, g_ipc_recv_buf, &g_ipc_recv_len, &tok);
    thread_exit(0);
}

/* ---- ipc_recv_from worker (P0 地基: sender identity test) ---- */

static int      g_recvfrom_port;
static char     g_recvfrom_buf[64];
static int      g_recvfrom_len;
static int      g_recvfrom_ret;
static uint64_t g_recvfrom_subj;

static void ipc_recvfrom_worker(void *arg) {
    (void)arg;
    int tok        = 0;
    g_recvfrom_len = sizeof(g_recvfrom_buf);
    g_recvfrom_ret =
        ipc_recv_from(g_recvfrom_port, g_recvfrom_buf, &g_recvfrom_len, &tok, &g_recvfrom_subj);
    thread_exit(0);
}

/* ---- thread_ctx_t mirror (layout must match kernel thread_ctx.h:
 * 9 x u64 = 72 bytes, context_switch.S save order) ---- */

typedef struct {
    unsigned long rsp;
    unsigned long rbx;
    unsigned long rbp;
    unsigned long r12;
    unsigned long r13;
    unsigned long r14;
    unsigned long r15;
    unsigned long rflags;
    unsigned long rip;
} test_thread_ctx_t;

/* ---- Stress test shared state ---- */

#define STRESS_THREADS   1000
#define STRESS_IPC_CALLS 100000

static volatile int g_stress_counter;
static int          g_stress_mutex;

static void stress_worker(void *arg) {
    (void)arg;
    mutex_lock(g_stress_mutex);
    g_stress_counter++;
    mutex_unlock(g_stress_mutex);
    thread_exit(0);
}

static int          g_ipc_stress_port;
static volatile int g_ipc_stress_fail;

static void ipc_stress_server(void *arg) {
    (void)arg;
    for (int i = 0; i < STRESS_IPC_CALLS; i++) {
        char buf[16];
        int  len = sizeof(buf);
        int  tok = 0;
        if (ipc_recv(g_ipc_stress_port, buf, &len, &tok) != 0) {
            g_ipc_stress_fail = 1;
            thread_exit(1);
        }
        if (ipc_reply(tok, buf, len) != 0) {
            g_ipc_stress_fail = 1;
            thread_exit(1);
        }
    }
    thread_exit(0);
}

/* ---- Phase 1: Syscall tests ---- */

static void test_debug_log(void) {
    TEST("debug_log (ring3->ring0->ring3)");
    int ret = debug_log("  [syscall] debug_log ok\n");
    ASSERT(ret == 0, "debug_log returned non-zero");
    PASS();
}

static void test_yield(void) {
    TEST("thread_yield (ring3->ring0->ring3)");
    thread_yield();
    PASS();
}

static void test_port_create(void) {
    TEST("ipc_port_create");
    int port = ipc_port_create();
    ASSERT(port > 0, "port <= 0");
    printf("(port=%d) ", port);
    PASS();
}

static void test_port_register_get(void) {
    TEST("port_register + port_get");
    int port = ipc_port_create();
    ASSERT(port > 0, "port_create failed");
    int ret = port_register("test_svc", port);
    ASSERT(ret == 0, "port_register failed");
    int got = port_get("test_svc");
    ASSERT(got == port, "port_get mismatch");
    PASS();
}

static void test_port_get_nonexistent(void) {
    TEST("port_get (nonexistent)");
    int got = port_get("no_such_service");
    ASSERT(got < 0, "should return error");
    PASS();
}

static void test_cap_create(void) {
    TEST("cap_create");
    int cap = cap_create(0, 0);
    ASSERT(cap > 0, "cap <= 0");
    printf("(cap=%d) ", cap);
    PASS();
}

static void test_map_memory(void) {
    TEST("map_memory");
    /* Create a memory capability with write rights */
    int mem_cap = cap_create(CAP_TYPE_MEM, RIGHT_WRITE);
    ASSERT(mem_cap > 0, "cap_create for MEM failed");
    /* Map one page at 0x10000000 with read/write, no exec */
    void *p = map_memory(mem_cap, 0x10000000, 4096, PROT_READ | PROT_WRITE);
    ASSERT(p != 0, "map_memory returned NULL");
    PASS();
}

static void test_get_pid(void) {
    TEST("get_pid");
    int pid = get_pid();
    ASSERT(pid == 1, "PID should be 1");
    PASS();
}

static void test_get_free_pages(void) {
    TEST("get_free_pages");
    int pages = get_free_pages();
    ASSERT(pages > 0, "free pages should be > 0");
    printf("(%d pages, %d MB) ", pages, pages / 256);
    PASS();
}

static void test_thread_create(void) {
    TEST("thread_create (worker)");
    int tid = thread_create(worker_thread, (void *)0L, 10);
    ASSERT(tid > 0, "thread_create failed");
    printf("(tid=%d) ", tid);
    /* Yield to let the worker run at least once */
    thread_yield();
    thread_yield();
    int exit_code = -1;
    thread_join(tid, &exit_code);
    PASS();
}

static void test_get_time(void) {
    TEST("get_time");
    int t1 = get_time();
    ASSERT(t1 > 0, "get_time should return > 0");
    printf("(ticks=%d) ", t1);
    PASS();
}

static void test_sleep(void) {
    TEST("sleep");
    int t1  = get_time();
    int ret = sleep(10);
    ASSERT(ret == 0, "sleep returned error");
    int t2      = get_time();
    int elapsed = t2 - t1;
    ASSERT(elapsed >= 3, "sleep returned too early (< 3 ticks)");
    printf("(elapsed=%d ticks) ", elapsed);
    PASS();
}

/* Temporary microbenchmark: 100k bare get_time syscalls (no IPC, no
 * blocking).  Contrast with the 100k IPC test to separate syscall-entry
 * cost from IPC logic cost. */
static void bench_syscall_100k(void) {
    TEST("bench: 100k get_time syscalls");
    int                t0   = get_time();
    unsigned long long c0   = rdtsc64();
    volatile int       sink = 0;
    for (int i = 0; i < 100000; i++)
        sink += get_time();
    unsigned long long c1      = rdtsc64();
    int                elapsed = get_time() - t0;
    /* user printf lacks %llu: print 64-bit cycles as hi/lo 32-bit halves */
    unsigned long long dc = c1 - c0;
    printf("(%d calls, %d ticks, cyc_hi=%u cyc_lo=%u) ",
           100000,
           elapsed,
           (unsigned)(dc >> 32),
           (unsigned)(dc & 0xFFFFFFFFu));
    (void)sink; /* the loop is the benchmark; value is intentionally discarded */
    PASS();
}

/* Temporary microbenchmark: 100k round-trips between two threads via
 * pure thread_yield ping-pong (no IPC, no blocking primitives).  This
 * isolates scheduler + context-switch cost from IPC logic cost. */
static volatile int g_bench_yield_turn;
static volatile int g_bench_yield_other_done;
static volatile int g_bench_yield_stop;

/* Single-thread control: yield with NO other ready thread.  reschedule()
 * must take the `next == cur` early-return path, so this measures pure
 * syscall + reschedule overhead with ZERO context switches. */
static void bench_yield_solo(void) {
    TEST("bench: 1k solo yields (no switch)");
    unsigned long long c0 = rdtsc64();
    int                t0 = get_time();
    for (int i = 0; i < 1000; i++)
        thread_yield();
    unsigned long long c1      = rdtsc64();
    int                elapsed = get_time() - t0;
    unsigned long long dc      = c1 - c0;
    printf("(%d yields, %d ticks, cyc_hi=%u cyc_lo=%u) ",
           1000,
           elapsed,
           (unsigned)(dc >> 32),
           (unsigned)(dc & 0xFFFFFFFFu));
    PASS();
}

static void bench_yield_partner(void *arg) {
    (void)arg;
    /* Ping-pong with main until it signals stop, then exit so the
     * test suite is not left hanging forever. */
    g_bench_yield_other_done = 1;
    while (!g_bench_yield_stop)
        thread_yield();
    thread_exit(0);
}

static void bench_yield_100k(void) {
    TEST("bench: 1k yield round-trips");
    g_bench_yield_stop = 0;
    int tid            = thread_create(bench_yield_partner, 0, 10);
    ASSERT(tid > 0, "partner thread_create failed");
    /* Let the partner spin in thread_yield so the scheduler ping-pongs
     * between main and partner on every yield. */
    for (int i = 0; i < 3; i++)
        thread_yield();

    int                t0 = get_time();
    unsigned long long c0 = rdtsc64();
    g_bench_yield_turn    = 0;
    for (int i = 0; i < 1000; i++) {
        g_bench_yield_turn = (g_bench_yield_turn + 1) & 1;
        thread_yield();
    }
    unsigned long long c1      = rdtsc64();
    int                elapsed = get_time() - t0;
    g_bench_yield_stop         = 1;
    /* Give the partner a chance to observe the flag and exit, then join
     * it so the suite's main thread stays alive for later tests. */
    for (int i = 0; i < 4; i++)
        thread_yield();
    int code = -1;
    thread_join(tid, &code);
    /* user printf lacks %llu: print 64-bit cycles as hi/lo 32-bit halves */
    unsigned long long dc = c1 - c0;
    printf("(%d yields, %d ticks, cyc_hi=%u cyc_lo=%u, other_done=%d) ",
           1000,
           elapsed,
           (unsigned)(dc >> 32),
           (unsigned)(dc & 0xFFFFFFFFu),
           (int)g_bench_yield_other_done);
    PASS();
}

static void test_thread_join(void) {
    TEST("thread_join");
    int tid = thread_create(worker_thread, (void *)2L, 10);
    ASSERT(tid > 0, "thread_create failed");
    int exit_code = -999;
    int ret       = thread_join(tid, &exit_code);
    ASSERT(ret == 0, "thread_join failed");
    ASSERT(exit_code == 0, "exit_code should be 0");
    printf("(tid=%d, exit=%d) ", tid, exit_code);
    PASS();
}

static void test_unmap_memory(void) {
    TEST("unmap_memory");
    int mem_cap = cap_create(CAP_TYPE_MEM, RIGHT_WRITE);
    ASSERT(mem_cap > 0, "cap_create for MEM failed");
    void *p = map_memory(mem_cap, 0x30000000, 4096, PROT_READ | PROT_WRITE);
    ASSERT(p != 0, "map_memory returned NULL");
    int ret = unmap_memory(p, 4096);
    ASSERT(ret == 0, "unmap_memory failed");
    PASS();
}

static void test_heap_guard(void) {
    TEST("heap guard pages");
    int mem_cap = cap_create(CAP_TYPE_MEM, RIGHT_WRITE);
    ASSERT(mem_cap > 0, "cap_create for MEM failed");

    /* The heap base is randomized per process (ASLR, design item ⑭);
     * fetch it so the guard addresses match the kernel's layout.
     * LP64: unsigned long is 64-bit, matching sys_call's long args. */
    unsigned long heap_base = (unsigned long)get_heap_base();
    ASSERT(heap_base != 0, "get_heap_base failed");

    unsigned long low_guard  = heap_base - 4096;
    unsigned long high_guard = heap_base + 0x10000000UL; /* + HEAP_USER_SIZE */
    unsigned long below_low  = low_guard - 4096;

    /* Low guard page below the heap base must refuse mapping.
     * Use the raw syscall with 64-bit addresses (heap_base can exceed
     * INT_MAX after randomization). */
    long r = sys_call(SYS_MAP_MEMORY, mem_cap, (long)low_guard, 4096, PROT_READ | PROT_WRITE, 0);
    ASSERT(r == 0, "map_memory into low guard page should fail");

    /* High guard page at the heap max must refuse mapping. */
    r = sys_call(SYS_MAP_MEMORY, mem_cap, (long)high_guard, 4096, PROT_READ | PROT_WRITE, 0);
    ASSERT(r == 0, "map_memory into high guard page should fail");

    /* Unmapping a guard page must also fail (ERR_INVAL) */
    int u = unmap_memory((void *)low_guard, 4096);
    ASSERT(u == ERR_INVAL, "unmap_memory of guard page should fail");

    /* A page just below the low guard still maps fine */
    void *p =
        (void *)sys_call(SYS_MAP_MEMORY, mem_cap, (long)below_low, 4096, PROT_READ | PROT_WRITE, 0);
    ASSERT(p != 0, "map_memory below low guard should succeed");
    if (p)
        unmap_memory(p, 4096);

    PASS();
}

static void test_cap_grant(void) {
    TEST("cap_grant (error paths)");
    int cap = cap_create(CAP_TYPE_MEM, RIGHT_WRITE);
    ASSERT(cap > 0, "cap_create failed");
    /* Grant to nonexistent PID should fail */
    int ret = cap_grant(cap, 999, RIGHT_READ);
    ASSERT(ret < 0, "should fail for nonexistent PID");
    PASS();
}

static void test_cap_revoke(void) {
    TEST("cap_revoke");
    int cap = cap_create(CAP_TYPE_MEM, RIGHT_WRITE);
    ASSERT(cap > 0, "cap_create failed");
    int ret = cap_revoke(cap);
    ASSERT(ret == 0, "cap_revoke failed");
    /* After revoke, map_memory with the same handle should fail */
    void *p = map_memory(cap, 0x20000000, 4096, PROT_READ | PROT_WRITE);
    ASSERT(p == 0, "map_memory should fail after revoke");
    PASS();
}

/* ---- P0 地基: permission-model tests (docs/permission_model.md §十) ---- */

static void test_cap_expiry(void) {
    TEST("cap expiry (lazy revoke on consume)");
    uint64_t now = get_subject() ? (uint64_t)get_time() : 0;
    ASSERT(now > 0, "tick base not running");

    /* Permanent (expiry=0, quota=0 unlimited): consume is a no-op OK. */
    int perm = cap_create_atom(ATOM_DATA_DOCS_READ, RIGHT_READ, 0, 0, 0);
    ASSERT(perm > 0, "cap_create_atom(permanent) failed");
    ASSERT(cap_consume(perm) == 0, "permanent cap consume failed");

    /* Expired in the past: create succeeds, the first consume lazily
     * revokes the entry in place and reports ERR_NOENT.  A repeat
     * consume must also fail (the entry is gone, not a one-shot). */
    int exp = cap_create_atom(ATOM_DATA_DOCS_READ, RIGHT_READ, now - 1, 0, 0);
    ASSERT(exp > 0, "cap_create_atom(expired) failed");
    ASSERT(cap_consume(exp) == ERR_NOENT, "expired cap not revoked");
    ASSERT(cap_consume(exp) == ERR_NOENT, "expired cap resurrected");
    PASS();
}

static void test_cap_quota(void) {
    TEST("cap quota (consume until revoked)");
    int cap = cap_create_atom(ATOM_NET_CONNECT, RIGHT_READ, 0, 2, 0);
    ASSERT(cap > 0, "cap_create_atom(quota) failed");
    ASSERT(cap_consume(cap) == 0, "consume #1 failed");
    /* Second use drops quota 2 -> 0: entry revoked in place. */
    ASSERT(cap_consume(cap) == 0, "consume #2 failed");
    ASSERT(cap_consume(cap) == ERR_NOENT, "consume past quota succeeded");
    ASSERT(cap_consume(cap) == ERR_NOENT, "revoked cap resurrected");
    PASS();
}

static void test_cap_revoke_by_atom(void) {
    TEST("cap_revoke_by_atom (atom + scope)");
    uint64_t subj = get_subject();
    ASSERT(subj >= 1, "subject < 1");

    int a1    = cap_create_atom(ATOM_DATA_DL_WRITE, RIGHT_WRITE, 0, 0, 0);
    int a2    = cap_create_atom(ATOM_DATA_DL_WRITE, RIGHT_WRITE, 0, 0, 0);
    int other = cap_create_atom(ATOM_NET_BIND, RIGHT_READ, 0, 0, 0);
    ASSERT(a1 > 0 && a2 > 0 && other > 0, "cap_create_atom failed");

    /* Revoke by atom (scope 0 = any scope): both DL_WRITE caps die,
     * the different-atom cap survives. */
    int n = cap_revoke_by_atom(subj, ATOM_DATA_DL_WRITE, 0);
    ASSERT(n >= 2, "revoke_by_atom count < 2");
    ASSERT(cap_consume(a1) == ERR_NOENT, "a1 survived revoke");
    ASSERT(cap_consume(a2) == ERR_NOENT, "a2 survived revoke");
    ASSERT(cap_consume(other) == 0, "different atom revoked");

    /* Scope restriction: only matching scope_hash is revoked. */
    int s1 = cap_create_atom(ATOM_NET_WIFI_SCAN, RIGHT_READ, 0, 0, 0xAAAA);
    int s2 = cap_create_atom(ATOM_NET_WIFI_SCAN, RIGHT_READ, 0, 0, 0xAAAA);
    int s3 = cap_create_atom(ATOM_NET_WIFI_SCAN, RIGHT_READ, 0, 0, 0xBBBB);
    ASSERT(s1 > 0 && s2 > 0 && s3 > 0, "scope cap create failed");

    n = cap_revoke_by_atom(subj, ATOM_NET_WIFI_SCAN, 0xBBBB);
    ASSERT(n == 1, "scope revoke count != 1");
    ASSERT(cap_consume(s3) == ERR_NOENT, "s3 survived scope revoke");
    ASSERT(cap_consume(s1) == 0, "s1 wrongly revoked");
    ASSERT(cap_consume(s2) == 0, "s2 wrongly revoked");

    /* Unmatched atom / wrong subject revoke nothing.  (ATOM_SYS_DEBUG
     * is deliberately NOT used here: init holds it — seeded for the
     * SYS_PANIC gate — so it would match.) */
    n = cap_revoke_by_atom(subj, ATOM_HW_CAMERA_CAPTURE, 0);
    ASSERT(n == 0, "unknown atom revoked something");
    n = cap_revoke_by_atom(subj + 1, ATOM_NET_WIFI_SCAN, 0);
    ASSERT(n == 0, "wrong subject revoked something");

    printf("(revoked=%d) ", n);
    PASS();
}

static void test_ipc_send_recv(void) {
    TEST("ipc_send + ipc_recv");
    int port = ipc_port_create();
    ASSERT(port > 0, "port_create failed");

    g_ipc_recv_port = port;
    g_ipc_recv_ret  = -1;

    /* Spawn a receiver thread */
    int tid = thread_create(ipc_recv_worker, 0, 10);
    ASSERT(tid > 0, "thread_create for recv worker failed");

    /* Yield to let receiver block on ipc_recv */
    for (int i = 0; i < 3; i++)
        thread_yield();

    /* Send a message */
    const char *msg = "hello";
    int         ret = ipc_send(port, msg, 5);
    ASSERT(ret == 0, "ipc_send failed");

    /* Wait for receiver to finish */
    int exit_code = -1;
    thread_join(tid, &exit_code);

    ASSERT(g_ipc_recv_ret == 0, "recv inside worker failed");
    ASSERT(g_ipc_recv_len == 5, "received wrong length");
    ASSERT(g_ipc_recv_buf[0] == 'h', "received wrong data");
    ASSERT(g_ipc_recv_buf[4] == 'o', "received wrong data");
    PASS();
}

static void test_subject_identity(void) {
    TEST("subject identity + unforgeable sender (ipc_recv_from)");
    uint64_t subj = get_subject();
    ASSERT(subj >= 1, "subject < 1");
    ASSERT(get_subject() == subj, "subject not stable");

    /* Self-send: the kernel fills sender_subject from the PCB, so a
     * forged subject claimed inside the payload must NOT leak through
     * to the receiver (docs/permission_model.md §三). */
    int port = ipc_port_create();
    ASSERT(port > 0, "port_create failed");
    g_recvfrom_port = port;
    g_recvfrom_ret  = -1;
    g_recvfrom_subj = 0;

    int tid = thread_create(ipc_recvfrom_worker, 0, 10);
    ASSERT(tid > 0, "recvfrom worker create failed");
    for (int i = 0; i < 3; i++)
        thread_yield();

    uint64_t fake = 0xF00DF00DF00DF00DULL; /* forged subject claim */
    ASSERT(ipc_send(port, &fake, sizeof(fake)) == 0, "ipc_send failed");

    int exit_code = -1;
    thread_join(tid, &exit_code);
    ASSERT(g_recvfrom_ret == 0, "ipc_recv_from failed");
    ASSERT(g_recvfrom_len == (int)sizeof(fake), "wrong length");
    ASSERT(*(uint64_t *)g_recvfrom_buf == fake, "payload corrupted");
    ASSERT(g_recvfrom_subj == subj, "sender_subject != real subject");
    ASSERT(g_recvfrom_subj != fake, "forged subject leaked through");
    printf("(subject=%llu) ", (unsigned long long)subj);
    PASS();
}

static void test_ipc_call_err(void) {
    TEST("ipc_call (error paths)");
    /* Call to nonexistent port should fail */
    const char *req = "req";
    char        resp[16];
    int         resp_len = sizeof(resp);
    int         ret      = ipc_call(9999, req, 3, resp, &resp_len);
    ASSERT(ret < 0, "should fail for nonexistent port");
    PASS();
}

/*
 * Crash recovery (production hardening): a client blocked in ipc_call
 * on a port whose owner process dies must be woken with ERR_NOENT —
 * never hang.  process_reap() → ipc_cleanup_process() destroys the
 * dead process's ports (waking all blocked peers) and frees its
 * registry names so a restarted service can re-register them.
 */
static void test_ipc_peer_death(void) {
    TEST("ipc_call to dead peer wakes with ERR_NOENT");
    static char blob[131072]; /* must hold crashpeer.elf */
    int size = blob_get("crashpeer", blob, sizeof(blob));
    ASSERT(size > 0, "blob_get(crashpeer) failed");

    int pid = process_create("crashpeer", blob, size);
    ASSERT(pid > 0, "process_create(crashpeer) failed");

    /* Wait for the helper to register its port (bounded poll). */
    int port = 0;
    for (int i = 0; i < 200 && port <= 0; i++) {
        port = port_get("crashpeer");
        if (port <= 0)
            thread_yield();
    }
    ASSERT(port > 0, "crashpeer port never registered");

    /* The call blocks until crashpeer receives it and then exits
     * WITHOUT replying (simulated crash mid-call).  The kernel must
     * wake us with ERR_NOENT, not leave us blocked forever. */
    const char req[] = "ping";
    char       resp[16];
    int        resp_len = (int)sizeof(resp);
    int        ret      = ipc_call(port, req, 4, resp, &resp_len);
    printf("(call_ret=%d) ", ret);
    ASSERT(ret == ERR_NOENT, "call did not fail with ERR_NOENT (hang?)");

    /* The registry name must be freed too: a fresh spawn can re-own it.
     * (The port NUMBER may be reused — slots are table indices.) */
    int pid2 = process_create("crashpeer", blob, size);
    ASSERT(pid2 > 0, "second process_create failed");
    int port2 = 0;
    for (int i = 0; i < 200 && port2 <= 0; i++) {
        port2 = port_get("crashpeer");
        if (port2 <= 0)
            thread_yield();
    }
    ASSERT(port2 > 0, "crashpeer name not re-registrable");

    /* Let the second helper die too (call -> receives -> exits without
     * replying -> we wake ERR_NOENT again); no blocked test peer left. */
    char resp2[16];
    int  rlen2 = (int)sizeof(resp2);
    int  ret2  = ipc_call(port2, req, 4, resp2, &rlen2);
    ASSERT(ret2 == ERR_NOENT, "second peer-death wake failed");
    PASS();
}

static void test_set_affinity(void) {
    TEST("set_affinity");
    /* Set affinity of init thread (tid=1) to CPU 0 */
    int ret = thread_set_affinity(1, 0);
    ASSERT(ret == 0, "set_affinity to cpu0 failed");
    PASS();
}

/* ---- Roadmap P1: vspace_alloc smoke test ---- */

static void test_vspace_smoke(void) {
    TEST("vspace_alloc (alloc + map + pattern + ERR_INVAL)");

    /* Allocate 16 KB (4 pages) of virtual address space.  The kernel
     * returns a page-aligned base at or above the 1 GiB scan floor. */
    void *base = vspace_alloc(0x4000, 0);
    ASSERT(base != 0, "vspace_alloc returned NULL");
    ASSERT(((unsigned long)base & 0xFFF) == 0, "base not page-aligned");
    ASSERT((unsigned long)base >= 0x40000000UL, "base below 1 GiB floor");

    /* Map 4 pages into the reserved range and touch them. */
    int mem_cap = cap_create(CAP_TYPE_MEM, RIGHT_WRITE);
    ASSERT(mem_cap > 0, "cap_create for MEM failed");

    void *mapped = map_memory(mem_cap, (unsigned long)base, 0x4000, PROT_READ | PROT_WRITE);
    ASSERT(mapped != 0, "map_memory into vspace failed");
    ASSERT((unsigned long)mapped == (unsigned long)base, "map_memory returned wrong address");

    volatile unsigned char *p = (volatile unsigned char *)base;
    for (int i = 0; i < 0x4000; i++)
        p[i] = (unsigned char)(i * 31 + 7);

    int ok = 1;
    for (int i = 0; i < 0x4000; i++) {
        if (p[i] != (unsigned char)(i * 31 + 7)) {
            ok = 0;
            break;
        }
    }
    ASSERT(ok, "vspace pattern mismatch");

    /* Unmap and verify cleanup works. */
    int ur = unmap_memory(base, 0x4000);
    ASSERT(ur == 0, "unmap_memory of vspace failed");

    /* Error paths: misaligned size and nonzero flags both -> ERR_INVAL. */
    void *bad = vspace_alloc(0x4001, 0);
    ASSERT(bad == (void *)(long)ERR_INVAL, "misaligned size accepted");
    bad = vspace_alloc(0x4000, 1);
    ASSERT(bad == (void *)(long)ERR_INVAL, "nonzero flags accepted");

    printf("(base=0x%x, 4 pages) ", (unsigned int)(unsigned long)base);
    PASS();
}

/* ---- Roadmap P2: thread_set_ctx smoke test ---- */

static void test_thread_set_ctx(void) {
    TEST("thread_set_ctx (valid + error paths)");

    test_thread_ctx_t ctx;
    ctx.rsp    = 0x1000;
    ctx.rbx    = 1;
    ctx.rbp    = 2;
    ctx.r12    = 3;
    ctx.r13    = 4;
    ctx.r14    = 5;
    ctx.r15    = 6;
    ctx.rflags = 0x202;
    ctx.rip    = 0x400000;

    /* Valid: setting the context of our own main thread (tid=1) is
     * permitted (a running thread's saved slot is dead data until the
     * next switch away, so the write is a harmless no-op in effect). */
    int ret = thread_set_ctx(1, &ctx, sizeof(ctx));
    ASSERT(ret == OK, "set_ctx on own tid failed");

    /* Wrong ctx_size -> ERR_INVAL */
    ret = thread_set_ctx(1, &ctx, sizeof(ctx) + 1);
    ASSERT(ret == ERR_INVAL, "wrong ctx_size accepted");

    /* Negative tid -> ERR_INVAL */
    ret = thread_set_ctx(-1, &ctx, sizeof(ctx));
    ASSERT(ret == ERR_INVAL, "negative tid accepted");

    /* Unknown tid -> ERR_NOENT */
    ret = thread_set_ctx(9999, &ctx, sizeof(ctx));
    ASSERT(ret == ERR_NOENT, "unknown tid accepted");

    PASS();
}

/* ---- Stress: 1000 concurrent threads ---- */

static void test_stress_threads_1000(void) {
    TEST("stress: 1000 threads");

    g_stress_counter = 0;
    g_stress_mutex   = mutex_create();
    ASSERT(g_stress_mutex > 0, "mutex_create failed");

    /* Create STRESS_THREADS threads; each bumps the shared counter
     * under the mutex and exits.  This is the core requirement: the
     * kernel must support >= 1000 live threads in one process.
     *
     * We process threads in BATCHES so peak memory usage is limited:
     * each thread needs ~10 pages (8k stack + 1 user stack + ~1 page
     * table overhead).  After a batch is joined, its pages are
     * returned to the PMM before the next batch starts. */
    static int tids[STRESS_THREADS]; /* static: init stack is 1 page */
    int        total_created = 0;
    int        max_threads   = STRESS_THREADS;

    /* Reserve enough memory for the manager process + shell + term
     * services that must boot after this test.  A process_create with
     * ~40KB ELF needs ~4 user pages + 9 pages for main thread + a few
     * page-table pages.  128 pages reserves ~512KB which is ample. */
    {
        int free_pages = get_free_pages();
        int reserve    = 128;
        int available  = free_pages - reserve;
        /* Each thread costs ~10 pages (8 kstack + 1 user stack + 1 pgtbl).
         * To stay safe, assume 12 pages per thread in peak usage. */
        int per_thread_cost = 12;
        int max_concurrent  = available / per_thread_cost;
        if (max_concurrent <= 0) {
            printf("(low-memory: skipping, only %d pages free) ", free_pages);
            mutex_destroy(g_stress_mutex);
            PASS();
            return;
        }
        if (max_threads > max_concurrent) {
            printf("(low-memory: capping to %d threads, %d reserve) ", max_concurrent, reserve);
            max_threads = max_concurrent;
        }
    }

    /* Process in batches of 32 to limit peak concurrent memory. */
    const int BATCH_SIZE = 32;
    int       i          = 0;
    while (i < max_threads) {
        int batch_count   = (max_threads - i < BATCH_SIZE) ? (max_threads - i) : BATCH_SIZE;
        int batch_created = 0;

        for (int j = 0; j < batch_count; j++) {
            int tid = thread_create(stress_worker, 0, 10);
            if (tid <= 0) {
                /* Cleanup: join already-created threads in this batch */
                for (int k = 0; k < batch_created; k++) {
                    int ec;
                    (void)thread_join(tids[total_created + k], &ec);
                }
                /* Cleanup: join all previous batches too */
                for (int k = 0; k < total_created; k++) {
                    int ec;
                    (void)thread_join(tids[k], &ec);
                }
                mutex_destroy(g_stress_mutex);
                printf("FAIL: thread_create #%d returned %d\n", i + j, tid);
                FAIL("thread_create failed in stress");
                return;
            }
            tids[total_created + batch_created] = tid;
            batch_created++;
        }

        /* Join this batch before creating the next one.  This returns
         * all kernel/user-stack pages to the PMM, so the next batch
         * can allocate from the same pool. */
        for (int j = 0; j < batch_created; j++) {
            int ec;
            int ret = thread_join(tids[total_created + j], &ec);
            if (ret != 0) {
                printf("FAIL: join #%d (tid=%d) ret=%d\n",
                       total_created + j,
                       tids[total_created + j],
                       ret);
                /* Join remaining to avoid leaks */
                for (int k = j + 1; k < batch_created; k++) {
                    int ec2;
                    (void)thread_join(tids[total_created + k], &ec2);
                }
                /* Join all previous batches */
                for (int k = 0; k < total_created; k++) {
                    int ec2;
                    (void)thread_join(tids[k], &ec2);
                }
                mutex_destroy(g_stress_mutex);
                FAIL("thread_join failed in stress");
                return;
            }
        }

        total_created += batch_created;
        i += batch_count;
    }

    /* Final assertion: all threads bumped the counter. */
    ASSERT(g_stress_counter == max_threads, "shared counter != expected count (lost increments?)");
    printf("(counter=%d) ", g_stress_counter);

    mutex_destroy(g_stress_mutex);
    PASS();
}

/* ---- Stress: 100k IPC round-trips ---- */

static void test_stress_ipc_100k(void) {
    TEST("stress: 100k IPC round-trips");

    int port = ipc_port_create();
    ASSERT(port > 0, "port_create failed");
    g_ipc_stress_port = port;
    g_ipc_stress_fail = 0;

    /* Server thread: recv + reply STRESS_IPC_CALLS times. */
    int stid = thread_create(ipc_stress_server, 0, 10);
    ASSERT(stid > 0, "server thread_create failed");

    /* Let the server block in ipc_recv first (matches the established
     * send/recv test pattern). */
    for (int i = 0; i < 3; i++)
        thread_yield();

    /* Client: send a sequence number, expect the echo back. */
    int t0 = get_time();
    for (int i = 0; i < STRESS_IPC_CALLS; i++) {
        int req      = i;
        int resp     = -1;
        int resp_len = sizeof(resp);
        int ret      = ipc_call(port, &req, sizeof(req), &resp, &resp_len);
        if (ret != 0 || resp != i) {
            printf("FAIL: call #%d ret=%d resp=%d\n", i, ret, resp);
            g_ipc_stress_fail = 1;
            break;
        }
    }
    int elapsed = get_time() - t0;

    int exit_code = -1;
    thread_join(stid, &exit_code);

    ASSERT(g_ipc_stress_fail == 0, "IPC stress failed");
    ASSERT(exit_code == 0, "server exited non-zero");
    printf("(%d calls, %d ticks) ", STRESS_IPC_CALLS, elapsed);
    PASS();
}

/* ---- FPU/SSE state save/restore across context switches ----
 * Two threads interleave SSE2 double-precision series with yields.
 * Without fpu_switch (fxsave/fxrstor) on context_switch, the XMM
 * registers of one worker clobber the other's live series and the
 * exactness checks below fail.  Sum-of-squares is exact in doubles
 * (n(n+1)(2n+1)/6, all terms < 2^53), so == is the right check. */

static volatile int g_fpu_stage;
static volatile int g_fpu_a_fail;
static volatile int g_fpu_b_fail;

static void fpu_worker_a(void *arg) {
    (void)arg;
    volatile double acc = 0.0;
    for (int i = 1; i <= 400; i++) {
        acc += (double)i * (double)i; /* SSE2: live in XMM across yield */
        if ((i & 0x3F) == 0) {
            thread_yield();
            double expect =
                (double)i * (double)(i + 1) * (double)(2 * i + 1) / 6.0;
            if (acc != expect)
                g_fpu_a_fail = 1;
        }
    }
    g_fpu_stage++;
    thread_exit(0);
}

static void fpu_worker_b(void *arg) {
    (void)arg;
    volatile double acc = 1.0;
    for (int i = 1; i <= 400; i++) {
        acc *= 1.0001; /* different pattern, different XMM values */
        if ((i & 0x3F) == 0) {
            thread_yield();
            if (!(acc > 1.0)) /* monotonic series must stay > 1 */
                g_fpu_b_fail = 1;
        }
    }
    g_fpu_stage++;
    thread_exit(0);
}

static void test_fpu_sse_switch(void) {
    TEST("FPU/SSE state across context switches");
    g_fpu_stage = 0;
    g_fpu_a_fail = 0;
    g_fpu_b_fail = 0;
    int ta = thread_create(fpu_worker_a, 0, 10);
    int tb = thread_create(fpu_worker_b, 0, 10);
    ASSERT(ta > 0 && tb > 0, "thread_create failed");
    thread_join(ta, NULL);
    thread_join(tb, NULL);
    ASSERT(g_fpu_stage == 2, "workers did not finish");
    ASSERT(g_fpu_a_fail == 0, "worker A result corrupted");
    ASSERT(g_fpu_b_fail == 0, "worker B result corrupted");
    PASS();
}

static void run_tests(void) {
    printf("=== Syscall Tests ===\n");
    test_debug_log();
    test_yield();
    test_port_create();
    test_port_register_get();
    test_port_get_nonexistent();
    test_cap_create();
    test_map_memory();
    test_get_pid();
    test_get_free_pages();
    test_thread_create();
    test_get_time();
    test_sleep();
    test_thread_join();
    test_fpu_sse_switch();
    test_unmap_memory();
    test_heap_guard();
    test_cap_grant();
    test_cap_revoke();
    test_cap_expiry();
    test_cap_quota();
    test_cap_revoke_by_atom();
    test_ipc_send_recv();
    test_subject_identity();
    test_ipc_call_err();
    test_ipc_peer_death();
    test_set_affinity();
    bench_syscall_100k();
    bench_yield_solo();
    bench_yield_100k();
    test_vspace_smoke();
    test_thread_set_ctx();
    test_stress_threads_1000();
    test_stress_ipc_100k();
    printf("=== Results: %d/%d passed ===\n", tests_pass, tests_run);
}

/* ---- Phase 2: Init protocol ---- */

static void init_protocol(void) {
    printf("\n=== Init Protocol ===\n");

    /* Step 1: Query kernel state */
    int pid = get_pid();
    printf("  init: PID=%d\n", pid);

    int free = get_free_pages();
    printf("  init: free memory=%d pages (%d MB)\n", free, free / 256);

    /* Step 2: Create a well-known IPC port for services */
    int port = ipc_port_create();
    printf("  init: IPC port=%d\n", port);

    int ret = port_register("init", port);
    printf("  init: register 'init' -> %d\n", ret);

    /* Step 3: Spawn a worker to prove multi-thread works */
    printf("  init: spawning worker thread...\n");
    int tid = thread_create(worker_thread, (void *)1L, 10);
    printf("  init: worker tid=%d\n", tid);

    /* Yield several times so worker gets CPU */
    for (int i = 0; i < 5; i++)
        thread_yield();

    if (tid > 0) {
        int ec;
        thread_join(tid, &ec);
    }

    printf("  init: system ready\n");
    printf("=== Init Protocol Done ===\n");
}

/* ====================================================================
 * P1 地基: permission role engine — LIVE vfs_server + perm-manager
 * (docs/permission_model.md §二/§四).  This process (kernel subject 1)
 * plays the sandboxed fake "app": every request goes through real IPC
 * to vfs_server (CREATE_BOOKMARK on the mounted System volume) and
 * perm-manager (roles, rules, grants, Powerbox).  Separate counters so
 * the classic syscall suite (run_tests) stays untouched.
 * ==================================================================== */

static int p1_run  = 0;
static int p1_pass = 0;

#define P1_TEST(name)                  \
    do {                               \
        printf("  P1: %s ... ", name); \
        p1_run++;                      \
    } while (0)

#define P1_PASS()         \
    do {                  \
        p1_pass++;        \
        printf("PASS\n"); \
    } while (0)

#define P1_FAIL(msg)               \
    do {                           \
        printf("FAIL: %s\n", msg); \
    } while (0)

#define P1_ASSERT(cond, msg) \
    do {                     \
        if (!(cond)) {       \
            P1_FAIL(msg);    \
            return;          \
        }                    \
    } while (0)

/* Test item: a kernel blob ELF on the read-only System volume.  It
 * exists as soon as fs_mem_driver seeds the volume. */
#define P1_ITEM_PATH "System/Kernel/init.elf"

/* The perm-engine grants/revokes by RESOURCE (volume UUID + itemID).
 * Stored from the first successful CREATE_BOOKMARK so later GRANT /
 * REVOKE ops hit the same (app, resource) key as the live checks. */
static vfs_resource_t g_p1_res;

/* Poll a named service port until it is up (services start in parallel
 * after the manager spawn; perm/vfs may lag behind this thread). */
static int p1_port_get(const char *name) {
    for (int i = 0; i < 200; i++) {
        int p = port_get(name);
        if (p >= 0)
            return p;
        sleep(10);
    }
    return -1;
}

/* One CREATE_BOOKMARK against the live vfs_server.  Returns resp.ret
 * (0 = authorized, VFS_ERR_ACCESS = denied, ERR_* = transport/parse). */
static int p1_create_bookmark(int vfs_port, u32 access, vfs_resp_create_bookmark_t *resp) {
    vfs_req_create_bookmark_t req;
    memset(&req, 0, sizeof(req));
    req.op = VFS_OP_CREATE_BOOKMARK;
    strncpy(req.path, P1_ITEM_PATH, sizeof(req.path) - 1);
    req.access = access;
    int rlen   = (int)sizeof(*resp);
    int r      = ipc_call(vfs_port, &req, (int)sizeof(req), resp, &rlen);
    if (r < 0)
        return r;
    return resp->ret;
}

/* Retry until the volume is mounted AND the first check passes (the
 * System volume seeds async, so the very first calls can hit ERR_NOENT
 * or an unreachable perm port).  The perm engine is a prerequisite for
 * ANY bookmark check (vfs lazy-resolves "perm" on the first op), so
 * wait for its port explicitly FIRST — the manager spawns perm before
 * the block-device driver, but a degraded driver must never be able to
 * starve this suite out of its bookmark budget. */
static int p1_wait_ready(int vfs_port) {
    if (p1_port_get("perm") < 0)
        return -1;
    for (int i = 0; i < 200; i++) {
        vfs_resp_create_bookmark_t resp;
        if (p1_create_bookmark(vfs_port, VFS_ACCESS_WRITE, &resp) == 0) {
            /* OWNER chain allows WRITE (§二.2) — the check passed. */
            vfs_bookmark_t blob;
            memcpy(&blob, resp.data, sizeof(blob));
            g_p1_res = blob.resource;
            return 0;
        }
        sleep(10);
    }
    return -1;
}

/* ---- P1 test 1: subject identity through the service layer ---- */

static void test_p1_whoami(void) {
    P1_TEST("subject identity: get_subject + vfs WHOAMI");
    uint64_t subj = get_subject();
    /* Deterministic kernel numbering (kernel/process/process.c):
     * init is the first user process → subject 1.  The perm-engine
     * bootstraps init as OWNER on exactly this invariant. */
    P1_ASSERT(subj == 1, "init subject != 1 (kernel numbering changed?)");

    int vfs_port = p1_port_get("vfs");
    P1_ASSERT(vfs_port > 0, "vfs port unavailable");

    /* WHOAMI (VFS_OP_WHOAMI) proxies SYS_GET_SUBJECT through the
     * trusted server; the subject comes from vfs's ipc_recv_from,
     * never from the request bytes — unforgeable. */
    vfs_req_whoami_t req;
    memset(&req, 0, sizeof(req));
    req.op = VFS_OP_WHOAMI;
    vfs_resp_whoami_t resp;
    int               rlen = (int)sizeof(resp);
    P1_ASSERT(ipc_call(vfs_port, &req, (int)sizeof(req), &resp, &rlen) == 0,
              "WHOAMI transport failed");
    P1_ASSERT(resp.ret == 0, "WHOAMI ret != 0");
    P1_ASSERT(resp.subject_id == subj, "WHOAMI subject != get_subject()");
    P1_ASSERT(resp.subject_id == 1, "service-layer subject mismatch (init=1)");
    P1_PASS();
}

/* ---- P1 test 2: OWNER chain auto-allow (rule chain, §四.2) ---- */

static void test_p1_owner_auto_allow(void) {
    P1_TEST("OWNER chain auto-allows WRITE on System (no Powerbox)");
    int vfs_port = p1_port_get("vfs");
    P1_ASSERT(vfs_port > 0, "vfs port unavailable");

    /* Blocks until the volume is mounted + perm reachable; the WRITE
     * check passes because init (subject 1) is seeded OWNER and the
     * OWNER chain ALLOWs ATOM_DATA_DOCS_WRITE.  g_p1_res is captured
     * for the GRANT/REVOKE tests below. */
    P1_ASSERT(p1_wait_ready(vfs_port) == 0,
              "WRITE bookmark never auto-allowed (OWNER seed broken?)");
    P1_ASSERT(g_p1_res.vol.hi != 0 || g_p1_res.vol.lo != 0, "captured resource empty");
    P1_PASS();
}

/* ---- P1 test 3: ROLE_SET hot reload (management plane) ---- */

static void test_p1_role_set_hot_reload(void) {
    P1_TEST("ROLE_SET init→GUEST + live hot reload");
    int perm_port = p1_port_get("perm");
    P1_ASSERT(perm_port > 0, "perm port unavailable");

    /* init is OWNER (bootstrap) → management plane → allowed. */
    perm_req_role_set_t req;
    memset(&req, 0, sizeof(req));
    req.op         = PERM_OP_ROLE_SET;
    req.subject_id = get_subject(); /* 1 = init */
    req.role       = PERM_ROLE_GUEST;
    perm_resp_role_set_t resp;
    int                  rlen = (int)sizeof(resp);
    P1_ASSERT(ipc_call(perm_port, &req, (int)sizeof(req), &resp, &rlen) == 0,
              "ROLE_SET transport failed");
    P1_ASSERT(resp.ret == 0, "ROLE_SET denied (init not OWNER?)");
    P1_ASSERT(resp.role == PERM_ROLE_GUEST, "role not applied");

    /* Hot reload proof: the SAME WRITE request that OWNER auto-allowed
     * is now denied by the GUEST chain — the engine switched live. */
    int                        vfs_port = p1_port_get("vfs");
    vfs_resp_create_bookmark_t cr;
    P1_ASSERT(p1_create_bookmark(vfs_port, VFS_ACCESS_WRITE, &cr) == VFS_ERR_ACCESS,
              "WRITE not denied after demotion to GUEST");
    P1_PASS();
}

/* ---- P1 test 4: chain DENY is a policy verdict, not a prompt ---- */

static void test_p1_chain_deny_no_prompt(void) {
    P1_TEST("GUEST chain DENY (READ) raises NO Powerbox prompt");
    int vfs_port  = p1_port_get("vfs");
    int perm_port = p1_port_get("perm");
    P1_ASSERT(vfs_port > 0 && perm_port > 0, "ports unavailable");

    /* GUEST chain DENYs ATOM_DATA_DOCS_READ. */
    vfs_resp_create_bookmark_t cr;
    P1_ASSERT(p1_create_bookmark(vfs_port, VFS_ACCESS_READ, &cr) == VFS_ERR_ACCESS,
              "READ not denied for GUEST");

    /* A chain DENY is a POLICY verdict: no pending query is created
     * (QUERY returns ERR_NOENT) — the denial never reaches the user. */
    perm_req_query_t q;
    memset(&q, 0, sizeof(q));
    q.op = PERM_OP_QUERY;
    perm_resp_query_t qr;
    int               rlen = (int)sizeof(qr);
    P1_ASSERT(ipc_call(perm_port, &q, (int)sizeof(q), &qr, &rlen) == 0, "QUERY transport failed");
    P1_ASSERT(qr.ret == ERR_NOENT, "chain-deny leaked a Powerbox prompt");
    P1_PASS();
}

/* ---- P1 test 5: default deny → Powerbox → ANSWER allow ---- */

static void test_p1_powerbox_allow(void) {
    P1_TEST("default-deny → Powerbox → ANSWER allow → grant");
    int vfs_port  = p1_port_get("vfs");
    int perm_port = p1_port_get("perm");
    P1_ASSERT(vfs_port > 0 && perm_port > 0, "ports unavailable");

    /* EXEC has no GUEST chain rule → default deny → Powerbox prompt
     * (pending query + UI_SHOW push to term). */
    vfs_resp_create_bookmark_t cr;
    P1_ASSERT(p1_create_bookmark(vfs_port, VFS_ACCESS_EXEC, &cr) == VFS_ERR_ACCESS,
              "EXEC not denied by default");

    perm_req_query_t q;
    memset(&q, 0, sizeof(q));
    q.op = PERM_OP_QUERY;
    perm_resp_query_t qr;
    int               rlen = (int)sizeof(qr);
    P1_ASSERT(ipc_call(perm_port, &q, (int)sizeof(q), &qr, &rlen) == 0 && qr.ret == 0,
              "pending query not created");
    P1_ASSERT(qr.state == PERM_QUERY_PENDING, "query not PENDING");
    P1_ASSERT(qr.subject_id == get_subject(), "query subject != real subject");
    P1_ASSERT(qr.label[0] != '\0', "query label empty");

    /* User says yes: grant upserted (+ atom decision encoded). */
    perm_req_answer_t a;
    memset(&a, 0, sizeof(a));
    a.op       = PERM_OP_ANSWER;
    a.query_id = qr.query_id;
    a.allow    = 1;
    perm_resp_answer_t ar;
    rlen = (int)sizeof(ar);
    P1_ASSERT(ipc_call(perm_port, &a, (int)sizeof(a), &ar, &rlen) == 0 && ar.ret == 0,
              "ANSWER failed");

    /* Grant beat (§四.1): the retry succeeds without any prompt. */
    P1_ASSERT(p1_create_bookmark(vfs_port, VFS_ACCESS_EXEC, &cr) == 0,
              "EXEC not granted after ANSWER");
    P1_PASS();
}

/* ---- P1 test 6: REVOKE restores default deny ---- */

static void test_p1_revoke(void) {
    P1_TEST("REVOKE drops grants → default deny again");
    int vfs_port  = p1_port_get("vfs");
    int perm_port = p1_port_get("perm");
    P1_ASSERT(vfs_port > 0 && perm_port > 0, "ports unavailable");

    perm_req_revoke_t r;
    memset(&r, 0, sizeof(r));
    r.op         = PERM_OP_REVOKE;
    r.subject_id = get_subject(); /* revoke init's own grants */
    perm_resp_revoke_t rr;
    int                rlen = (int)sizeof(rr);
    P1_ASSERT(ipc_call(perm_port, &r, (int)sizeof(r), &rr, &rlen) == 0, "REVOKE transport failed");
    P1_ASSERT(rr.revoked >= 1, "nothing revoked");

    vfs_resp_create_bookmark_t cr;
    P1_ASSERT(p1_create_bookmark(vfs_port, VFS_ACCESS_EXEC, &cr) == VFS_ERR_ACCESS,
              "EXEC still granted after REVOKE");
    P1_PASS();
}

/* ---- P1 test 7: explicit grant beats the role default (§四.1) ---- */

static void test_p1_grant_beats_role(void) {
    P1_TEST("GRANT beats role default (GUEST denies READ)");
    int vfs_port  = p1_port_get("vfs");
    int perm_port = p1_port_get("perm");
    P1_ASSERT(vfs_port > 0 && perm_port > 0, "ports unavailable");

    /* Direct grant (test/management surface): READ on the captured
     * resource, signed with atom ATOM_DATA_DOCS_READ into subject 1's
     * kernel table (decision encoding, §四). */
    perm_req_grant_t g;
    memset(&g, 0, sizeof(g));
    g.op         = PERM_OP_GRANT;
    g.resource   = g_p1_res;
    g.access     = VFS_ACCESS_READ;
    g.subject_id = get_subject();
    g.atom       = ATOM_DATA_DOCS_READ;
    perm_resp_grant_t gr;
    int               rlen = (int)sizeof(gr);
    P1_ASSERT(ipc_call(perm_port, &g, (int)sizeof(g), &gr, &rlen) == 0 && gr.ret == 0,
              "GRANT failed");

    /* The grant beats the GUEST DENY chain: READ is allowed now. */
    vfs_resp_create_bookmark_t cr;
    P1_ASSERT(p1_create_bookmark(vfs_port, VFS_ACCESS_READ, &cr) == 0,
              "READ denied despite explicit grant (grant-beat broken?)");
    P1_PASS();
}

/* ---- P1 test 8: ROLE_SET is management-plane only ---- */

static void test_p1_role_set_denied(void) {
    P1_TEST("ROLE_SET denied for non-management (GUEST)");
    int perm_port = p1_port_get("perm");
    P1_ASSERT(perm_port > 0, "perm port unavailable");

    /* init is GUEST now — not management → cannot change policy
     * (no self-repromotion either). */
    perm_req_role_set_t req;
    memset(&req, 0, sizeof(req));
    req.op         = PERM_OP_ROLE_SET;
    req.subject_id = get_subject();
    req.role       = PERM_ROLE_OWNER; /* try to repromote */
    perm_resp_role_set_t resp;
    int                  rlen = (int)sizeof(resp);
    P1_ASSERT(ipc_call(perm_port, &req, (int)sizeof(req), &resp, &rlen) == 0,
              "ROLE_SET transport failed");
    P1_ASSERT(resp.ret == ERR_DENIED, "non-management ROLE_SET accepted");
    P1_PASS();
}

/* ---- P1 test 9: policy export (DUMP) ---- */

static void test_p1_dump(void) {
    P1_TEST("DUMP exports role map + rule table");
    int perm_port = p1_port_get("perm");
    P1_ASSERT(perm_port > 0, "perm port unavailable");

    perm_req_dump_t d;
    memset(&d, 0, sizeof(d));
    d.op = PERM_OP_DUMP;
    perm_resp_dump_t dr;
    int              rlen = (int)sizeof(dr);
    P1_ASSERT(ipc_call(perm_port, &d, (int)sizeof(d), &dr, &rlen) == 0 && dr.ret == 0,
              "DUMP failed");
    P1_ASSERT(dr.role_count >= 2, "role map empty");
    P1_ASSERT(dr.rule_count >= 1, "rule table empty");

    int found_guest = 0, found_rule = 0;
    for (int i = 0; i < 8; i++) {
        /* init (subject 1) was hot-reloaded to GUEST by test 3, and
         * test 8's repromote to OWNER was denied — so subject 1 must
         * now carry PERM_ROLE_GUEST (=4) in the exported role map. */
        if (strstr(dr.lines[i], "subject=1 role=4") != 0)
            found_guest = 1;
        if (strncmp(dr.lines[i], "rule:", 5) == 0)
            found_rule = 1;
    }
    P1_ASSERT(found_guest, "init role not GUEST in dump");
    P1_ASSERT(found_rule, "no rule lines in dump");
    printf("(roles=%u rules=%u) ", dr.role_count, dr.rule_count);
    P1_PASS();
}

/* ---- P1 test 10: subject-targeted kernel issuance ---- */

static void test_p1_grant_to_subject(void) {
    P1_TEST("cap_grant_to_subject (self + unknown subject)");
    uint64_t subj = get_subject();

    /* The perm-engine's signing path, called directly: issues an atom
     * cap INTO the subject's kernel table.  Self-target → own table. */
    int h = cap_grant_to_subject(subj, ATOM_DATA_DOCS_READ, RIGHT_ALL, 0, 0);
    P1_ASSERT(h > 0, "grant_to_subject(self) failed");
    P1_ASSERT(cap_consume(h) == 0, "issued cap not consumable");

    /* Unknown subject (no live process holds it) → ERR_NOENT. */
    P1_ASSERT(cap_grant_to_subject(0x7FFFFFFFULL, ATOM_DATA_DOCS_READ, RIGHT_ALL, 0, 0) ==
                  ERR_NOENT,
              "unknown subject accepted");
    printf("(handle=%d) ", h);
    P1_PASS();
}

/* ---- P1 runner: live-stack permission engine tests ---- */

static void run_p1_perm_tests(void) {
    printf("\n=== P1 Permission Engine (live vfs+perm) ===\n");
    test_p1_whoami();
    test_p1_owner_auto_allow();
    test_p1_role_set_hot_reload();
    test_p1_chain_deny_no_prompt();
    test_p1_powerbox_allow();
    test_p1_revoke();
    test_p1_grant_beats_role();
    test_p1_role_set_denied();
    test_p1_dump();
    test_p1_grant_to_subject();
    printf("=== P1 Permissions: %d/%d passed ===\n", p1_pass, p1_run);
}

/* ====================================================================
 * P2 地基: sensitive syscall gate (docs/permission_model.md §四).
 * The gate is a pure kernel cap-table lookup — 决策下沉, 能力即决策,
 * ZERO IPC.  init (subject 1) plays the caller: first UNAUTHORIZED
 * (must get ERR_NOCAP), then after self-issuing an ATOM_SYS_SET_TIME
 * cap AUTHORIZED (passes).  Separate counters from the classic suite
 * and the P1 tests.
 * ==================================================================== */

static int p2_run  = 0;
static int p2_pass = 0;

#define P2_TEST(name)                  \
    do {                               \
        printf("  P2: %s ... ", name); \
        p2_run++;                      \
    } while (0)

#define P2_PASS()         \
    do {                  \
        p2_pass++;        \
        printf("PASS\n"); \
    } while (0)

#define P2_FAIL(msg)               \
    do {                           \
        printf("FAIL: %s\n", msg); \
    } while (0)

#define P2_ASSERT(cond, msg) \
    do {                     \
        if (!(cond)) {       \
            P2_FAIL(msg);    \
            return;          \
        }                    \
    } while (0)

/* ---- P2 test 1: unauthorized set_time -> ERR_NOCAP (gate closed) ---- */

static void test_p2_set_time_unauthorized(void) {
    P2_TEST("set_time unauthorized -> ERR_NOCAP");
    /* A valid-looking time; the gate must reject the call BEFORE any
     * user memory is touched or CMOS access happens.  init holds no
     * ATOM_SYS_SET_TIME cap at this point — that absence is exactly
     * what this assertion proves. */
    rtc_time_t t;
    t.year   = 2026;
    t.month  = 8;
    t.day    = 13;
    t.hour   = 12;
    t.minute = 0;
    t.second = 0;
    int ret  = os_set_time(&t);
    P2_ASSERT(ret == ERR_NOCAP, "unauthorized set_time accepted");
    P2_PASS();
}

/* ---- P2 test 2: authorized set_time -> 0 (gate opens on a cap) ---- */

static void test_p2_set_time_authorized(void) {
    P2_TEST("set_time authorized (ATOM_SYS_SET_TIME cap) -> 0");
    /* init self-issues an ATOM_SYS_SET_TIME cap into its own table via
     * the (P1/P2-ungated) atom-cap syscall.  After the grant the same
     * call must pass the gate and reach the RTC. */
    int h = cap_create_atom(ATOM_SYS_SET_TIME, RIGHT_ALL, 0, 0, 0);
    P2_ASSERT(h > 0, "cap_create_atom(ATOM_SYS_SET_TIME) failed");

    rtc_time_t t;
    t.year   = 2026;
    t.month  = 8;
    t.day    = 13;
    t.hour   = 12;
    t.minute = 0;
    t.second = 0;
    int ret  = os_set_time(&t);
    printf("(handle=%d, ret=%d) ", h, ret);
    P2_ASSERT(ret == 0, "authorized set_time failed");
    P2_PASS();
}

/* ---- P2 test 3: reboot gate (ATOM_SYS_SHUTDOWN) ---- */

static void test_p2_reboot_unauthorized(void) {
    P2_TEST("reboot unauthorized -> ERR_NOCAP");
    /* init holds no ATOM_SYS_SHUTDOWN cap, so the gate must return
     * ERR_NOCAP BEFORE the serial print / cli / 8042 reset line.  If
     * the gate were misplaced, the VM would reset here and this line
     * would never print — a live proof of the gate's placement. */
    int ret = sys_reboot();
    printf("(ret=%d) ", ret);
    P2_ASSERT(ret == ERR_NOCAP, "unauthorized reboot accepted");
    P2_PASS();
}

/* ---- P2 test 4: notify is same-process only ---- */

static void test_p2_notify_foreign(void) {
    P2_TEST("notify foreign/nonexistent thread -> ERR_NOENT");
    /* The gate rejects any target outside the calling process; foreign
     * and unknown TIDs are reported identically (no existence leak). */
    int ret = notify(0x7FFFFFFF, 1u << 2);
    printf("(ret=%d) ", ret);
    P2_ASSERT(ret == ERR_NOENT, "notify foreign thread accepted");
    P2_PASS();
}

/* ---- P2 test 5: console input is COM1-driver only ---- */

static void test_p2_debug_getchar_gate(void) {
    P2_TEST("debug_getchar unauthorized -> ERR_NOCAP");
    /* init holds no COM1 IO-port cap (that belongs to the serial
     * service), so the console-input gate must deny. */
    int c = debug_getchar();
    printf("(ret=%d) ", c);
    P2_ASSERT(c == ERR_NOCAP, "console input not gated");
    P2_PASS();
}

/* ---- P2 runner: sensitive syscall gate tests ---- */

static void run_p2_gate_tests(void) {
    printf("\n=== P2 Sensitive Syscall Gate ===\n");
    test_p2_set_time_unauthorized();
    test_p2_set_time_authorized();
    test_p2_reboot_unauthorized();
    test_p2_notify_foreign();
    test_p2_debug_getchar_gate();
    printf("=== P2 Gate: %d/%d passed ===\n", p2_pass, p2_run);
}

/* ====================================================================
 * P2 VFS: full-op authorization on the live vfs+perm stack
 * (docs/permission_model.md §四 / docs/vfs_design.md §9.4).
 *
 * P1 covered bookmark create/resolve only.  P2 gates the five remaining
 * ops (create_dir / delete / enum_begin / enum_next / move) AND proves
 * 能力化抹位: a READ-only grant must never mint a WRITE-carrying
 * FileHandle.  The state carried in here (init = GUEST; grant
 * (subject 1, g_p1_res, READ) on System/Kernel/init.elf) is exactly
 * what run_p1_perm_tests leaves behind.
 *
 * Separate P2V_* counters keep this suite independent of the classic
 * and P1/P2 counters.
 * ==================================================================== */

static int p2v_run  = 0;
static int p2v_pass = 0;

#define P2V_TEST(name)                  \
    do {                                \
        printf("  P2V: %s ... ", name); \
        p2v_run++;                      \
    } while (0)

#define P2V_PASS()        \
    do {                  \
        p2v_pass++;       \
        printf("PASS\n"); \
    } while (0)

#define P2V_FAIL(msg)              \
    do {                           \
        printf("FAIL: %s\n", msg); \
    } while (0)

#define P2V_ASSERT(cond, msg) \
    do {                      \
        if (!(cond)) {        \
            P2V_FAIL(msg);    \
            return;           \
        }                     \
    } while (0)

/* P2V test IPC staging buffers.  The VFS/perm protocol structs embed
 * 1-4 KB arrays (path[1024], read/write data[4032], policy[3584],
 * audit entries).  Keeping them on the stack would overflow init's
 * stack guard region — the 4776-byte frame of test_p2v_open_gate is
 * what crashed the P1 suite.  The P2V tests run strictly sequentially
 * inside run_p2_vfs_tests, so one shared static set is safe. */
static vfs_resp_open_t       s_v_open1, s_v_open2;
static vfs_resp_read_t       s_v_read_resp;
static vfs_req_write_t       s_v_write;
static vfs_resp_write_t      s_v_write_resp;
static perm_req_grant_t      s_p_grant;
static perm_resp_grant_t     s_p_grant_resp;
static perm_req_context_t    s_p_ctx;
static perm_resp_context_t   s_p_ctx_resp;
static perm_req_freq_t       s_p_freq;
static perm_resp_freq_t      s_p_freq_resp;
static perm_req_policy_t     s_p_pol_save, s_p_pol_load;
static perm_resp_policy_t    s_p_pol1, s_p_pol2, s_p_pol3;
static perm_req_audit_t      s_p_audit_req;
static perm_resp_audit_t     s_p_audit_resp;
static vfs_req_create_dir_t  s_v_mkdir;
static vfs_resp_create_dir_t s_v_mkdir_resp;
static vfs_req_delete_t      s_v_del;
static vfs_resp_delete_t     s_v_del_resp;
static vfs_req_enum_begin_t  s_v_eb1, s_v_eb2;
static vfs_resp_enum_begin_t s_v_ebr1, s_v_ebr2;
static vfs_req_move_t        s_v_move;
static vfs_resp_move_t       s_v_move_resp;
static vfs_req_get_item_t    s_v_get;
static vfs_resp_get_item_t   s_v_get_resp;
static vfs_req_enum_next_t   s_v_enum;
static vfs_resp_enum_next_t  s_v_enum_resp;
static perm_req_revoke_t     s_p_revoke;
static perm_resp_revoke_t    s_p_revoke_resp;

/* One OPEN_ITEM against the live vfs_server (returns resp.ret).  The
 * caller's identity comes from ipc_recv_from inside vfs_server — the
 * request carries no app hash, only op/path/flags/access. */
static int p2v_open(int vfs_port, const char *path, u32 flags, u32 access, vfs_resp_open_t *resp) {
    vfs_req_open_t req;
    memset(&req, 0, sizeof(req));
    req.op = VFS_OP_OPEN_ITEM;
    strncpy(req.path, path, sizeof(req.path) - 1);
    req.flags  = flags;
    req.access = access;
    int rlen   = (int)sizeof(*resp);
    int r      = ipc_call(vfs_port, &req, (int)sizeof(req), resp, &rlen);
    if (r < 0)
        return r;
    return resp->ret;
}

/* ---- P2V test 1: open gate (unauthorized denied, grant beat ok) ---- */

static void test_p2v_open_gate(void) {
    P2V_TEST("OPEN: unauthorized WRITE denied, P1 grant READ ok");
    int vfs_port = p1_port_get("vfs");
    P2V_ASSERT(vfs_port > 0, "vfs port unavailable");

    /* Unauthorized CREATE|WRITE on the writable Users volume: no grant
     * for the unauthorized subject → GUEST chain DENYs WRITE → VFS_ERR_ACCESS.
     * (The pre-existing MKFILE-before-gate quirk creates the empty file, but
     * the open itself must fail — T4's delete denial reuses it.) */
    vfs_resp_open_t *r1 = &s_v_open1;
    int ret = p2v_open(vfs_port, "Users/p2vfs.bin", VFS_OPEN_CREATE, VFS_ACCESS_WRITE, r1);
    printf("(unauth=%d) ", ret);
    P2V_ASSERT(ret == VFS_ERR_ACCESS, "unauthorized WRITE open allowed");

    /* Authorized READ on the P1-captured resource: the grant beats the
     * GUEST DENY chain → open succeeds with granted=READ. */
    vfs_resp_open_t *r2 = &s_v_open2;
    ret                 = p2v_open(vfs_port, P1_ITEM_PATH, 0, VFS_ACCESS_READ, r2);
    printf("(auth=%d) ", ret);
    P2V_ASSERT(ret == 0, "authorized READ open denied");
    P2V_ASSERT(r2->handle != 0, "no handle minted");

    /* The handle must actually read (grant re-check on READ passes). */
    vfs_req_read_t rd;
    memset(&rd, 0, sizeof(rd));
    rd.op                 = VFS_OP_READ;
    rd.handle             = r2->handle;
    rd.offset             = 0;
    rd.len                = 32;
    vfs_resp_read_t *rr   = &s_v_read_resp;
    int              rlen = (int)sizeof(*rr);
    P2V_ASSERT(ipc_call(vfs_port, &rd, (int)sizeof(rd), rr, &rlen) == 0, "READ transport failed");
    P2V_ASSERT(rr->ret > 0, "authorized handle could not read");
    printf("(read=%d) ", rr->ret);
    P2V_PASS();
}

/* ---- P2V test 2: 能力化抹位 — READ grant can't mint a WRITE handle ---- */

static void test_p2v_capability_trim(void) {
    P2V_TEST("抹位: READ|EXEC open yields READ-only handle");
    int vfs_port  = p1_port_get("vfs");
    int perm_port = p1_port_get("perm");
    P2V_ASSERT(vfs_port > 0 && perm_port > 0, "ports unavailable");

    /* Idempotent re-grant: (subject, g_p1_res) → READ.  The grant
     * already exists from P1 test 7; upserting again is a no-op. */
    perm_req_grant_t *g = &s_p_grant;
    memset(g, 0, sizeof(*g));
    g->op                   = PERM_OP_GRANT;
    g->resource             = g_p1_res;
    g->access               = VFS_ACCESS_READ;
    g->subject_id           = get_subject();
    g->atom                 = ATOM_DATA_DOCS_READ;
    perm_resp_grant_t *gr   = &s_p_grant_resp;
    int                rlen = (int)sizeof(*gr);
    P2V_ASSERT(ipc_call(perm_port, g, (int)sizeof(*g), gr, &rlen) == 0 && gr->ret == 0,
               "GRANT(READ) failed");

    /* Request READ|EXEC: only READ is covered → partial-intersect beat,
     * granted = READ.  The open SUCCEEDS — but the handle must not carry
     * EXEC/WRITE. */
    vfs_resp_open_t *r = &s_v_open1;
    int ret            = p2v_open(vfs_port, P1_ITEM_PATH, 0, VFS_ACCESS_READ | VFS_ACCESS_EXEC, r);
    printf("(open=%d) ", ret);
    P2V_ASSERT(ret == 0, "READ|EXEC open denied (partial beat broken)");
    P2V_ASSERT(r->handle != 0, "no handle minted");

    /* READ on the trimmed handle works. */
    vfs_req_read_t rd;
    memset(&rd, 0, sizeof(rd));
    rd.op               = VFS_OP_READ;
    rd.handle           = r->handle;
    rd.offset           = 0;
    rd.len              = 16;
    vfs_resp_read_t *rr = &s_v_read_resp;
    rlen                = (int)sizeof(*rr);
    P2V_ASSERT(ipc_call(vfs_port, &rd, (int)sizeof(rd), rr, &rlen) == 0, "READ transport failed");
    P2V_ASSERT(rr->ret > 0, "trimmed handle cannot read");
    printf("(read=%d) ", rr->ret);

    /* WRITE must die at the handle-access gate (VFS_ERR_PERM): the
     * handle only carries the granted READ bits, never the requested
     * WRITE/EXEC — 能力化抹位. */
    vfs_req_write_t *wr = &s_v_write;
    memset(wr, 0, sizeof(*wr));
    wr->op     = VFS_OP_WRITE;
    wr->handle = r->handle;
    wr->offset = 0;
    wr->len    = 4;
    memcpy(wr->data, "test", 4);
    vfs_resp_write_t *wresp = &s_v_write_resp;
    rlen                    = (int)sizeof(*wresp);
    P2V_ASSERT(ipc_call(vfs_port, wr, (int)sizeof(*wr), wresp, &rlen) == 0,
               "WRITE transport failed");
    printf("(write=%d) ", wresp->ret);
    P2V_ASSERT(wresp->ret == VFS_ERR_PERM, "trimmed handle wrote (抹位 broken)");
    P2V_PASS();
}

/* ---- P2V test 3: P3/P4 reserved interfaces (context/freq/policy/audit) ----
 */

static void test_p2v_p3p4_ifaces(void) {
    P2V_TEST("P3/P4 ifaces: context, freq, policy round-trip, audit");
    int perm_port = p1_port_get("perm");
    P2V_ASSERT(perm_port > 0, "perm port unavailable");
    u64 subj = get_subject();
    /* CONTEXT (P3 预留): foreground upsert. */
    perm_req_context_t *c = &s_p_ctx;
    memset(c, 0, sizeof(*c));
    c->op                     = PERM_OP_CONTEXT;
    c->subject_id             = subj;
    c->foreground             = 1;
    perm_resp_context_t *cr   = &s_p_ctx_resp;
    int                  rlen = (int)sizeof(*cr);
    P2V_ASSERT(ipc_call(perm_port, c, (int)sizeof(*c), cr, &rlen) == 0 && cr->ret == 0,
               "CONTEXT failed");

    /* FREQ (P3 预留): grant-beat checks above bumped (subject, READ). */
    perm_req_freq_t *f = &s_p_freq;
    memset(f, 0, sizeof(*f));
    f->op                = PERM_OP_FREQ;
    f->subject_id        = subj;
    f->atom              = ATOM_DATA_DOCS_READ;
    perm_resp_freq_t *fr = &s_p_freq_resp;
    rlen                 = (int)sizeof(*fr);
    P2V_ASSERT(ipc_call(perm_port, f, (int)sizeof(*f), fr, &rlen) == 0 && fr->ret == 0,
               "FREQ query failed");
    P2V_ASSERT(fr->count >= 1, "freq count == 0 (no grant hits?)");
    printf("(freq=%u) ", fr->count);

    /* Reset → slot kept, count zeroed. */
    f->reset = 1;
    P2V_ASSERT(ipc_call(perm_port, f, (int)sizeof(*f), fr, &rlen) == 0 && fr->ret == 0,
               "FREQ reset failed");
    f->reset = 0;
    P2V_ASSERT(ipc_call(perm_port, f, (int)sizeof(*f), fr, &rlen) == 0 && fr->count == 0,
               "freq not zeroed after reset");

    /* POLICY (P4 预留): SAVE → LOAD → SAVE byte-identical round trip. */
    perm_req_policy_t *ps = &s_p_pol_save;
    memset(ps, 0, sizeof(*ps));
    ps->op                  = PERM_OP_POLICY_SAVE;
    perm_resp_policy_t *pr1 = &s_p_pol1;
    rlen                    = (int)sizeof(*pr1);
    P2V_ASSERT(ipc_call(perm_port, ps, (int)sizeof(*ps), pr1, &rlen) == 0 && pr1->ret == 0 &&
                   pr1->size > 0,
               "POLICY_SAVE failed");
    printf("(snap=%uB) ", pr1->size);

    perm_req_policy_t *pl = &s_p_pol_load;
    memset(pl, 0, sizeof(*pl));
    pl->op   = PERM_OP_POLICY_LOAD;
    pl->size = pr1->size;
    memcpy(pl->data, pr1->data, pr1->size);

    perm_resp_policy_t *lr = &s_p_pol2;
    rlen                   = (int)sizeof(*lr);
    P2V_ASSERT(ipc_call(perm_port, pl, (int)sizeof(*pl), lr, &rlen) == 0 && lr->ret == 0,
               "POLICY_LOAD failed");

    perm_resp_policy_t *pr2 = &s_p_pol3;
    rlen                    = (int)sizeof(*pr2);
    P2V_ASSERT(ipc_call(perm_port, ps, (int)sizeof(*ps), pr2, &rlen) == 0 && pr2->ret == 0,
               "POLICY_SAVE (2nd) failed");
    P2V_ASSERT(pr2->size == pr1->size, "snapshot sizes differ");
    int same = 1;
    for (u32 i = 0; i < pr1->size; i++)
        if (pr2->data[i] != pr1->data[i]) {
            same = 0;
            break;
        }
    P2V_ASSERT(same, "save→load→save not byte-identical");

    /* AUDIT (P3 预留): subject 1 has both granted and denied entries. */
    perm_req_audit_t *a = &s_p_audit_req;
    memset(a, 0, sizeof(*a));
    a->op                 = PERM_OP_AUDIT;
    perm_resp_audit_t *ar = &s_p_audit_resp;
    rlen                  = (int)sizeof(*ar);
    P2V_ASSERT(ipc_call(perm_port, a, (int)sizeof(*a), ar, &rlen) == 0 && ar->ret == 0 &&
                   ar->count > 0,
               "AUDIT failed");
    int g = 0, d = 0;
    for (u32 i = 0; i < ar->count && i < PERM_AUDIT_MAX; i++) {
        if (ar->entries[i].subject_id != subj)
            continue;
        if (ar->entries[i].verdict == 0)
            g = 1;
        else
            d = 1;
    }
    printf("(audit=%u g=%d d=%d) ", ar->count, g, d);
    P2V_ASSERT(g && d, "audit lacks granted+denied for subject 1");
    P2V_PASS();
}

/* ---- P2V test 4: five-op gates + enum lifecycle (grant → revoke) ---- */

static void test_p2v_ops_gate(void) {
    P2V_TEST("5-op gates + enum lifecycle (grant → iterate → revoke)");
    int vfs_port  = p1_port_get("vfs");
    int perm_port = p1_port_get("perm");
    P2V_ASSERT(vfs_port > 0 && perm_port > 0, "ports unavailable");

    /* create_dir: WRITE into Users via unknown app → GUEST denies. */
    vfs_req_create_dir_t *cd = &s_v_mkdir;
    memset(cd, 0, sizeof(*cd));
    cd->op = VFS_OP_CREATE_DIR;
    strncpy(cd->path, "Users/p2test_dir", sizeof(cd->path) - 1);
    vfs_resp_create_dir_t *cdr  = &s_v_mkdir_resp;
    int                    rlen = (int)sizeof(*cdr);
    P2V_ASSERT(ipc_call(vfs_port, cd, (int)sizeof(*cd), cdr, &rlen) == 0 &&
                   cdr->ret == VFS_ERR_ACCESS,
               "create_dir not gated (VFS_ERR_ACCESS)");
    printf("(mkdir=%d) ", cdr->ret);

    /* delete: the file exists (T1's MKFILE); gate denies WRITE. */
    vfs_req_delete_t *dl = &s_v_del;
    memset(dl, 0, sizeof(*dl));
    dl->op = VFS_OP_DELETE_ITEM;
    strncpy(dl->path, "Users/p2vfs.bin", sizeof(dl->path) - 1);
    dl->recursive          = 0;
    vfs_resp_delete_t *dlr = &s_v_del_resp;
    rlen                   = (int)sizeof(*dlr);
    P2V_ASSERT(ipc_call(vfs_port, dl, (int)sizeof(*dl), dlr, &rlen) == 0 &&
                   dlr->ret == VFS_ERR_ACCESS,
               "delete not gated (VFS_ERR_ACCESS)");
    printf("(rm=%d) ", dlr->ret);

    /* enum_begin: READ on Users root via unprivileged subject (GUEST,
     * no grant) → GUEST chain DENYs READ. */
    vfs_req_enum_begin_t *eb = &s_v_eb1;
    memset(eb, 0, sizeof(*eb));
    eb->op = VFS_OP_ENUM_BEGIN;
    strncpy(eb->path, "Users", sizeof(eb->path) - 1);
    vfs_resp_enum_begin_t *ebr = &s_v_ebr1;
    rlen                       = (int)sizeof(*ebr);
    P2V_ASSERT(ipc_call(vfs_port, eb, (int)sizeof(*eb), ebr, &rlen) == 0 &&
                   ebr->ret == VFS_ERR_ACCESS,
               "enum_begin not gated (VFS_ERR_ACCESS)");
    printf("(enum0=%d) ", ebr->ret);

    /* move: WRITE on the source (Users/p2vfs.bin) → GUEST denies. */
    vfs_req_move_t *mv = &s_v_move;
    memset(mv, 0, sizeof(*mv));
    mv->op = VFS_OP_MOVE;
    strncpy(mv->src, "Users/p2vfs.bin", sizeof(mv->src) - 1);
    strncpy(mv->dst_dir, "Users", sizeof(mv->dst_dir) - 1);
    mv->new_name[0]      = '\0';
    vfs_resp_move_t *mvr = &s_v_move_resp;
    rlen                 = (int)sizeof(*mvr);
    P2V_ASSERT(ipc_call(vfs_port, mv, (int)sizeof(*mv), mvr, &rlen) == 0 &&
                   mvr->ret == VFS_ERR_ACCESS,
               "move not gated (VFS_ERR_ACCESS)");
    printf("(mv=%d) ", mvr->ret);

    /* ---- enum lifecycle on System/Kernel (read-only dir) ---- */

    /* GET_ITEM is NOT perm-gated → learn the Kernel dir item id. */
    vfs_req_get_item_t *gi = &s_v_get;
    memset(gi, 0, sizeof(*gi));
    gi->op = VFS_OP_GET_ITEM;
    strncpy(gi->path, "System/Kernel", sizeof(gi->path) - 1);
    vfs_resp_get_item_t *gir = &s_v_get_resp;
    rlen                     = (int)sizeof(*gir);
    P2V_ASSERT(ipc_call(vfs_port, gi, (int)sizeof(*gi), gir, &rlen) == 0 && gir->ret == 0,
               "GET_ITEM System/Kernel failed");

    /* Grant READ on the kernel dir (System volume UUID + dir id). */
    vfs_resource_t kres;
    memset(&kres, 0, sizeof(kres));
    kres.vol            = g_p1_res.vol;
    kres.id             = gir->item.item_id;
    perm_req_grant_t *g = &s_p_grant;
    memset(g, 0, sizeof(*g));
    g->op                 = PERM_OP_GRANT;
    g->resource           = kres;
    g->access             = VFS_ACCESS_READ;
    g->subject_id         = get_subject();
    g->atom               = ATOM_DATA_DOCS_READ;
    perm_resp_grant_t *gr = &s_p_grant_resp;
    rlen                  = (int)sizeof(*gr);
    P2V_ASSERT(ipc_call(perm_port, g, (int)sizeof(*g), gr, &rlen) == 0 && gr->ret == 0,
               "kernel-dir GRANT failed");

    /* Authorized ENUM_BEGIN with the grant → 0, enumerator minted. */
    vfs_req_enum_begin_t *eb2 = &s_v_eb2;
    memset(eb2, 0, sizeof(*eb2));
    eb2->op = VFS_OP_ENUM_BEGIN;
    strncpy(eb2->path, "System/Kernel", sizeof(eb2->path) - 1);
    /* Authorization now uses subject_id from get_subject() */
    vfs_resp_enum_begin_t *ebr2 = &s_v_ebr2;
    rlen                        = (int)sizeof(*ebr2);
    P2V_ASSERT(ipc_call(vfs_port, eb2, (int)sizeof(*eb2), ebr2, &rlen) == 0 && ebr2->ret == 0 &&
                   ebr2->handle != 0,
               "authorized enum_begin failed");
    printf("(enum=%u) ", ebr2->handle);

    /* First batch iterates (Kernel contains init.elf). */
    vfs_req_enum_next_t *en = &s_v_enum;
    memset(en, 0, sizeof(*en));
    en->op                    = VFS_OP_ENUM_NEXT;
    en->handle                = ebr2->handle;
    vfs_resp_enum_next_t *enr = &s_v_enum_resp;
    rlen                      = (int)sizeof(*enr);
    P2V_ASSERT(ipc_call(vfs_port, en, (int)sizeof(*en), enr, &rlen) == 0 && enr->ret > 0,
               "authorized enum_next returned nothing");
    printf("(items=%d) ", enr->ret);

    /* Revoke the dir grant → the enumerator's re-check dies. */
    perm_req_revoke_t *rv = &s_p_revoke;
    memset(rv, 0, sizeof(*rv));
    rv->op                  = PERM_OP_REVOKE;
    rv->subject_id          = get_subject();
    rv->resource            = kres;
    perm_resp_revoke_t *rvr = &s_p_revoke_resp;
    rlen                    = (int)sizeof(*rvr);
    P2V_ASSERT(ipc_call(perm_port, rv, (int)sizeof(*rv), rvr, &rlen) == 0 && rvr->ret == 0 &&
                   rvr->revoked >= 1,
               "kernel-dir REVOKE failed");
    printf("(revoked=%u) ", rvr->revoked);

    /* Next batch: grant gone → VFS_ERR_ACCESS (enum re-check works). */
    P2V_ASSERT(ipc_call(vfs_port, en, (int)sizeof(*en), enr, &rlen) == 0 &&
                   enr->ret == VFS_ERR_ACCESS,
               "enum_next not re-checked after revoke");
    printf("(after-revoke=%d) ", enr->ret);
    P2V_PASS();
}

/* ---- P2V runner: full-op VFS authorization tests ---- */

static void run_p2_vfs_tests(void) {
    printf("\n=== P2 VFS Authorization (vfs+perm) ===\n");
    test_p2v_open_gate();
    test_p2v_capability_trim();
    test_p2v_p3p4_ifaces();
    test_p2v_ops_gate();
    printf("=== P2 VFS: %d/%d passed ===\n", p2v_pass, p2v_run);
}

/* ====================================================================
 * KBD focus: TAKE_FOCUS / RELEASE_FOCUS ownership round-trip
 *
 * No shell-level hook exists for keyboard focus (R2.1), so exercise the
 * live keyboard service over IPC directly.  Separate counters keep the
 * classic (31) / P1 (10) / P2 (3) / P2V (4) anchors untouched.
 *
 * Coverage split: the *ownership protocol* (take -> owned, retake ->
 * idempotent, release-by-owner, release-by-non-owner -> ERR_NOCAP) is
 * verified here deterministically.  The *physical key routing* while a
 * holder owns focus is exercised by the QEMU-side smoke test (R2.2:
 * sendkey characters reach the shell; R3.4: keyboard flood) — init
 * cannot inject scancodes into its own IRQ line.
 *
 * Timing safety: init takes focus for a few IPC round-trips and always
 * releases; TAKE_FOCUS never completes a parked read, so the shell's
 * parked READ_BLOCK (if any) is untouched and resumes normal routing
 * once focus is free again.  Keys typed by the smoke test arrive only
 * after init reaches the idle loop, never while focus is held here.
 * ==================================================================== */

static int kbd_run  = 0;
static int kbd_pass = 0;

#define KBD_TEST(name)                  \
    do {                                \
        printf("  KBD: %s ... ", name); \
        kbd_run++;                      \
    } while (0)

#define KBD_PASS()         \
    do {                  \
        kbd_pass++;       \
        printf("PASS\n"); \
    } while (0)

#define KBD_FAIL(msg)               \
    do {                            \
        printf("FAIL: %s\n", msg);  \
    } while (0)

#define KBD_ASSERT(cond, msg) \
    do {                      \
        if (!(cond)) {        \
            KBD_FAIL(msg);    \
            return;           \
        }                     \
    } while (0)

/* Keyboard focus protocol (mirrors user/services/keyboard/keyboard.c;
 * same local-mirror pattern as shell.c's KBD_OP_READ_BLOCK). */
#define KBD_OP_TAKE_FOCUS    3
#define KBD_OP_RELEASE_FOCUS 4

typedef struct {
    u32 op;
    u32 len;
} kbd_req_t;

typedef struct {
    i32 ret;
    u8  data[256]; /* receiver buffer; focus ops carry no payload */
} kbd_resp_t;

static void test_kbd_focus_roundtrip(void) {
    KBD_TEST("TAKE_FOCUS/RELEASE_FOCUS ownership");
    int kbd_port = p1_port_get("keyboard");
    KBD_ASSERT(kbd_port > 0, "keyboard port unavailable");

    kbd_req_t  req;
    kbd_resp_t resp;
    int        rlen = (int)sizeof(resp);

    /* TAKE_FOCUS: init becomes the focus owner. */
    memset(&req, 0, sizeof(req));
    req.op = KBD_OP_TAKE_FOCUS;
    rlen   = (int)sizeof(resp);
    KBD_ASSERT(ipc_call(kbd_port, &req, (int)sizeof(req), &resp, &rlen) == 0 &&
                   resp.ret == 0,
               "TAKE_FOCUS failed");

    /* Re-take by the same owner: idempotent no-op success. */
    memset(&req, 0, sizeof(req));
    req.op = KBD_OP_TAKE_FOCUS;
    rlen   = (int)sizeof(resp);
    KBD_ASSERT(ipc_call(kbd_port, &req, (int)sizeof(req), &resp, &rlen) == 0 &&
                   resp.ret == 0,
               "TAKE_FOCUS re-take not idempotent");

    /* RELEASE_FOCUS by the owner: succeeds, focus free again. */
    memset(&req, 0, sizeof(req));
    req.op = KBD_OP_RELEASE_FOCUS;
    rlen   = (int)sizeof(resp);
    KBD_ASSERT(ipc_call(kbd_port, &req, (int)sizeof(req), &resp, &rlen) == 0 &&
                   resp.ret == 0,
               "RELEASE_FOCUS by owner failed");

    /* RELEASE_FOCUS by a non-owner (focus free now): ERR_NOCAP — the
     * kernel-filled caller subject must match the holder. */
    memset(&req, 0, sizeof(req));
    req.op = KBD_OP_RELEASE_FOCUS;
    rlen   = (int)sizeof(resp);
    KBD_ASSERT(ipc_call(kbd_port, &req, (int)sizeof(req), &resp, &rlen) == 0 &&
                   resp.ret == ERR_NOCAP,
               "non-owner RELEASE_FOCUS accepted");
    printf("(non-owner release=%d) ", resp.ret);
    KBD_PASS();
}

static void run_kbd_focus_tests(void) {
    printf("\n=== KBD Focus (live keyboard service) ===\n");
    test_kbd_focus_roundtrip();
    printf("=== KBD Focus: %d/%d passed ===\n", kbd_pass, kbd_run);
}

/* ====================================================================
 * P3: crash recovery (production hardening) — kill a live service,
 * assert the manager auto-restarts it and its well-known port comes
 * back (process_reap → ipc_cleanup_process frees the dead process's
 * registry names; the manager's service_monitor re-spawns it).
 * ==================================================================== */

static int p3_run = 0, p3_pass = 0;

#define P3_TEST(name)                  \
    do {                               \
        printf("  P3: %s ... ", name); \
        p3_run++;                      \
    } while (0)

#define P3_PASS()        \
    do {                 \
        p3_pass++;       \
        printf("PASS\n"); \
    } while (0)

#define P3_FAIL(msg)              \
    do {                          \
        printf("FAIL: %s\n", msg); \
    } while (0)

#define P3_ASSERT(cond, msg) \
    do {                     \
        if (!(cond)) {       \
            P3_FAIL(msg);    \
            return;          \
        }                    \
    } while (0)

static int find_pid_by_name(const char *name) {
    /* Static: proc_info_t is 84 B; 64 entries = 5.4 KB would overflow
     * init's single-page user stack if allocated locally. */
    static proc_info_t list[64];
    int n = process_list(list, 64);
    if (n <= 0)
        return -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i].name, name) == 0)
            return list[i].pid;
    }
    return -1;
}

static void test_restart_pkg(void) {
    P3_TEST("kill pkg -> manager auto-restart (port returns)");
    int pid = find_pid_by_name("pkg");
    P3_ASSERT(pid > 0, "pkg not running");
    P3_ASSERT(port_get("pkg") > 0, "pkg port unavailable");

    /* init holds ATOM_SERVICE_MANAGE → cross-process SIGKILL allowed. */
    int ret = kill(pid, SIGKILL);
    P3_ASSERT(ret == 0, "kill(pkg, SIGKILL) failed");

    /* The manager's service_monitor wakes on pkg's death, re-spawns it,
     * and the re-spawned pkg re-registers its "pkg" name (the dead
     * process's registry entry was cleaned by ipc_cleanup_process). */
    int port_after = 0;
    for (int i = 0; i < 400 && port_after <= 0; i++) {
        port_after = port_get("pkg");
        if (port_after <= 0)
            thread_yield();
    }
    P3_ASSERT(port_after > 0, "pkg port did not return after restart");
    printf("(pid=%d restarted) ", pid);
    P3_PASS();
}

static void run_crash_recovery_tests(void) {
    printf("\n=== P3 Crash Recovery (service auto-restart) ===\n");
    test_restart_pkg();
    printf("=== P3 Crash Recovery: %d/%d passed ===\n", p3_pass, p3_run);
}

/* ====================================================================
 * P4: resource exhaustion (production hardening) — fill kernel tables
 * to their limits, assert graceful negative errors instead of a crash,
 * then drain and prove the system recovers.  Every resource grabbed
 * here is released before the test returns, so the running services
 * are unaffected afterwards.
 * ==================================================================== */

static int p4_run = 0, p4_pass = 0;

#define P4_TEST(name)                   \
    do {                                \
        printf("  P4: %s ... ", name);  \
        p4_run++;                       \
    } while (0)

#define P4_PASS()        \
    do {                 \
        p4_pass++;       \
        printf("PASS\n"); \
    } while (0)

#define P4_FAIL(msg)               \
    do {                           \
        printf("FAIL: %s\n", msg); \
    } while (0)

#define P4_ASSERT(cond, ...) \
    do {                     \
        if (!(cond)) {       \
            printf("FAIL: "); \
            printf(__VA_ARGS__); \
            printf("\n");    \
            return;          \
        }                    \
    } while (0)

/* Thread that exits immediately (used to fill and release the table). */
static void p4_worker_exit(void *arg) {
    (void)arg;
    thread_exit(0);
}

/* Boundary: a message larger than MAX_MSG_SIZE (4096, kernel/ipc/ipc.c)
 * must be rejected with ERR_INVAL before any queueing/blocking. */
static void test_ipc_msg_size_boundary(void) {
    P4_TEST("IPC message > MAX_MSG_SIZE -> ERR_INVAL");
    int port = ipc_port_create();
    P4_ASSERT(port > 0, "ipc_port_create failed");
    static char big[8192];
    for (int i = 0; i < (int)sizeof(big); i++)
        big[i] = (char)i;
    int r = ipc_send(port, big, (int)sizeof(big));
    P4_ASSERT(r == ERR_INVAL, "oversized send returned %d", r);
    P4_PASS();
}

/* Thread table (MAX_THREADS = 1024): creating until the table is full
 * must return ERR_NOMEM (never crash), and joining every worker must
 * free the slots so the system can create threads again. */
static void test_thread_table_exhaustion(void) {
    P4_TEST("thread table exhaustion -> ERR_NOMEM, recoverable");
    /* Static: 1024 tids × 4 B; a stack-allocated array this size is
     * fine for init's stack, but static keeps the stack lean. */
    static int tids[1024];
    int        n = 0;
    for (int i = 0; i < 1100; i++) {
        int tid = thread_create(p4_worker_exit, 0, 10);
        if (tid < 0) {
            P4_ASSERT(tid == ERR_NOMEM, "unexpected thread_create error");
            break;
        }
        tids[n++] = tid;
    }
    P4_ASSERT(n >= 1000, "could not fill thread table (%d)", n);

    for (int i = 0; i < n; i++)
        thread_join(tids[i], NULL);

    /* Recoverable: a fresh create+join succeeds after the drain. */
    int tid = thread_create(p4_worker_exit, 0, 10);
    P4_ASSERT(tid > 0, "not recoverable after thread drain");
    thread_join(tid, NULL);
    P4_PASS();
}

static void run_resource_exhaustion_tests(void) {
    printf("\n=== P4 Resource Exhaustion ===\n");
    test_ipc_msg_size_boundary();
    test_thread_table_exhaustion();
    printf("=== P4 Resource Exhaustion: %d/%d passed ===\n", p4_pass, p4_run);
}

/* ====================================================================
 * P5: zero-copy read path (Phase 3, vfs_design §8.4)
 * A System-volume blob backed by the driver's shared page pool is
 * mapped READ-ONLY into init's address space (vfs re-checks the READ
 * grant, then SYS_SHM_MAP maps the pool pages).  Content must match a
 * chunked fs_read — proving the zero-copy path serves the same bytes.
 * ==================================================================== */

static int p5_run = 0, p5_pass = 0;

#define P5_TEST(name)                  \
    do {                               \
        printf("  P5: %s ... ", name); \
        p5_run++;                      \
    } while (0)

#define P5_PASS()        \
    do {                 \
        p5_pass++;       \
        printf("PASS\n"); \
    } while (0)

#define P5_ASSERT(cond, ...)   \
    do {                       \
        if (!(cond)) {         \
            printf("FAIL: ");  \
            printf(__VA_ARGS__); \
            printf("\n");      \
            return;            \
        }                      \
    } while (0)

static void test_zero_copy_read(void) {
    P5_TEST("zero-copy READ_MAP: pool-backed blob matches chunked read");
    /* Idempotent re-grant of READ on System/Kernel/init.elf (P2V may
     * have revoked it); init holds ATOM_SERVICE_MANAGE so the GRANT
     * passes. */
    int perm_port = p1_port_get("perm");
    P5_ASSERT(perm_port > 0, "perm port unavailable");
    perm_req_grant_t g;
    memset(&g, 0, sizeof(g));
    g.op         = PERM_OP_GRANT;
    g.resource   = g_p1_res;
    g.access     = VFS_ACCESS_READ;
    g.subject_id = get_subject();
    g.atom       = ATOM_DATA_DOCS_READ;
    perm_resp_grant_t gr;
    int rlen = (int)sizeof(gr);
    P5_ASSERT(ipc_call(perm_port, &g, (int)sizeof(g), &gr, &rlen) == 0 && gr.ret == 0,
              "GRANT READ failed (%d)", gr.ret);

    vfs_handle_t h = 0;
    int r = fs_open_item("/Volumes/System/Kernel/init.elf", VFS_OPEN_READONLY,
                         VFS_ACCESS_READ, &h);
    P5_ASSERT(r == 0, "open init.elf failed (%d)", r);

    /* Reserve a generous mapping range (init.elf < 1 MiB). */
    void *map = vspace_alloc(0x100000, 0);
    P5_ASSERT(map != 0, "vspace_alloc failed");

    u32 msize = 0;
    r         = fs_read_map(h, map, &msize);
    P5_ASSERT(r == 0, "read_map failed (%d)", r);
    P5_ASSERT(msize > 4096u, "mapped size too small (%u)", msize);

    /* Compare head + tail against the chunked read path. */
    u8  buf[256];
    u32 got = 0;
    P5_ASSERT(fs_read(h, 0, buf, sizeof(buf), &got) == 0 && got == sizeof(buf),
              "chunked head read failed");
    int ok = 1;
    for (u32 i = 0; i < sizeof(buf); i++) {
        if (((u8 *)map)[i] != buf[i]) {
            ok = 0;
            break;
        }
    }
    P5_ASSERT(ok, "mapped head != chunked head");

    u64 tail_off = (u64)msize - sizeof(buf);
    P5_ASSERT(fs_read(h, tail_off, buf, sizeof(buf), &got) == 0 && got == sizeof(buf),
              "chunked tail read failed");
    ok = 1;
    for (u32 i = 0; i < sizeof(buf); i++) {
        if (((u8 *)map)[tail_off + i] != buf[i]) {
            ok = 0;
            break;
        }
    }
    P5_ASSERT(ok, "mapped tail != chunked tail");

    printf("(mapped=%u bytes, verified head+tail) ", msize);
    (void)fs_close(h);
    P5_PASS();
}

static void run_zero_copy_tests(void) {
    printf("\n=== P5 Zero-Copy Read ===\n");
    test_zero_copy_read();
    printf("=== P5 Zero-Copy Read: %d/%d passed ===\n", p5_pass, p5_run);
}

/* ---- Entry point ---- */

int main(void) {
    printf("\ninit: Starting (PID 1)\n");

    run_tests();
    init_protocol();

    /* P2: the service manager is now its own PROCESS.  Fetch its
     * embedded ELF image from the kernel blob table (SYS_BLOB_GET)
     * and spawn it via SYS_PROCESS_CREATE.  The manager in turn
     * spawns the serial / flaky / shell services as processes. */
    printf("init: spawning service manager process...\n");
    static char mgr_blob[131072]; /* must hold manager.elf (grew after libc
                                     migration: math/time/threads/wchar etc.) */
    int size = blob_get("manager", mgr_blob, sizeof(mgr_blob));
    if (size < 0) {
        printf("init: blob_get(manager) FAILED (%d)\n", size);
        for (;;)
            thread_yield();
    }
    printf("init: fetched manager.elf blob (%d bytes)\n", size);
    int mgr_pid = process_create("manager", mgr_blob, size);
    if (mgr_pid < 0) {
        printf("init: process_create(manager) FAILED (%d)\n", mgr_pid);
        for (;;)
            thread_yield();
    }
    printf("init: service manager PID=%d\n", mgr_pid);

    /* P1: permission engine tests.  The manager's spawned vfs_server +
     * perm-manager boot in parallel — every test polls for its ports,
     * so there is no startup race.  These run in THIS thread (subject
     * 1, seeded OWNER) and exercise the live request path end to end. */
    run_p1_perm_tests();

    /* P2: sensitive syscall gate tests (docs/permission_model.md §四) —
     * pure kernel cap-table lookup, zero IPC. */
    run_p2_gate_tests();

    /* P2 VFS: full-op authorization — P1 gated bookmark create/resolve;
     * P2 gates the five remaining ops (create_dir/delete/enum_begin/
     * enum_next/move) and proves 能力化抹位 on the live vfs+perm stack. */
    run_p2_vfs_tests();

    /* KBD focus ownership round-trip (R2.1) — live keyboard service. */
    run_kbd_focus_tests();

    /* P3 crash recovery: kill pkg → manager auto-restarts it. */
    run_crash_recovery_tests();

    /* P4 resource exhaustion: fill kernel tables to the limit, assert
     * graceful ERR_NOMEM instead of a crash, drain, and prove the
     * system recovers.  Runs AFTER the services are up; every resource
     * it grabs is released before the test returns. */
    run_resource_exhaustion_tests();

    /* P5 zero-copy read path (Phase 3): map a pool-backed file and
     * verify its content matches the chunked path. */
    run_zero_copy_tests();

    /* Idle forever — this thread stays alive as the last scheduler
     * participant while the manager and service processes run. */
    printf("init: idle loop\n");
    for (;;)
        thread_yield();

    return 0; /* unreachable */
}
