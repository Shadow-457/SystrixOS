/* ================================================================
 *  Systrix OS — drivers/ata.c
 *  ATA PIO driver (28-bit LBA, primary channel 0x1F0)
 *
 *  Extracted from kernel.c.  Provides polled sector-level read/write
 *  to the primary ATA bus.  Used by fat32.c and the FAT32 VFS layer.
 *
 *  Features:
 *    - 28-bit LBA addressing (up to 128 GB)
 *    - Timeout-guarded BSY/DRQ polling (never hangs on absent disk)
 *    - Watchdog integration (suspend during long waits)
 *    - ATA FLUSH CACHE after every write
 *    - Identity read for disk detection
 * ================================================================ */
#include "../include/kernel.h"

/* ── Status / Error bits ───────────────────────────────────── */
#define ATA_SR_BSY   0x80   /* busy */
#define ATA_SR_DRDY  0x40   /* drive ready */
#define ATA_SR_DF    0x20   /* drive fault */
#define ATA_SR_DRQ   0x08   /* data request */
#define ATA_SR_ERR   0x01   /* error */

/* ── Alternate status / control port ───────────────────────── */
#define ATA_ALT_STATUS  0x3F6   /* read = alt status, write = device control */

/* Small delay: read alt-status 4 times (~400 ns on real hw) */
static inline void ata_delay400(void) {
    inb(ATA_ALT_STATUS); inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS); inb(ATA_ALT_STATUS);
}

/* Poll until BSY clears and DRDY sets; returns 0 on timeout */
static int ata_wait_ready(void) {
    u32 t = 0x100000;
    u8 st;
    do {
        st = inb(ATA_STATUS);
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRDY)) return 1;
    } while (--t);
    return 0;
}

/* Poll until DRQ sets (data ready); returns 0 on timeout */
static int ata_wait_drq(void) {
    u32 t = 0x100000;
    while (!(inb(ATA_STATUS) & ATA_SR_DRQ) && --t) {}
    return t != 0;
}

/* ── Public API ─────────────────────────────────────────────── */

/* Initialise the primary ATA channel.
 * Must be called before any read/write operations.
 * Performs a software reset and waits for DRDY. */
void ata_init(void) {
    /* Disable interrupts on primary channel (nIEN=1) */
    outb(ATA_ALT_STATUS, 0x02);
    /* Select master drive */
    outb(ATA_DRIVE, 0xA0);
    ata_delay400();
    /* Software reset: assert SRST, hold, then clear */
    outb(ATA_ALT_STATUS, 0x06);   /* nIEN | SRST */
    ata_delay400();
    outb(ATA_ALT_STATUS, 0x02);   /* nIEN only — release reset */
    /* Wait up to ~30 s for BSY to clear and DRDY to set */
    for (u32 t = 0; t < 0x4000000; t++) {
        u8 st = inb(ATA_ALT_STATUS);
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRDY)) break;
    }
    /* Re-select master in LBA mode */
    outb(ATA_DRIVE, 0xE0);
    ata_delay400();
}

/* Detect whether a primary ATA drive is present.
 * Returns 1 if a drive responded, 0 otherwise. */
int ata_detect(void) {
    outb(ATA_DRIVE, 0xA0);   /* select master */
    ata_delay400();
    u8 st = inb(ATA_STATUS);
    /* 0xFF = no drive; 0x00 = ATAPI before identify */
    if (st == 0xFF) return 0;
    return 1;
}

void ata_read_sector(u32 lba, void *buf) {
    watchdog_suspend();
    if (!ata_wait_ready()) {
        u16 *p = (u16*)buf;
        for (int i = 0; i < 256; i++) p[i] = 0;
        watchdog_resume();
        return;
    }
    outb(ATA_DRIVE,   0xE0 | (u8)((lba >> 24) & 0x0F));
    outb(ATA_COUNT,   1);
    outb(ATA_LBA_LO,  (u8)(lba));
    outb(ATA_LBA_MID, (u8)(lba >> 8));
    outb(ATA_LBA_HI,  (u8)(lba >> 16));
    outb(ATA_CMD,     ATA_CMD_READ);
    ata_delay400();
    if (!ata_wait_drq()) {
        u16 *p = (u16*)buf;
        for (int i = 0; i < 256; i++) p[i] = 0;
        watchdog_resume();
        return;
    }
    u16 *p = (u16*)buf;
    for (int i = 0; i < 256; i++) p[i] = inw(ATA_DATA);
    watchdog_resume();
}

void ata_write_sector(u32 lba, const void *buf) {
    watchdog_suspend();
    if (!ata_wait_ready()) { watchdog_resume(); return; }
    outb(ATA_DRIVE,   0xE0 | (u8)((lba >> 24) & 0x0F));
    outb(ATA_COUNT,   1);
    outb(ATA_LBA_LO,  (u8)(lba));
    outb(ATA_LBA_MID, (u8)(lba >> 8));
    outb(ATA_LBA_HI,  (u8)(lba >> 16));
    outb(ATA_CMD,     ATA_CMD_WRITE);
    ata_delay400();
    if (!ata_wait_drq()) { watchdog_resume(); return; }
    const u16 *p = (const u16*)buf;
    for (int i = 0; i < 256; i++) outw(ATA_DATA, p[i]);
    outb(ATA_CMD, ATA_CMD_FLUSH);
    ata_wait_ready();   /* wait for flush to complete */
    watchdog_resume();
}

/* Read multiple consecutive sectors (count <= 255).
 * More efficient than looping ata_read_sector for large transfers. */
void ata_read_sectors(u32 lba, u8 count, void *buf) {
    if (count == 0) return;
    watchdog_suspend();
    if (!ata_wait_ready()) { watchdog_resume(); return; }
    outb(ATA_DRIVE,   0xE0 | (u8)((lba >> 24) & 0x0F));
    outb(ATA_COUNT,   count);
    outb(ATA_LBA_LO,  (u8)(lba));
    outb(ATA_LBA_MID, (u8)(lba >> 8));
    outb(ATA_LBA_HI,  (u8)(lba >> 16));
    outb(ATA_CMD,     ATA_CMD_READ);
    u16 *p = (u16*)buf;
    for (int s = 0; s < count; s++) {
        ata_delay400();
        if (!ata_wait_drq()) break;
        for (int i = 0; i < 256; i++) p[s * 256 + i] = inw(ATA_DATA);
    }
    watchdog_resume();
}

/* Fill ata_identity_t with drive information (model, serial, LBA28/48). */
int ata_identify(ata_identity_t *out) {
    outb(ATA_DRIVE, 0xA0);
    outb(ATA_COUNT, 0);
    outb(ATA_LBA_LO, 0); outb(ATA_LBA_MID, 0); outb(ATA_LBA_HI, 0);
    outb(ATA_CMD, 0xEC);   /* IDENTIFY DEVICE */
    ata_delay400();
    if (inb(ATA_STATUS) == 0) return 0;   /* no drive */
    if (!ata_wait_drq()) return 0;
    for (int i = 0; i < 256; i++) out->raw[i] = inw(ATA_DATA);
    return 1;
}
