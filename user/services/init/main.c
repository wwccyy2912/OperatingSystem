/*
 * main.c - Init process (PID 1)
 * Copyright (c) 2026 OpSys Project
 *
 * The first user-space process. Exercises syscalls to validate
 * the ring 3 -> ring 0 -> ring 3 round trip, then performs the
 * init protocol (query kernel state, spawn worker thread).
 */

#include "../lib/libc/stdio.h"
#include "../lib/libos/syscalls.h"
#include "../lib/libc/stdlib.h"

/* ---- Test framework ---- */

static int tests_run  = 0;
static int tests_pass = 0;

#define TEST(name) \
    do { printf("  TEST: %s ... ", name); tests_run++; } while (0)

#define PASS() \
    do { tests_pass++; printf("PASS\n"); } while (0)

#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); } while (0)

#define ASSERT(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while (0)

/* ---- Cycle-accurate timer (bypasses the ambiguous PIT tick rate) ---- */

static inline unsigned long long rdtsc64(void)
{
    unsigned int lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

/* ---- Worker thread ---- */

static void worker_thread(void *arg)
{
    int id = (int)(long)arg;
    printf("  worker[%d]: started, PID=%d\n", id, get_pid());
    printf("  worker[%d]: free pages=%d\n", id, get_free_pages());
    printf("  worker[%d]: done\n", id);
    thread_exit(0);
}

/* ---- IPC receiver worker (for send/recv test) ---- */

static int g_ipc_recv_port;
static char g_ipc_recv_buf[64];
static int  g_ipc_recv_len;
static int  g_ipc_recv_ret;

static void ipc_recv_worker(void *arg)
{
    (void)arg;
    int tok = 0;
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

static void ipc_recvfrom_worker(void *arg)
{
    (void)arg;
    int tok = 0;
    g_recvfrom_len = sizeof(g_recvfrom_buf);
    g_recvfrom_ret = ipc_recv_from(g_recvfrom_port, g_recvfrom_buf,
                                   &g_recvfrom_len, &tok, &g_recvfrom_subj);
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

#define STRESS_THREADS      1000
#define STRESS_IPC_CALLS    100000

static volatile int g_stress_counter;
static int          g_stress_mutex;

static void stress_worker(void *arg)
{
    (void)arg;
    mutex_lock(g_stress_mutex);
    g_stress_counter++;
    mutex_unlock(g_stress_mutex);
    thread_exit(0);
}

static int  g_ipc_stress_port;
static volatile int g_ipc_stress_fail;

static void ipc_stress_server(void *arg)
{
    (void)arg;
    for (int i = 0; i < STRESS_IPC_CALLS; i++) {
        char buf[16];
        int len = sizeof(buf);
        int tok = 0;
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

static void test_debug_log(void)
{
    TEST("debug_log (ring3->ring0->ring3)");
    int ret = debug_log("  [syscall] debug_log ok\n");
    ASSERT(ret == 0, "debug_log returned non-zero");
    PASS();
}

static void test_yield(void)
{
    TEST("thread_yield (ring3->ring0->ring3)");
    thread_yield();
    PASS();
}

static void test_port_create(void)
{
    TEST("ipc_port_create");
    int port = ipc_port_create();
    ASSERT(port > 0, "port <= 0");
    printf("(port=%d) ", port);
    PASS();
}

static void test_port_register_get(void)
{
    TEST("port_register + port_get");
    int port = ipc_port_create();
    ASSERT(port > 0, "port_create failed");
    int ret = port_register("test_svc", port);
    ASSERT(ret == 0, "port_register failed");
    int got = port_get("test_svc");
    ASSERT(got == port, "port_get mismatch");
    PASS();
}

static void test_port_get_nonexistent(void)
{
    TEST("port_get (nonexistent)");
    int got = port_get("no_such_service");
    ASSERT(got < 0, "should return error");
    PASS();
}

static void test_cap_create(void)
{
    TEST("cap_create");
    int cap = cap_create(0, 0);
    ASSERT(cap > 0, "cap <= 0");
    printf("(cap=%d) ", cap);
    PASS();
}

static void test_map_memory(void)
{
    TEST("map_memory");
    /* Create a memory capability with write rights */
    int mem_cap = cap_create(CAP_TYPE_MEM, RIGHT_WRITE);
    ASSERT(mem_cap > 0, "cap_create for MEM failed");
    /* Map one page at 0x10000000 with read/write, no exec */
    void *p = map_memory(mem_cap, 0x10000000, 4096, PROT_READ | PROT_WRITE);
    ASSERT(p != 0, "map_memory returned NULL");
    PASS();
}

static void test_get_pid(void)
{
    TEST("get_pid");
    int pid = get_pid();
    ASSERT(pid == 1, "PID should be 1");
    PASS();
}

static void test_get_free_pages(void)
{
    TEST("get_free_pages");
    int pages = get_free_pages();
    ASSERT(pages > 0, "free pages should be > 0");
    printf("(%d pages, %d MB) ", pages, pages / 256);
    PASS();
}

static void test_thread_create(void)
{
    TEST("thread_create (worker)");
    int tid = thread_create(worker_thread, (void *)0L, 10);
    ASSERT(tid > 0, "thread_create failed");
    printf("(tid=%d) ", tid);
    /* Yield to let the worker run at least once */
    thread_yield();
    thread_yield();
    PASS();
}

static void test_get_time(void)
{
    TEST("get_time");
    int t1 = get_time();
    ASSERT(t1 > 0, "get_time should return > 0");
    printf("(ticks=%d) ", t1);
    PASS();
}

static void test_sleep(void)
{
    TEST("sleep");
    int t1 = get_time();
    int ret = sleep(10);
    ASSERT(ret == 0, "sleep returned error");
    int t2 = get_time();
    int elapsed = t2 - t1;
    ASSERT(elapsed >= 3, "sleep returned too early (< 3 ticks)");
    printf("(elapsed=%d ticks) ", elapsed);
    PASS();
}

/* Temporary microbenchmark: 100k bare get_time syscalls (no IPC, no
 * blocking).  Contrast with the 100k IPC test to separate syscall-entry
 * cost from IPC logic cost. */
static void bench_syscall_100k(void)
{
    TEST("bench: 100k get_time syscalls");
    int t0 = get_time();
    unsigned long long c0 = rdtsc64();
    volatile int sink = 0;
    for (int i = 0; i < 100000; i++)
        sink += get_time();
    unsigned long long c1 = rdtsc64();
    int elapsed = get_time() - t0;
    /* user printf lacks %llu: print 64-bit cycles as hi/lo 32-bit halves */
    unsigned long long dc = c1 - c0;
    printf("(%d calls, %d ticks, cyc_hi=%u cyc_lo=%u) ", 100000, elapsed,
           (unsigned)(dc >> 32), (unsigned)(dc & 0xFFFFFFFFu));
    (void)sink;  /* the loop is the benchmark; value is intentionally discarded */
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
static void bench_yield_solo(void)
{
    TEST("bench: 1k solo yields (no switch)");
    unsigned long long c0 = rdtsc64();
    int t0 = get_time();
    for (int i = 0; i < 1000; i++)
        thread_yield();
    unsigned long long c1 = rdtsc64();
    int elapsed = get_time() - t0;
    unsigned long long dc = c1 - c0;
    printf("(%d yields, %d ticks, cyc_hi=%u cyc_lo=%u) ", 1000, elapsed,
           (unsigned)(dc >> 32), (unsigned)(dc & 0xFFFFFFFFu));
    PASS();
}

static void bench_yield_partner(void *arg)
{
    (void)arg;
    /* Ping-pong with main until it signals stop, then exit so the
     * test suite is not left hanging forever. */
    g_bench_yield_other_done = 1;
    while (!g_bench_yield_stop)
        thread_yield();
    thread_exit(0);
}

static void bench_yield_100k(void)
{
    TEST("bench: 1k yield round-trips");
    g_bench_yield_stop = 0;
    int tid = thread_create(bench_yield_partner, 0, 10);
    ASSERT(tid > 0, "partner thread_create failed");
    /* Let the partner spin in thread_yield so the scheduler ping-pongs
     * between main and partner on every yield. */
    for (int i = 0; i < 3; i++)
        thread_yield();

    int t0 = get_time();
    unsigned long long c0 = rdtsc64();
    g_bench_yield_turn = 0;
    for (int i = 0; i < 1000; i++) {
        g_bench_yield_turn = (g_bench_yield_turn + 1) & 1;
        thread_yield();
    }
    unsigned long long c1 = rdtsc64();
    int elapsed = get_time() - t0;
    g_bench_yield_stop = 1;
    /* Give the partner a chance to observe the flag and exit, then join
     * it so the suite's main thread stays alive for later tests. */
    for (int i = 0; i < 4; i++)
        thread_yield();
    int code = -1;
    thread_join(tid, &code);
    /* user printf lacks %llu: print 64-bit cycles as hi/lo 32-bit halves */
    unsigned long long dc = c1 - c0;
    printf("(%d yields, %d ticks, cyc_hi=%u cyc_lo=%u, other_done=%d) ",
           1000, elapsed, (unsigned)(dc >> 32), (unsigned)(dc & 0xFFFFFFFFu),
           (int)g_bench_yield_other_done);
    PASS();
}

static void test_thread_join(void)
{
    TEST("thread_join");
    int tid = thread_create(worker_thread, (void *)2L, 10);
    ASSERT(tid > 0, "thread_create failed");
    int exit_code = -999;
    int ret = thread_join(tid, &exit_code);
    ASSERT(ret == 0, "thread_join failed");
    ASSERT(exit_code == 0, "exit_code should be 0");
    printf("(tid=%d, exit=%d) ", tid, exit_code);
    PASS();
}

static void test_unmap_memory(void)
{
    TEST("unmap_memory");
    int mem_cap = cap_create(CAP_TYPE_MEM, RIGHT_WRITE);
    ASSERT(mem_cap > 0, "cap_create for MEM failed");
    void *p = map_memory(mem_cap, 0x30000000, 4096, PROT_READ | PROT_WRITE);
    ASSERT(p != 0, "map_memory returned NULL");
    int ret = unmap_memory(p, 4096);
    ASSERT(ret == 0, "unmap_memory failed");
    PASS();
}

static void test_heap_guard(void)
{
    TEST("heap guard pages");
    int mem_cap = cap_create(CAP_TYPE_MEM, RIGHT_WRITE);
    ASSERT(mem_cap > 0, "cap_create for MEM failed");

    /* The heap base is randomized per process (ASLR, design item ⑭);
     * fetch it so the guard addresses match the kernel's layout.
     * LP64: unsigned long is 64-bit, matching sys_call's long args. */
    unsigned long heap_base = (unsigned long)get_heap_base();
    ASSERT(heap_base != 0, "get_heap_base failed");

    unsigned long low_guard  = heap_base - 4096;
    unsigned long high_guard = heap_base + 0x10000000UL;  /* + HEAP_USER_SIZE */
    unsigned long below_low  = low_guard - 4096;

    /* Low guard page below the heap base must refuse mapping.
     * Use the raw syscall with 64-bit addresses (heap_base can exceed
     * INT_MAX after randomization). */
    long r = sys_call(SYS_MAP_MEMORY, mem_cap, (long)low_guard, 4096,
                      PROT_READ | PROT_WRITE, 0);
    ASSERT(r == 0, "map_memory into low guard page should fail");

    /* High guard page at the heap max must refuse mapping. */
    r = sys_call(SYS_MAP_MEMORY, mem_cap, (long)high_guard, 4096,
                 PROT_READ | PROT_WRITE, 0);
    ASSERT(r == 0, "map_memory into high guard page should fail");

    /* Unmapping a guard page must also fail (ERR_INVAL) */
    int u = unmap_memory((void *)low_guard, 4096);
    ASSERT(u == ERR_INVAL, "unmap_memory of guard page should fail");

    /* A page just below the low guard still maps fine */
    void *p = (void *)sys_call(SYS_MAP_MEMORY, mem_cap, (long)below_low, 4096,
                               PROT_READ | PROT_WRITE, 0);
    ASSERT(p != 0, "map_memory below low guard should succeed");
    if (p)
        unmap_memory(p, 4096);

    PASS();
}

static void test_cap_grant(void)
{
    TEST("cap_grant (error paths)");
    int cap = cap_create(CAP_TYPE_MEM, RIGHT_WRITE);
    ASSERT(cap > 0, "cap_create failed");
    /* Grant to nonexistent PID should fail */
    int ret = cap_grant(cap, 999, RIGHT_READ);
    ASSERT(ret < 0, "should fail for nonexistent PID");
    PASS();
}

static void test_cap_revoke(void)
{
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

static void test_cap_expiry(void)
{
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

static void test_cap_quota(void)
{
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

static void test_cap_revoke_by_atom(void)
{
    TEST("cap_revoke_by_atom (atom + scope)");
    uint64_t subj = get_subject();
    ASSERT(subj >= 1, "subject < 1");

    int a1 = cap_create_atom(ATOM_DATA_DL_WRITE, RIGHT_WRITE, 0, 0, 0);
    int a2 = cap_create_atom(ATOM_DATA_DL_WRITE, RIGHT_WRITE, 0, 0, 0);
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

    /* Unmatched atom / wrong subject revoke nothing. */
    n = cap_revoke_by_atom(subj, ATOM_SYS_DEBUG, 0);
    ASSERT(n == 0, "unknown atom revoked something");
    n = cap_revoke_by_atom(subj + 1, ATOM_NET_WIFI_SCAN, 0);
    ASSERT(n == 0, "wrong subject revoked something");

    printf("(revoked=%d) ", n);
    PASS();
}

static void test_ipc_send_recv(void)
{
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
    int ret = ipc_send(port, msg, 5);
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

static void test_subject_identity(void)
{
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

    uint64_t fake = 0xDEADBEEFDEADBEEFULL;   /* forged subject claim */
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

static void test_ipc_call_err(void)
{
    TEST("ipc_call (error paths)");
    /* Call to nonexistent port should fail */
    const char *req = "req";
    char resp[16];
    int resp_len = sizeof(resp);
    int ret = ipc_call(9999, req, 3, resp, &resp_len);
    ASSERT(ret < 0, "should fail for nonexistent port");
    PASS();
}

static void test_set_affinity(void)
{
    TEST("set_affinity");
    /* Set affinity of init thread (tid=1) to CPU 0 */
    int ret = thread_set_affinity(1, 0);
    ASSERT(ret == 0, "set_affinity to cpu0 failed");
    PASS();
}

/* ---- Roadmap P1: vspace_alloc smoke test ---- */

static void test_vspace_smoke(void)
{
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

    void *mapped = map_memory(mem_cap, (int)(long)base, 0x4000,
                              PROT_READ | PROT_WRITE);
    ASSERT(mapped != 0, "map_memory into vspace failed");
    ASSERT((unsigned long)mapped == (unsigned long)base,
           "map_memory returned wrong address");

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

static void test_thread_set_ctx(void)
{
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

static void test_stress_threads_1000(void)
{
    TEST("stress: 1000 threads");

    g_stress_counter = 0;
    g_stress_mutex = mutex_create();
    ASSERT(g_stress_mutex > 0, "mutex_create failed");

    /* Create STRESS_THREADS threads; each bumps the shared counter
     * under the mutex and exits.  This is the core requirement: the
     * kernel must support >= 1000 live threads in one process. */
    static int tids[STRESS_THREADS];    /* static: init stack is 1 page */
    int created = 0;
    for (int i = 0; i < STRESS_THREADS; i++) {
        int tid = thread_create(stress_worker, 0, 10);
        if (tid <= 0) {
            printf("FAIL: thread_create #%d returned %d\n", i, tid);
            FAIL("thread_create failed in stress");
            return;
        }
        tids[created++] = tid;
    }

    /* Join all of them. */
    for (int i = 0; i < created; i++) {
        int exit_code = -1;
        int ret = thread_join(tids[i], &exit_code);
        if (ret != 0) {
            printf("FAIL: join #%d (tid=%d) ret=%d\n", i, tids[i], ret);
            FAIL("thread_join failed in stress");
            return;
        }
    }

    ASSERT(g_stress_counter == STRESS_THREADS,
           "shared counter != 1000 (lost increments?)");
    printf("(counter=%d) ", g_stress_counter);

    mutex_destroy(g_stress_mutex);
    PASS();
}

/* ---- Stress: 100k IPC round-trips ---- */

static void test_stress_ipc_100k(void)
{
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
        int req = i;
        int resp = -1;
        int resp_len = sizeof(resp);
        int ret = ipc_call(port, &req, sizeof(req), &resp, &resp_len);
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

static void run_tests(void)
{
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

static void init_protocol(void)
{
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

    printf("  init: system ready\n");
    printf("=== Init Protocol Done ===\n");
}

/* ---- Entry point ---- */

int main(void)
{
    printf("\ninit: Starting (PID 1)\n");

    run_tests();
    init_protocol();

    /* P2: the service manager is now its own PROCESS.  Fetch its
     * embedded ELF image from the kernel blob table (SYS_BLOB_GET)
     * and spawn it via SYS_PROCESS_CREATE.  The manager in turn
     * spawns the serial / flaky / shell services as processes. */
    printf("init: spawning service manager process...\n");
    static char mgr_blob[65536];    /* must hold manager.elf */
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

    /* Idle forever — this thread stays alive as the last scheduler
     * participant while the manager and service processes run. */
    printf("init: idle loop\n");
    for (;;)
        thread_yield();

    return 0;   /* unreachable */
}
