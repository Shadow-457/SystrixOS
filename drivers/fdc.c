/* ================================================================
 *  Systrix OS — drivers/fdc.c
 *  Floppy Disk Controller (NEC µPD765 / Intel 8272A)
 *
 *  Provides:
 *    - Controller reset and ready polling
 *    - Seek / calibrate commands
 *    - Read/write sector (DMA channel 2)
 *    - 1.44 MB 3.5" geometry hardcoded (2 heads, 80 tracks, 18 spt)
 *
 *  IRQ6 is used; must be unmasked via pic_unmask(6) before use.
 *  Most modern x86-64 systems have no floppy controller — fdc_detect()
 *  returns 0 safely in that case.
 * ================================================================ */
#include "../include/kernel.h"

/* ── Ports ──────────────────────────────────────────────────── */
#define FDC_DOR   0x3F2   /* Digital Output Register */
#define FDC_MSR   0x3F4   /* Main Status Register */
#define FDC_DATA  0x3F5   /* Data FIFO */
#define FDC_DIR   0x3F7   /* Digital Input Register (read) */
#define FDC_CCR   0x3F7   /* Configuration Control Register (write) */

/* DOR bits */
#define DOR_MOTOR_A  0x10
#define DOR_DMA_IRQ  0x08
#define DOR_RESET_N  0x04   /* active-low reset */
#define DOR_DRIVE_A  0x00

/* MSR bits */
#define MSR_RQM   0x80   /* Request for Master — controller ready */
#define MSR_DIO   0x40   /* Data I/O: 1=controller→CPU, 0=CPU→controller */
#define MSR_NONDMA 0x20

/* 1.44 MB geometry */
#define FDC_SECTORS   18
#define FDC_HEADS      2
#define FDC_TRACKS    80

static int fdc_irq_fired = 0;

/* Poll until controller is ready to accept/send a byte */
static int fdc_wait_ready(void) {
    u32 t = 500000;
    while (--t) {
        u8 msr = inb(FDC_MSR);
        if (msr & MSR_RQM) return 1;
    }
    return 0;
}

static void fdc_write(u8 b) {
    if (fdc_wait_ready()) outb(FDC_DATA, b);
}

static u8 fdc_read(void) {
    if (fdc_wait_ready()) return inb(FDC_DATA);
    return 0xFF;
}

/* Called from IRQ6 handler in isr.S */
void fdc_irq(void) {
    fdc_irq_fired = 1;
    pic_eoi(6);
}

static void fdc_wait_irq(void) {
    u32 t = 2000000;
    while (!fdc_irq_fired && --t) __asm__ volatile("pause");
    fdc_irq_fired = 0;
}

/* ── Controller reset ───────────────────────────────────────── */
void fdc_reset(void) {
    outb(FDC_DOR, 0x00);                      /* assert reset */
    for (u32 i = 0; i < 100000; i++) __asm__ volatile("nop");
    outb(FDC_DOR, DOR_DMA_IRQ | DOR_RESET_N | DOR_MOTOR_A);
    fdc_wait_irq();
    /* Sense interrupt (4 times — one per drive implied) */
    for (int i = 0; i < 4; i++) {
        fdc_write(0x08);   /* SENSE INTERRUPT STATUS */
        fdc_read();        /* ST0 */
        fdc_read();        /* PCN */
    }
    /* Set transfer rate: 500 kbps for 1.44 MB */
    outb(FDC_CCR, 0x00);
}

/* ── Calibrate (seek to track 0) ────────────────────────────── */
static void fdc_calibrate(void) {
    fdc_write(0x07);   /* RECALIBRATE */
    fdc_write(0x00);   /* drive 0 */
    fdc_wait_irq();
    fdc_write(0x08);   /* SENSE INTERRUPT */
    fdc_read(); fdc_read();
}

/* ── Detect presence ────────────────────────────────────────── */
int fdc_detect(void) {
    /* CMOS byte 0x10: bits [7:4] = drive A type (0 = none) */
    outb(0x70, 0x10);
    u8 cmos = inb(0x71);
    return (cmos >> 4) != 0;
}

/* ── Init ────────────────────────────────────────────────────── */
void fdc_init(void) {
    if (!fdc_detect()) {
        print_str("[FDC] no floppy drive detected\r\n");
        return;
    }
    pic_unmask(6);
    fdc_reset();
    fdc_calibrate();
    print_str("[FDC] 1.44 MB floppy ready\r\n");
}

/* ── LBA → CHS ───────────────────────────────────────────────── */
static void lba_to_chs(u32 lba, u8 *cyl, u8 *head, u8 *sec) {
    *cyl  = (u8)(lba / (FDC_HEADS * FDC_SECTORS));
    *head = (u8)((lba / FDC_SECTORS) % FDC_HEADS);
    *sec  = (u8)((lba % FDC_SECTORS) + 1);
}

/* ── Read one 512-byte sector into buf ──────────────────────── */
int fdc_read_sector(u32 lba, void *buf) {
    u8 cyl, head, sec;
    lba_to_chs(lba, &cyl, &head, &sec);

    /* Setup ISA DMA channel 2 for read (device→memory) */
    dma_setup(2, buf, 512, 0x46);   /* mode: single, read */

    /* Seek */
    fdc_write(0x0F);   /* SEEK */
    fdc_write(head);
    fdc_write(cyl);
    fdc_wait_irq();
    fdc_write(0x08); fdc_read(); fdc_read();

    /* Read Data */
    fdc_write(0xE6);   /* READ DATA, MFM */
    fdc_write(head);
    fdc_write(cyl);
    fdc_write(head);
    fdc_write(sec);
    fdc_write(2);      /* 512 bytes/sector */
    fdc_write(FDC_SECTORS);
    fdc_write(0x1B);   /* gap length */
    fdc_write(0xFF);   /* data length */

    fdc_wait_irq();

    /* Read result bytes */
    for (int i = 0; i < 7; i++) fdc_read();

    return 0;
}
