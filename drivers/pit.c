/* ================================================================
 *  Systrix OS — drivers/pit.c
 *  Intel 8253/8254 Programmable Interval Timer (PIT) driver
 *
 *  Channel 0 → IRQ0 → vector 0x20 (timer ISR in isr.S)
 *  Default frequency: 1000 Hz (1ms per tick)
 *    PIT_DIV = 1193182 / 1000 = 1193 (defined in kernel.h)
 *
 *  pit_ticks is incremented by the timer ISR in isr.S.
 * ================================================================ */
#include "../include/kernel.h"

void pit_init(void) {
    /* Mode 3: square wave generator, channel 0, 16-bit LSB+MSB */
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, (u8)(PIT_DIV & 0xFF));
    outb(PIT_CH0, (u8)(PIT_DIV >> 8));
}

/* Set a custom frequency in Hz (min ~19 Hz, max ~1.19 MHz) */
void pit_set_freq(u32 hz) {
    u32 div = 1193182 / hz;
    if (div > 0xFFFF) div = 0xFFFF;
    if (div < 1) div = 1;
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, (u8)(div & 0xFF));
    outb(PIT_CH0, (u8)(div >> 8));
}

/* Busy-wait for approximately ms milliseconds using pit_ticks */
void pit_sleep_ms(u32 ms) {
    u64 target = pit_ticks + ms;
    while (pit_ticks < target) __asm__ volatile("pause");
}

/* One-shot delay using PIT channel 2 (speaker, no IRQ needed) */
void pit_delay_us(u32 us) {
    /* Rough busy-loop calibrated against 1193182 Hz */
    u32 count = us * 12;   /* ~12 ticks per microsecond */
    while (count--) __asm__ volatile("nop");
}
