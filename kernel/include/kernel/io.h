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
 * io.h - x86 Port I/O routines
 * Copyright (c) 2026 OpSys Project
 */

#ifndef KERNEL_IO_H
#define KERNEL_IO_H

#include <kernel/types.h>

/**
 * Read a byte from an I/O port.
 * @param port  The 16-bit port number.
 * @return The byte read.
 */
u8 IoInb(u16 port);

/**
 * Write a byte to an I/O port.
 * @param port  The 16-bit port number.
 * @param val   The byte to write.
 */
void IoOutb(u16 port, u8 val);

/**
 * Read a word (16-bit) from an I/O port.
 * @param port  The 16-bit port number.
 * @return The word read.
 */
u16 IoInw(u16 port);

/**
 * Write a word (16-bit) to an I/O port.
 * @param port  The 16-bit port number.
 * @param val   The word to write.
 */
void IoOutw(u16 port, u16 val);

/**
 * Read a dword (32-bit) from an I/O port.
 * @param port  The 16-bit port number.
 * @return The dword read.
 */
u32 IoInl(u16 port);

/**
 * Write a dword (32-bit) to an I/O port.
 * @param port  The 16-bit port number.
 * @param val   The dword to write.
 */
void IoOutl(u16 port, u32 val);

/**
 * Brief delay using a dummy I/O read.
 */
void IoDelay(void);

#endif /* KERNEL_IO_H */
