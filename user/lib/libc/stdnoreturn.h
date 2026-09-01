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
