/* ================================================================
 *  Systrix OS — drivers/pic.c
 *  Intel 8259A Programmable Interrupt Controller (PIC) driver
 *
 *  Remaps IRQ 0–7  → vectors 0x20–0x27 (PIC1)
 *  Remaps IRQ 8–15 → vectors 0x28–0x2F (PIC2)
 *
 *  Default mask: only IRQ0 (timer) unmasked.
 *  Keyboard is polled — no IRQ1 needed.
 * ================================================================ */
#include "../include/kernel.h"

void pic_init(void) {
    /* ICW1: cascade mode, ICW4 needed */
    outb(PIC1_CMD,  0x11); io_wait();
    outb(PIC2_CMD,  0x11); io_wait();

    /* ICW2: remap PIC1 to 0x20, PIC2 to 0x28 */
    outb(PIC1_DATA, 0x20); io_wait();
    outb(PIC2_DATA, 0x28); io_wait();

    /* ICW3: PIC1 has slave on IRQ2, PIC2 ID = 2 */
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    /* Mask all except IRQ0 (timer) */
    outb(PIC1_DATA, 0xFE);
    outb(PIC2_DATA, 0xFF);
}

/* Unmask a specific IRQ line (0–15) */
void pic_unmask(int irq) {
    if (irq < 8) {
        u8 mask = inb(PIC1_DATA);
        outb(PIC1_DATA, mask & ~(1 << irq));
    } else {
        u8 mask = inb(PIC2_DATA);
        outb(PIC2_DATA, mask & ~(1 << (irq - 8)));
        /* Also unmask IRQ2 on PIC1 (cascade line) */
        u8 m1 = inb(PIC1_DATA);
        outb(PIC1_DATA, m1 & ~0x04);
    }
}

/* Mask a specific IRQ line (0–15) */
void pic_mask(int irq) {
    if (irq < 8) {
        u8 mask = inb(PIC1_DATA);
        outb(PIC1_DATA, mask | (1 << irq));
    } else {
        u8 mask = inb(PIC2_DATA);
        outb(PIC2_DATA, mask | (1 << (irq - 8)));
    }
}

/* Send End-Of-Interrupt signal */
void pic_eoi(int irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

/* Disable both PICs entirely (use when switching to APIC) */
void pic_disable(void) {
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}
