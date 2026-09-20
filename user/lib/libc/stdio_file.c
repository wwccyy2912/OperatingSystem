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
 * stdio_file.c - buffered FILE I/O over a pluggable backend
 * Copyright (c) 2026 OpSys Project
 *
 * OpSys has no kernel file descriptors: a file is a VFS handle owned by
 * the vfs_server.  libc therefore keeps FILE as an opaque record that
 * wraps a small BACKEND vtable; another library installs the vtable at
 * link time (libfs installs the VFS backend from its .init_array
 * constructor, see user/lib/libfs/stdio_vfs.c).  With no backend
 * installed, fopen() fails with errno = ENOSYS while the three standard
 * streams keep working over the serial debug channel.
 *
 * ------------------------------------------------------------------
 * Structure (FILE + backend vtable):
 *   FILE { ctx; read buffer; up to 4 push-back bytes; flags; fd; pos }
 *   stdio_backend_t { open, read, write, seek, close, size, unlink, rename }
 *   fopen()  -> be->open() + alloc FILE (+ 512-byte read buffer)
 *   fread()  -> buffered; single large requests bypass the buffer
 *   fwrite() -> straight through to be->write()
 *   vsscanf()/fscanf() -> one scanner over a byte source (string or FILE)
 * How it works:
 *   Every operation goes through the single process-wide backend, so
 *   libc never needs to know what a path means.  The standard streams
 *   (stdin/stdout/stderr) are static FILE records with fd 0/1/2 and no
 *   backend: writes land in the serial debug log, reads come from the
 *   serial input syscall (which blocks until a byte arrives).
 *   A FILE keeps a 512-byte read-ahead buffer plus a small push-back
 *   stack (ungetc and the scanf look-ahead).  f->pos is the logical
 *   offset; before a write that would break the read-ahead invariant
 *   (update streams) the backend is re-seeked to f->pos, so "r+" works
 *   without an intervening fseek().  Backend failures come back as
 *   negative ERR_* codes and are translated to positive errno values.
 * Purpose:
 *   Give user-space programs a familiar stdio API (fopen/fgets/fprintf/
 *   fscanf) on top of the VFS without introducing kernel file
 *   descriptors.
 * Caveats:
 *   One backend per process (installed once, first call wins); no write
 *   buffering (every fwrite reaches the backend immediately, so fflush
 *   only drops read-ahead); no wide-character or locale-aware
 *   formatting; the scanf engine covers the conversions listed in its
 *   own comment below but has no scansets (%[); paths are VFS URLs, not
 *   POSIX paths (libc has no current directory).
 * ------------------------------------------------------------------
 */

#include "stdio.h"

#include "../libos/syscalls.h" /* DebugLog / DebugGetchar / ERR_* */
#include "stdlib.h"            /* strtod (the %f conversions)     */
#include <errno.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Freestanding GCC headers do not chain to a system <limits.h>, so the
 * one constant the scanner needs is spelled out here (as stdlib.c does
 * for the signed limits). */
#ifndef ULLONG_MAX
#define ULLONG_MAX 18446744073709551615ULL
#endif
/* ---- FILE record ---------------------------------------------------- */

#define _F_READ    0x01u
#define _F_WRITE   0x02u
#define _F_APPEND  0x04u
#define _F_EOF     0x08u
#define _F_ERR     0x10u
#define _F_CONSOLE 0x20u

#define _F_RDBUF   512u
/* ungetc() bytes (and the scanf look-ahead) parked in the FILE.  C only
 * promises one push-back; four keep the scanner's widest look-ahead
 * ("0x" without a hex digit, "1e" without exponent digits) intact. */
#define _F_PUSHMAX 4u

struct __osys_file {
    void          *ctx;   /* backend context (NULL for the console streams) */
    unsigned char *rbuf;  /* owned read buffer, _F_RDBUF bytes, or NULL     */
    unsigned long  rpos;  /* next byte to hand out inside rbuf              */
    unsigned long  rlen;  /* valid bytes inside rbuf                        */
    unsigned long  pos;   /* logical file offset (bytes consumed)           */
    unsigned       flags;
    int            fd;    /* console descriptor (0/1/2), else -1            */
    int            npush; /* bytes parked in push[] (0 = none)              */
    unsigned char  push[_F_PUSHMAX]; /* LIFO: push[npush - 1] is next      */
};

static const stdio_backend_t *s_be;

/* The three standard streams: no backend, straight to the debug log. */
static struct __osys_file s_stdin  = {
    .flags = _F_READ | _F_CONSOLE, .fd = 0, .npush = 0, .push = {0}};
static struct __osys_file s_stdout = {
    .flags = _F_WRITE | _F_CONSOLE, .fd = 1, .npush = 0, .push = {0}};
static struct __osys_file s_stderr = {
    .flags = _F_WRITE | _F_CONSOLE, .fd = 2, .npush = 0, .push = {0}};

FILE *stdin  = &s_stdin;
FILE *stdout = &s_stdout;
FILE *stderr = &s_stderr;

int StdioSetBackend(const stdio_backend_t *be) {
    if (!be || !be->open || !be->read || !be->write || !be->close)
        return ERR_INVAL;
    /* First call wins: a second install would leave the FILE records
     * created against the old backend dangling. */
    if (s_be)
        return 0;
    s_be = be;
    return 0;
}

const stdio_backend_t *StdioGetBackend(void); /* internal, used by tests */
const stdio_backend_t *StdioGetBackend(void) {
    return s_be;
}

/* ---- shared helpers -------------------------------------------------- */

/* Backend calls report negative ERR_* codes (libos/syscalls.h); errno is
 * what the C library promises, so translate instead of storing the raw
 * value (the tree has no EILSEQ / EOVERFLOW / ENOTEMPTY yet). */
static int ErrnoFromCode(int code) {
    switch (code) {
    case ERR_NOMEM: return ENOMEM;
    case ERR_INVAL: return EINVAL;
    case ERR_NOCAP: return EACCES;
    case ERR_DENIED: return EACCES;
    case ERR_NOENT: return ENOENT;
    case ERR_BUSY: return EBUSY;
    case ERR_AGAIN: return EAGAIN;
    case ERR_FAULT: return EFAULT;
    case ERR_OVERFLOW: return ERANGE;
    case ERR_INTERRUPTED: return EINTR;
    default: return code < 0 ? EIO : code;
    }
}

/* Forget read-ahead and push-back bytes: after this the backend position
 * is authoritative again and f->pos is the only offset that matters. */
static void ResetReadState(FILE *f) {
    f->rpos  = 0;
    f->rlen  = 0;
    f->npush = 0;
}

/* Re-seek the backend to f->pos.  Needed when read-ahead (or a push-back
 * byte) left the backend ahead of the logical offset and the next
 * operation is a write or a fflush() of an input stream. */
static int SyncBackendPos(FILE *f) {
    if (!s_be || !s_be->seek || !f->ctx)
        return 0; /* nothing to synchronise (console / bare libc) */
    unsigned long pos = 0;
    int           r   = s_be->seek(f->ctx, (long)f->pos, SEEK_SET, &pos);
    if (r < 0) {
        errno = ErrnoFromCode(r);
        return -1;
    }
    return 0;
}

/* ---- console helpers ------------------------------------------------ */

static int ConsoleWrite(const char *buf, unsigned long len) {
    char          chunk[129];
    unsigned long done = 0;
    while (done < len) {
        unsigned long n = len - done;
        if (n > sizeof(chunk) - 1)
            n = sizeof(chunk) - 1;
        memcpy(chunk, buf + done, n);
        chunk[n] = '\0';
        DebugLog(chunk);
        done += n;
    }
    return 0;
}

/* Console input.  DebugGetchar() returns the byte, or a negative ERR_*
 * code when the caller lacks the COM1 IO-port capability — that is an
 * error, not a 255-character, so normalise it to EOF. */
static int ConsoleGetchar(FILE *f) {
    int c = DebugGetchar();
    if (c < 0) {
        f->flags |= _F_ERR;
        errno = (c == ERR_NOCAP) ? EACCES : EIO;
        return EOF;
    }
    return c;
}

/* ---- open / close --------------------------------------------------- */

static int ModeParse(const char *mode, unsigned *flags) {
    unsigned f    = 0;
    int      plus = 0;
    if (!mode || !mode[0])
        return ERR_INVAL;
    for (const char *p = mode; *p; p++) {
        switch (*p) {
        case 'r': f |= _F_READ; break;
        case 'w': f |= _F_WRITE; break;
        case 'a': f |= _F_WRITE | _F_APPEND; break;
        case '+': plus = 1; break;
        case 'b': break; /* binary == text on OpSys */
        default: return ERR_INVAL;
        }
    }
    if (plus)
        f |= _F_READ | _F_WRITE;
    if (!(f & (_F_READ | _F_WRITE)))
        return ERR_INVAL;
    *flags = f;
    return 0;
}

FILE *fopen(const char *path, const char *mode) {
    unsigned flags = 0;
    if (!path || !path[0]) {
        errno = EINVAL;
        return NULL;
    }
    if (ModeParse(mode, &flags) < 0) {
        errno = EINVAL;
        return NULL;
    }
    if (!s_be) {
        /* No backend: only the console streams exist. */
        errno = ENOSYS;
        return NULL;
    }

    void *ctx = NULL;
    int   r   = s_be->open(path, mode, &ctx);
    if (r < 0 || !ctx) {
        errno = r < 0 ? ErrnoFromCode(r) : EIO;
        return NULL;
    }

    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) {
        s_be->close(ctx);
        errno = ENOMEM;
        return NULL;
    }
    memset(f, 0, sizeof(*f));
    f->ctx   = ctx;
    f->flags = flags;
    f->fd    = -1;
    if (flags & _F_READ) {
        f->rbuf = (unsigned char *)malloc(_F_RDBUF);
        if (!f->rbuf) {
            s_be->close(ctx);
            free(f);
            errno = ENOMEM;
            return NULL;
        }
    }
    /* Ask the backend where the file starts: append mode begins at the
     * end of the file, so ftell() would otherwise lie until the first
     * fseek(). */
    if (s_be->seek) {
        unsigned long pos = 0;
        if (s_be->seek(ctx, 0, SEEK_CUR, &pos) == 0)
            f->pos = pos;
    }
    return f;
}

