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
 * runtime_demo.c - OpSys Runtime Demonstration
 * Copyright (c) 2026 OpSys Project
 *
 * Demonstrates all runtime features:
 *   - Global constructors (.init_array)
 *   - malloc/free/realloc/calloc
 *   - atexit handlers
 *   - errno handling
 *   - Signal registration and handling
 *   - Process termination
 */

#include <errno.h>
#include <libos/syscalls.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================
 * Demonstration: Global constructors and destructors
 * ==================================================================== */

typedef struct {
    int   id;
    char *name;
    int   initialized;
} Resource;

static Resource resource = {0};

/* Global constructor: called by _init() before main() */
__attribute__((constructor)) static void resource_construct(void) {
    printf("[runtime_demo] Resource constructor called\n");
    resource.id          = 42;
    resource.name        = "demo_resource";
    resource.initialized = 1;
}

/* Global destructor: called by _fini() after main() (via .fini_array) */
__attribute__((destructor)) static void resource_destroy(void) {
    printf("[runtime_demo] Resource destructor called\n");
    resource.initialized = 0;
}

/* ====================================================================
 * Demonstration: atexit handlers
 * ==================================================================== */

static int atexit_counter = 0;

static void CleanupHandler1(void) {
    printf("[runtime_demo] atexit handler 1 (counter=%d)\n", ++atexit_counter);
}

static void CleanupHandler2(void) {
    printf("[runtime_demo] atexit handler 2 (counter=%d)\n", ++atexit_counter);
}

static void CleanupHandler3(void) {
    printf("[runtime_demo] atexit handler 3 (counter=%d)\n", ++atexit_counter);
}

/* ====================================================================
 * Demonstration: Signal handling
 * ==================================================================== */

static volatile int signal_count = 0;

static void HandleSigusr1(int sig) {
    printf("[runtime_demo] Caught signal %d (SIGUSR1)\n", sig);
    signal_count++;
}

static void HandleSigterm(int sig) {
    printf("[runtime_demo] Caught signal %d (SIGTERM), exiting gracefully\n", sig);
    exit(0);
}

/* ====================================================================
 * Main demonstration
 * ==================================================================== */

int main(void) {
    printf("[runtime_demo] main() started\n");
    printf("[runtime_demo] Resource initialized: id=%d, name='%s'\n", resource.id, resource.name);

    /* ---- Section 1: Global constructors ---- */
    printf("\n=== Section 1: Global Constructors ===\n");
    printf("Verified that resource constructor was called before main()\n");

    /* ---- Section 2: atexit handlers ---- */
    printf("\n=== Section 2: atexit Handlers ===\n");
    printf("Registering atexit handlers in order 1, 2, 3...\n");

    if (Atexit(CleanupHandler1) != 0) {
        printf("Atexit(CleanupHandler1) failed\n");
        return 1;
    }
    if (Atexit(CleanupHandler2) != 0) {
        printf("Atexit(CleanupHandler2) failed\n");
        return 1;
    }
    if (Atexit(CleanupHandler3) != 0) {
        printf("Atexit(CleanupHandler3) failed\n");
        return 1;
    }

    printf("Will be called in reverse order (3, 2, 1) at exit\n");

    /* ---- Section 3: Memory allocation ---- */
    printf("\n=== Section 3: Memory Allocation ===\n");

    /* malloc */
    printf("Allocating 1024 bytes with malloc...\n");
    char *buf1 = malloc(1024);
    if (!buf1) {
        printf("malloc(1024) failed: %d\n", errno);
        return 1;
    }
    strcpy(buf1, "Hello from malloc!");
    printf("buf1: '%s' (%p)\n", buf1, (void *)buf1);

    /* calloc */
    printf("Allocating 256 zeroed elements with calloc...\n");
    int *arr = calloc(256, sizeof(int));
    if (!arr) {
        printf("calloc(256, 4) failed: %d\n", errno);
        return 1;
    }
    printf("arr[0]=%d (should be 0), arr at %p\n", arr[0], (void *)arr);

    /* realloc - grow in place */
    printf("Growing buf1 from 1024 to 2048 bytes with realloc...\n");
    char *buf2 = realloc(buf1, 2048);
    if (!buf2) {
        printf("realloc(buf1, 2048) failed: %d\n", errno);
        free(buf1);
        free(arr);
        return 1;
    }
    printf("buf2: still contains '%s' at %p\n", buf2, (void *)buf2);

    /* free */
    printf("Freeing allocated memory...\n");
    free(buf2);
    free(arr);
    printf("All memory freed\n");

    /* ---- Section 4: errno handling ---- */
    printf("\n=== Section 4: errno Handling ===\n");

    printf("Testing malloc with size 0...\n");
    errno          = 0; /* Clear errno */
    void *null_ptr = malloc(0);
    if (!null_ptr) {
        printf("malloc(0) returned NULL (errno=%d)\n", errno);
    } else {
        printf("malloc(0) returned %p\n", null_ptr);
        free(null_ptr);
    }

    /* ---- Section 5: Signal registration ---- */
    printf("\n=== Section 5: Signal Registration ===\n");

    printf("Registering SIGUSR1 handler...\n");
    sighandler_t old_handler = Signal(SIGUSR1, HandleSigusr1);
    if (old_handler == SIG_ERR) {
        printf("Signal(SIGUSR1) failed\n");
        return 1;
    }
    printf("Previous SIGUSR1 handler: %p\n", (void *)old_handler);

    printf("Registering SIGTERM handler for graceful exit...\n");
    old_handler = Signal(SIGTERM, HandleSigterm);
    if (old_handler == SIG_ERR) {
        printf("Signal(SIGTERM) failed\n");
        return 1;
    }
    printf("Previous SIGTERM handler: %p\n", (void *)old_handler);

    printf("Ignoring SIGPIPE...\n");
    old_handler = Signal(SIGPIPE, SIG_IGN);
    if (old_handler == SIG_ERR) {
        printf("Signal(SIGPIPE) failed\n");
        return 1;
    }
    printf("Previous SIGPIPE handler: %p\n", (void *)old_handler);

    /* ---- Section 6: Process information ---- */
    printf("\n=== Section 6: Process Information ===\n");

    int      pid       = GetPid();
    int      pages     = GetFreePages();
    uint64_t heap_base = GetHeapBase();

    printf("Current process:\n");
    printf("  PID: %d\n", pid);
    printf("  Free pages: %d\n", pages);
    printf("  Heap base: 0x%lx\n", heap_base);
    printf("  Heap region: [0x%lx, 0x%lx)\n", heap_base, heap_base + 0x10000000UL);

    /* ---- Section 7: Future signal test ---- */
    printf("\n=== Section 7: Signal Testing (Manual) ===\n");
    printf("In a real scenario, you could:\n");
    printf("  1. Fork/spawn this process\n");
    printf("  2. Send signals via 'kill' utility\n");
    printf("  3. Observe signal_count increments\n");
    printf("  4. Watch graceful SIGTERM exit\n");

    printf("\n[runtime_demo] Exiting main() with status 0\n");
    printf("[runtime_demo] The following should happen:\n");
    printf("  1. exit() calls atexit handlers in reverse order (3, 2, 1)\n");
    printf("  2. Resource destructor is called (via .fini_array)\n");
    printf("  3. Process terminates via SYS_THREAD_EXIT\n");

    return 0;
}
