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
 * malloc.h - Heap memory allocator
 * Copyright (c) 2026 OpSys Project
 *
 * Simple free-list allocator backed by map_memory syscall.
 * Thread-safety: the free list is guarded by a user-space spinlock
 * (zero-syscall fast path, contention yields to the scheduler), so all
 * entry points may be called from any thread.
 */

#ifndef MALLOC_H
#define MALLOC_H

#include <stddef.h>

void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

#endif /* MALLOC_H */