int fclose(FILE *f) {
    if (!f)
        return EOF;
    if (f->flags & _F_CONSOLE)
        return 0; /* never close the standard streams */
    int r = 0;
    if (f->ctx && s_be)
        r = s_be->close(f->ctx);
    if (f->rbuf)
        free(f->rbuf);
    free(f);
    if (r < 0) {
        errno = ErrnoFromCode(r);
        return EOF;
    }
    return 0;
}

/* ---- reading -------------------------------------------------------- */

/* Refill the read-ahead buffer.  Returns the byte count, 0 at end of
 * file (EOF flag set, sticky until clearerr()/fseek()) and -1 on error. */
static int FillBuf(FILE *f) {
    if (!f->rbuf || !s_be || !f->ctx) {
        f->flags |= _F_EOF;
        return 0;
    }
    if (f->flags & _F_EOF)
        return 0; /* don't hammer the backend once EOF is known */
    unsigned long got = 0;
    int           r   = s_be->read(f->ctx, f->rbuf, _F_RDBUF, &got);
    if (r < 0) {
        f->flags |= _F_ERR;
        errno = ErrnoFromCode(r);
        return -1;
    }
    f->rpos = 0;
    f->rlen = got;
    if (got == 0)
        f->flags |= _F_EOF;
    return (int)got;
}

