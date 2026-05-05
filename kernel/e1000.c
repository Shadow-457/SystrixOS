/* ============================================================
 *  Systrix OS — kernel/e1000.c
 *
 *  Ethernet NIC driver supporting two hardware types:
 *    1. Intel e1000  (82540EM / 82545EM / 82574L)
 *    2. Realtek RTL8139
 *
 *  Auto-detection order: e1000 is tried first; RTL8139 is the
 *  fallback.  Both drivers expose the same three-function API:
 *
 *    nic_init()             — scan PCI and initialize whichever NIC is found
 *    nic_send(buf, len)     — transmit one raw Ethernet frame
 *    nic_poll()             — drain the RX ring; deliver each received frame
 *                             to the network stack via net_rx_deliver()
 *
 *  The MAC address of the active NIC is stored in nic_mac[6].
 *
 *  Ring design:
 *    e1000   — 16-descriptor TX ring, 16-descriptor RX ring, DMA into
 *              static 2048-byte per-slot buffers.
 *    RTL8139 — 4-slot TX ring (separate 1536-byte buffers per slot),
 *              single 64KB RX ring buffer with a software read pointer.
 *
 *  Memory model: Systrix OS uses an identity map (virtual == physical),
 *  so we pass buffer addresses directly to the hardware.
 * ============================================================ */

#include "../include/kernel.h"


/* ============================================================
 *  Exported NIC State
 * ============================================================ */

u8  nic_mac[6] = {0};   /* MAC address of the active NIC */
int nic_ready  = 0;     /* 1 once a NIC has been successfully initialized */


/* ============================================================
 *  PCI Vendor / Device IDs
 * ============================================================ */

#define E1000_VENDOR_ID   0x8086   /* Intel */
#define E1000_DEVICE_82540EM  0x100E   /* QEMU default e1000 model */
#define E1000_DEVICE_82545EM  0x100F
#define E1000_DEVICE_82574L   0x10D3

#define RTL_VENDOR_ID   0x10EC   /* Realtek */
#define RTL_DEVICE_8139 0x8139


/* ============================================================
 *
 *  PART 1: INTEL e1000 DRIVER
 *
 * ============================================================ */

/* ------------------------------------------------------------
 *  e1000 MMIO Register Offsets
 *  All offsets are from the BAR0 MMIO base address.
 *  See Intel 8254x Software Developer's Manual §13.
 * ------------------------------------------------------------ */

#define E1000_CTRL    0x0000   /* Device Control */
#define E1000_STATUS  0x0008   /* Device Status */
#define E1000_EECD    0x0010   /* EEPROM/Flash Control */
#define E1000_EERD    0x0014   /* EEPROM Read */
#define E1000_ICR     0x00C0   /* Interrupt Cause Read (clears on read) */
#define E1000_IMS     0x00D0   /* Interrupt Mask Set */
#define E1000_IMC     0x00D8   /* Interrupt Mask Clear */
#define E1000_RCTL    0x0100   /* Receive Control */
#define E1000_TCTL    0x0400   /* Transmit Control */
#define E1000_TIPG    0x0410   /* Transmit Inter-Packet Gap */
#define E1000_RDBAL   0x2800   /* RX Descriptor Base Address (low 32 bits) */
#define E1000_RDBAH   0x2804   /* RX Descriptor Base Address (high 32 bits) */
#define E1000_RDLEN   0x2808   /* RX Descriptor Ring Length in bytes */
#define E1000_RDH     0x2810   /* RX Descriptor Head (hardware-owned, read-only) */
#define E1000_RDT     0x2818   /* RX Descriptor Tail (driver writes here) */
#define E1000_TDBAL   0x3800   /* TX Descriptor Base Address (low 32 bits) */
#define E1000_TDBAH   0x3804   /* TX Descriptor Base Address (high 32 bits) */
#define E1000_TDLEN   0x3808   /* TX Descriptor Ring Length in bytes */
#define E1000_TDH     0x3810   /* TX Descriptor Head (hardware-owned, read-only) */
#define E1000_TDT     0x3818   /* TX Descriptor Tail (driver writes here to submit) */
#define E1000_RAL0    0x5400   /* Receive Address Low  — bytes 0-3 of MAC */
#define E1000_RAH0    0x5404   /* Receive Address High — bytes 4-5 of MAC + valid bit */
#define E1000_MTA     0x5200   /* Multicast Table Array (128 × u32 entries) */

/* ------------------------------------------------------------
 *  CTRL Register Bits
 * ------------------------------------------------------------ */
#define CTRL_AUTO_SPEED_DETECT  (1u << 5)   /* enable auto-speed detection */
#define CTRL_SET_LINK_UP        (1u << 6)   /* force link to come up */
#define CTRL_DEVICE_RESET       (1u << 26)  /* software reset — self-clearing */
#define CTRL_PHY_RESET          (1u << 31)  /* PHY reset */

