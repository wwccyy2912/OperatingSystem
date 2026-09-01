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
 * errno.h - Error number definitions
 * Copyright (c) 2026 OpSys Project
 */

#ifndef ERRNO_H
#define ERRNO_H

extern int __errno;
#define errno __errno

/* Standard POSIX errno accessor — returns the address of the per-thread
 * errno storage.  v0.1: single global; v1.0+ will return a TLS slot.
 * Provided so libc and user code can use the standard idiom
 * (*__errno_location()) = ENOMEM; without depending on the macro. */
int *__errno_location(void);

/* Standard POSIX error codes */
#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EINTR   4
#define EIO     5
#define ENXIO   6
#define E2BIG   7
#define ENOEXEC 8
#define EBADF   9
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EBUSY   16
#define EEXIST  17
#define EXDEV   18
#define ENODEV  19
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ENFILE  23
#define EMFILE  24
#define ENOSPC  28
#define ESPIPE  29
#define EROFS   30
#define ERANGE  34
#define ENOSYS  38

#endif /* ERRNO_H */
