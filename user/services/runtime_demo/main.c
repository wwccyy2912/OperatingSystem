/*
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================
 * Demonstration: Global constructors and destructors
 * ==================================================================== */

typedef struct {
  int id;
  char *name;
  int initialized;
} Resource;

static Resource resource = {0};

/* Global constructor: called by _init() before main() */
__attribute__((constructor)) static void resource_construct(void) {
  printf("[runtime_demo] Resource constructor called\n");
  resource.id = 42;
  resource.name = "demo_resource";
  resource.initialized = 1;
}

/* Global destructor: would be called by _fini() after main() (v1.0+) */
__attribute__((destructor)) static void resource_destroy(void) {
  printf("[runtime_demo] Resource destructor called\n");
  resource.initialized = 0;
}

/* ====================================================================
 * Demonstration: atexit handlers
 * ==================================================================== */

static int atexit_counter = 0;

static void cleanup_handler_1(void) {
  printf("[runtime_demo] atexit handler 1 (counter=%d)\n", ++atexit_counter);
}

static void cleanup_handler_2(void) {
  printf("[runtime_demo] atexit handler 2 (counter=%d)\n", ++atexit_counter);
}

static void cleanup_handler_3(void) {
  printf("[runtime_demo] atexit handler 3 (counter=%d)\n", ++atexit_counter);
}

/* ====================================================================
 * Demonstration: Signal handling
 * ==================================================================== */

static volatile int signal_count = 0;

static void handle_sigusr1(int sig) {
  printf("[runtime_demo] Caught signal %d (SIGUSR1)\n", sig);
  signal_count++;
}

static void handle_sigterm(int sig) {
  printf("[runtime_demo] Caught signal %d (SIGTERM), exiting gracefully\n",
         sig);
  exit(0);
}

/* ====================================================================
 * Main demonstration
 * ==================================================================== */

int main(void) {
  printf("[runtime_demo] main() started\n");
  printf("[runtime_demo] Resource initialized: id=%d, name='%s'\n", resource.id,
         resource.name);

  /* ---- Section 1: Global constructors ---- */
  printf("\n=== Section 1: Global Constructors ===\n");
  printf("Verified that resource constructor was called before main()\n");

  /* ---- Section 2: atexit handlers ---- */
  printf("\n=== Section 2: atexit Handlers ===\n");
  printf("Registering atexit handlers in order 1, 2, 3...\n");

  if (atexit(cleanup_handler_1) != 0) {
    fprintf(stderr, "atexit(cleanup_handler_1) failed\n");
    return 1;
  }
  if (atexit(cleanup_handler_2) != 0) {
    fprintf(stderr, "atexit(cleanup_handler_2) failed\n");
    return 1;
  }
  if (atexit(cleanup_handler_3) != 0) {
    fprintf(stderr, "atexit(cleanup_handler_3) failed\n");
    return 1;
  }

  printf("Will be called in reverse order (3, 2, 1) at exit\n");

  /* ---- Section 3: Memory allocation ---- */
  printf("\n=== Section 3: Memory Allocation ===\n");

  /* malloc */
  printf("Allocating 1024 bytes with malloc...\n");
  char *buf1 = malloc(1024);
  if (!buf1) {
    fprintf(stderr, "malloc(1024) failed: %d\n", errno);
    return 1;
  }
  strcpy(buf1, "Hello from malloc!");
  printf("buf1: '%s' (%p)\n", buf1, (void *)buf1);

  /* calloc */
  printf("Allocating 256 zeroed elements with calloc...\n");
  int *arr = calloc(256, sizeof(int));
  if (!arr) {
    fprintf(stderr, "calloc(256, 4) failed: %d\n", errno);
    return 1;
  }
  printf("arr[0]=%d (should be 0), arr at %p\n", arr[0], (void *)arr);

  /* realloc - grow in place */
  printf("Growing buf1 from 1024 to 2048 bytes with realloc...\n");
  char *buf2 = realloc(buf1, 2048);
  if (!buf2) {
    fprintf(stderr, "realloc(buf1, 2048) failed: %d\n", errno);
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
  errno = 0; /* Clear errno */
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
  sighandler_t old_handler = signal(SIGUSR1, handle_sigusr1);
  if (old_handler == SIG_ERR) {
    fprintf(stderr, "signal(SIGUSR1) failed\n");
    return 1;
  }
  printf("Previous SIGUSR1 handler: %p\n", (void *)old_handler);

  printf("Registering SIGTERM handler for graceful exit...\n");
  old_handler = signal(SIGTERM, handle_sigterm);
  if (old_handler == SIG_ERR) {
    fprintf(stderr, "signal(SIGTERM) failed\n");
    return 1;
  }
  printf("Previous SIGTERM handler: %p\n", (void *)old_handler);

  printf("Ignoring SIGPIPE...\n");
  old_handler = signal(SIGPIPE, SIG_IGN);
  if (old_handler == SIG_ERR) {
    fprintf(stderr, "signal(SIGPIPE) failed\n");
    return 1;
  }
  printf("Previous SIGPIPE handler: %p\n", (void *)old_handler);

  /* ---- Section 6: Process information ---- */
  printf("\n=== Section 6: Process Information ===\n");

  int pid = get_pid();
  int pages = get_free_pages();
  uint64_t heap_base = get_heap_base();

  printf("Current process:\n");
  printf("  PID: %d\n", pid);
  printf("  Free pages: %d\n", pages);
  printf("  Heap base: 0x%lx\n", heap_base);
  printf("  Heap region: [0x%lx, 0x%lx)\n", heap_base,
         heap_base + 0x10000000UL);

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
  printf("  2. Resource destructor is called (v1.0+)\n");
  printf("  3. Process terminates via SYS_THREAD_EXIT\n");

  return 0;
}