/* ------------------------------------------------------------
 *  RCTL Register Bits (Receive Control)
 * ------------------------------------------------------------ */
#define RCTL_ENABLE             (1u << 1)
#define RCTL_STORE_BAD_PACKETS  (1u << 2)
#define RCTL_UNICAST_PROMISC    (1u << 3)   /* receive all unicast frames */
#define RCTL_MULTICAST_PROMISC  (1u << 4)   /* receive all multicast frames */
#define RCTL_ACCEPT_BROADCAST   (1u << 15)
#define RCTL_STRIP_CRC          (1u << 26)  /* strip 4-byte CRC before delivery */
#define RCTL_BUFFER_SIZE_2048   0           /* default receive buffer size */

/* ------------------------------------------------------------
 *  TCTL Register Bits (Transmit Control)
 * ------------------------------------------------------------ */
#define TCTL_ENABLE             (1u << 1)
#define TCTL_PAD_SHORT_PACKETS  (1u << 3)   /* pad frames shorter than 64 bytes */
#define TCTL_COLLISION_THRESHOLD_SHIFT  4   /* bits [11:4] — retries before abort */
#define TCTL_COLLISION_DISTANCE_SHIFT  12   /* bits [21:12] — half-duplex slot time */

/* ------------------------------------------------------------
 *  TX Descriptor Command and Status Bits
 * ------------------------------------------------------------ */
#define TDESC_CMD_END_OF_PACKET     (1u << 0)   /* this descriptor is the last in the frame */
#define TDESC_CMD_INSERT_FCS        (1u << 1)   /* hardware should insert the Ethernet FCS */
#define TDESC_CMD_REPORT_STATUS     (1u << 3)   /* set DD bit in status when done */
#define TDESC_STATUS_DESCRIPTOR_DONE (1u << 0)  /* hardware has finished with this descriptor */

/* ------------------------------------------------------------
 *  RX Descriptor Status Bits
 * ------------------------------------------------------------ */
#define RDESC_STATUS_DESCRIPTOR_DONE (1u << 0)  /* hardware has written a packet here */
#define RDESC_STATUS_END_OF_PACKET   (1u << 1)  /* this descriptor holds the last fragment */

/* ------------------------------------------------------------
 *  Ring Sizes and Buffer Sizes
 * ------------------------------------------------------------ */
#define E1000_TX_RING_SIZE  16
#define E1000_RX_RING_SIZE  16
#define E1000_PACKET_SIZE   2048   /* must be >= max Ethernet frame (1522 bytes) */


/* ------------------------------------------------------------
 *  Descriptor Structures
 *
 *  The hardware DMAs into and out of these structures directly,
 *  so they must be packed exactly as the spec defines them.
 * ------------------------------------------------------------ */

/* Receive Descriptor (16 bytes) — hardware fills this in when a packet arrives */
typedef struct __attribute__((packed)) {
    u64 buffer_addr;    /* physical address of the 2048-byte receive buffer */
    u16 length;         /* number of bytes written by hardware (excluding CRC if stripped) */
    u16 checksum;       /* optional packet checksum */
    u8  status;         /* status bits: DD, EOP, etc. */
    u8  errors;         /* error bits */
    u16 special;        /* VLAN tag info */
} E1000RxDescriptor;

/* Transmit Descriptor (16 bytes) — driver fills this in to send a packet */
typedef struct __attribute__((packed)) {
    u64 buffer_addr;    /* physical address of the data to transmit */
    u16 length;         /* number of bytes to transmit */
    u8  checksum_offset;
    u8  command;        /* command bits: EOP, IFCS, RS, etc. */
    u8  status;         /* DD bit set by hardware when transmission completes */
    u8  checksum_start;
    u16 special;
} E1000TxDescriptor;


/* ------------------------------------------------------------
 *  e1000 Driver State
 * ------------------------------------------------------------ */

static volatile u32 *g_e1000_mmio = NULL;   /* base address of the HBA MMIO region */

static E1000TxDescriptor g_e1000_tx_ring[E1000_TX_RING_SIZE] __attribute__((aligned(16)));
static E1000RxDescriptor g_e1000_rx_ring[E1000_RX_RING_SIZE] __attribute__((aligned(16)));

/* One packet buffer per ring slot */
static u8 g_e1000_tx_bufs[E1000_TX_RING_SIZE][E1000_PACKET_SIZE] __attribute__((aligned(16)));
static u8 g_e1000_rx_bufs[E1000_RX_RING_SIZE][E1000_PACKET_SIZE] __attribute__((aligned(16)));

static u32 g_e1000_tx_tail = 0;   /* next TX slot the driver will use */
static u32 g_e1000_rx_tail = 0;   /* last RX slot given back to hardware */


/* ------------------------------------------------------------
 *  e1000 MMIO Access Helpers
 * ------------------------------------------------------------ */

static inline u32 e1000_read(u32 reg_offset)
{
    return *(volatile u32 *)((u8 *)g_e1000_mmio + reg_offset);
}

