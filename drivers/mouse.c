/* ================================================================
 *  Systrix OS — drivers/mouse.c
 *  PS/2 mouse driver — polled, text-mode software cursor
 *
 *  Relies on ps2.c for the i8042 controller init (ps2_init,
 *  ps2_mouse_ok, ps2_mouse_refresh).  This file owns the
 *  3-byte packet assembly, delta decoding, and the VGA text-cell
 *  cursor (inverted attribute).
 *
 *  Extracted from kernel.c.  The input_push_mouse() call feeds
 *  the generic input ring buffer in input.c.
 * ================================================================ */
#include "../include/kernel.h"

static int  mouse_col     = 40;
static int  mouse_row     = 12;
static u16  mouse_saved   = 0;
static int  mouse_visible = 0;
static int  mouse_cycle   = 0;
static u8   mouse_pkt[3];

/* ── Cursor rendering ───────────────────────────────────────── */
static void mouse_draw(void) {
    if (!mouse_visible) return;
    u16 *fb  = (u16*)VGA_BASE;
    int  idx = mouse_row * VGA_COLS + mouse_col;
    u16  cell = fb[idx];
    u8   attr = (u8)(cell >> 8);
    u8   inv  = (u8)(((attr & 0x0F) << 4) | ((attr >> 4) & 0x0F));
    fb[idx]   = (u16)((u16)inv << 8) | (cell & 0xFF);
}

static void mouse_erase(void) {
    if (!mouse_visible) return;
    u16 *fb  = (u16*)VGA_BASE;
    fb[mouse_row * VGA_COLS + mouse_col] = mouse_saved;
}

void mouse_show(void) {
    u16 *fb  = (u16*)VGA_BASE;
    mouse_saved   = fb[mouse_row * VGA_COLS + mouse_col];
    mouse_visible = 1;
    mouse_draw();
}

void mouse_hide(void) {
    mouse_erase();
    mouse_visible = 0;
}

static void mouse_move(int dr, int dc) {
    mouse_erase();
    mouse_row += dr;
    mouse_col += dc;
    if (mouse_row < 0) mouse_row = 0;
    if (mouse_row >= VGA_ROWS) mouse_row = VGA_ROWS - 1;
    if (mouse_col < 0) mouse_col = 0;
    if (mouse_col >= VGA_COLS) mouse_col = VGA_COLS - 1;
    u16 *fb = (u16*)VGA_BASE;
    mouse_saved = fb[mouse_row * VGA_COLS + mouse_col];
    mouse_draw();
}

/* ── Packet assembly — call from keyboard poll loop ─────────── */
void mouse_poll(void) {
    u8 b = inb(0x60);
    mouse_pkt[mouse_cycle++] = b;
    if (mouse_cycle < 3) return;
    mouse_cycle = 0;

    u8 flags = mouse_pkt[0];
    if (!(flags & 0x08)) { mouse_cycle = 0; return; }   /* resync */
    if (flags & 0xC0) return;                            /* overflow */

    int dx =  (int)mouse_pkt[1] - ((flags & 0x10) ? 256 : 0);
    int dy =  (int)mouse_pkt[2] - ((flags & 0x20) ? 256 : 0);

    int dc =  dx / 8;
    int dr = -dy / 8;

    if (dc || dr) mouse_move(dr, dc);

    u8 btns = 0;
    if (flags & 0x01) btns |= INPUT_BTN_LEFT;
    if (flags & 0x02) btns |= INPUT_BTN_RIGHT;
    if (flags & 0x04) btns |= INPUT_BTN_MIDDLE;
    input_push_mouse(dx, dy, btns);
}

/* Get current text-mode mouse position */
void mouse_get_pos(int *row, int *col) {
    if (row) *row = mouse_row;
    if (col) *col = mouse_col;
}
