/* ================================================================
 *  Systrix OS — drivers/apic.c
 *  Local APIC + I/O APIC driver
 *
 *  Provides:
 *    - Local APIC enable & calibration (timer via LAPIC)
 *    - LAPIC timer as alternative to PIT for per-CPU ticks
 *    - I/O APIC routing helpers
 *    - EOI for APIC-based interrupts
 *
 *  The PIC is kept active for legacy IRQs (timer, keyboard polled).
 *  Call apic_init() after IDT and PIC are set up if you want to
 *  use APIC for SMP.  Safe to skip on UP systems.
 * ================================================================ */
#include "../include/kernel.h"

/* ── Local APIC registers (MMIO at 0xFEE00000) ─────────────── */
#define LAPIC_BASE       0xFEE00000UL

#define LAPIC_ID         0x020
#define LAPIC_VER        0x030
#define LAPIC_TPR        0x080   /* Task Priority Register */
#define LAPIC_EOI        0x0B0
#define LAPIC_SVR        0x0F0   /* Spurious Vector Register */
#define LAPIC_ICR_LO     0x300
#define LAPIC_ICR_HI     0x310
#define LAPIC_LVT_TIMER  0x320
#define LAPIC_LINT0      0x350
#define LAPIC_LINT1      0x360
#define LAPIC_LVT_ERROR  0x370
#define LAPIC_TIMER_IC   0x380   /* Initial Count */
#define LAPIC_TIMER_CC   0x390   /* Current Count */
#define LAPIC_TIMER_DIV  0x3E0   /* Divide Configuration */

#define LAPIC_SVR_ENABLE (1 << 8)
#define LAPIC_MASKED     (1 << 16)
#define LAPIC_TIMER_ONESHOT  0x00000
#define LAPIC_TIMER_PERIODIC 0x20000

/* I/O APIC base (typically 0xFEC00000) */
#define IOAPIC_BASE      0xFEC00000UL
#define IOAPIC_REGSEL    0x00
#define IOAPIC_IOWIN     0x10

static volatile u32 *lapic = (volatile u32*)LAPIC_BASE;

static inline u32 lapic_read(u32 reg) {
    return lapic[reg / 4];
}
static inline void lapic_write(u32 reg, u32 val) {
    lapic[reg / 4] = val;
}

/* ── I/O APIC helpers ───────────────────────────────────────── */
static volatile u32 *ioapic = (volatile u32*)IOAPIC_BASE;

static u32 ioapic_read(u8 reg) {
    ioapic[IOAPIC_REGSEL / 4] = reg;
    return ioapic[IOAPIC_IOWIN / 4];
}
static void ioapic_write(u8 reg, u32 val) {
    ioapic[IOAPIC_REGSEL / 4] = reg;
    ioapic[IOAPIC_IOWIN / 4] = val;
}

/* Route GSI line to the given vector on BSP (LAPIC ID 0) */
void ioapic_route(u8 gsi, u8 vector, int active_low, int level_trig) {
    u64 entry = (u64)vector;
    if (active_low)  entry |= (1ULL << 13);
    if (level_trig)  entry |= (1ULL << 15);
    /* destination: BSP (LAPIC ID 0) */
    entry |= (u64)0 << 56;

    u8 lo_reg = (u8)(0x10 + gsi * 2);
    u8 hi_reg = (u8)(lo_reg + 1);
    ioapic_write(lo_reg, (u32)(entry & 0xFFFFFFFF));
    ioapic_write(hi_reg, (u32)(entry >> 32));
}

/* Mask / unmask an I/O APIC pin */
void ioapic_mask(u8 gsi)   {
    u8 reg = (u8)(0x10 + gsi * 2);
    ioapic_write(reg, ioapic_read(reg) | (1 << 16));
}
void ioapic_unmask(u8 gsi) {
    u8 reg = (u8)(0x10 + gsi * 2);
    ioapic_write(reg, ioapic_read(reg) & ~(u32)(1 << 16));
}

/* ── Local APIC init ────────────────────────────────────────── */
void apic_init(void) {
    /* Check IA32_APIC_BASE MSR */
    u32 lo, hi;
    __asm__ volatile(
        "rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1B));
    if (!(lo & (1 << 11))) {
        /* APIC globally disabled in MSR — enable it */
        lo |= (1 << 11);
        __asm__ volatile(
            "wrmsr" :: "c"(0x1B), "a"(lo), "d"(hi));
    }

    /* Set spurious vector 0xFF and enable LAPIC */
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | 0xFF);

    /* Clear task priority (accept all interrupts) */
    lapic_write(LAPIC_TPR, 0);

    /* Mask LINT0/LINT1 (PIC handles legacy IRQs) */
    lapic_write(LAPIC_LINT0, LAPIC_MASKED);
    lapic_write(LAPIC_LINT1, LAPIC_MASKED);

    /* Mask LAPIC timer — PIT is used for tick generation */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_MASKED);

    print_str("[APIC] local APIC enabled, ID=");
    print_hex_byte((u8)(lapic_read(LAPIC_ID) >> 24));
    print_str("\r\n");
}

/* Send APIC EOI — must be called from interrupt handlers that use APIC */
void apic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

/* Send an IPI to another CPU (used by SMP wakeup) */
void apic_send_ipi(u8 dest_lapic_id, u8 vector) {
    lapic_write(LAPIC_ICR_HI, (u32)dest_lapic_id << 24);
    lapic_write(LAPIC_ICR_LO, (u32)vector | (1 << 14));  /* level=assert */
}

/* ── LAPIC one-shot timer (for future SMP per-CPU ticks) ────── */
void apic_timer_oneshot_us(u32 us) {
    /* Divide by 16 */
    lapic_write(LAPIC_TIMER_DIV, 0x3);
    lapic_write(LAPIC_LVT_TIMER, 0x20 | LAPIC_TIMER_ONESHOT);  /* vector 0x20 */
    /* Rough calibration: assume ~100 MHz LAPIC clock → 100 ticks/us */
    lapic_write(LAPIC_TIMER_IC, us * 100);
}