static inline void e1000_write(u32 reg_offset, u32 value)
{
    *(volatile u32 *)((u8 *)g_e1000_mmio + reg_offset) = value;
}


/* ------------------------------------------------------------
 *  e1000 EEPROM Read
 *
 *  Initiates a read from the NIC's EEPROM at the given word
 *  address by writing to the EERD register, then polls until
 *  the "done" bit (bit 4) is set.  Used to read the MAC when
 *  the receive address registers contain zeroes.
 * ------------------------------------------------------------ */
static u16 e1000_eeprom_read(u8 word_addr)
{
    /* Writing bit 0 starts the read; bits [7:2] carry the address */
    e1000_write(E1000_EERD, 1u | ((u32)word_addr << 2));

    u32 result  = 0;
    int timeout = 100000;

    do {
        result = e1000_read(E1000_EERD);
    } while (!(result & (1u << 4)) && --timeout);

    return (u16)(result >> 16);
}


/* ------------------------------------------------------------
 *  e1000 Delay
 *
 *  Busy-wait for roughly `iterations` NOP cycles.  Used during
 *  reset sequences where the hardware needs a small settling time.
 * ------------------------------------------------------------ */
static void e1000_delay(u32 iterations)
{
    for (volatile u32 i = 0; i < iterations; i++) {
        __asm__ volatile("nop");
    }
}


/* ------------------------------------------------------------
 *  e1000_init
 *
 *  Full initialization sequence for the Intel e1000 NIC:
 *    1. Identity-map the 128KB MMIO region into the page tables.
 *    2. Read the MAC address from RAL0/RAH0 (QEMU pre-loads these).
 *       Fall back to EEPROM if the registers are zero.
 *    3. Reset the device, then bring the link up.
 *    4. Mask all interrupts (we use polled mode, not IRQs).
 *    5. Re-program the MAC into the receive address registers.
 *    6. Clear the multicast filter table.
 *    7. Set up the RX descriptor ring.
 *    8. Set up the TX descriptor ring.
 *    9. Configure the inter-packet gap timer.
 *
 *  Returns 1 on success, 0 if no valid MAC was found (NIC absent).
 * ------------------------------------------------------------ */
