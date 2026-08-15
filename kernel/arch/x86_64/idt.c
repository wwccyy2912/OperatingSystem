/*
 * idt.c - Interrupt Descriptor Table for x86_64
 * Copyright (c) 2026 OpSys Project
 *
 * 256 IDT entries with ISR stubs for all vectors.
 * PIC remapping: IRQ 0-15 → vectors 32-47.
 * Individual ISR stubs defined as naked functions with inline asm.
 */

#include <kernel/idt.h>
#include <kernel/io.h>
#include <kernel/irq.h>
#include <kernel/sched.h>
#include <kernel/serial.h>
#include <kernel/string.h>
#include <kernel/signal.h>
#include <kernel/thread.h>
#include <kernel/process.h>
#include <kernel/panic.h>

/* ------------------------------------------------------------------ */
/* IDT gate descriptor (16 bytes per entry)                           */
/* ------------------------------------------------------------------ */

typedef struct {
    u16 offset_low;  /* Offset bits 0-15 */
    u16 selector;    /* Code segment selector */
    u8  ist;         /* IST index (0 = none) */
    u8  type_attr;   /* Gate type + DPL + present */
    u16 offset_mid;  /* Offset bits 16-31 */
    u32 offset_high; /* Offset bits 32-63 */
    u32 reserved;    /* Must be zero */
} __attribute__((packed)) idt_entry_t;

typedef struct {
    u16 limit;
    u64 base;
} __attribute__((packed)) idt_ptr_t;

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

#define IDT_ENTRIES 256
#define KERNEL_CS   0x08
#define IST_STACK_1 1 /* IST for double-fault */
#define IST_STACK_2 2 /* IST for NMI */

/* Gate type_attr:
 *   Bit 7:   Present (P)
 *   Bits 6-5: DPL
 *   Bit 4:   0 (storage segment, must be 0 for gates)
 *   Bits 3-0: gate type */
#define GATE_INTERRUPT_KERNEL 0x8E /* P=1, DPL=0, type=0xE */
#define GATE_TRAP_KERNEL      0x8F /* P=1, DPL=0, type=0xF */
#define GATE_INTERRUPT_USER   0xEE /* P=1, DPL=3, type=0xE */

/* PIC ports */
#define PIC_MASTER_CMD  0x20
#define PIC_MASTER_DATA 0x21
#define PIC_SLAVE_CMD   0xA0
#define PIC_SLAVE_DATA  0xA1
#define PIC_EOI         0x20

/* IRQ-to-vector offset */
#define IRQ_VECTOR_BASE 32

/* ------------------------------------------------------------------ */
/* Handler table                                                      */
/* ------------------------------------------------------------------ */

/* Syscall entry point (defined in syscall_entry.S) */
extern void syscall_entry_stub(void);

static void (*isr_handlers[IDT_ENTRIES])(void);

/* Static storage */
static idt_entry_t idt[IDT_ENTRIES];
static idt_ptr_t   idt_ptr;

/* ------------------------------------------------------------------ */
/* IDT entry setup                                                    */
/* ------------------------------------------------------------------ */

static void idt_set_entry(idt_entry_t *entry, u64 handler, u8 type_attr, u8 ist) {
    entry->offset_low  = handler & 0xFFFF;
    entry->offset_mid  = (handler >> 16) & 0xFFFF;
    entry->offset_high = (handler >> 32) & 0xFFFFFFFF;
    entry->selector    = KERNEL_CS;
    entry->ist         = ist & 0x07;
    entry->type_attr   = type_attr;
    entry->reserved    = 0;
}

/* ------------------------------------------------------------------ */
/* ISR stubs: naked functions with inline asm                         */
/*                                                                      */
/* Each stub either pushes a dummy error code (0) then the vector      */
/* number, or (for CPU exceptions with error codes) just pushes the    */
/* vector number. Then jumps to isr_common_stub.                       */
/* ------------------------------------------------------------------ */

/*
 * Common ISR stub: saves all GPRs, calls C handler, restores, IRETQ.
 *
 * After the 15 pushes (120 bytes), rsp points to the start of
 * interrupt_frame_t. We pass rsp as the sole argument (RDI) so the
 * C handler can inspect the full register context.
 *
 * Stack layout on entry (after individual stub pushed vector + err):
 *   [rsp+0]   = r15  (last GPR pushed)   ─┐
 *   ...                                     │ interrupt_frame_t
 *   [rsp+112] = rax                       ─┘
 *   [rsp+120] = vector number
 *   [rsp+128] = error code
 *   [rsp+136] = RIP  (pushed by CPU)
 *   [rsp+144] = CS
 *   [rsp+152] = RFLAGS
 *   [rsp+160] = RSP  (if privilege change)
 *   [rsp+168] = SS   (if privilege change)
 */
