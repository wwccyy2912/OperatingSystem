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
 * io.c - x86 Port I/O routines
 * Copyright (c) 2026 OpSys Project *
 * ------------------------------------------------------------------
 * Structure (io):
 *   IoInb/IoOutb/IoInw/IoOutw/IoInl/IoOutl -> one __asm__ volatile
 *   per port access (inb/outb/inw/outw/inl/outl, "Nd" constraint).
 * How it works:
 *   Each access is a single privileged I/O instruction; the volatile
 *   asm forbids reordering across the access.
 * Purpose:
 *   Userspace drivers (serial/keyboard services) get I/O via the
 *   SYS_IO_* capability path; this kernel copy drives early console
 *   and hardware setup before services exist.
 * Caveats:
 *   Must be compiled with IF=0 sections kept short (port I/O is not
 *   interrupt-safe by itself).
 * ------------------------------------------------------------------
 */
#include <kernel/io.h>

u8 IoInb(u16 port) {
    u8 ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void IoOutb(u16 port, u8 val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

u16 IoInw(u16 port) {
    u16 ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void IoOutw(u16 port, u16 val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

u32 IoInl(u16 port) {
    u32 ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void IoOutl(u16 port, u32 val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

void IoDelay(void) {
    __asm__ volatile("outb %0, $0x80" : : "a"((u8)0));
}