static int e1000_init(u64 mmio_phys_addr)
{
    /* Map the full 128KB MMIO region (identity map: virt == phys) */
    for (u64 offset = 0; offset < 0x20000; offset += PAGE_SIZE) {
        vmm_map(read_cr3(), mmio_phys_addr + offset, mmio_phys_addr + offset,
                PTE_PRESENT | PTE_WRITE | PTE_NX);
    }

    g_e1000_mmio = (volatile u32 *)(usize)mmio_phys_addr;

    /* Step 1: Read MAC from the Receive Address registers.
     * QEMU pre-loads these before the OS boots, so we check here first
     * before issuing a reset (which might clear them on real hardware). */
    {
        u32 ral = e1000_read(E1000_RAL0);
        u32 rah = e1000_read(E1000_RAH0);

        nic_mac[0] = (u8)(ral);
        nic_mac[1] = (u8)(ral >>  8);
        nic_mac[2] = (u8)(ral >> 16);
        nic_mac[3] = (u8)(ral >> 24);
        nic_mac[4] = (u8)(rah);
        nic_mac[5] = (u8)(rah >>  8);
    }

    /* Step 2: Fall back to the EEPROM if the registers held all zeroes */
    if (nic_mac[0] == 0 && nic_mac[1] == 0 && nic_mac[2] == 0) {
        u16 word0 = e1000_eeprom_read(0);
        u16 word1 = e1000_eeprom_read(1);
        u16 word2 = e1000_eeprom_read(2);

        nic_mac[0] = (u8)(word0);       nic_mac[1] = (u8)(word0 >> 8);
        nic_mac[2] = (u8)(word1);       nic_mac[3] = (u8)(word1 >> 8);
        nic_mac[4] = (u8)(word2);       nic_mac[5] = (u8)(word2 >> 8);
    }

    /* If the MAC is still all-zero, there is no NIC here */
    int mac_all_zero = (nic_mac[0] == 0 && nic_mac[1] == 0 && nic_mac[2] == 0 &&
                        nic_mac[3] == 0 && nic_mac[4] == 0 && nic_mac[5] == 0);
    if (mac_all_zero) {
        print_str("[e1000] no MAC address found — NIC absent\r\n");
        return 0;
    }

    /* Step 3: Software reset — clears all registers to defaults */
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | CTRL_DEVICE_RESET);
    e1000_delay(200000);

    /* Step 4: Bring the link up with auto-speed detection */
    u32 ctrl = e1000_read(E1000_CTRL);
    ctrl |=  (CTRL_SET_LINK_UP | CTRL_AUTO_SPEED_DETECT);
    ctrl &= ~CTRL_DEVICE_RESET;
    e1000_write(E1000_CTRL, ctrl);
    e1000_delay(10000);

    /* Step 5: Mask all interrupts — we use polling, not IRQs */
    e1000_write(E1000_IMC, 0xFFFFFFFF);
    e1000_read(E1000_ICR);              /* reading ICR clears any pending cause bits */

    /* Step 6: Re-program our MAC into the receive address registers.
     * Bit 31 of RAH0 is the "address valid" bit — must be set. */
    e1000_write(E1000_RAL0,
        (u32)nic_mac[0]         |
        ((u32)nic_mac[1] <<  8) |
        ((u32)nic_mac[2] << 16) |
        ((u32)nic_mac[3] << 24));

    e1000_write(E1000_RAH0,
        (u32)nic_mac[4]         |
        ((u32)nic_mac[5] <<  8) |
        (1u << 31));                    /* address valid bit */

    /* Step 7: Zero the multicast filter table (128 × 32-bit entries) */
    for (int i = 0; i < 128; i++) {
        e1000_write(E1000_MTA + i * 4, 0);
    }

    /* Step 8: Initialize the RX descriptor ring */
    for (int i = 0; i < E1000_RX_RING_SIZE; i++) {
        g_e1000_rx_ring[i].buffer_addr = (u64)(usize)g_e1000_rx_bufs[i];
        g_e1000_rx_ring[i].status      = 0;
    }

    u64 rx_ring_phys = (u64)(usize)g_e1000_rx_ring;
    e1000_write(E1000_RDBAL, (u32)(rx_ring_phys & 0xFFFFFFFF));
    e1000_write(E1000_RDBAH, (u32)(rx_ring_phys >> 32));
    e1000_write(E1000_RDLEN, (u32)(E1000_RX_RING_SIZE * sizeof(E1000RxDescriptor)));
    e1000_write(E1000_RDH, 0);
    e1000_write(E1000_RDT, E1000_RX_RING_SIZE - 1);   /* give all slots to hardware */
    g_e1000_rx_tail = E1000_RX_RING_SIZE - 1;

    e1000_write(E1000_RCTL,
        RCTL_ENABLE           |
        RCTL_ACCEPT_BROADCAST |
        RCTL_STRIP_CRC        |
        RCTL_MULTICAST_PROMISC);

    /* Step 9: Initialize the TX descriptor ring */
    for (int i = 0; i < E1000_TX_RING_SIZE; i++) {
        g_e1000_tx_ring[i].buffer_addr = (u64)(usize)g_e1000_tx_bufs[i];
        g_e1000_tx_ring[i].status      = TDESC_STATUS_DESCRIPTOR_DONE; /* mark all as free */
    }

    u64 tx_ring_phys = (u64)(usize)g_e1000_tx_ring;
    e1000_write(E1000_TDBAL, (u32)(tx_ring_phys & 0xFFFFFFFF));
    e1000_write(E1000_TDBAH, (u32)(tx_ring_phys >> 32));
    e1000_write(E1000_TDLEN, (u32)(E1000_TX_RING_SIZE * sizeof(E1000TxDescriptor)));
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);
    g_e1000_tx_tail = 0;

    e1000_write(E1000_TCTL,
        TCTL_ENABLE           |
        TCTL_PAD_SHORT_PACKETS |
        (0x0F << TCTL_COLLISION_THRESHOLD_SHIFT) |   /* 15 retries */
        (0x40 << TCTL_COLLISION_DISTANCE_SHIFT));     /* 512-bit slot time */

    /* Step 10: Inter-packet gap — standard IEEE 802.3 values */
    e1000_write(E1000_TIPG, 0x00702008u);

    return 1;
}


/* ------------------------------------------------------------
 *  e1000_send
 *
 *  Transmits one Ethernet frame.  Copies the frame data into
 *  the next available TX slot's buffer, fills in the descriptor,
 *  then advances the tail pointer to kick the hardware.
 *
 *  Polls for completion before returning so that the buffer
 *  is safe to reuse.  In a production driver this would be
 *  asynchronous, but synchronous TX keeps the code simple.
 * ------------------------------------------------------------ */
static void e1000_send(const void *frame_data, u16 frame_length)
{
    if (g_e1000_mmio == NULL) {
        return;
    }

    /* Wait for the current TX slot to be free (DD bit set by hardware) */
    int timeout = 100000;
    while (!(g_e1000_tx_ring[g_e1000_tx_tail].status & TDESC_STATUS_DESCRIPTOR_DONE)
           && --timeout > 0) {
        e1000_delay(10);
    }
    if (timeout == 0) {
        return;     /* TX ring stuck — drop the packet */
    }

    /* Copy the frame into the slot's DMA buffer */
    memcpy(g_e1000_tx_bufs[g_e1000_tx_tail], frame_data, frame_length);

    /* Fill in the descriptor */
    g_e1000_tx_ring[g_e1000_tx_tail].length  = frame_length;
    g_e1000_tx_ring[g_e1000_tx_tail].command  = TDESC_CMD_END_OF_PACKET
                                               | TDESC_CMD_INSERT_FCS
                                               | TDESC_CMD_REPORT_STATUS;
    g_e1000_tx_ring[g_e1000_tx_tail].status   = 0;   /* clear DD so hardware owns it */

    u32 submitted_slot = g_e1000_tx_tail;

    /* Advance tail — this is the "doorbell" that tells the hardware to transmit */
    g_e1000_tx_tail = (g_e1000_tx_tail + 1) % E1000_TX_RING_SIZE;
    e1000_write(E1000_TDT, g_e1000_tx_tail);

    /* Poll until the hardware sets DD on the descriptor we just submitted */
    timeout = 100000;
    while (!(g_e1000_tx_ring[submitted_slot].status & TDESC_STATUS_DESCRIPTOR_DONE)
           && --timeout > 0) {
        e1000_delay(10);
    }
}