__attribute__((naked)) void isr_common_stub(void) {
    __asm__ volatile(
        /* Save all general-purpose registers */
        "pushq %%rax\n"
        "pushq %%rbx\n"
        "pushq %%rcx\n"
        "pushq %%rdx\n"
        "pushq %%rsi\n"
        "pushq %%rdi\n"
        "pushq %%rbp\n"
        "pushq %%r8\n"
        "pushq %%r9\n"
        "pushq %%r10\n"
        "pushq %%r11\n"
        "pushq %%r12\n"
        "pushq %%r13\n"
        "pushq %%r14\n"
        "pushq %%r15\n"

        /* rsp now points to interrupt_frame_t (r15 at offset 0) */
        "movq %%rsp, %%rdi\n" /* arg1 = pointer to frame */
        "call isr_handler\n"

        /* Restore all general-purpose registers */
        "popq %%r15\n"
        "popq %%r14\n"
        "popq %%r13\n"
        "popq %%r12\n"
        "popq %%r11\n"
        "popq %%r10\n"
        "popq %%r9\n"
        "popq %%r8\n"
        "popq %%rbp\n"
        "popq %%rdi\n"
        "popq %%rsi\n"
        "popq %%rdx\n"
        "popq %%rcx\n"
        "popq %%rbx\n"
        "popq %%rax\n"

        /* Skip vector number and error code */
        "addq $16, %%rsp\n"

        "iretq\n" ::
            : "memory");
}

/* Macros for stub generation: exceptions WITHOUT error codes */
#define ISR_STUB_NOERR(n)                                         \
    __attribute__((naked)) void isr_stub_##n(void) {              \
        __asm__ volatile("pushq $0\n"      /* dummy error code */ \
                         "pushq $" #n "\n" /* vector number */    \
                         "jmp isr_common_stub\n" ::               \
                             : "memory");                         \
    }

/* Macros for stub generation: exceptions WITH error codes (pushed by CPU) */
#define ISR_STUB_ERR(n)                                        \
    __attribute__((naked)) void isr_stub_##n(void) {           \
        __asm__ volatile("pushq $" #n "\n" /* vector number */ \
                         "jmp isr_common_stub\n" ::            \
                             : "memory");                      \
    }

/* ------------------------------------------------------------------ */
/* Exception stubs (vectors 0-31)                                     */
/* ------------------------------------------------------------------ */

/* Exceptions WITHOUT error codes */
// clang-format off
ISR_STUB_NOERR(0)
ISR_STUB_NOERR(1)
ISR_STUB_NOERR(2)
ISR_STUB_NOERR(3)
ISR_STUB_NOERR(4)
ISR_STUB_NOERR(5)
ISR_STUB_NOERR(6)
ISR_STUB_NOERR(7)
ISR_STUB_NOERR(9)
ISR_STUB_NOERR(15)
ISR_STUB_NOERR(16)
ISR_STUB_NOERR(18)
ISR_STUB_NOERR(19)
ISR_STUB_NOERR(20)
ISR_STUB_NOERR(22)
ISR_STUB_NOERR(23)
ISR_STUB_NOERR(24)
ISR_STUB_NOERR(25)
ISR_STUB_NOERR(26)
ISR_STUB_NOERR(27)
ISR_STUB_NOERR(28)
ISR_STUB_NOERR(31)

/* Exceptions WITH error codes (CPU pushes error code onto stack) */
ISR_STUB_ERR(8)
ISR_STUB_ERR(10)
ISR_STUB_ERR(11)
ISR_STUB_ERR(12)
ISR_STUB_ERR(13)
ISR_STUB_ERR(14)
ISR_STUB_ERR(17)
ISR_STUB_ERR(21)
ISR_STUB_ERR(29)
ISR_STUB_ERR(30)

/* ------------------------------------------------------------------ */
/* Hardware IRQ and software interrupt stubs (vectors 32-255)        */
/* ------------------------------------------------------------------ */