/* Consume one byte that is already in the FILE (push-back or read-ahead
 * buffer).  Returns the byte, or EOF when both are empty. */
static int GetBuffered(FILE *f) {
    if (f->npush > 0)
        return (int)f->push[--f->npush];
    if (f->rpos < f->rlen)
        return (int)f->rbuf[f->rpos++];
    return EOF;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (!f || !ptr || size == 0 || nmemb == 0)
        return 0;
    if (nmemb > SIZE_MAX / size) { /* the product would wrap */
        f->flags |= _F_ERR;
        errno = EINVAL;
        return 0;
    }
    size_t want = size * nmemb;
    if (!(f->flags & _F_READ)) {
        f->flags |= _F_ERR;
        errno = EACCES;
        return 0;
    }

    unsigned char *dst  = (unsigned char *)ptr;
    size_t         done = 0;

    /* The console has no read-ahead buffer: serve it byte by byte (each
     * fgetc() blocks until the kernel has a byte). */
    if (f->flags & _F_CONSOLE) {
        while (done < want) {
            int c = fgetc(f);
            if (c == EOF)
                break;
            dst[done++] = (unsigned char)c;
        }
        return done / size;
    }

    /* Serve whatever is already buffered (push-back bytes first). */
    for (;;) {
        int c = GetBuffered(f);
        if (c == EOF || done >= want)
            break;
        dst[done++] = (unsigned char)c;
    }

    /* Large requests bypass the buffer entirely (one backend call per
     * chunk instead of a copy through rbuf).  The loop keeps going
     * while the remainder is still worth a direct read, so a backend
     * that answers with short reads is handled without falling back to
     * buffering in between. */
    while (f->ctx && s_be && !(f->flags & _F_EOF) && want - done >= _F_RDBUF) {
        unsigned long got = 0;
        int           r   = s_be->read(f->ctx, dst + done, (unsigned long)(want - done), &got);
        if (r < 0) {
            f->flags |= _F_ERR;
            errno = ErrnoFromCode(r);
            return done / size;
        }
        done += got;
        if (got == 0) {
            f->flags |= _F_EOF;
            break;
        }
    }

    while (done < want) {
        if (f->rpos >= f->rlen && FillBuf(f) <= 0)
            break;
        dst[done++] = f->rbuf[f->rpos++];
    }

    f->pos += done;
    return done / size;
}

int fgetc(FILE *f) {
    if (!f)
        return EOF;
    if (f->npush > 0) {
        f->pos++;
        return (int)f->push[--f->npush];
    }
    if (f->flags & _F_CONSOLE) {
        int c = ConsoleGetchar(f);
        if (c != EOF)
            f->pos++;
        return c;
    }
    if (f->rpos >= f->rlen && FillBuf(f) <= 0)
        return EOF;
    f->pos++;
    return (int)f->rbuf[f->rpos++];
}

char *fgets(char *s, int size, FILE *f) {
    if (!s || size <= 0 || !f)
        return NULL;
    int n = 0;
    while (n < size - 1) {
        int c = fgetc(f);
        if (c == EOF)
            break; /* the EOF/error flag was set by the read itself */
        s[n++] = (char)c;
        if (c == '\n')
            break;
    }
    if (n == 0)
        return NULL;
    s[n] = '\0';
    return s;
}

int ungetc(int c, FILE *f) {
    if (!f || c == EOF)
        return EOF;
    if (f->npush >= (int)_F_PUSHMAX)
        return EOF; /* push-back stack full (C only requires one) */
    f->push[f->npush++] = (unsigned char)c;
    if (f->pos > 0)
        f->pos--; /* the byte is logically unread */
    f->flags &= ~_F_EOF;
    return (unsigned char)c;
}

/* ---- writing -------------------------------------------------------- */

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (!f || !ptr || size == 0 || nmemb == 0)
        return 0;
    if (nmemb > SIZE_MAX / size) {
        f->flags |= _F_ERR;
        errno = EINVAL;
        return 0;
    }
    size_t len = size * nmemb;
    if (!(f->flags & _F_WRITE)) {
        f->flags |= _F_ERR;
        errno = EACCES;
        return 0;
    }
    if (f->flags & _F_CONSOLE) {
        ConsoleWrite((const char *)ptr, (unsigned long)len);
        f->pos += len;
        return nmemb;
    }
    if (!s_be || !f->ctx) {
        f->flags |= _F_ERR;
        errno = ENOSYS;
        return 0;
    }
    /* Update streams ("r+"): read-ahead (or an ungetc) leaves the
     * backend ahead of f->pos, so realign it before writing. */
    if ((f->rlen > f->rpos || f->npush > 0) && SyncBackendPos(f) < 0) {
        f->flags |= _F_ERR;
        return 0;
    }
    ResetReadState(f);
    int r = s_be->write(f->ctx, ptr, (unsigned long)len);
    if (r < 0) {
        f->flags |= _F_ERR;
        errno = ErrnoFromCode(r);
        return 0;
    }
    f->pos += len;
    return nmemb;
}