/* ------------------------------------------------------------
 *  e1000_poll
 *
 *  Called repeatedly from the main loop to drain the RX ring.
 *  For each slot where the hardware has set the DD (Descriptor
 *  Done) bit, we deliver the packet to the network stack and
 *  give the slot back to hardware by advancing RDT.
 * ------------------------------------------------------------ */
static void e1000_poll(void)
{
    for (;;) {
        /* The slot after our tail pointer is where hardware writes next */
        u32 next_slot = (g_e1000_rx_tail + 1) % E1000_RX_RING_SIZE;

        /* If DD is not set, the hardware hasn't written a packet here yet */
        if (!(g_e1000_rx_ring[next_slot].status & RDESC_STATUS_DESCRIPTOR_DONE)) {
            break;
        }

        u16 length = g_e1000_rx_ring[next_slot].length;

        /* Only deliver complete, single-fragment packets (EOP bit set) */
        if (length > 0 && (g_e1000_rx_ring[next_slot].status & RDESC_STATUS_END_OF_PACKET)) {
            net_rx_deliver(g_e1000_rx_bufs[next_slot], length);
        }

        /* Reset status and give this slot back to hardware */
        g_e1000_rx_ring[next_slot].status = 0;
        g_e1000_rx_tail = next_slot;
        e1000_write(E1000_RDT, g_e1000_rx_tail);
    }
}


/* ============================================================
 *
 *  PART 2: REALTEK RTL8139 DRIVER
 *
 *  Fallback for systems or VMs that expose an RTL8139 rather
 *  than an Intel e1000.  The RTL8139 is older and simpler:
 *  it uses port I/O instead of MMIO, and has a linear ring
 *  buffer for RX rather than a descriptor ring.
 *
 * ============================================================ */

/* ------------------------------------------------------------
 *  RTL8139 Port I/O Register Offsets
 *  All offsets are from the PCI I/O BAR base address.
 * ------------------------------------------------------------ */

#define RTL_MAC0          0x00   /* MAC address (6 bytes) */
#define RTL_MAR0          0x08   /* Multicast Address Register (8 bytes) */
#define RTL_TXSTATUS0     0x10   /* TX Status register for slot 0 (4 × u32 at 0x10,0x14,0x18,0x1C) */
#define RTL_TXADDR0       0x20   /* TX Buffer Address for slot 0 (4 × u32) */
#define RTL_RXBUF         0x30   /* Physical address of the RX ring buffer */
#define RTL_CMD           0x37   /* Command register */
#define RTL_CAPR          0x38   /* Current Address of Packet Read (driver-owned read pointer) */
#define RTL_CBR           0x3A   /* Current Buffer Address (hardware write pointer, read-only) */
#define RTL_IMR           0x3C   /* Interrupt Mask Register */
#define RTL_ISR           0x3E   /* Interrupt Status Register */
#define RTL_TCR           0x40   /* Transmit Configuration Register */
#define RTL_RCR           0x44   /* Receive Configuration Register */
#define RTL_TSAD          0x60   /* TX Status of All Descriptors */
#define RTL_CONFIG1       0x52   /* Configuration Register 1 */

/* CMD register bits */
#define RTL_CMD_RESET       (1u << 4)   /* software reset — self-clearing */
#define RTL_CMD_RX_ENABLE   (1u << 3)
#define RTL_CMD_TX_ENABLE   (1u << 2)

/* RCR (Receive Configuration) bits */
#define RTL_RCR_ACCEPT_ALL_PHYS      (1u << 0)   /* accept all unicast (promiscuous) */
#define RTL_RCR_ACCEPT_PHYS_MATCH    (1u << 1)   /* accept frames matching our MAC */
#define RTL_RCR_ACCEPT_MULTICAST     (1u << 2)
#define RTL_RCR_ACCEPT_BROADCAST     (1u << 3)
#define RTL_RCR_WRAP_DISABLE         (1u << 7)   /* allow ring buffer to wrap */
#define RTL_RCR_RBLEN_64K            (3u << 11)  /* 64KB ring buffer */
#define RTL_RCR_DMA_BURST_UNLIMITED  (7u <<  8)  /* no DMA burst limit */
#define RTL_RCR_RX_FIFO_NO_THRESH   (7u << 13)  /* start DMA as soon as first byte arrives */