ISR_STUB_NOERR(32) ISR_STUB_NOERR(33) ISR_STUB_NOERR(34) ISR_STUB_NOERR(35)
ISR_STUB_NOERR(36) ISR_STUB_NOERR(37) ISR_STUB_NOERR(38) ISR_STUB_NOERR(39)
ISR_STUB_NOERR(40) ISR_STUB_NOERR(41) ISR_STUB_NOERR(42) ISR_STUB_NOERR(43)
ISR_STUB_NOERR(44) ISR_STUB_NOERR(45) ISR_STUB_NOERR(46) ISR_STUB_NOERR(47)
ISR_STUB_NOERR(48) ISR_STUB_NOERR(49) ISR_STUB_NOERR(50) ISR_STUB_NOERR(51)
ISR_STUB_NOERR(52) ISR_STUB_NOERR(53) ISR_STUB_NOERR(54) ISR_STUB_NOERR(55)
ISR_STUB_NOERR(56) ISR_STUB_NOERR(57) ISR_STUB_NOERR(58) ISR_STUB_NOERR(59)
ISR_STUB_NOERR(60) ISR_STUB_NOERR(61) ISR_STUB_NOERR(62) ISR_STUB_NOERR(63)
ISR_STUB_NOERR(64) ISR_STUB_NOERR(65) ISR_STUB_NOERR(66) ISR_STUB_NOERR(67)
ISR_STUB_NOERR(68) ISR_STUB_NOERR(69) ISR_STUB_NOERR(70) ISR_STUB_NOERR(71)
ISR_STUB_NOERR(72) ISR_STUB_NOERR(73) ISR_STUB_NOERR(74) ISR_STUB_NOERR(75)
ISR_STUB_NOERR(76) ISR_STUB_NOERR(77) ISR_STUB_NOERR(78) ISR_STUB_NOERR(79)
ISR_STUB_NOERR(80) ISR_STUB_NOERR(81) ISR_STUB_NOERR(82) ISR_STUB_NOERR(83)
ISR_STUB_NOERR(84) ISR_STUB_NOERR(85) ISR_STUB_NOERR(86) ISR_STUB_NOERR(87)
ISR_STUB_NOERR(88) ISR_STUB_NOERR(89) ISR_STUB_NOERR(90) ISR_STUB_NOERR(91)
ISR_STUB_NOERR(92) ISR_STUB_NOERR(93) ISR_STUB_NOERR(94) ISR_STUB_NOERR(95)
ISR_STUB_NOERR(96) ISR_STUB_NOERR(97) ISR_STUB_NOERR(98) ISR_STUB_NOERR(99)
ISR_STUB_NOERR(100) ISR_STUB_NOERR(101) ISR_STUB_NOERR(102) ISR_STUB_NOERR(103)
ISR_STUB_NOERR(104) ISR_STUB_NOERR(105) ISR_STUB_NOERR(106) ISR_STUB_NOERR(107)
ISR_STUB_NOERR(108) ISR_STUB_NOERR(109) ISR_STUB_NOERR(110) ISR_STUB_NOERR(111)
ISR_STUB_NOERR(112) ISR_STUB_NOERR(113) ISR_STUB_NOERR(114) ISR_STUB_NOERR(115)
ISR_STUB_NOERR(116) ISR_STUB_NOERR(117) ISR_STUB_NOERR(118) ISR_STUB_NOERR(119)
ISR_STUB_NOERR(120) ISR_STUB_NOERR(121) ISR_STUB_NOERR(122) ISR_STUB_NOERR(123)
ISR_STUB_NOERR(124) ISR_STUB_NOERR(125) ISR_STUB_NOERR(126) ISR_STUB_NOERR(127)
ISR_STUB_NOERR(128) ISR_STUB_NOERR(129) ISR_STUB_NOERR(130) ISR_STUB_NOERR(131)
ISR_STUB_NOERR(132) ISR_STUB_NOERR(133) ISR_STUB_NOERR(134) ISR_STUB_NOERR(135)
ISR_STUB_NOERR(136) ISR_STUB_NOERR(137) ISR_STUB_NOERR(138) ISR_STUB_NOERR(139)
ISR_STUB_NOERR(140) ISR_STUB_NOERR(141) ISR_STUB_NOERR(142) ISR_STUB_NOERR(143)
ISR_STUB_NOERR(144) ISR_STUB_NOERR(145) ISR_STUB_NOERR(146) ISR_STUB_NOERR(147)
ISR_STUB_NOERR(148) ISR_STUB_NOERR(149) ISR_STUB_NOERR(150) ISR_STUB_NOERR(151)
ISR_STUB_NOERR(152) ISR_STUB_NOERR(153) ISR_STUB_NOERR(154) ISR_STUB_NOERR(155)
ISR_STUB_NOERR(156) ISR_STUB_NOERR(157) ISR_STUB_NOERR(158) ISR_STUB_NOERR(159)
ISR_STUB_NOERR(160) ISR_STUB_NOERR(161) ISR_STUB_NOERR(162) ISR_STUB_NOERR(163)
ISR_STUB_NOERR(164) ISR_STUB_NOERR(165) ISR_STUB_NOERR(166) ISR_STUB_NOERR(167)
ISR_STUB_NOERR(168) ISR_STUB_NOERR(169) ISR_STUB_NOERR(170) ISR_STUB_NOERR(171)
ISR_STUB_NOERR(172) ISR_STUB_NOERR(173) ISR_STUB_NOERR(174) ISR_STUB_NOERR(175)
ISR_STUB_NOERR(176) ISR_STUB_NOERR(177) ISR_STUB_NOERR(178) ISR_STUB_NOERR(179)
ISR_STUB_NOERR(180) ISR_STUB_NOERR(181) ISR_STUB_NOERR(182) ISR_STUB_NOERR(183)
ISR_STUB_NOERR(184) ISR_STUB_NOERR(185) ISR_STUB_NOERR(186) ISR_STUB_NOERR(187)
ISR_STUB_NOERR(188) ISR_STUB_NOERR(189) ISR_STUB_NOERR(190) ISR_STUB_NOERR(191)
ISR_STUB_NOERR(192) ISR_STUB_NOERR(193) ISR_STUB_NOERR(194) ISR_STUB_NOERR(195)
ISR_STUB_NOERR(196) ISR_STUB_NOERR(197) ISR_STUB_NOERR(198) ISR_STUB_NOERR(199)
ISR_STUB_NOERR(200) ISR_STUB_NOERR(201) ISR_STUB_NOERR(202) ISR_STUB_NOERR(203)
ISR_STUB_NOERR(204) ISR_STUB_NOERR(205) ISR_STUB_NOERR(206) ISR_STUB_NOERR(207)
ISR_STUB_NOERR(208) ISR_STUB_NOERR(209) ISR_STUB_NOERR(210) ISR_STUB_NOERR(211)
ISR_STUB_NOERR(212) ISR_STUB_NOERR(213) ISR_STUB_NOERR(214) ISR_STUB_NOERR(215)
ISR_STUB_NOERR(216) ISR_STUB_NOERR(217) ISR_STUB_NOERR(218) ISR_STUB_NOERR(219)
ISR_STUB_NOERR(220) ISR_STUB_NOERR(221) ISR_STUB_NOERR(222) ISR_STUB_NOERR(223)
ISR_STUB_NOERR(224) ISR_STUB_NOERR(225) ISR_STUB_NOERR(226) ISR_STUB_NOERR(227)
ISR_STUB_NOERR(228) ISR_STUB_NOERR(229) ISR_STUB_NOERR(230) ISR_STUB_NOERR(231)
ISR_STUB_NOERR(232) ISR_STUB_NOERR(233) ISR_STUB_NOERR(234) ISR_STUB_NOERR(235)
ISR_STUB_NOERR(236) ISR_STUB_NOERR(237) ISR_STUB_NOERR(238) ISR_STUB_NOERR(239)
ISR_STUB_NOERR(240) ISR_STUB_NOERR(241) ISR_STUB_NOERR(242) ISR_STUB_NOERR(243)
ISR_STUB_NOERR(244) ISR_STUB_NOERR(245) ISR_STUB_NOERR(246) ISR_STUB_NOERR(247)
ISR_STUB_NOERR(248) ISR_STUB_NOERR(249) ISR_STUB_NOERR(250) ISR_STUB_NOERR(251)
ISR_STUB_NOERR(252) ISR_STUB_NOERR(253) ISR_STUB_NOERR(254) ISR_STUB_NOERR(255)
// clang-format on

