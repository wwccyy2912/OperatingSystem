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
 * stdalign.h - Alignment macros (C11)
 * Copyright (c) 2026 OpSys Project
 *
 * C11 <stdalign.h> provides the alignas and alignof convenience macros
 * that map to the language keywords _Alignas and _Alignof.
 * The __alignas_is_defined / __alignof_is_defined macros are set to 1
 * per ISO/IEC 9899:2011 §7.15.
 */

#ifndef LIBC_STDALIGN_H
#define LIBC_STDALIGN_H

#define alignas _Alignas
#define alignof _Alignof

#define __alignas_is_defined 1
#define __alignof_is_defined 1

#endif /* LIBC_STDALIGN_H */