/* TX Status register bits (one u32 per slot at RTL_TXSTATUS0 + slot * 4) */
#define RTL_TXS_TX_OK   (1u << 15)  /* frame transmitted successfully */
#define RTL_TXS_OWN     (1u << 13)  /* 1 = driver owns this slot; 0 = NIC owns it */

/* Ring buffer sizes */
#define RTL_RX_BUF_SIZE   (64 * 1024 + 16)  /* 64KB + 16 bytes wrap padding */
#define RTL_TX_BUF_SIZE   1536               /* max Ethernet frame with some headroom */
#define RTL_TX_SLOT_COUNT 4                  /* RTL8139 has exactly 4 TX slots */


/* ------------------------------------------------------------
 *  RTL8139 Driver State
 * ------------------------------------------------------------ */

static u16  g_rtl_io_base = 0;   /* PCI I/O BAR base port */

static u8 g_rtl_tx_bufs[RTL_TX_SLOT_COUNT][RTL_TX_BUF_SIZE] __attribute__((aligned(4)));
static u8 g_rtl_rx_buf[RTL_RX_BUF_SIZE]                     __attribute__((aligned(4)));

static int  g_rtl_tx_slot = 0;   /* next TX slot to use (0–3, wraps) */
static u16  g_rtl_rx_ptr  = 0;   /* our read offset into the RX ring buffer */
static int  g_rtl_active  = 0;   /* 1 if RTL8139 was initialized successfully */


/* ------------------------------------------------------------
 *  RTL8139 Port I/O Helpers
 *
 *  Inline wrappers so callers don't have to add g_rtl_io_base
 *  to every offset manually.
 * ------------------------------------------------------------ */

static inline void rtl_write8 (u16 offset, u8  value) { outb((u16)(g_rtl_io_base + offset), value);  }
static inline void rtl_write16(u16 offset, u16 value) { outw((u16)(g_rtl_io_base + offset), value);  }
static inline void rtl_write32(u16 offset, u32 value) { outl((u16)(g_rtl_io_base + offset), value);  }
static inline u8   rtl_read8  (u16 offset)            { return inb((u16)(g_rtl_io_base + offset));   }
static inline u16  rtl_read16 (u16 offset)            { return inw((u16)(g_rtl_io_base + offset));   }
static inline u32  rtl_read32 (u16 offset)            { return inl((u16)(g_rtl_io_base + offset));   }


/* ------------------------------------------------------------
 *  rtl8139_init
 *
 *  Initializes the RTL8139:
 *    1. Power on the chip (write 0 to CONFIG1).
 *    2. Software reset.
 *    3. Read the MAC address from the MAC0 registers.
 *    4. Set the RX buffer address.
 *    5. Set the four TX buffer addresses.
 *    6. Mask all interrupts (polled mode).
 *    7. Configure the RX receive filter.
 *    8. Configure the TX DMA burst size.
 *    9. Enable RX and TX.
 *
 *  Returns 1 on success.
 * ------------------------------------------------------------ */
static int rtl8139_init(u16 io_base)
{
    g_rtl_io_base = io_base;

    /* Step 1: Power on — writing 0x00 to CONFIG1 wakes the chip up */
    rtl_write8(RTL_CONFIG1, 0x00);
    for (volatile int d = 0; d < 10000; d++);

    /* Step 2: Software reset — poll until the RST bit self-clears */
    rtl_write8(RTL_CMD, RTL_CMD_RESET);
    for (int timeout = 10000; timeout-- > 0 && (rtl_read8(RTL_CMD) & RTL_CMD_RESET); ) {
        for (volatile int d = 0; d < 100; d++);
    }

    /* Step 3: Read the MAC address (6 bytes starting at offset RTL_MAC0) */
    u32 mac_lo = rtl_read32(RTL_MAC0);
    u16 mac_hi = rtl_read16(RTL_MAC0 + 4);

    nic_mac[0] = (u8)(mac_lo);
    nic_mac[1] = (u8)(mac_lo >>  8);
    nic_mac[2] = (u8)(mac_lo >> 16);
    nic_mac[3] = (u8)(mac_lo >> 24);
    nic_mac[4] = (u8)(mac_hi);
    nic_mac[5] = (u8)(mac_hi >> 8);

    /* Step 4: Tell the NIC where the RX ring buffer lives */
    rtl_write32(RTL_RXBUF, (u32)(usize)g_rtl_rx_buf);

    /* Step 5: Point each TX slot at its dedicated buffer */
    for (int i = 0; i < RTL_TX_SLOT_COUNT; i++) {
        rtl_write32((u16)(RTL_TXADDR0 + i * 4), (u32)(usize)g_rtl_tx_bufs[i]);
    }

    /* Step 6: Mask all interrupts — clear any pending ones first */
    rtl_write16(RTL_IMR, 0x0000);
    rtl_write16(RTL_ISR, 0xFFFF);

    /* Step 7: Receive configuration — accept broadcast and unicast into 64KB ring */
    rtl_write32(RTL_RCR,
        RTL_RCR_ACCEPT_BROADCAST    |
        RTL_RCR_ACCEPT_PHYS_MATCH   |
        RTL_RCR_ACCEPT_MULTICAST    |
        RTL_RCR_RBLEN_64K           |
        RTL_RCR_DMA_BURST_UNLIMITED |
        RTL_RCR_RX_FIFO_NO_THRESH   |
        RTL_RCR_WRAP_DISABLE);

    /* Step 8: TX configuration — max DMA burst, standard IFG */
    rtl_write32(RTL_TCR, (6u << 8) | (3u << 24));

    /* Step 9: Enable the receiver and transmitter */
    rtl_write8(RTL_CMD, RTL_CMD_RX_ENABLE | RTL_CMD_TX_ENABLE);

    g_rtl_rx_ptr  = 0;
    g_rtl_tx_slot = 0;
    g_rtl_active  = 1;

    return 1;
}


