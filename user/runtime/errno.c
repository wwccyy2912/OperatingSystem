/*
 * errno.c - Per-thread error number storage
 * Copyright (c) 2026 OpSys Project
 *
 * Single global __errno for v0.1 (single-threaded user runtime).
 * Multi-threaded user-space will require TLS-backed errno.
 */

int __errno = 0;