int fputc(int c, FILE *f) {
    unsigned char b = (unsigned char)c;
    return fwrite(&b, 1, 1, f) == 1 ? (int)b : EOF;
}

int fputs(const char *s, FILE *f) {
    if (!s)
        return EOF;
    size_t len = strlen(s);
    return fwrite(s, 1, len, f) == len ? 0 : EOF;
}

int fflush(FILE *f) {
    /* Nothing is buffered on the write path: every fwrite has already
     * reached the backend.  On an input stream POSIX asks fflush to
     * discard buffered data and reposition to the last read offset,
     * which is exactly what syncing the backend here does.  fflush(NULL)
     * means "every output stream" and stays a no-op. */
    if (!f)
        return 0;
    if (f->flags & _F_READ) {
        ResetReadState(f);
        return SyncBackendPos(f);
    }
    return 0;
}

/* ---- positioning ---------------------------------------------------- */

int fseek(FILE *f, long offset, int whence) {
    if (!f)
        return -1;
    if (f->flags & _F_CONSOLE) {
        errno = ESPIPE; /* the console is not seekable */
        return -1;
    }

    long target = offset;
    if (whence == SEEK_END) {
        unsigned long sz = (s_be && s_be->size && f->ctx) ? s_be->size(f->ctx) : 0;
        target           = (long)sz + offset;
    } else if (whence == SEEK_CUR) {
        target = (long)f->pos + offset;
    } else if (whence != SEEK_SET) {
        errno = EINVAL;
        return -1;
    }
    if (target < 0) {
        errno = EINVAL;
        return -1;
    }

    /* Move the backend first: on failure the buffered state is still
     * valid and the stream is left untouched. */
    if (s_be && s_be->seek && f->ctx) {
        unsigned long pos = 0;
        int           r   = s_be->seek(f->ctx, target, SEEK_SET, &pos);
        if (r < 0) {
            errno = ErrnoFromCode(r);
            return -1;
        }
        ResetReadState(f);
        f->flags &= ~_F_EOF;
        f->pos = pos;
    } else {
        ResetReadState(f);
        f->flags &= ~_F_EOF;
        f->pos = (unsigned long)target;
    }
    return 0;
}

int fseeko(FILE *f, long long offset, int whence) {
    /* 64-bit alias: offsets are narrowed to long, which is 64-bit on
     * x86_64 anyway. */
    return fseek(f, (long)offset, whence);
}

long ftell(FILE *f) {
    if (!f)
        return -1;
    if (f->flags & _F_CONSOLE) {
        errno = ESPIPE;
        return -1;
    }
    return (long)f->pos;
}

void rewind(FILE *f) {
    if (!f)
        return;
    if (fseek(f, 0, SEEK_SET) == 0)
        clearerr(f); /* C11: rewind() also clears the error indicator */
}

/* ---- status --------------------------------------------------------- */

int feof(FILE *f) {
    return f && (f->flags & _F_EOF);
}

int ferror(FILE *f) {
    return f && (f->flags & _F_ERR);
}

void clearerr(FILE *f) {
    if (f)
        f->flags &= ~(_F_EOF | _F_ERR);
}

int fileno(FILE *f) {
    return f ? f->fd : -1;
}

/* ---- formatted output ----------------------------------------------- */

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    if (!f || !fmt)
        return -1;
    char buf[1024];
    int  n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0)
        return -1;
    unsigned long len = (unsigned long)n;
    if (len > sizeof(buf) - 1)
        len = sizeof(buf) - 1; /* truncated: see the header note */
    if (len > 0 && fwrite(buf, 1, len, f) != len)
        return -1;
    return n;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- formatted input ------------------------------------------------
 * One scanner serves both sscanf() (over a string) and fscanf() (over a
 * FILE): the engine reads bytes through a tiny source vtable and pushes
 * back what it looked at but did not use.
 *
 * Supported: %d %i %u %o %x %X (integers, %i auto-detects base),
 *   %p (hexadecimal pointer), %c (exactly WIDTH bytes, no whitespace
 *   skip, no NUL), %s (whitespace-delimited word, NUL-terminated),
 *   %a %e %f %g and their upper-case forms (floating point parsed like
 *   strtod(), l = double, L = long double), %n (bytes consumed so far;
 *   does not count as an assignment), %% (literal per cent), the '*'
 *   suppression flag, decimal field widths (%31s, %*d) and the length
 *   modifiers h hh l ll z t j L.
 * Not supported: scansets (%[...]), the GNU %m allocation flag, and the
 *   wide conversions %C/%S/%ls.  Meeting one stops the scan and returns
 *   the number of items assigned so far.
 *
 * Semantics: input whitespace is skipped before every conversion except
 *   %c/%n; each integer item needs at least one digit; EOF is returned
 *   when the input runs out before the first assignment; otherwise the
 *   count of assigned items is returned.  An integer item that does not
 *   fit the destination type is truncated modulo that type (C11 calls
 *   this undefined); the internal 64-bit accumulator saturates instead
 *   of wrapping.  A floating-point item longer than 63 bytes is cut
 *   short like a field width, leaving the rest of it unread.
 *
 * Caveats: stdio's own look-ahead reads one byte past an item and gives
 *   it back with ungetc(); on the console (stdin) each fgetc() blocks on
 *   the serial port, so fscanf(stdin, "%d") blocks until the byte after
 *   the number arrives.  Interactive code should read a whole line with
 *   fgets() and then parse it with sscanf().
 * ==================================================================== */