/* ------------------------------------------------------------------ */
/* ISR stub function table (indexed by vector number)                  */
/* ------------------------------------------------------------------ */

// clang-format off
#define DECLARE_STUB_REF(n) [n] = isr_stub_##n,

typedef void (*isr_stub_fn)(void);

static const isr_stub_fn isr_stub_table[IDT_ENTRIES] = {
    DECLARE_STUB_REF(0) DECLARE_STUB_REF(1) DECLARE_STUB_REF(2) DECLARE_STUB_REF(3)
    DECLARE_STUB_REF(4) DECLARE_STUB_REF(5) DECLARE_STUB_REF(6) DECLARE_STUB_REF(7)
    DECLARE_STUB_REF(8) DECLARE_STUB_REF(9) DECLARE_STUB_REF(10) DECLARE_STUB_REF(11)
    DECLARE_STUB_REF(12) DECLARE_STUB_REF(13) DECLARE_STUB_REF(14) DECLARE_STUB_REF(15)
    DECLARE_STUB_REF(16) DECLARE_STUB_REF(17) DECLARE_STUB_REF(18) DECLARE_STUB_REF(19)
    DECLARE_STUB_REF(20) DECLARE_STUB_REF(21) DECLARE_STUB_REF(22) DECLARE_STUB_REF(23)
    DECLARE_STUB_REF(24) DECLARE_STUB_REF(25) DECLARE_STUB_REF(26) DECLARE_STUB_REF(27)
    DECLARE_STUB_REF(28) DECLARE_STUB_REF(29) DECLARE_STUB_REF(30) DECLARE_STUB_REF(31)
    DECLARE_STUB_REF(32) DECLARE_STUB_REF(33) DECLARE_STUB_REF(34) DECLARE_STUB_REF(35)
    DECLARE_STUB_REF(36) DECLARE_STUB_REF(37) DECLARE_STUB_REF(38) DECLARE_STUB_REF(39)
    DECLARE_STUB_REF(40) DECLARE_STUB_REF(41) DECLARE_STUB_REF(42) DECLARE_STUB_REF(43)
    DECLARE_STUB_REF(44) DECLARE_STUB_REF(45) DECLARE_STUB_REF(46) DECLARE_STUB_REF(47)
    DECLARE_STUB_REF(48) DECLARE_STUB_REF(49) DECLARE_STUB_REF(50) DECLARE_STUB_REF(51)
    DECLARE_STUB_REF(52) DECLARE_STUB_REF(53) DECLARE_STUB_REF(54) DECLARE_STUB_REF(55)
    DECLARE_STUB_REF(56) DECLARE_STUB_REF(57) DECLARE_STUB_REF(58) DECLARE_STUB_REF(59)
    DECLARE_STUB_REF(60) DECLARE_STUB_REF(61) DECLARE_STUB_REF(62) DECLARE_STUB_REF(63)
    DECLARE_STUB_REF(64) DECLARE_STUB_REF(65) DECLARE_STUB_REF(66) DECLARE_STUB_REF(67)
    DECLARE_STUB_REF(68) DECLARE_STUB_REF(69) DECLARE_STUB_REF(70) DECLARE_STUB_REF(71)
    DECLARE_STUB_REF(72) DECLARE_STUB_REF(73) DECLARE_STUB_REF(74) DECLARE_STUB_REF(75)
    DECLARE_STUB_REF(76) DECLARE_STUB_REF(77) DECLARE_STUB_REF(78) DECLARE_STUB_REF(79)
    DECLARE_STUB_REF(80) DECLARE_STUB_REF(81) DECLARE_STUB_REF(82) DECLARE_STUB_REF(83)
    DECLARE_STUB_REF(84) DECLARE_STUB_REF(85) DECLARE_STUB_REF(86) DECLARE_STUB_REF(87)
    DECLARE_STUB_REF(88) DECLARE_STUB_REF(89) DECLARE_STUB_REF(90) DECLARE_STUB_REF(91)
    DECLARE_STUB_REF(92) DECLARE_STUB_REF(93) DECLARE_STUB_REF(94) DECLARE_STUB_REF(95)
    DECLARE_STUB_REF(96) DECLARE_STUB_REF(97) DECLARE_STUB_REF(98) DECLARE_STUB_REF(99)
    DECLARE_STUB_REF(100) DECLARE_STUB_REF(101) DECLARE_STUB_REF(102) DECLARE_STUB_REF(103)
    DECLARE_STUB_REF(104) DECLARE_STUB_REF(105) DECLARE_STUB_REF(106) DECLARE_STUB_REF(107)
    DECLARE_STUB_REF(108) DECLARE_STUB_REF(109) DECLARE_STUB_REF(110) DECLARE_STUB_REF(111)
    DECLARE_STUB_REF(112) DECLARE_STUB_REF(113) DECLARE_STUB_REF(114) DECLARE_STUB_REF(115)
    DECLARE_STUB_REF(116) DECLARE_STUB_REF(117) DECLARE_STUB_REF(118) DECLARE_STUB_REF(119)
    DECLARE_STUB_REF(120) DECLARE_STUB_REF(121) DECLARE_STUB_REF(122) DECLARE_STUB_REF(123)
    DECLARE_STUB_REF(124) DECLARE_STUB_REF(125) DECLARE_STUB_REF(126) DECLARE_STUB_REF(127)
    DECLARE_STUB_REF(128) DECLARE_STUB_REF(129) DECLARE_STUB_REF(130) DECLARE_STUB_REF(131)
    DECLARE_STUB_REF(132) DECLARE_STUB_REF(133) DECLARE_STUB_REF(134) DECLARE_STUB_REF(135)
    DECLARE_STUB_REF(136) DECLARE_STUB_REF(137) DECLARE_STUB_REF(138) DECLARE_STUB_REF(139)
    DECLARE_STUB_REF(140) DECLARE_STUB_REF(141) DECLARE_STUB_REF(142) DECLARE_STUB_REF(143)
    DECLARE_STUB_REF(144) DECLARE_STUB_REF(145) DECLARE_STUB_REF(146) DECLARE_STUB_REF(147)
    DECLARE_STUB_REF(148) DECLARE_STUB_REF(149) DECLARE_STUB_REF(150) DECLARE_STUB_REF(151)
    DECLARE_STUB_REF(152) DECLARE_STUB_REF(153) DECLARE_STUB_REF(154) DECLARE_STUB_REF(155)
    DECLARE_STUB_REF(156) DECLARE_STUB_REF(157) DECLARE_STUB_REF(158) DECLARE_STUB_REF(159)
    DECLARE_STUB_REF(160) DECLARE_STUB_REF(161) DECLARE_STUB_REF(162) DECLARE_STUB_REF(163)
    DECLARE_STUB_REF(164) DECLARE_STUB_REF(165) DECLARE_STUB_REF(166) DECLARE_STUB_REF(167)
    DECLARE_STUB_REF(168) DECLARE_STUB_REF(169) DECLARE_STUB_REF(170) DECLARE_STUB_REF(171)
    DECLARE_STUB_REF(172) DECLARE_STUB_REF(173) DECLARE_STUB_REF(174) DECLARE_STUB_REF(175)
    DECLARE_STUB_REF(176) DECLARE_STUB_REF(177) DECLARE_STUB_REF(178) DECLARE_STUB_REF(179)
    DECLARE_STUB_REF(180) DECLARE_STUB_REF(181) DECLARE_STUB_REF(182) DECLARE_STUB_REF(183)
    DECLARE_STUB_REF(184) DECLARE_STUB_REF(185) DECLARE_STUB_REF(186) DECLARE_STUB_REF(187)
    DECLARE_STUB_REF(188) DECLARE_STUB_REF(189) DECLARE_STUB_REF(190) DECLARE_STUB_REF(191)
    DECLARE_STUB_REF(192) DECLARE_STUB_REF(193) DECLARE_STUB_REF(194) DECLARE_STUB_REF(195)
    DECLARE_STUB_REF(196) DECLARE_STUB_REF(197) DECLARE_STUB_REF(198) DECLARE_STUB_REF(199)
    DECLARE_STUB_REF(200) DECLARE_STUB_REF(201) DECLARE_STUB_REF(202) DECLARE_STUB_REF(203)
    DECLARE_STUB_REF(204) DECLARE_STUB_REF(205) DECLARE_STUB_REF(206) DECLARE_STUB_REF(207)
    DECLARE_STUB_REF(208) DECLARE_STUB_REF(209) DECLARE_STUB_REF(210) DECLARE_STUB_REF(211)
    DECLARE_STUB_REF(212) DECLARE_STUB_REF(213) DECLARE_STUB_REF(214) DECLARE_STUB_REF(215)
    DECLARE_STUB_REF(216) DECLARE_STUB_REF(217) DECLARE_STUB_REF(218) DECLARE_STUB_REF(219)
    DECLARE_STUB_REF(220) DECLARE_STUB_REF(221) DECLARE_STUB_REF(222) DECLARE_STUB_REF(223)
    DECLARE_STUB_REF(224) DECLARE_STUB_REF(225) DECLARE_STUB_REF(226) DECLARE_STUB_REF(227)
    DECLARE_STUB_REF(228) DECLARE_STUB_REF(229) DECLARE_STUB_REF(230) DECLARE_STUB_REF(231)
    DECLARE_STUB_REF(232) DECLARE_STUB_REF(233) DECLARE_STUB_REF(234) DECLARE_STUB_REF(235)
    DECLARE_STUB_REF(236) DECLARE_STUB_REF(237) DECLARE_STUB_REF(238) DECLARE_STUB_REF(239)
    DECLARE_STUB_REF(240) DECLARE_STUB_REF(241) DECLARE_STUB_REF(242) DECLARE_STUB_REF(243)
    DECLARE_STUB_REF(244) DECLARE_STUB_REF(245) DECLARE_STUB_REF(246) DECLARE_STUB_REF(247)
    DECLARE_STUB_REF(248) DECLARE_STUB_REF(249) DECLARE_STUB_REF(250) DECLARE_STUB_REF(251)
    DECLARE_STUB_REF(252) DECLARE_STUB_REF(253) DECLARE_STUB_REF(254) DECLARE_STUB_REF(255)
};
// clang-format on

