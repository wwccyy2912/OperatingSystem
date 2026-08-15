/*
 * malloc.h - Heap memory allocator
 * Copyright (c) 2026 OpSys Project
 *
 * Simple free-list allocator backed by map_memory syscall.
 * Thread-safety: none (single-threaded user-space for v0.1).
 */

#ifndef MALLOC_H
#define MALLOC_H

#include <stddef.h>

void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

#endif /* MALLOC_H */