#define SCAN_STASH 8  /* bytes the scanner may look ahead and give back   */
#define SCAN_TOK   64 /* longest floating-point item the scanner accepts  */

typedef struct {
    int (*get)(void *ctx); /* next byte, or EOF */
    void (*putback)(void *ctx, int c);
    void *ctx;
} scan_src_t;

typedef struct {
    scan_src_t src;
    int        stash[SCAN_STASH]; /* ungot bytes, most recent on top */
    int        nstash;
    int        eof;   /* some raw read hit the end of the input */
    long       count; /* bytes consumed from the source so far (%n) */
} scan_t;

/* Length modifiers recognized by the scanner. */
typedef enum {
    LEN_NONE = 0,
    LEN_HH,
    LEN_H,
    LEN_L,
    LEN_LL,
    LEN_Z,
    LEN_T,
    LEN_J,
    LEN_CAP_L, /* 'L' — long double for the floating conversions */
} scan_len_t;

static int ScanGet(scan_t *st) {
    if (st->nstash > 0) {
        st->count++;
        return st->stash[--st->nstash];
    }
    int c = st->src.get(st->src.ctx);
    if (c == EOF)
        st->eof = 1;
    else
        st->count++;
    return c;
}

static void ScanUnget(scan_t *st, int c) {
    if (c == EOF)
        return; /* nothing to give back */
    st->eof = 0;
    if (st->nstash >= SCAN_STASH)
        return; /* cannot happen: every conversion looks ahead by <= 3 */
    st->stash[st->nstash++] = c;
    st->count--;
}

/* Give the leftover look-ahead back to the source.  The stash is a LIFO
 * with the oldest byte at index 0, so the puts happen oldest-first: the
 * source (also a LIFO for FILE) then hands the bytes out in order. */
static void ScanFlushStash(scan_t *st) {
    for (int i = 0; i < st->nstash; i++)
        st->src.putback(st->src.ctx, st->stash[i]);
    st->nstash = 0;
}

/* Read the next byte for a conversion that is limited by a field width.
 * A width-exhausted item behaves like the end of the input (nothing is
 * consumed and the raw EOF flag is left alone). */
static int ScanNext(scan_t *st, int width, int *used) {
    if (width != 0 && *used >= width)
        return EOF;
    int c = ScanGet(st);
    if (c != EOF)
        (*used)++;
    return c;
}