/* ------------------------------------------------------------------ */
/* PIC initialization (8259A)                                         */
/* ------------------------------------------------------------------ */

static void pic_remap(void) {
    /* Save current masks */
    u8 master_mask = io_inb(PIC_MASTER_DATA);
    u8 slave_mask  = io_inb(PIC_SLAVE_DATA);

    /* ICW1: begin initialization, ICW4 needed */
    io_outb(PIC_MASTER_CMD, 0x11);
    io_outb(PIC_SLAVE_CMD, 0x11);

    /* ICW2: vector offset */
    io_outb(PIC_MASTER_DATA, IRQ_VECTOR_BASE);    /* IRQ 0-7 → 32-39 */
    io_outb(PIC_SLAVE_DATA, IRQ_VECTOR_BASE + 8); /* IRQ 8-15 → 40-47 */

    /* ICW3: cascade */
    io_outb(PIC_MASTER_DATA, 0x04); /* Slave on IRQ2 */
    io_outb(PIC_SLAVE_DATA, 0x02);  /* Cascade identity */

    /* ICW4: 8086 mode */
    io_outb(PIC_MASTER_DATA, 0x01);
    io_outb(PIC_SLAVE_DATA, 0x01);

    /* Restore masks */
    io_outb(PIC_MASTER_DATA, master_mask);
    io_outb(PIC_SLAVE_DATA, slave_mask);
}

