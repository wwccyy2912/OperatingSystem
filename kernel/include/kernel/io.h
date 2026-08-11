/*
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
u8 io_inb(u16 port);

/**
 * Write a byte to an I/O port.
 * @param port  The 16-bit port number.
 * @param val   The byte to write.
 */
void io_outb(u16 port, u8 val);

/**
 * Read a word (16-bit) from an I/O port.
 * @param port  The 16-bit port number.
 * @return The word read.
 */
u16 io_inw(u16 port);

/**
 * Write a word (16-bit) to an I/O port.
 * @param port  The 16-bit port number.
 * @param val   The word to write.
 */
void io_outw(u16 port, u16 val);

/**
 * Read a dword (32-bit) from an I/O port.
 * @param port  The 16-bit port number.
 * @return The dword read.
 */
u32 io_inl(u16 port);

/**
 * Write a dword (32-bit) to an I/O port.
 * @param port  The 16-bit port number.
 * @param val   The dword to write.
 */
void io_outl(u16 port, u32 val);

/**
 * Brief delay using a dummy I/O read.
 */
void io_delay(void);

#endif /* KERNEL_IO_H */