static int ScanIsSpace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static int ScanDigitValue(int c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Skip input whitespace, leaving the first non-space byte in the stash. */
static void ScanSkipSpace(scan_t *st) {
    int c = ScanGet(st);
    while (c != EOF && ScanIsSpace(c))
        c = ScanGet(st);
    ScanUnget(st, c);
}

/* Scan an integer item.  Returns 1 when at least one digit was
 * converted (magnitude in *val, sign in *neg), 0 on matching failure. */
static int ScanInt(scan_t *st, int width, int base_in, int *neg_out, unsigned long long *val_out) {
    int                used   = 0;
    int                neg    = 0;
    int                base   = base_in;
    unsigned long long acc    = 0;
    int                digits = 0;
    int                c      = ScanNext(st, width, &used);

    if (c == '+' || c == '-') {
        neg = (c == '-');
        c   = ScanNext(st, width, &used);
    }

    if (c == '0' && (base == 0 || base == 16)) {
        int c2 = ScanNext(st, width, &used);
        if (c2 == 'x' || c2 == 'X') {
            int c3 = ScanNext(st, width, &used);
            int d3 = (c3 == EOF) ? -1 : ScanDigitValue(c3);
            if (d3 >= 0) {
                base   = 16;
                digits = 1;
                acc    = (unsigned long long)d3;
                c      = ScanNext(st, width, &used);
            } else {
                /* "0x" without a hex digit: the item is just "0". */
                ScanUnget(st, c3);
                ScanUnget(st, c2);
                c      = EOF;
                digits = 1;
            }
        } else {
            /* A leading zero only means octal for %i (base 0). */
            base   = (base_in == 0) ? 8 : base_in;
            digits = 1;
            c      = c2;
        }
    } else if (base == 0) {
        base = 10;
    }

    while (c != EOF) {
        int d = ScanDigitValue(c);
        if (d < 0 || d >= base)
            break;
        if (acc > (ULLONG_MAX - (unsigned long long)d) / (unsigned long long)base)
            acc = ULLONG_MAX; /* saturate; C11 leaves overflow undefined */
        else
            acc = acc * (unsigned long long)base + (unsigned long long)d;
        digits = 1;
        c      = ScanNext(st, width, &used);
    }
    ScanUnget(st, c);

    if (!digits)
        return 0; /* matching failure: nothing was converted */
    *neg_out = neg;
    *val_out = acc;
    return 1;
}

/* Append a byte to a floating-point token.  Returns 0 when the token is
 * full: the caller then stops, so the item is cut short exactly like a
 * field width instead of silently dropping high-order digits. */
static int TokPut(char *tok, size_t toksz, size_t *len, int c) {
    if (*len + 1 >= toksz)
        return 0;
    tok[(*len)++] = (char)c;
    return 1;
}

/* Scan a floating-point item (also inf/nan, also hex floats) into tok.
 * Returns 1 when an item was produced, 0 on matching failure. */
static int ScanFloatToken(scan_t *st, int width, char *tok, size_t toksz) {
    int    used   = 0;
    size_t len    = 0;
    int    digits = 0;
    int    c      = ScanNext(st, width, &used);

    if (c == EOF)
        return 0;
    if (c == '+' || c == '-') {
        if (!TokPut(tok, toksz, &len, c))
            return 0;
        c = ScanNext(st, width, &used);
        if (c == EOF)
            return 0; /* a lone sign is not an item */
    }

    /* inf / infinity. */
    if (c == 'i' || c == 'I') {
        static const char inf_word[] = "inf";
        static const char inf_tail[] = "inity";
        for (int i = 0; i < 3; i++) {
            int up = (c >= 'A' && c <= 'Z') ? c + 32 : c;
            if (up != inf_word[i] || !TokPut(tok, toksz, &len, c))
                return 0;
            c = ScanNext(st, width, &used);
        }
        char tail[sizeof(inf_tail) - 1];
        int  ntail = 0;
        for (int i = 0; i < (int)(sizeof(inf_tail) - 1); i++) {
            int up = (c >= 'A' && c <= 'Z') ? c + 32 : c;
            if (up != inf_tail[i] || !TokPut(tok, toksz, &len, c))
                break;
            tail[ntail++] = (char)c;
            c             = ScanNext(st, width, &used);
        }
        /* "inf" is an item, "infi" is not: give a partial tail back. */
        if (ntail < (int)(sizeof(inf_tail) - 1)) {
            while (ntail > 0)
                ScanUnget(st, tail[--ntail]);
        }
        ScanUnget(st, c);
        tok[len] = '\0';
        return 1;
    }

    /* nan / nan(n-char-sequence) — the payload is optional and its
     * contents do not affect the value, so a short look-ahead decides
     * whether it belongs to the item. */
    if (c == 'n' || c == 'N') {
        static const char nan_word[] = "nan";
        for (int i = 0; i < 3; i++) {
            int up = (c >= 'A' && c <= 'Z') ? c + 32 : c;
            if (up != nan_word[i] || !TokPut(tok, toksz, &len, c))
                return 0;
            c = ScanNext(st, width, &used);
        }
        if (c == '(') {
            char look[SCAN_STASH];
            int  n      = 0;
            int  closed = 0;
            while (n < (int)sizeof(look)) {
                c = ScanNext(st, width, &used);
                if (c == EOF)
                    break;
                look[n++] = (char)c;
                if (c == ')') {
                    closed = 1;
                    break;
                }
            }
            if (closed) {
                for (int i = 0; i < n; i++)
                    (void)TokPut(tok, toksz, &len, look[i]);
                c = ScanNext(st, width, &used);
            } else {
                while (n > 0)
                    ScanUnget(st, look[--n]);
            }
        }
        ScanUnget(st, c);
        tok[len] = '\0';
        return 1;
    }

    /* Numeric forms: [0x] digits [. digits] [exponent]. */
    int hex = 0;
    if (c == '0') {
        if (!TokPut(tok, toksz, &len, c))
            return 0;
        digits = 1;
        c      = ScanNext(st, width, &used);
        if (c == 'x' || c == 'X') {
            char skip = (char)c;
            c         = ScanNext(st, width, &used);
            int d     = (c == EOF) ? -1 : ScanDigitValue(c);
            if (d >= 0) {
                if (!TokPut(tok, toksz, &len, skip) || !TokPut(tok, toksz, &len, c))
                    return 0;
                hex = 1;
                c   = ScanNext(st, width, &used);
            } else {
                /* "0x" without a hex digit: the item is just "0". */
                ScanUnget(st, c);
                ScanUnget(st, skip);
                c = EOF;
            }
        }
    }

    int base = hex ? 16 : 10;
    while (c != EOF) {
        int d = ScanDigitValue(c);
        if (d < 0 || d >= base)
            break;
        if (!TokPut(tok, toksz, &len, c))
            break; /* token full: leave the rest unread (c is ungot below) */
        digits = 1;
        c      = ScanNext(st, width, &used);
    }
    if (c == '.') {
        if (TokPut(tok, toksz, &len, c)) {
            c = ScanNext(st, width, &used);
            while (c != EOF) {
                int d = ScanDigitValue(c);
                if (d < 0 || d >= base)
                    break;
                if (!TokPut(tok, toksz, &len, c))
                    break;
                digits = 1;
                c      = ScanNext(st, width, &used);
            }
        }
    }

    /* Exponent: it counts only when at least one decimal digit follows
     * the marker (and an optional sign). */
    if ((hex && (c == 'p' || c == 'P')) || (!hex && (c == 'e' || c == 'E'))) {
        int marker = c;
        int c1     = ScanNext(st, width, &used);
        int sgnch  = 0;
        int c2     = EOF;
        int ok;
        if (c1 == '+' || c1 == '-') {
            sgnch = c1;
            c2    = ScanNext(st, width, &used);
            ok    = (c2 >= '0' && c2 <= '9');
        } else {
            ok = (c1 >= '0' && c1 <= '9');
        }
        if (ok) {
            (void)TokPut(tok, toksz, &len, marker);
            if (sgnch)
                (void)TokPut(tok, toksz, &len, sgnch);
            c = sgnch ? c2 : c1;
            while (c >= '0' && c <= '9') {
                if (!TokPut(tok, toksz, &len, c))
                    break;
                c = ScanNext(st, width, &used);
            }
        } else {
            ScanUnget(st, sgnch ? c2 : c1);
            if (sgnch)
                ScanUnget(st, sgnch);
            ScanUnget(st, marker);
            c = EOF; /* the item ends before the exponent marker */
        }
    }

    ScanUnget(st, c);
    tok[len] = '\0';
    return digits;
}

/* ---- assignment helpers --------------------------------------------- */

/* Store an integer item.  Out-of-range items are truncated modulo the
 * destination type (C11 declares the behaviour undefined). */
static void StoreSigned(va_list *ap, scan_len_t len, int neg, unsigned long long v) {
    unsigned long long uv = neg ? (0ULL - v) : v;
    switch (len) {
    case LEN_HH: *va_arg(*ap, signed char *) = (signed char)uv; break;
    case LEN_H: *va_arg(*ap, short *) = (short)uv; break;
    case LEN_L: *va_arg(*ap, long *) = (long)uv; break;
    case LEN_LL: *va_arg(*ap, long long *) = (long long)uv; break;
    case LEN_Z: *va_arg(*ap, long *) = (long)uv; break; /* signed size_t */
    case LEN_T: *va_arg(*ap, ptrdiff_t *) = (ptrdiff_t)uv; break;
    case LEN_J: *va_arg(*ap, intmax_t *) = (intmax_t)uv; break;
    default: *va_arg(*ap, int *) = (int)uv; break;
    }
}

static void StoreUnsigned(va_list *ap, scan_len_t len, int neg, unsigned long long v) {
    if (neg)
        v = 0ULL - v;
    switch (len) {
    case LEN_HH: *va_arg(*ap, unsigned char *) = (unsigned char)v; break;
    case LEN_H: *va_arg(*ap, unsigned short *) = (unsigned short)v; break;
    case LEN_L: *va_arg(*ap, unsigned long *) = (unsigned long)v; break;
    case LEN_LL: *va_arg(*ap, unsigned long long *) = v; break;
    case LEN_Z: *va_arg(*ap, size_t *) = (size_t)v; break;
    case LEN_T: *va_arg(*ap, size_t *) = (size_t)v; break; /* unsigned ptrdiff_t */
    case LEN_J: *va_arg(*ap, uintmax_t *) = (uintmax_t)v; break;
    default: *va_arg(*ap, unsigned int *) = (unsigned int)v; break;
    }
}

/* ---- the scanner ---------------------------------------------------- */

static int VscanCore(scan_t *st, const char *fmt, va_list *ap) {
    int assigned = 0;

    if (!fmt)
        return 0;

    while (*fmt) {
        if (ScanIsSpace((unsigned char)*fmt)) {
            while (ScanIsSpace((unsigned char)*fmt))
                fmt++;
            ScanSkipSpace(st); /* a directive matches any amount of it */
            continue;
        }
        if (*fmt != '%') {
            int c = ScanGet(st);
            if (c == EOF)
                break; /* input failure before the first conversion */
            if (c != (unsigned char)*fmt) {
                ScanUnget(st, c); /* matching failure at this byte */
                break;
            }
            fmt++;
            continue;
        }

        fmt++; /* past '%' */
        if (*fmt == '%') {
            int c = ScanGet(st);
            if (c == EOF)
                break;
            if (c != '%') {
                ScanUnget(st, c);
                break;
            }
            fmt++;
            continue;
        }

        int suppress = 0;
        if (*fmt == '*') {
            suppress = 1;
            fmt++;
        }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            if (width < 1000000)
                width = width * 10 + (*fmt - '0');
            fmt++;
        }
        scan_len_t len = LEN_NONE;
        if (*fmt == 'h') {
            len = LEN_H;
            fmt++;
            if (*fmt == 'h') {
                len = LEN_HH;
                fmt++;
            }
        } else if (*fmt == 'l') {
            len = LEN_L;
            fmt++;
            if (*fmt == 'l') {
                len = LEN_LL;
                fmt++;
            }
        } else if (*fmt == 'z') {
            len = LEN_Z;
            fmt++;
        } else if (*fmt == 't') {
            len = LEN_T;
            fmt++;
        } else if (*fmt == 'j') {
            len = LEN_J;
            fmt++;
        } else if (*fmt == 'L') {
            len = LEN_CAP_L;
            fmt++;
        }

        int conv = (unsigned char)*fmt;
        if (conv == '\0')
            break; /* trailing '%': nothing to convert */
        fmt++;

        /* Wide characters/strings (%lc, %ls) are not supported: stop
         * rather than write a wchar_t through a char* argument. */
        if ((conv == 'c' || conv == 's') && (len == LEN_L || len == LEN_CAP_L))
            goto done;

        if (conv != 'c' && conv != 'n')
            ScanSkipSpace(st);

        switch (conv) {
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'p': {
            int base = 10;
            if (conv == 'o')
                base = 8;
            else if (conv == 'i')
                base = 0; /* auto-detect: 0x hex, leading 0 octal */
            else if (conv == 'x' || conv == 'X' || conv == 'p')
                base = 16;

            int                neg = 0;
            unsigned long long v   = 0;
            if (!ScanInt(st, width, base, &neg, &v))
                goto done;
            if (!suppress) {
                if (conv == 'p') {
                    void **pp = va_arg(*ap, void **);
                    *pp       = (void *)(uintptr_t)(neg ? (0ULL - v) : v);
                } else if (conv == 'd' || conv == 'i') {
                    StoreSigned(ap, len, neg, v);
                } else {
                    StoreUnsigned(ap, len, neg, v);
                }
                assigned++;
            }
            break;
        }
        case 'a':
        case 'A':
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G': {
            char tok[SCAN_TOK];
            if (!ScanFloatToken(st, width, tok, sizeof(tok)))
                goto done;
            if (!suppress) {
                double v = strtod(tok, NULL);
                if (len == LEN_L)
                    *va_arg(*ap, double *) = v;
                else if (len == LEN_CAP_L)
                    *va_arg(*ap, long double *) = (long double)v;
                else
                    *va_arg(*ap, float *) = (float)v;
                assigned++;
            }
            break;
        }
        case 'c': {
            int   want = width ? width : 1;
            char *dst  = suppress ? NULL : va_arg(*ap, char *);
            int   got  = 0;
            while (got < want) {
                int c = ScanGet(st);
                if (c == EOF)
                    break;
                if (dst)
                    dst[got] = (char)c;
                got++;
            }
            if (got < want)
                goto done; /* incomplete item: no assignment is made */
            if (!suppress)
                assigned++;
            break;
        }
        case 's': {
            char *dst = suppress ? NULL : va_arg(*ap, char *);
            int   n   = 0;
            int   c   = EOF;
            for (;;) {
                if (width != 0 && n >= width)
                    break; /* width reached: nothing extra consumed */
                c = ScanGet(st);
                if (c == EOF || ScanIsSpace(c))
                    break;
                if (dst)
                    dst[n] = (char)c;
                n++;
            }
            ScanUnget(st, c);
            if (n == 0)
                goto done; /* matching failure */
            if (dst)
                dst[n] = '\0';
            if (!suppress)
                assigned++;
            break;
        }
        case 'n': {
            /* %n stores the bytes consumed so far and is not counted as
             * an assignment (C11 §7.21.6.2p12). */
            if (!suppress)
                StoreSigned(ap, len, 0, (unsigned long long)st->count);
            break;
        }
        default:
            /* Unknown or unsupported conversion (%[ and friends): stop
             * and report what has been assigned so far. */
            goto done;
        }
    }

