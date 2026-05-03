/* ================================================================
 *  Systrix OS — drivers/speaker.c
 *  PC Speaker (beeper) driver via PIT Channel 2 + port 0x61
 *
 *  The PC speaker is wired to PIT channel 2.  Setting it up:
 *    1. Program PIT channel 2 with desired frequency
 *    2. Set bits 0+1 of port 0x61 to gate the speaker
 *
 *  Useful for boot beeps, error tones, and simple audio cues
 *  before the sound card (sound.c) is ready.
 * ================================================================ */
#include "../include/kernel.h"

#define PIT_CH2     0x42    /* PIT channel 2 data port */
#define PIT_CMD_CH2 0xB6    /* mode 3, channel 2, 16-bit LSB+MSB */
#define PORT_SPEAKER 0x61

/* Turn speaker off */
void speaker_off(void) {
    u8 v = inb(PORT_SPEAKER);
    outb(PORT_SPEAKER, v & ~0x03);
}

/* Turn speaker on at currently programmed frequency */
void speaker_on(void) {
    u8 v = inb(PORT_SPEAKER);
    outb(PORT_SPEAKER, v | 0x03);
}

/* Set PC speaker frequency in Hz */
void speaker_set_freq(u32 hz) {
    if (hz == 0) { speaker_off(); return; }
    u32 div = 1193182 / hz;
    outb(PIT_CMD, PIT_CMD_CH2);
    outb(PIT_CH2, (u8)(div & 0xFF));
    outb(PIT_CH2, (u8)(div >> 8));
}

/* Play a tone at hz for approximately ms milliseconds */
void speaker_beep(u32 hz, u32 ms) {
    speaker_set_freq(hz);
    speaker_on();
    pit_sleep_ms(ms);
    speaker_off();
}

/* Boot-complete beep: short 880 Hz tone */
void speaker_boot_beep(void) {
    speaker_beep(880, 80);
}

/* Error beep: two short low tones */
void speaker_error_beep(void) {
    speaker_beep(220, 150);
    pit_sleep_ms(60);
    speaker_beep(220, 150);
}
