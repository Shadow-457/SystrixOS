/* ================================================================
 *  Systrix OS — drivers/dma.c
 *  ISA DMA controller (Intel 8237) + PCI bus-master DMA helpers
 *
 *  ISA DMA is used by the FDC (floppy) and legacy sound cards.
 *  PCI bus-master DMA is handled per-device (e1000, ahci, nvme).
 *  This file provides:
 *    - DMA channel setup for 8-bit (channels 0-3) and
 *      16-bit (channels 4-7) transfers
 *    - Physical address bounce-buffer allocation
 *    - Simple PCI DMA-capable memory region descriptor
 * ================================================================ */
#include "../include/kernel.h"

/* ── ISA DMA port map ───────────────────────────────────────── */
/* DMA 1 (channels 0-3): 8-bit */
#define DMA1_STATUS   0x08
#define DMA1_CMD      0x08
#define DMA1_REQ      0x09
#define DMA1_MASK1    0x0A
#define DMA1_MODE     0x0B
#define DMA1_FLIP     0x0C   /* byte pointer flip-flop */
#define DMA1_MASTER   0x0D
#define DMA1_MASK_ALL 0x0F

/* DMA 2 (channels 4-7): 16-bit */
#define DMA2_STATUS   0xD0
#define DMA2_CMD      0xD0
#define DMA2_REQ      0xD2
#define DMA2_MASK1    0xD4
#define DMA2_MODE     0xD6
#define DMA2_FLIP     0xD8
#define DMA2_MASTER   0xDA
#define DMA2_MASK_ALL 0xDE

/* Channel 0-3 base+count ports */
static const u16 dma1_addr[4] = {0x00, 0x02, 0x04, 0x06};
static const u16 dma1_cnt [4] = {0x01, 0x03, 0x05, 0x07};
/* Channel page registers (high 8 bits of 24-bit address) */
static const u16 dma1_page[4] = {0x87, 0x83, 0x81, 0x82};

/* ── Transfer mode codes ────────────────────────────────────── */
#define DMA_MODE_READ   0x44   /* read from device into memory */
#define DMA_MODE_WRITE  0x48   /* write from memory to device  */
#define DMA_MODE_AUTO   0x10   /* auto-init (repeat)           */

/* ── ISA DMA setup (channels 0-3 only) ─────────────────────── */
void dma_setup(u8 channel, void *buffer, u16 length, u8 mode) {
    if (channel > 3) return;

    u32 addr = (u32)(u64)buffer;
    u8  page = (u8)(addr >> 16);
    u16 base = (u16)(addr & 0xFFFF);

    /* mask channel */
    outb(DMA1_MASK1, (u8)(channel | 0x04));

    /* clear flip-flop */
    outb(DMA1_FLIP, 0x00);

    /* set mode (channel | read/write | single cycle) */
    outb(DMA1_MODE, (u8)(mode | channel));

    /* address low, high */
    outb(dma1_addr[channel], (u8)(base & 0xFF));
    outb(dma1_addr[channel], (u8)(base >> 8));

    /* page register */
    outb(dma1_page[channel], page);

    /* count (length - 1) */
    u16 cnt = (u16)(length - 1);
    outb(dma1_cnt[channel], (u8)(cnt & 0xFF));
    outb(dma1_cnt[channel], (u8)(cnt >> 8));

    /* unmask (start) */
    outb(DMA1_MASK1, channel);
}

void dma_mask(u8 channel) {
    if (channel > 3) return;
    outb(DMA1_MASK1, (u8)(channel | 0x04));
}

void dma_unmask(u8 channel) {
    if (channel > 3) return;
    outb(DMA1_MASK1, channel);
}

/* ── PCI bus-master DMA helpers ─────────────────────────────── */
/* Allocate a DMA-capable (< 4 GB) physically contiguous buffer.
 * For a bare-metal kernel without IOMMU, heap_malloc() already
 * returns identity-mapped physical addresses < 4 GB, so this is
 * just a wrapper that documents the constraint. */
void *dma_alloc(usize size) {
    /* heap_alloc is defined in heap.c */
    void *p = heap_malloc(size);
    /* Ensure below 4 GB (always true in our 64-bit identity map) */
    if ((u64)(u64)p >= 0x100000000ULL) return (void*)0;
    return p;
}

void dma_free(void *p) {
    heap_free(p);
}

/* Physical address of a kernel virtual address (identity-mapped) */
u64 dma_phys(void *virt) {
    return (u64)(u64)virt;
}