/* ------------------------------------------------------------
 *  rtl8139_send
 *
 *  Transmits one Ethernet frame using the next available TX slot.
 *
 *  The RTL8139 TX mechanism works differently from e1000:
 *    - Writing the length to the TXSTATUS register (with bit 13
 *      clear) hands the buffer to the NIC.
 *    - The NIC sets the TOK (TX OK) bit when transmission completes.
 *
 *  We poll for completion before advancing the slot, which keeps
 *  the implementation synchronous and avoids slot aliasing.
 * ------------------------------------------------------------ */
static void rtl8139_send(const void *frame_data, u16 frame_length)
{
    int slot = g_rtl_tx_slot;

    /* Wait for this TX slot to be owned by the driver (OWN bit = 1) */
    for (int timeout = 100000; timeout-- > 0; ) {
        u32 status = rtl_read32((u16)(RTL_TXSTATUS0 + slot * 4));
        if (status & RTL_TXS_OWN) {
            break;
        }
        for (volatile int d = 0; d < 10; d++);
    }

    /* Clamp frame length to buffer size */
    if (frame_length > RTL_TX_BUF_SIZE) {
        frame_length = RTL_TX_BUF_SIZE;
    }

    /* Copy the frame into the slot's buffer */
    memcpy(g_rtl_tx_bufs[slot], frame_data, frame_length);

    /* Writing only the length (with bit 13 = OWN clear) hands the buffer to the NIC.
     * The NIC will set TOK when it finishes transmitting. */
    rtl_write32((u16)(RTL_TXSTATUS0 + slot * 4), (u32)frame_length & 0x1FFF);

    /* Poll for TX OK */
    for (int timeout = 100000; timeout-- > 0; ) {
        if (rtl_read32((u16)(RTL_TXSTATUS0 + slot * 4)) & RTL_TXS_TX_OK) {
            break;
        }
        for (volatile int d = 0; d < 10; d++);
    }

    g_rtl_tx_slot = (g_rtl_tx_slot + 1) % RTL_TX_SLOT_COUNT;
}


/* ------------------------------------------------------------
 *  rtl8139_poll
 *
 *  Drains received packets from the linear 64KB RX ring buffer.
 *
 *  The RTL8139 does not use a descriptor ring.  Instead, it
 *  fills a contiguous ring buffer with back-to-back packets.
 *  Each packet is preceded by a 4-byte header:
 *    byte 0: receive status (bit 0 = ROK)
 *    byte 1: reserved
 *    bytes 2-3: packet length (including this 4-byte header)
 *
 *  We track our read position in g_rtl_rx_ptr and update the
 *  CAPR register to tell the NIC how far we have consumed.
 *  Note the CAPR quirk: per the datasheet, we must write
 *  (our_ptr - 0x10) to avoid a hardware off-by-16 issue.
 * ------------------------------------------------------------ */
