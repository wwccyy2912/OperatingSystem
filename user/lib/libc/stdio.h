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
 * stdio.h - Standard I/O (C11 §7.21)
 * Copyright (c) 2026 OpSys Project
 *
 * Formatted output via the SYS_DEBUG_LOG syscall (printf/fprintf) and
 * into caller-provided buffers (sprintf/snprintf).  No FILE* streams
 * in v0.1 — all output goes to the debug log.
 *
 * Supported format specifiers:
 *   %d %i %u %x %X %o %c %s %p %%
 *   Length modifiers: l ll h hh z t
 *   Width / precision (limited): %8d %08x %.3s
 *   Flags: 0 (zero-pad), - (left-align), + (force sign), space
 */

#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H

#include <stdarg.h>
#include <stddef.h>

/* ====================================================================
 * Formatted output (C11 §7.21.6)
 * ==================================================================== */

/* Print to the debug log.  Returns chars written. */
int printf(const char *fmt, ...);

/* Print to the debug log from a va_list. */
int vprintf(const char *fmt, va_list ap);

/* fprintf/vfprintf are declared with the real FILE* signature in the
 * buffered-I/O section at the bottom of this header. */

/* Write into buf (unbounded — use snprintf in production). */
int sprintf(char *buf, const char *fmt, ...);
int vsprintf(char *buf, const char *fmt, va_list ap);

/* Write at most n-1 chars into buf, NUL-terminated.  Returns the
 * total number of chars that WOULD be written (C11 semantics). */
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

/* ====================================================================
 * Character / string output (C11 §7.21.7)
 * ==================================================================== */

int puts(const char *str);
int putchar(int c);

/* ====================================================================
 * Character input (C11 §7.21.7) — limited
 * ==================================================================== */

int getchar(void);

/* ====================================================================
 * Buffered file I/O (C11 §7.21.3 / §7.21.5) — v0.9
 *
 * OpSys has no kernel file descriptors: a file is a VFS handle owned by
 * the vfs_server.  libc therefore keeps FILE as an opaque buffered
 * wrapper around a small BACKEND vtable that another library installs
 * at link time.  libfs installs the VFS backend from its own
 * .init_array constructor (user/lib/libfs/stdio_vfs.c), and because
 * every service links the shared user objects, fopen() works
 * out of the box.  With no backend installed (libc linked alone),
 * fopen() fails with errno = ENOSYS while the three standard streams
 * keep working.
 * ==================================================================== */

typedef struct __osys_file FILE;

extern FILE *stdin;  /* console input  — serial debug channel */
extern FILE *stdout; /* console output — serial debug channel */
extern FILE *stderr; /* console output — serial debug channel */

#ifndef EOF
#define EOF (-1)
#endif
#define SEEK_SET     0
#define SEEK_CUR     1
#define SEEK_END     2
#define BUFSIZ       512
#define FILENAME_MAX 256
#define FOPEN_MAX    16

/* Backend operations.  All return 0 on success or a negative error
 * code; read() also reports the byte count through *got (0 = EOF). */
typedef struct {
    int (*open)(const char *path, const char *mode, void **ctx);
    int (*read)(void *ctx, void *buf, unsigned long len, unsigned long *got);
    int (*write)(void *ctx, const void *buf, unsigned long len);
    int (*seek)(void *ctx, long offset, int whence, unsigned long *pos);
    int (*close)(void *ctx);
    unsigned long (*size)(void *ctx);
    int (*unlink)(const char *path);                          /* remove() */
    int (*rename)(const char *oldpath, const char *newpath);  /* rename() */
} stdio_backend_t;

/* Install the process-wide backend (idempotent; the first call wins).
 * Returns 0 on success, ERR_INVAL for a NULL/incomplete table. */
int StdioSetBackend(const stdio_backend_t *be);

/* ---- open / close ---- */
FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *f);
int   fflush(FILE *f);
int   remove(const char *path);              /* VFS delete */
int   rename(const char *oldpath, const char *newpath);

/* ---- sequential I/O ---- */
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int    fgetc(FILE *f);
int    fputc(int c, FILE *f);
char  *fgets(char *s, int size, FILE *f);
int    fputs(const char *s, FILE *f);
int    ungetc(int c, FILE *f);

/* ---- positioning ---- */
int  fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
int  fseeko(FILE *f, long long offset, int whence); /* 64-bit alias */

/* ---- status ---- */
int  feof(FILE *f);
int  ferror(FILE *f);
void clearerr(FILE *f);
int  fileno(FILE *f);

/* ---- formatted I/O ---- */
int fprintf(FILE *f, const char *fmt, ...);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int fscanf(FILE *f, const char *fmt, ...); /* see sscanf for the subset */
int sscanf(const char *s, const char *fmt, ...);
int vsscanf(const char *s, const char *fmt, va_list ap);
void perror(const char *s);

#endif /* LIBC_STDIO_H */