static void pic_mask_all(void) {
    io_outb(PIC_MASTER_DATA, 0xFF);
    io_outb(PIC_SLAVE_DATA, 0xFF);
}

static void pic_unmask_irq(u8 irq) {
    u16 port  = (irq < 8) ? PIC_MASTER_DATA : PIC_SLAVE_DATA;
    u8  shift = irq % 8;
    u8  mask  = io_inb(port);
    mask &= ~(1 << shift);
    io_outb(port, mask);
}

static void pic_mask_irq(u8 irq) {
    u16 port  = (irq < 8) ? PIC_MASTER_DATA : PIC_SLAVE_DATA;
    u8  shift = irq % 8;
    u8  mask  = io_inb(port);
    mask |= (1 << shift);
    io_outb(port, mask);
}

static void pic_send_eoi(u8 irq) {
    if (irq >= 8)
        io_outb(PIC_SLAVE_CMD, PIC_EOI);
    io_outb(PIC_MASTER_CMD, PIC_EOI);
}

/* ------------------------------------------------------------------ */
/* C-level ISR dispatch                                               */
/* ------------------------------------------------------------------ */

/* Exception names for pretty-printing */
static const char *const s_exception_names[32] = {
    [0]  = "#DE Divide Error",
    [1]  = "#DB Debug",
    [2]  = "NMI",
    [3]  = "#BP Breakpoint",
    [4]  = "#OF Overflow",
    [5]  = "#BR Bound Range",
    [6]  = "#UD Invalid Opcode",
    [7]  = "#NM Device Not Available",
    [8]  = "#DF Double Fault",
    [9]  = "Coprocessor Segment Overrun",
    [10] = "#TS Invalid TSS",
    [11] = "#NP Segment Not Present",
    [12] = "#SS Stack-Segment Fault",
    [13] = "#GP General Protection",
    [14] = "#PF Page Fault",
    [15] = "Reserved",
    [16] = "#MF x87 FP Exception",
    [17] = "#AC Alignment Check",
    [18] = "#MC Machine Check",
    [19] = "#XM SIMD Exception",
    [20] = "#VE Virtualization",
    [21] = "#CP Control Protection",
};

