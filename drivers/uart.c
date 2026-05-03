/* ================================================================
 *  Systrix OS — drivers/uart.c
 *  16550A UART serial driver (COM1 = 0x3F8, COM2 = 0x2F8)
 *
 *  Provides:
 *    - Polled TX/RX (no interrupts)
 *    - 115200 baud, 8N1
 *    - printf-style debug output via uart_puts()
 *    - Can be used as a debug console before VGA is ready
 * ================================================================ */
#include "../include/kernel.h"

#define COM1_BASE   0x3F8
#define COM2_BASE   0x2F8

/* UART register offsets */
#define UART_DATA   0   /* RX/TX data (DLAB=0) */
#define UART_IER    1   /* Interrupt enable (DLAB=0) */
#define UART_DLL    0   /* Divisor latch low  (DLAB=1) */
#define UART_DLH    1   /* Divisor latch high (DLAB=1) */
#define UART_FCR    2   /* FIFO control */
#define UART_LCR    3   /* Line control */
#define UART_MCR    4   /* Modem control */
#define UART_LSR    5   /* Line status */
#define UART_MSR    6   /* Modem status */

/* LSR bits */
#define UART_LSR_DR     0x01    /* Data ready (RX) */
#define UART_LSR_THRE   0x20    /* TX holding register empty */

/* Baud rate divisors (base clock = 1.8432 MHz) */
#define BAUD_115200  1
#define BAUD_57600   2
#define BAUD_38400   3
#define BAUD_9600    12
#define BAUD_1200    96

static u16 uart_base = COM1_BASE;

void uart_init(u16 base, u16 divisor) {
    uart_base = base;

    outb(base + UART_IER, 0x00);        /* disable interrupts */
    outb(base + UART_LCR, 0x80);        /* enable DLAB */
    outb(base + UART_DLL, (u8)(divisor & 0xFF));
    outb(base + UART_DLH, (u8)(divisor >> 8));
    outb(base + UART_LCR, 0x03);        /* 8 data bits, no parity, 1 stop, DLAB=0 */
    outb(base + UART_FCR, 0xC7);        /* enable FIFO, clear, 14-byte threshold */
    outb(base + UART_MCR, 0x0B);        /* DTR + RTS + OUT2 */
}

/* Check if transmitter is ready */
static int uart_tx_ready(void) {
    return inb(uart_base + UART_LSR) & UART_LSR_THRE;
}

/* Check if received byte is waiting */
int uart_rx_ready(void) {
    return inb(uart_base + UART_LSR) & UART_LSR_DR;
}

/* Send one byte (blocking) */
void uart_putc(u8 c) {
    u32 timeout = 100000;
    while (!uart_tx_ready() && --timeout) {}
    outb(uart_base + UART_DATA, c);
}

/* Receive one byte (blocking) */
u8 uart_getc(void) {
    while (!uart_rx_ready()) {}
    return inb(uart_base + UART_DATA);
}

/* Receive one byte (non-blocking — returns 0xFF if nothing ready) */
u8 uart_getc_nb(void) {
    if (!uart_rx_ready()) return 0xFF;
    return inb(uart_base + UART_DATA);
}

/* Send a null-terminated string */
void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc((u8)*s++);
    }
}

/* Send a hex byte for debug */
void uart_hex_byte(u8 v) {
    const char *h = "0123456789ABCDEF";
    uart_putc((u8)h[v >> 4]);
    uart_putc((u8)h[v & 0xF]);
}

/* Send a 64-bit value as hex */
void uart_hex64(u64 v) {
    uart_puts("0x");
    for (int i = 56; i >= 0; i -= 8) uart_hex_byte((u8)(v >> i));
}

/* Default init: COM1 at 115200 baud */
void uart_init_default(void) {
    uart_init(COM1_BASE, BAUD_115200);
}
