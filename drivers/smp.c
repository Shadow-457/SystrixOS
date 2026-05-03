/* ================================================================
 *  Systrix OS — drivers/smp.c
 *  Symmetric Multi-Processing (SMP) — AP bringup stubs
 *
 *  On a single-core or QEMU UP system this is mostly stubs.
 *  On real multi-core hardware, smp_init() reads the ACPI MADT
 *  to discover Application Processors, sends INIT/SIPI IPIs via
 *  the LAPIC, and waits for APs to signal readiness.
 *
 *  Currently implemented:
 *    - CPU count detection via CPUID
 *    - Per-CPU spinlock primitives
 *    - smp_init() stub that reports core count
 *
 *  Full AP bringup (SIPI trampolines, per-AP GDT/TSS) will be
 *  added in a future release.
 * ================================================================ */
#include "../include/kernel.h"

static int cpu_count = 1;

/* ── CPUID helpers ──────────────────────────────────────────── */
static void cpuid(u32 leaf, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx) {
    __asm__ volatile(
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0));
}

/* ── Spinlock ───────────────────────────────────────────────── */
void spinlock_acquire(volatile int *lock) {
    while (__sync_lock_test_and_set(lock, 1))
        while (*lock) __asm__ volatile("pause");
}

void spinlock_release(volatile int *lock) {
    __sync_lock_release(lock);
}

/* ── SMP init ───────────────────────────────────────────────── */
void smp_init(void) {
    /* Detect logical CPU count from CPUID leaf 1 EBX[23:16] */
    u32 eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    cpu_count = (int)((ebx >> 16) & 0xFF);
    if (cpu_count < 1) cpu_count = 1;

    print_str("[SMP] detected ");
    vga_putchar('0' + (u8)cpu_count);
    print_str(" logical CPU(s)\r\n");

    if (cpu_count == 1) {
        print_str("[SMP] single-core mode\r\n");
        return;
    }

    /* Multi-core: would send INIT/SIPI here.
     * Deferred until per-AP stack + GDT + TSS scaffolding is ready. */
    print_str("[SMP] AP bringup deferred (stub)\r\n");
}

int smp_cpu_count(void) {
    return cpu_count;
}

/* Return current CPU ID from LAPIC (0 on UP systems) */
int smp_current_cpu(void) {
    /* LAPIC ID is at MMIO 0xFEE00020 bits [31:24] */
    volatile u32 *lapic = (volatile u32*)0xFEE00020UL;
    return (int)((*lapic) >> 24);
}
