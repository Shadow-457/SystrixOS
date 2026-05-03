/* ================================================================
 *  Systrix OS — drivers/idt.c
 *  Interrupt Descriptor Table (IDT) setup
 *
 *  Installs 256 IDT entries.  Exception handlers come from isr.S.
 *  The timer ISR (IRQ0 → vector 0x20) is the only hardware interrupt
 *  handled via IDT — keyboard/mouse are polled.
 *
 *  Called by scheduler_init() before pic_init() / pit_init().
 * ================================================================ */
#include "../include/kernel.h"

/* IDT gate descriptor (16 bytes per AMD64 spec) */
typedef struct {
    u16 off_lo;
    u16 cs;
    u8  ist;
    u8  attr;       /* 0x8E = present, DPL=0, 64-bit interrupt gate */
    u16 off_mid;
    u32 off_hi;
    u32 reserved;
} __attribute__((packed)) IDTGate;

typedef struct {
    u16 limit;
    u64 base;
} __attribute__((packed)) IDTPtr;

/* ISR stubs from isr.S */
extern void null_isr(void);
extern void timer_isr(void);
extern void isr_errcode(void);
extern void isr_page_fault(void);

static void idt_set_gate(IDTGate *idt, int vec, void (*handler)(void), u8 attr) {
    u64 addr = (u64)handler;
    idt[vec].off_lo   = (u16)(addr & 0xFFFF);
    idt[vec].cs       = KERNEL_CS;
    idt[vec].ist      = 0;
    idt[vec].attr     = attr;
    idt[vec].off_mid  = (u16)((addr >> 16) & 0xFFFF);
    idt[vec].off_hi   = (u32)(addr >> 32);
    idt[vec].reserved = 0;
}

void idt_init(void) {
    IDTGate *idt = (IDTGate*)IDT_BASE;

    /* Fill all 256 entries with the null ISR first */
    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set_gate(idt, i, null_isr, 0x8E);

    /* Hardware exceptions that push an error code */
    idt_set_gate(idt, 8,  isr_errcode,    0x8E);   /* #DF double fault       */
    idt_set_gate(idt, 10, isr_errcode,    0x8E);   /* #TS invalid TSS        */
    idt_set_gate(idt, 11, isr_errcode,    0x8E);   /* #NP segment not present*/
    idt_set_gate(idt, 12, isr_errcode,    0x8E);   /* #SS stack fault        */
    idt_set_gate(idt, 13, isr_errcode,    0x8E);   /* #GP general protection */
    idt_set_gate(idt, 14, isr_page_fault, 0x8E);   /* #PF page fault         */
    idt_set_gate(idt, 17, isr_errcode,    0x8E);   /* #AC alignment check    */
    idt_set_gate(idt, 21, isr_errcode,    0x8E);   /* #CP control protection */
    idt_set_gate(idt, 29, isr_errcode,    0x8E);   /* #VC VMM communication  */
    idt_set_gate(idt, 30, isr_errcode,    0x8E);   /* #SX security exception */

    /* IRQ0: PIT timer (remapped to vector 0x20 by PIC) */
    idt_set_gate(idt, 0x20, timer_isr, 0x8E);

    /* Load IDT register */
    IDTPtr p = { (u16)(IDT_ENTRIES * sizeof(IDTGate) - 1), (u64)idt };
    __asm__ volatile("lidt %0" :: "m"(p));
}

/* Set a single gate at runtime (e.g. for dynamic driver IRQ registration) */
void idt_set(int vec, void (*handler)(void)) {
    IDTGate *idt = (IDTGate*)IDT_BASE;
    idt_set_gate(idt, vec, handler, 0x8E);
}
