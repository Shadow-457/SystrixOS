/* ================================================================
 *  Systrix OS — drivers/power.c
 *  Power management: ACPI shutdown, CPU halt, reboot
 *
 *  Provides:
 *    - power_halt()    — halt all CPUs (HLT loop)
 *    - power_reboot()  — triple-fault or keyboard controller reboot
 *    - power_shutdown() — ACPI S5 soft-off via PM1a_CNT
 *    - power_sleep_ms() — busy-wait wrapper around pit_ticks
 *
 *  ACPI S5 shutdown: reads FADT from ACPI tables for PM1a_CNT_BLK.
 *  Falls back to QEMU/Bochs magic port (0x604) if ACPI parse fails.
 * ================================================================ */
#include "../include/kernel.h"

/* ── CPU halt ───────────────────────────────────────────────── */
void __attribute__((noreturn)) power_halt(void) {
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

/* ── Keyboard controller reboot ─────────────────────────────── */
void power_reboot(void) {
    /* Method 1: pulse reset line via i8042 port 0x64 */
    u32 t = 100000;
    while ((inb(0x64) & 2) && --t) {}   /* wait for input buffer empty */
    outb(0x64, 0xFE);                   /* pulse reset pin */
    /* Method 2: triple fault if keyboard reboot doesn't take */
    for (u32 i = 0; i < 1000000; i++) __asm__ volatile("nop");
    __asm__ volatile(
        "lidt %0\n"
        "int $0\n"
        :: "m"(*(u64*)0));   /* load null IDT and trigger fault */
    power_halt();
}

/* ── ACPI shutdown ───────────────────────────────────────────── */

/* ACPI RSDP search range */
#define RSDP_SEARCH_START  0x000E0000UL
#define RSDP_SEARCH_END    0x000FFFFFUL

typedef struct __attribute__((packed)) {
    char   sig[8];      /* "RSD PTR " */
    u8     checksum;
    char   oem_id[6];
    u8     revision;
    u32    rsdt_addr;
    /* extended fields (revision >= 2) */
    u32    length;
    u64    xsdt_addr;
    u8     ext_checksum;
    u8     reserved[3];
} RSDP;

typedef struct __attribute__((packed)) {
    char  sig[4];
    u32   length;
    u8    revision;
    u8    checksum;
    char  oem_id[6];
    char  oem_table_id[8];
    u32   oem_revision;
    u32   creator_id;
    u32   creator_revision;
} ACPITableHeader;

typedef struct __attribute__((packed)) {
    ACPITableHeader hdr;   /* "FACP" */
    u32  firmware_ctrl;
    u32  dsdt;
    u8   _reserved1;
    u8   preferred_pm_profile;
    u16  sci_int;
    u32  smi_cmd;
    u8   acpi_enable;
    u8   acpi_disable;
    u8   s4bios_req;
    u8   pstate_cnt;
    u32  pm1a_evt_blk;
    u32  pm1b_evt_blk;
    u32  pm1a_cnt_blk;   /* we need this */
    u32  pm1b_cnt_blk;
    /* ... rest omitted */
} FADT;

static RSDP *find_rsdp(void) {
    for (u64 addr = RSDP_SEARCH_START; addr < RSDP_SEARCH_END; addr += 16) {
        if (*(u64*)addr == 0x2052545020445352ULL)   /* "RSD PTR " */
            return (RSDP*)addr;
    }
    return (void*)0;
}

static u32 acpi_pm1a_cnt = 0;
static u16 acpi_slp_typa = 0;   /* SLP_TYPx value for S5 */

/* Minimal ACPI init: locate FADT, read PM1a_CNT_BLK */
void acpi_init(void) {
    RSDP *rsdp = find_rsdp();
    if (!rsdp) {
        print_str("[ACPI] RSDP not found\r\n");
        return;
    }

    /* Walk RSDT to find FADT ("FACP") */
    ACPITableHeader *rsdt = (ACPITableHeader*)(u64)rsdp->rsdt_addr;
    if (!rsdt) return;

    u32 entries = (rsdt->length - sizeof(ACPITableHeader)) / 4;
    u32 *ptrs = (u32*)((u64)rsdt + sizeof(ACPITableHeader));

    for (u32 i = 0; i < entries; i++) {
        ACPITableHeader *tbl = (ACPITableHeader*)(u64)ptrs[i];
        if (!tbl) continue;
        if (tbl->sig[0]=='F' && tbl->sig[1]=='A' &&
            tbl->sig[2]=='C' && tbl->sig[3]=='P') {
            FADT *fadt = (FADT*)tbl;
            acpi_pm1a_cnt = fadt->pm1a_cnt_blk;
            /* S5 sleep type: parse _S5 in DSDT is complex;
             * hardcode 0x1C00 which works on QEMU/Bochs */
            acpi_slp_typa = 0x1C00;   /* SLP_TYP=5, SLP_EN=1 */
            print_str("[ACPI] PM1a_CNT=");
            print_hex_byte((u8)(acpi_pm1a_cnt >> 8));
            print_hex_byte((u8)acpi_pm1a_cnt);
            print_str("\r\n");
            return;
        }
    }
    print_str("[ACPI] FADT not found\r\n");
}

void power_shutdown(void) {
    print_str("[PWR] shutting down...\r\n");

    /* Try ACPI S5 */
    if (acpi_pm1a_cnt) {
        outw((u16)acpi_pm1a_cnt, acpi_slp_typa | (1 << 13));
    }

    /* Fallback: QEMU/Bochs magic port */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);

    /* If still running: just halt */
    power_halt();
}

/* ── Busy-wait wrappers ─────────────────────────────────────── */
void power_sleep_ms(u32 ms) {
    pit_sleep_ms(ms);
}