static void rtl8139_poll(void)
{
    for (;;) {
        /* ISR bit 0 = ROK (Receive OK) — at least one packet waiting */
        u16 isr = rtl_read16(RTL_ISR);
        if (!(isr & 0x01)) {
            break;
        }

        /* Acknowledge the ROK interrupt so we can detect the next one */
        rtl_write16(RTL_ISR, 0x01);

        /* CBR is the hardware's current write offset into the ring buffer */
        u16 hw_write_ptr = rtl_read16(RTL_CBR);

        while (g_rtl_rx_ptr != hw_write_ptr) {
            /* Packet header sits at g_rtl_rx_ptr in the ring */
            u8  *packet_header = g_rtl_rx_buf + g_rtl_rx_ptr;
            u8   rx_status     = packet_header[0];
            u16  packet_length = (u16)(packet_header[2] | ((u16)packet_header[3] << 8));

            /* ROK bit must be set — if not, something went wrong; stop processing */
            if (!(rx_status & 0x01)) {
                break;
            }

            /* Sanity-check the length (minimum Ethernet frame + 4-byte CRC) */
            if (packet_length >= 4 && packet_length <= 1522 + 4) {
                /* Deliver the frame, skipping the 4-byte header and excluding
                 * the 4-byte CRC that the RTL8139 appends */
                net_rx_deliver(packet_header + 4, (u16)(packet_length - 4));
            }

            /* Advance our read pointer past this packet (header + payload),
             * rounded up to the next 4-byte boundary to maintain alignment */
            g_rtl_rx_ptr = (u16)((g_rtl_rx_ptr + packet_length + 4 + 3) & ~3u);

            /* Wrap within the 64KB buffer */
            if (g_rtl_rx_ptr >= 64 * 1024) {
                g_rtl_rx_ptr -= 64 * 1024;
            }

            /* Update CAPR — subtract 0x10 per the RTL8139 datasheet quirk */
            rtl_write16(RTL_CAPR, (u16)(g_rtl_rx_ptr - 0x10));
        }
    }
}


/* ============================================================
 *
 *  PART 3: NIC ABSTRACTION LAYER
 *
 *  The three functions below (nic_init, nic_send, nic_poll)
 *  are the only entry points the rest of the kernel uses.
 *  They detect which NIC is present and dispatch accordingly.
 *
 * ============================================================ */

typedef enum {
    NIC_NONE,
    NIC_E1000,
    NIC_RTL8139,
} NicType;

static NicType g_active_nic = NIC_NONE;


/* ------------------------------------------------------------
 *  nic_init
 *
 *  Scans PCI for a supported NIC.  Tries the Intel e1000 first,
 *  then falls back to the Realtek RTL8139.
 *
 *  On success, sets nic_ready = 1 and prints the MAC address.
 * ------------------------------------------------------------ */
void nic_init(void)
{
    g_active_nic = NIC_NONE;
    nic_ready    = 0;

    /* --- Try Intel e1000 first --- */
    void *pci_device = NULL;
    u64   bar_addr   = 0;

    if ((pci_device = pci_find_device(E1000_VENDOR_ID, E1000_DEVICE_82540EM)) ||
        (pci_device = pci_find_device(E1000_VENDOR_ID, E1000_DEVICE_82545EM)) ||
        (pci_device = pci_find_device(E1000_VENDOR_ID, E1000_DEVICE_82574L))) {

        pci_enable_device(pci_device);
        pci_enable_bus_master(pci_device);     /* required for DMA */
        bar_addr = pci_bar_base(pci_device, 0);

        if (bar_addr != 0 && e1000_init(bar_addr)) {
            g_active_nic = NIC_E1000;
            nic_ready    = 1;

            print_str("[e1000] MAC=");
            for (int i = 0; i < 6; i++) {
                print_hex_byte(nic_mac[i]);
                if (i < 5) { print_str(":"); }
            }
            print_str("\r\n");
            return;
        }
    }

    /* --- Fall back to Realtek RTL8139 --- */
    if ((pci_device = pci_find_device(RTL_VENDOR_ID, RTL_DEVICE_8139))) {
        pci_enable_device(pci_device);
        pci_enable_bus_master(pci_device);
        u16 io_port = pci_bar_io(pci_device, 0);

        if (io_port != 0 && rtl8139_init(io_port)) {
            g_active_nic = NIC_RTL8139;
            nic_ready    = 1;

            print_str("[rtl8139] MAC=");
            for (int i = 0; i < 6; i++) {
                print_hex_byte(nic_mac[i]);
                if (i < 5) { print_str(":"); }
            }
            print_str("\r\n");
            return;
        }
    }

    print_str("[nic] no supported NIC found\r\n");
}


/* ------------------------------------------------------------
 *  nic_send — transmit one raw Ethernet frame
 *
 *  `len` must not exceed 1514 bytes (max Ethernet payload +
 *  14-byte header, without the FCS which hardware appends).
 * ------------------------------------------------------------ */
void nic_send(const u8 *frame_data, usize frame_length)
{
    if (!nic_ready || frame_length > 1514) {
        return;
    }

    switch (g_active_nic) {
    case NIC_E1000:
        e1000_send(frame_data, (u16)frame_length);
        break;
    case NIC_RTL8139:
        rtl8139_send(frame_data, (u16)frame_length);
        break;
    default:
        break;
    }
}


/* ------------------------------------------------------------
 *  nic_poll — drain the receive ring
 *
 *  Called from the main kernel loop.  Delivers each received
 *  frame to the network stack via net_rx_deliver(buf, len).
 * ------------------------------------------------------------ */
void nic_poll(void)
{
    if (!nic_ready) {
        return;
    }

    switch (g_active_nic) {
    case NIC_E1000:
        e1000_poll();
        break;
    case NIC_RTL8139:
        rtl8139_poll();
        break;
    default:
        break;
    }
}
