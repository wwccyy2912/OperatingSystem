/*
 * stdnoreturn.h - noreturn macro (C11)
 * Copyright (c) 2026 OpSys Project
 *
 * C11 <stdnoreturn.h> provides the noreturn convenience macro that maps
 * to the _Noreturn function specifier keyword.
 * Per ISO/IEC 9899:2011 §7.23, the macro is a function-specifier and
 * may be used like _Noreturn.
 */

#ifndef LIBC_STDNORETURN_H
#define LIBC_STDNORETURN_H

#define noreturn _Noreturn

#endif /* LIBC_STDNORETURN_H */
