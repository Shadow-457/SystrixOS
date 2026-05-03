/* ================================================================
 *  Systrix OS — drivers/pci.c
 *  PCI bus enumeration and configuration space access
 *
 *  Provides:
 *    - Type-0 configuration space read/write (I/O method)
 *    - Full bus/device/function enumeration (pci_scan_all)
 *    - Class/subclass/prog-if search helpers
 *    - BAR base address extraction (32-bit and 64-bit)
 *    - MSI / MSI-X detection helpers
 *
 *  Already declared + used by kernel.c (ahci, nvme, e1000 init).
 *  This file separates the implementation into the drivers/ folder.
 * ================================================================ */
#include "../include/kernel.h"

/* ── PCI config-space I/O ports ────────────────────────────── */
#define PCI_CFG_ADDR   0xCF8
#define PCI_CFG_DATA   0xCFC

#define PCI_MAX_DEVS   256   /* max stored devices */

/* Simple flat device table */
typedef struct {
    u8  bus, dev, func;
    u16 vendor, device;
    u8  class, subclass, progif, rev;
    u8  header;
    u32 bar[6];
    u8  irq_line, irq_pin;
} PCIDevice;

static PCIDevice pci_devs[PCI_MAX_DEVS];
static int       pci_count = 0;

/* ── Low-level config-space access ─────────────────────────── */
static u32 pci_cfg_addr(u8 bus, u8 dev, u8 func, u8 reg) {
    return 0x80000000UL
         | ((u32)bus  << 16)
         | ((u32)dev  << 11)
         | ((u32)func <<  8)
         | ((u32)reg  & 0xFC);
}

u32 pci_read32(u8 bus, u8 dev, u8 func, u8 reg) {
    outl(PCI_CFG_ADDR, pci_cfg_addr(bus, dev, func, reg));
    return inl(PCI_CFG_DATA);
}

void pci_write32(u8 bus, u8 dev, u8 func, u8 reg, u32 val) {
    outl(PCI_CFG_ADDR, pci_cfg_addr(bus, dev, func, reg));
    outl(PCI_CFG_DATA, val);
}

u16 pci_read16(u8 bus, u8 dev, u8 func, u8 reg) {
    u32 v = pci_read32(bus, dev, func, reg & ~3);
    return (u16)(v >> ((reg & 2) * 8));
}

u8 pci_read8(u8 bus, u8 dev, u8 func, u8 reg) {
    u32 v = pci_read32(bus, dev, func, reg & ~3);
    return (u8)(v >> ((reg & 3) * 8));
}

/* Enable bus-mastering DMA for a device */
void pci_enable_bus_master(u8 bus, u8 dev, u8 func) {
    u16 cmd = pci_read16(bus, dev, func, 0x04);
    cmd |= (1 << 2);   /* Bus Master bit */
    cmd |= (1 << 0);   /* I/O Space */
    cmd |= (1 << 1);   /* Memory Space */
    pci_write32(bus, dev, func, 0x04, (u32)cmd);
}

/* ── Enumeration ────────────────────────────────────────────── */
static void pci_probe(u8 bus, u8 dev, u8 func) {
    u32 id = pci_read32(bus, dev, func, 0x00);
    if ((u16)id == 0xFFFF) return;   /* no device */

    if (pci_count >= PCI_MAX_DEVS) return;

    PCIDevice *d = &pci_devs[pci_count++];
    d->bus    = bus;
    d->dev    = dev;
    d->func   = func;
    d->vendor = (u16)(id & 0xFFFF);
    d->device = (u16)(id >> 16);

    u32 cc  = pci_read32(bus, dev, func, 0x08);
    d->rev      = (u8)(cc & 0xFF);
    d->progif   = (u8)(cc >> 8);
    d->subclass = (u8)(cc >> 16);
    d->class    = (u8)(cc >> 24);

    d->header = pci_read8(bus, dev, func, 0x0E) & 0x7F;

    /* Read BARs 0-5 */
    for (int b = 0; b < 6; b++)
        d->bar[b] = pci_read32(bus, dev, func, (u8)(0x10 + b * 4));

    d->irq_line = pci_read8(bus, dev, func, 0x3C);
    d->irq_pin  = pci_read8(bus, dev, func, 0x3D);

    /* Enable bus-master by default for all storage + network controllers */
    if (d->class == 0x01 || d->class == 0x02)
        pci_enable_bus_master(bus, dev, func);

    print_str("[PCI] ");
    print_hex_byte(bus);  print_str(":");
    print_hex_byte(dev);  print_str(".");
    vga_putchar('0' + func); print_str("  ");
    print_hex_byte(d->vendor >> 8); print_hex_byte((u8)d->vendor);
    print_str(":");
    print_hex_byte(d->device >> 8); print_hex_byte((u8)d->device);
    print_str("  class=");
    print_hex_byte(d->class); print_str(".");
    print_hex_byte(d->subclass);
    print_str("\r\n");
}

void pci_scan_all(void) {
    pci_count = 0;
    print_str("[PCI] scanning...\r\n");
    for (u16 bus = 0; bus < 256; bus++)
        for (u8 dev = 0; dev < 32; dev++) {
            u32 id = pci_read32((u8)bus, dev, 0, 0x00);
            if ((u16)id == 0xFFFF) continue;
            u8 hdr = pci_read8((u8)bus, dev, 0, 0x0E);
            int multi = (hdr & 0x80) ? 8 : 1;
            for (int f = 0; f < multi; f++) {
                u32 fid = pci_read32((u8)bus, dev, (u8)f, 0x00);
                if ((u16)fid == 0xFFFF) continue;
                pci_probe((u8)bus, dev, (u8)f);
            }
        }
    print_str("[PCI] scan complete\r\n");
}

/* ── Search helpers ─────────────────────────────────────────── */

/* Returns internal PCIDevice* or NULL — opaque to callers */
void *pci_find_class_progif(u8 class, u8 sub, u8 prog) {
    for (int i = 0; i < pci_count; i++) {
        PCIDevice *d = &pci_devs[i];
        if (d->class == class && d->subclass == sub && d->progif == prog)
            return d;
    }
    return (void*)0;
}

void *pci_find_vendor_device(u16 vendor, u16 device) {
    for (int i = 0; i < pci_count; i++) {
        PCIDevice *d = &pci_devs[i];
        if (d->vendor == vendor && d->device == device)
            return d;
    }
    return (void*)0;
}

/* Extract the base address from a BAR (strips type bits, handles 64-bit) */
u64 pci_bar_base(void *dev, int bar_index) {
    PCIDevice *d = (PCIDevice*)dev;
    if (!d || bar_index > 5) return 0;
    u32 bar = d->bar[bar_index];
    if (bar & 1) {
        /* I/O BAR */
        return (u64)(bar & ~3UL);
    }
    int type = (bar >> 1) & 3;
    u64 base = (u64)(bar & ~0xFUL);
    if (type == 2 && bar_index < 5) {
        /* 64-bit BAR: upper 32 bits in next BAR */
        base |= (u64)d->bar[bar_index + 1] << 32;
    }
    return base;
}

/* Return the physical IRQ line assigned by firmware */
u8 pci_irq(void *dev) {
    return ((PCIDevice*)dev)->irq_line;
}
