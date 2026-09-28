/* r8169.h - the registers a Realtek gigabit or 2.5-gigabit card answers to.
 *
 * Shared with the model in r8169_model.c, so that the driver and the thing it
 * is tested against agree on the hardware by construction rather than by two
 * people reading the same document twice.
 *
 * These are the RTL8168/8111 family and the RTL8125/8126 that followed it.
 * They are the ordinary Ethernet on a desktop motherboard: the 8111 has been
 * on boards for fifteen years, and the 8125 is what a board with a 2.5-gigabit
 * socket almost always has behind it.
 *
 * The 8125 is not a different design so much as the same one widened.  Its
 * descriptor rings, its command register and its reset are where they were;
 * what moved is the interrupt mask and status, which grew from sixteen bits to
 * thirty-two and moved to make room.  A driver that assumes the older places
 * on a newer card writes into the wrong registers and sees nothing.
 */
#ifndef KESTREL_R8169_H
#define KESTREL_R8169_H

#include "kernel.h"

/* ------------------------------------------------------------- registers */

#define R8169_MAC0          0x00    /* six bytes of station address        */
#define R8169_MAR0          0x08    /* multicast filter                    */
#define R8169_TNPDS         0x20    /* transmit descriptor ring, physical  */
#define R8169_CMD           0x37
#define R8169_TPPOLL        0x38
#define R8169_IMR           0x3C    /* 8168: interrupt mask                */
#define R8169_ISR           0x3E    /* 8168: interrupt status              */
#define R8169_TCR           0x40    /* transmit configuration              */
#define R8169_RCR           0x44    /* receive configuration               */
#define R8169_9346CR        0x50    /* unlocks the configuration registers */
#define R8169_CONFIG1       0x52
#define R8169_PHYSTATUS     0x6C
#define R8169_RMS           0xDA    /* the largest frame it will receive   */
#define R8169_CPCR          0xE0    /* "C+" command                        */
#define R8169_RDSAR         0xE4    /* receive descriptor ring, physical   */
#define R8169_MTPS          0xEC    /* the largest it will transmit        */

/* The 8125 moved these, and widened them to thirty-two bits. */
#define R8125_IMR           0x38
#define R8125_ISR           0x3C

/* And it moved the transmit doorbell out to 0x90 - the old 0x38 is the
 * interrupt mask on this part, so a driver that rings the doorbell at 0x38 on
 * an 8125 pokes the interrupt mask and the card is never told to send.  Rung
 * with bit 0 through a sixteen-bit write, where the gigabit parts use bit 6 of
 * the eight-bit register.  Verified against the Linux r8169 driver
 * (rtl8169_doorbell: RTL_W16(TxPoll_8125, BIT(0)) vs RTL_W8(TxPoll, NPQ)). */
#define R8125_TPPOLL        0x90
#define TPPOLL_8125_KICK    (1u << 0)

/* CMD */
#define CMD_TX_ENABLE       (1u << 2)
#define CMD_RX_ENABLE       (1u << 3)
#define CMD_RESET           (1u << 4)

/* TPPOLL: tell the card a transmit descriptor is ready. */
#define TPPOLL_NPQ          (1u << 6)

/* 9346CR: the configuration registers are locked until this says otherwise. */
#define LOCK_CONFIG_UNLOCK  0xC0
#define LOCK_CONFIG_LOCK    0x00

/* RCR */
#define RCR_ACCEPT_ALL_PHYS (1u << 0)   /* every address, not just ours    */
#define RCR_ACCEPT_OURS     (1u << 1)
#define RCR_ACCEPT_MULTI    (1u << 2)
#define RCR_ACCEPT_BROADCAST (1u << 3)

/* CPCR */
#define CPCR_RX_CHECKSUM    (1u << 5)
#define CPCR_RX_VLAN        (1u << 6)

/* PHYSTATUS */
#define PHY_LINK_OK         (1u << 1)
#define PHY_100M            (1u << 3)
#define PHY_1000M           (1u << 4)
#define PHY_2500M           (1u << 10)

/* --------------------------------------------------------- descriptors
 *
 * One shape for both directions, and the ring wraps because the last entry
 * says so rather than because the driver counted - which is why the end-of-
 * ring bit has to be set on exactly one descriptor and never on any other.
 */
typedef struct __attribute__((packed)) {
    u32 flags;          /* ownership, length, and the ring's end            */
    u32 vlan;
    u64 address;        /* where the frame is, in memory the card can reach */
} r8169_desc_t;

#define DESC_OWN            (1u << 31)  /* the card's, not ours             */
#define DESC_EOR            (1u << 30)  /* the last descriptor in the ring  */
#define DESC_FS             (1u << 29)  /* first fragment of a frame        */
#define DESC_LS             (1u << 28)  /* last fragment                    */
#define DESC_LEN_MASK       0x3FFF

/* Receive errors, all of which mean the frame is not worth passing up. */
#define DESC_RX_RES         (1u << 21)

/* ------------------------------------------------------------- the driver */

struct pci_dev;
void r8169_init(void);
int  r8169_selftest(void);

/* The model, which the self-test drives instead of a card. */
bool r8169_model_attach(volatile u8 **regs, size_t *size);
void r8169_model_detach(void);
void r8169_model_receive(const void *frame, int len);
int  r8169_model_transmitted(const void **frame, int *len);
bool r8169_model_is_2500(void);
void r8169_model_set_2500(bool yes);

#endif /* KESTREL_R8169_H */
