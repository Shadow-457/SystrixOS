/* ================================================================
 *  Systrix OS — drivers/keyboard.c
 *  PS/2 Keyboard driver (polled, scancode set 1)
 *
 *  Strategy: pure polled I/O — reads 0x60 in the main loop.
 *  No IRQ1 required.  The PS/2 controller (ps2.c) handles the
 *  low-level i8042 init; this file handles scancode → ASCII.
 *
 *  Supports:
 *    - Full US QWERTY scancode set 1 (make codes only)
 *    - Shift, Ctrl, Alt modifiers
 *    - Caps Lock toggle
 *    - Extended keys: arrows, Page Up/Down, Home, End, Insert, Delete
 *    - Ctrl+C (SIGINT), Ctrl+D (EOF), Ctrl+L (clear)
 * ================================================================ */
#include "../include/kernel.h"

/* ── Scancode → ASCII tables (set 1 make codes) ─────────────── */
static const u8 kb_sc[128] = {
    0,    0x1B, '1',  '2',  '3',  '4',  '5',  '6',   /* 0x00–0x07 */
    '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t',   /* 0x08–0x0F */
    'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',    /* 0x10–0x17 */
    'o',  'p',  '[',  ']',  '\r', 0,    'a',  's',    /* 0x18–0x1F */
    'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',    /* 0x20–0x27 */
    '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',    /* 0x28–0x2F */
    'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',    /* 0x30–0x37 */
    0,    ' ',  0,    0,    0,    0,    0,    0,       /* 0x38–0x3F */
    0,    0,    0,    0,    0,    0,    0,    '7',     /* 0x40–0x47 */
    '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',   /* 0x48–0x4F */
    '2',  '3',  '0',  '.',  0,    0,    0,    0,      /* 0x50–0x57 */
    0,    0,    0,    0,    0,    0,    0,    0,       /* 0x58–0x5F */
    0,    0,    0,    0,    0,    0,    0,    0,       /* 0x60–0x67 */
    0,    0,    0,    0,    0,    0,    0,    0,       /* 0x68–0x6F */
    0,    0,    0,    0,    0,    0,    0,    0,       /* 0x70–0x77 */
    0,    0,    0,    0,    0,    0,    0,    0,       /* 0x78–0x7F */
};

static const u8 kb_sc_shift[128] = {
    0,    0x1B, '!',  '@',  '#',  '$',  '%',  '^',
    '&',  '*',  '(',  ')',  '_',  '+',  '\b', '\t',
    'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',
    'O',  'P',  '{',  '}',  '\r', 0,    'A',  'S',
    'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',
    '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',
    'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',
    0,    ' ',  0,    0,    0,    0,    0,    0,
    [0x47]='7',[0x48]='8',[0x49]='9',[0x4A]='-',
    [0x4B]='4',[0x4C]='5',[0x4D]='6',[0x4E]='+',
    [0x4F]='1',[0x50]='2',[0x51]='3',[0x52]='0',[0x53]='.',
};

/* ── Key state ──────────────────────────────────────────────── */
static int kb_shift    = 0;
static int kb_ctrl     = 0;
static int kb_alt      = 0;
static int kb_caps     = 0;
static int kb_extended = 0;   /* set after 0xE0 prefix */

/* ── Translate scancode to ASCII/key code ───────────────────── */
u8 kb_translate(u8 sc) {
    /* Extended key prefix */
    if (sc == 0xE0) { kb_extended = 1; return 0; }

    /* Break codes (key release): high bit set */
    if (sc & 0x80) {
        u8 make = sc & 0x7F;
        if (make == 0x2A || make == 0x36) kb_shift = 0;
        if (make == 0x1D) kb_ctrl = 0;
        if (make == 0x38) kb_alt  = 0;
        kb_extended = 0;
        return 0;
    }

    /* Extended scancodes → special key codes */
    if (kb_extended) {
        kb_extended = 0;
        switch (sc) {
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        case 0x49: return KEY_PGUP;
        case 0x51: return KEY_PGDN;
        case 0x52: return KEY_INS;
        case 0x53: return KEY_DEL;
        case 0x1C: return '\r';   /* numpad enter */
        default:   return 0;
        }
    }

    /* Modifier keys */
    if (sc == 0x2A || sc == 0x36) { kb_shift = 1; return 0; }
    if (sc == 0x1D) { kb_ctrl = 1; return 0; }
    if (sc == 0x38) { kb_alt  = 1; return 0; }
    if (sc == 0x3A) { kb_caps = !kb_caps; return 0; }

    /* Shift+PageUp / Shift+PageDown for scrollback */
    if (kb_shift && sc == 0x49) { vga_scroll_up();   return 0; }
    if (kb_shift && sc == 0x51) { vga_scroll_down(); return 0; }

    /* Normal ASCII */
    u8 ascii = kb_shift ? kb_sc_shift[sc] : kb_sc[sc];
    if (!ascii) return 0;

    /* Caps Lock: flip case for letters */
    if (kb_caps && ascii >= 'a' && ascii <= 'z') ascii -= 32;
    if (kb_caps && ascii >= 'A' && ascii <= 'Z') ascii += 32;

    /* Ctrl modifier: Ctrl+A=1 .. Ctrl+Z=26 */
    if (kb_ctrl && ascii >= 'a' && ascii <= 'z') ascii -= 96;
    if (kb_ctrl && ascii >= 'A' && ascii <= 'Z') ascii -= 64;

    return ascii;
}

/* Poll keyboard — returns key code or 0 if nothing ready */
u8 kbd_poll(void) {
    u8 st = inb(0x64);
    if (!(st & 0x01)) return 0;     /* output buffer empty */
    if (st & 0x20)    return 0;     /* byte is from mouse, not keyboard */
    return kb_translate(inb(0x60));
}

/* Blocking read — waits until a non-zero key is available */
u8 kbd_read(void) {
    u8 k;
    do { k = kbd_poll(); } while (!k);
    return k;
}

/* Query current modifier state */
int kbd_shift(void) { return kb_shift; }
int kbd_ctrl(void)  { return kb_ctrl; }
int kbd_alt(void)   { return kb_alt; }
int kbd_caps(void)  { return kb_caps; }
