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
 * ctype.h - Character classification and case conversion
 * Copyright (c) 2026 OpSys Project
 *
 * Standard <ctype.h> subset for 7-bit ASCII.
 * All functions take int (EOF-safe) and return int.
 */

#ifndef LIBC_CTYPE_H
#define LIBC_CTYPE_H

/* --- Character classification --- */
int isalpha(int c);
int isdigit(int c);
int isalnum(int c);
int isxdigit(int c);
int isspace(int c);
int Isblank(int c);
int isupper(int c);
int islower(int c);
int isprint(int c);
int isgraph(int c);
int ispunct(int c);
int iscntrl(int c);

/* --- Case conversion --- */
int toupper(int c);
int tolower(int c);

#endif /* LIBC_CTYPE_H */
