/* ================================================================
 *  Systrix OS — drivers/vga.c
 *  VGA text-mode driver (80×25, 0xB8000)
 *
 *  Features:
 *    - 200-line circular scroll-back buffer
 *    - Shift+PageUp / Shift+PageDown to scroll view
 *    - Hardware cursor synchronisation (CRT registers 0x3D4/0x3D5)
 *    - Automatic live-view scrolling on overflow
 *    - PS/2 text-mode mouse cursor (inverted cell)
 *
 *  Extracted from kernel.c and extended with splash support.
 * ================================================================ */
#include "../include/kernel.h"

/* ── Scroll-back configuration ─────────────────────────────── */
#define SCROLLBACK_ROWS  200
#define SCROLL_STEP        5

static u16 back_buf[SCROLLBACK_ROWS][VGA_COLS];
static int buf_next  = 0;
static int buf_total = 0;
static int scroll_offset = 0;

static u8 cur_row = 0, cur_col = 0;

/* ── Hardware cursor ────────────────────────────────────────── */
static void vga_update_hw_cursor(void) {
    u16 pos = (u16)(cur_row * VGA_COLS + cur_col);
    outb(0x3D4, 0x0F); outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}

/* Enable hardware cursor (scanlines 14-15 = underline) */
void vga_cursor_enable(void) {
    outb(0x3D4, 0x0A); outb(0x3D5, (inb(0x3D5) & 0xC0) | 14);
    outb(0x3D4, 0x0B); outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);
}

/* Hide hardware cursor */
void vga_cursor_hide(void) {
    outb(0x3D4, 0x0A); outb(0x3D5, 0x20);
}

/* ── Internal helpers ───────────────────────────────────────── */
static void vga_fb_scroll_one(void) {
    u16 *fb = (u16*)VGA_BASE;
    for (int i = 0; i < VGA_COLS * (VGA_ROWS - 1); i++)
        fb[i] = fb[i + VGA_COLS];
    for (int i = VGA_COLS * (VGA_ROWS - 1); i < VGA_COLS * VGA_ROWS; i++)
        fb[i] = (u16)(VGA_ATTR << 8) | ' ';
}

static void backbuf_advance(void) {
    buf_next = (buf_next + 1) % SCROLLBACK_ROWS;
    buf_total++;
    for (int i = 0; i < VGA_COLS; i++)
        back_buf[buf_next][i] = (u16)(VGA_ATTR << 8) | ' ';
}

/* ── Public API ─────────────────────────────────────────────── */
static void vga_redraw(void) {
    u16 *fb = (u16*)VGA_BASE;
    int top = buf_total - VGA_ROWS - scroll_offset;
    for (int r = 0; r < VGA_ROWS; r++) {
        int src = top + r;
        if (src < 0) {
            for (int c = 0; c < VGA_COLS; c++)
                fb[r * VGA_COLS + c] = (u16)(VGA_ATTR << 8) | ' ';
        } else {
            int idx = src % SCROLLBACK_ROWS;
            for (int c = 0; c < VGA_COLS; c++)
                fb[r * VGA_COLS + c] = back_buf[idx][c];
        }
    }
}


/* ── Coloured print (fg 0–15, bg 0–7) ──────────────────────── */
void vga_putchar_color(u8 c, u8 attr) {
    if (c == '\r' || c == '\n') { vga_putchar(c); return; }
    back_buf[buf_next][cur_col] = (u16)((u16)attr << 8) | c;
    if (scroll_offset == 0)
        ((u16*)VGA_BASE)[cur_row * VGA_COLS + cur_col] =
            (u16)((u16)attr << 8) | c;
    if (++cur_col >= VGA_COLS) {
        cur_col = 0;
        backbuf_advance();
        if (cur_row < VGA_ROWS - 1) cur_row++;
        else if (scroll_offset == 0) vga_fb_scroll_one();
    }
    vga_update_hw_cursor();
}

void vga_print_color(const char *s, u8 attr) {
    while (*s) vga_putchar_color((u8)*s++, attr);
}

/* ── Splash screen: shown for ~3 seconds at boot ─────────────
 *  Draws a full-screen banner then busy-waits using PIT ticks. */