done:
    /* EOF means "input failure before the first conversion"; a matching
     * failure on a non-empty input reports zero assignments instead. */
    return (assigned == 0 && st->eof) ? EOF : assigned;
}

/* ---- string source -------------------------------------------------- */

typedef struct {
    const char *s;
    size_t      pos;
} scan_str_ctx_t;

static int ScanStrGet(void *ctx) {
    scan_str_ctx_t *c  = (scan_str_ctx_t *)ctx;
    unsigned char   ch = (unsigned char)c->s[c->pos];
    if (ch == '\0')
        return EOF;
    c->pos++;
    return ch;
}

static void ScanStrPutback(void *ctx, int c) {
    scan_str_ctx_t *sc = (scan_str_ctx_t *)ctx;
    (void)c;
    if (sc->pos > 0)
        sc->pos--;
}

/* ---- file source ---------------------------------------------------- */

static int ScanFileGet(void *ctx) {
    return fgetc((FILE *)ctx);
}

static void ScanFilePutback(void *ctx, int c) {
    (void)ungetc(c, (FILE *)ctx);
}

int vsscanf(const char *s, const char *fmt, va_list ap) {
    scan_str_ctx_t ctx;
    ctx.s   = s ? s : "";
    ctx.pos = 0;

    scan_t st;
    st.src.get     = ScanStrGet;
    st.src.putback = ScanStrPutback;
    st.src.ctx     = &ctx;
    st.nstash      = 0;
    st.eof         = 0;
    st.count       = 0;

    /* Work on a private copy: va_list is an array type on x86_64, so a
     * pointer to the copy is what the engine needs. */
    va_list args;
    va_copy(args, ap);
    int r = VscanCore(&st, fmt, &args);
    va_end(args);
    ScanFlushStash(&st); /* rewinds the string cursor past the look-ahead */
    return r;
}

int sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

/* No vfscanf() in the frozen header (stdio.h), so the FILE variant gets
 * an internal entry point with the same engine. */
static int Vfscanf(FILE *f, const char *fmt, va_list ap) {
    scan_t st;
    st.src.get     = ScanFileGet;
    st.src.putback = ScanFilePutback;
    st.src.ctx     = f;
    st.nstash      = 0;
    st.eof         = 0;
    st.count       = 0;

    va_list args;
    va_copy(args, ap);
    int r = VscanCore(&st, fmt, &args);
    va_end(args);
    /* Whatever the scanner looked at but did not use goes back into the
     * stream (up to _F_PUSHMAX bytes; ungetc() refuses the rest). */
    ScanFlushStash(&st);
    return r;
}

int fscanf(FILE *f, const char *fmt, ...) {
    if (!f || !fmt)
        return EOF;
    va_list ap;
    va_start(ap, fmt);
    int r = Vfscanf(f, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- perror --------------------------------------------------------- */

void perror(const char *s) {
    /* errno must survive the output calls below. */
    int e = errno;
    if (s && *s) {
        fputs(s, stderr);
        fputs(": ", stderr);
    }
    fputs(strerror(e), stderr);
    fputc('\n', stderr);
    errno = e;
}

/* ---- filesystem helpers --------------------------------------------- */

int remove(const char *path) {
    if (!s_be || !s_be->unlink) {
        errno = ENOSYS;
        return -1;
    }
    if (!path || !path[0]) {
        errno = EINVAL;
        return -1;
    }
    int r = s_be->unlink(path);
    if (r < 0) {
        errno = ErrnoFromCode(r);
        return -1;
    }
    return 0;
}

int rename(const char *oldpath, const char *newpath) {
    if (!s_be || !s_be->rename) {
        errno = ENOSYS;
        return -1;
    }
    if (!oldpath || !oldpath[0] || !newpath || !newpath[0]) {
        errno = EINVAL;
        return -1;
    }
    int r = s_be->rename(oldpath, newpath);
    if (r < 0) {
        errno = ErrnoFromCode(r);
        return -1;
    }
    return 0;
}