/* Read CR2 (faulting address for #PF) */
static inline u64 read_cr2(void) {
    u64 val;
    __asm__ volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}

/* Read CR3 (page table base) */
static inline u64 read_cr3(void) {
    u64 val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

/* Read CR0 */
static inline u64 read_cr0(void) {
    u64 val;
    __asm__ volatile("mov %%cr0, %0" : "=r"(val));
    return val;
}

void isr_handler(interrupt_frame_t *frame) {
    u64 vector   = frame->vector;
    u64 err_code = frame->error_code;

    /* ---- Hardware IRQ handling (vectors 32-47) ---- */
    if (vector >= IRQ_VECTOR_BASE && vector < IRQ_VECTOR_BASE + 16) {
        u8 irq = (u8)(vector - IRQ_VECTOR_BASE);

        /* Always send EOI to PIC for hardware IRQs.  Sent FIRST: a
         * signal checkpoint below may terminate the thread (thread_exit
         * never returns) and sched_tick may switch away, so the PIC
         * must not be left holding this IRQ in-service. */
        pic_send_eoi(irq);

        /* Signal delivery checkpoint: pending signals are delivered on
         * user-mode IRQ returns (no-op for kernel frames).  Runs before
         * sched_tick() so delivery is not deferred across a context
         * switch. */
        signal_check_interrupt(frame);

        /* Timer (IRQ0): kernel-owned, always ticks the scheduler */
        if (irq == 0)
            sched_tick();

        /* Forward bound device IRQs to the userspace owner */
        bool forwarded = irq_handle(irq);

        /* Legacy kernel fallback for unbound IRQ4: drain serial RX FIFO */
        if (irq == 4 && !forwarded) {
            while (io_inb(SERIAL_COM1_BASE + 5) & 0x01)
                (void)io_inb(SERIAL_COM1_BASE);
        }

        return;
    }

    /* ---- Software interrupts: call registered handler if present ---- */
    if (vector >= 32 && isr_handlers[vector]) {
        isr_handlers[vector]();
        return;
    }

    /* ---- CPU exceptions (vectors 0-31): detailed crash dump ---- */
    if (vector < 32) {
        const char *name = (vector < 32 && s_exception_names[vector]) ? s_exception_names[vector]
                                                                      : "Unknown";

        serial_printf("\n====== CPU EXCEPTION ======\n");
        serial_printf("  %s (vector=%u, error=0x%x)\n", name, (u32)vector, (u32)err_code);
        serial_printf("  RIP  = %p\n", frame->rip);
        serial_printf("  CS   = 0x%x\n", frame->cs);
        serial_printf("  RFLAGS = 0x%x\n", frame->rflags);
        serial_printf("  RSP  = %p\n", frame->rsp);
        serial_printf("  SS   = 0x%x\n", frame->ss);
        serial_printf("  RAX  = %p\n", frame->rax);
        serial_printf("  RBX  = %p\n", frame->rbx);
        serial_printf("  RCX  = %p\n", frame->rcx);
        serial_printf("  RDX  = %p\n", frame->rdx);
        serial_printf("  RSI  = %p\n", frame->rsi);
        serial_printf("  RDI  = %p\n", frame->rdi);
        serial_printf("  RBP  = %p\n", frame->rbp);

        if (vector == 14) {
            /*
             * User-mode page fault: deliver SIGSEGV (default action =
             * terminate the process) instead of halting the whole
             * system.  The faulting instruction cannot be resumed, so
             * the process must die: signal_kill_process() marks every
             * thread force_exit (waking blocked ones) and
             * thread_exit() finishes this one.  Never returns.
             */
            if ((frame->cs & 3) == 3) {
                serial_printf("\nUser-mode #PF at %p -> SIGSEGV\n", read_cr2());
                process_t *proc = process_current();
                if (proc)
                    signal_kill_process(proc, 128 + SIGSEGV);
                thread_exit(128 + SIGSEGV);
                /* unreachable */
            }

            /* Kernel-mode fault: existing crash dump below */
            serial_printf("  CR2  = %p (faulting address)\n", read_cr2());
            serial_printf("  CR3  = %p (page table base)\n", read_cr3());
            serial_printf("  Error: %s %s %s\n",
                          (err_code & 1) ? "PRESENT" : "NOT-PRESENT",
                          (err_code & 2) ? "WRITE" : "READ",
                          (err_code & 4) ? "USER" : "SUPERVISOR");

            /* Correlate to current thread for easier debugging */
            thread_t *th = sched_get_current();
            if (th) {
                u64 ksp;
                __asm__ volatile("mov %%rsp, %0" : "=r"(ksp));
                const char *state_names[] = {"READY", "RUNNING", "BLOCKED", "ZOMBIE", "FINISHED"};
                int         sidx          = (th->state >= 0 && th->state <= 4) ? th->state : 0;
                serial_printf(
                    "  Thread: TID=%d PID=%d state=%s\n", th->tid, th->pid, state_names[sidx]);
                serial_printf(
                    "  Kernel RSP=%p kstack=[%p-%p]\n", ksp, th->kstack_base, th->kstack_top);
                serial_printf("  Saved RIP=%p RSP=%p\n", th->rip, th->rsp);
            }
        }

        serial_printf("===========================\n");
        panic("Unhandled CPU exception %s (vector=%u, error=0x%x) in kernel mode",
              name,
              (u32)vector,
              (u32)err_code);
    }

    /* Signal delivery checkpoint for any other return-to-user path
     * (e.g. a software interrupt with a registered handler).  No-op
     * for kernel frames and when no signal is pending. */
    signal_check_interrupt(frame);

    /* Unknown vector with registered handler */
    if (isr_handlers[vector])
        isr_handlers[vector]();
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void idt_register_handler(u8 vector, void (*handler)(void), u8 dpl) {
    isr_handlers[vector] = handler;

    /* Update the IDT gate entry with the new DPL */
    u8 type_attr = (dpl == 3) ? GATE_INTERRUPT_USER : GATE_INTERRUPT_KERNEL;

    idt_set_entry(&idt[vector], (u64)isr_stub_table[vector], type_attr, 0);
}

void idt_enable_interrupts(void) {
    __asm__ volatile("sti");
}

void idt_disable_interrupts(void) {
    __asm__ volatile("cli");
}

void irq_enable(u8 irq) {
    /* PIC: unmask the IRQ line so the interrupt is delivered */
    pic_unmask_irq(irq);

    /* Device-level enable for COM1: serial_init() leaves the 16550
     * interrupt-enable register at 0, so the UART would never assert
     * IRQ4 on received data.  Set the received-data-available bit. */
    if (irq == 4) {
        u8 ier = io_inb(SERIAL_COM1_BASE + 1);
        io_outb(SERIAL_COM1_BASE + 1, ier | 0x01);
    }
}

void irq_disable(u8 irq) {
    /* Device-level disable for COM1 (mirror of irq_enable) */
    if (irq == 4) {
        u8 ier = io_inb(SERIAL_COM1_BASE + 1);
        io_outb(SERIAL_COM1_BASE + 1, ier & ~0x01);
    }

    /* PIC: mask the IRQ line */
    pic_mask_irq(irq);
}

/* ------------------------------------------------------------------ */
/* PIT (8253/8254) initialization                                     */
/* ------------------------------------------------------------------ */

#define PIT_CHANNEL0_DATA 0x40
#define PIT_CMD_REG       0x43
#define PIT_BASE_FREQ     1193182 /* PIT oscillator frequency (Hz) */

void pit_init(u32 freq) {
    u32 divisor = PIT_BASE_FREQ / freq;
    if (divisor == 0)
        divisor = 1;

    /* Command byte: channel 0, lobyte/hibyte, rate generator (mode 2) */
    io_outb(PIT_CMD_REG, 0x36);

    /* Send divisor (low byte first, then high byte) */
    io_outb(PIT_CHANNEL0_DATA, (u8)(divisor & 0xFF));
    io_outb(PIT_CHANNEL0_DATA, (u8)((divisor >> 8) & 0xFF));

    serial_printf("PIT: Channel 0 at %u Hz (divisor=%u)\n", freq, divisor);
}

void idt_init(void) {
    /* Clear IDT and handler table */
    memset(idt, 0, sizeof(idt));
    memset(isr_handlers, 0, sizeof(isr_handlers));

    /* Populate IDT entries for all 256 vectors */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        u64 stub_addr;
        u8  type_attr;

        if (i == 0x80) {
            /* Syscall vector 0x80: use dedicated syscall_entry_stub */
            stub_addr = (u64)syscall_entry_stub;
            type_attr = GATE_INTERRUPT_USER; /* DPL=3, user-callable */
        } else if (i < 32) {
            /* CPU exceptions: trap gates (DPL=0) */
            stub_addr = (u64)isr_stub_table[i];
            type_attr = GATE_TRAP_KERNEL;
        } else {
            /* Hardware IRQs and software interrupts: interrupt gates */
            stub_addr = (u64)isr_stub_table[i];
            type_attr = GATE_INTERRUPT_KERNEL;
        }

        /* IST selection for critical exceptions */
        u8 ist = 0;
        if (i == 8) /* #DF: Double fault → IST1 */
            ist = IST_STACK_1;
        if (i == 2) /* NMI → IST2 */
            ist = IST_STACK_2;

        idt_set_entry(&idt[i], stub_addr, type_attr, ist);
    }

    /* Build IDT pointer */
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (u64)&idt;

    /* Load IDT register */
    __asm__ volatile("lidt (%0)" : : "r"(&idt_ptr) : "memory");

    /* Remap PIC: IRQ 0-15 → vectors 32-47 */
    pic_remap();

    /* Mask all IRQs, then unmask only what we need */
    pic_mask_all();
    pic_unmask_irq(0); /* Timer (IRQ0) */
    pic_unmask_irq(2); /* Cascade (IRQ2) for slave PIC */
    pic_unmask_irq(4); /* Serial (IRQ4) */
}