void vga_splash(void) {
    u16 *fb = (u16*)VGA_BASE;

    /* Fill screen with dark-blue background */
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        fb[i] = (u16)(0x1F << 8) | ' ';   /* white-on-blue */

    /* ── Centre the banner ──────────────────────────────────── */
    /* Line 8: top border */
    const char *border =
        "  +=======================================================+  ";
    /* Line 9: blank */
    const char *blank  =
        "  |                                                       |  ";
    /* Line 10: title */
    const char *title  =
        "  |            SYSTRIX OS  v0.1  IS  BOOTING             |  ";
    /* Line 11: subtitle */
    const char *sub    =
        "  |         x86-64  |  bare-metal  |  preemptive         |  ";
    /* Line 12: blank */
    /* Line 13: bottom border */

    u8 bdr = 0x1E;   /* yellow-on-blue for border */
    u8 ttl = 0x1F;   /* white-on-blue  for text   */

    /* Helper: write a string to a VGA row, centred in 80 cols */
    for (int pass = 0; pass < 6; pass++) {
        const char *line;
        u8 attr;
        int row;
        switch (pass) {
            case 0: line = border; attr = bdr; row = 8;  break;
            case 1: line = blank;  attr = bdr; row = 9;  break;
            case 2: line = title;  attr = ttl; row = 10; break;
            case 3: line = sub;    attr = ttl; row = 11; break;
            case 4: line = blank;  attr = bdr; row = 12; break;
            default:line = border; attr = bdr; row = 13; break;
        }
        for (int c = 0; c < VGA_COLS && line[c]; c++)
            fb[row * VGA_COLS + c] = (u16)((u16)attr << 8) | (u8)line[c];
    }

    /* Progress bar row 15: fills over 3 s (3000 ms ticks) */
    int bar_x = 10, bar_w = 60, filled = 0;
    fb[15 * VGA_COLS + bar_x - 1]      = (u16)(0x1E << 8) | '[';
    fb[15 * VGA_COLS + bar_x + bar_w]  = (u16)(0x1E << 8) | ']';
    for (int i = 0; i < bar_w; i++)
        fb[15 * VGA_COLS + bar_x + i]  = (u16)(0x18 << 8) | '-';

    /* Status line row 17 */
    const char *msg = "        Please wait while the kernel initialises ...";
    for (int i = 0; msg[i]; i++)
        fb[17 * VGA_COLS + i] = (u16)(0x17 << 8) | (u8)msg[i];

    /* ── Wait ~3 seconds using PIT channel-2 one-shot ticks ─────
     * This uses a pure polling approach that works BEFORE interrupts
     * are enabled (pit_ticks requires IRQs; this does not).
     * PIT base freq = 1,193,182 Hz.  We fire STEPS one-shots of
     * ~50 ms each (divisor = 59659) and fill one bar segment each. */
    #define SPLASH_STEPS   60          /* 60 × 50 ms ≈ 3 s            */
    #define PIT2_DIV  59659u           /* 1193182 / 20 ≈ 50 ms tick   */

    /* Enable PIT ch2 gate (bit 0 of port 0x61), speaker off (bit 1) */
    outb(0x61, (inb(0x61) | 0x01) & ~0x02);

    for (int step = 0; step < SPLASH_STEPS; step++) {
        /* Program channel 2 in mode 0 (interrupt on terminal count) */
        outb(0x43, 0xB0);                          /* ch2, mode 0, binary */
        outb(0x42, (u8)(PIT2_DIV & 0xFF));
        outb(0x42, (u8)(PIT2_DIV >> 8));

        /* Poll OUT bit (bit 5 of port 0x61) until counter expires */
        while (!(inb(0x61) & 0x20))
            __asm__ volatile("pause");

        /* Advance progress bar proportionally */
        int want = (step + 1) * bar_w / SPLASH_STEPS;
        while (filled < want && filled < bar_w) {
            fb[15 * VGA_COLS + bar_x + filled] = (u16)(0x1A << 8) | '|';
            filled++;
        }
    }

    /* Fill bar completely */
    for (int i = 0; i < bar_w; i++)
        fb[15 * VGA_COLS + bar_x + i] = (u16)(0x1A << 8) | '|';
}
