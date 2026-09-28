/* ath9k.h - the Atheros AR9xxx register and descriptor interface.
 *
 * Split into its own header because two things need it: the driver, and the
 * hardware model that the driver is tested against.  Both must agree on the
 * register layout and the descriptor format, and having one definition of it
 * is what makes that true by construction rather than by inspection.
 *
 * The offsets are from the AR5416 and AR9280 programming documentation, which
 * Atheros published.  Where this file is confident it says so plainly; where a
 * value could not be established it is marked, because a plausible wrong
 * constant is worse than an absent one.
 */
#ifndef KESTREL_ATH9K_H
#define KESTREL_ATH9K_H

#include "kernel.h"

/* ---------------------------------------------------------- MAC registers */

#define AR_CR              0x0008   /* command                          */
#define AR_CR_RXE          (1u << 2)
#define AR_CR_RXD          (1u << 5)

#define AR_RXDP            0x000C   /* receive descriptor pointer       */
#define AR_CFG             0x0014
#define AR_CFG_SWTD        (1u << 0)   /* byte-swap descriptors         */

#define AR_IER             0x0024   /* interrupt enable, global         */
#define AR_IER_ENABLE      (1u << 0)

#define AR_TXCFG           0x0030
#define AR_RXCFG           0x0034

#define AR_ISR             0x0080   /* interrupt status                 */
#define AR_ISR_RXOK        (1u << 1)
#define AR_ISR_TXOK        (1u << 6)

#define AR_IMR             0x00A0   /* interrupt mask                   */

/* Ten transmit queues, each with its own descriptor pointer.  Queue zero is
 * used here for everything: prioritising traffic is a refinement, and one
 * queue is correct without it. */
#define AR_QTXDP(q)        (0x0800 + (q) * 4)
#define AR_Q_TXE           0x0840   /* start a queue, one bit each      */
#define AR_Q_TXD           0x0880   /* stop a queue                     */
#define AR_QSTS(q)         (0x0A00 + (q) * 4)
#define AR_QSTS_PENDING    0x0000FFFF
#define AR_Q_MISC(q)       (0x09C0 + (q) * 4)
#define AR_Q_MISC_FSP_ASAP 0x00000001

#define AR_RC              0x4000   /* reset control                    */
#define AR_RC_AHB          (1u << 0)
#define AR_RC_MAC_WARM     (1u << 1)
#define AR_RC_MAC_COLD     (1u << 2)
#define AR_RC_HOSTIF       (1u << 8)

#define AR_SREV            0x4020   /* silicon revision                 */
#define AR_SREV_VERSION_S  4

#define AR_STA_ID0         0x8000   /* our address, low four bytes      */
#define AR_STA_ID1         0x8004   /* our address, high two            */
#define AR_BSS_ID0         0x8008
#define AR_BSS_ID1         0x800C

#define AR_RX_FILTER       0x803C
#define AR_RX_FILTER_UCAST (1u << 0)
#define AR_RX_FILTER_MCAST (1u << 1)
#define AR_RX_FILTER_BCAST (1u << 2)
#define AR_RX_FILTER_BEACON (1u << 4)
#define AR_RX_FILTER_PROM  (1u << 5)
#define AR_RX_FILTER_PROBEREQ (1u << 7)

#define AR_MCAST_FIL0      0x8040
#define AR_MCAST_FIL1      0x8044
#define AR_DIAG_SW         0x8048
#define AR_DIAG_RX_DIS     (1u << 5)

/* The EEPROM holds the address and the per-board calibration.  On this family
 * it is read through an address register and a data register, with a status
 * bit saying when the word has arrived. */
#define AR_EEPROM_ADDR     0x6000
#define AR_EEPROM_DATA     0x6004
#define AR_EEPROM_CMD      0x6008
#define AR_EEPROM_CMD_READ (1u << 0)
#define AR_EEPROM_STATUS   0x600C
#define AR_EEPROM_STATUS_DONE (1u << 0)
#define AR_EEPROM_STATUS_FAIL (1u << 2)

/* Where the address lives in the EEPROM's word space. */
#define AR_EEPROM_MAC_WORD 0x001F

/* The synthesiser.  Programming a channel means writing the frequency in the
 * form the phase-locked loop wants and waiting for it to settle. */
#define AR_PHY_BASE        0x9800
#define AR_PHY_ACTIVE      0x981C
#define AR_PHY_ACTIVE_EN   (1u << 0)
#define AR_PHY_RFBUS_REQ   0x997C
#define AR_PHY_RFBUS_GRANT 0x9C20
#define AR_PHY_SYNTH_CONTROL 0x9874
#define AR_PHY_RX_DELAY    0x9914

/* --------------------------------------------------------------- descriptors
 *
 * Both rings use the same eight-word layout: a link to the next descriptor, a
 * buffer address, and control and status words whose meaning differs between
 * transmit and receive.  The hardware walks the link chain on its own, which is
 * why the last descriptor's link has to point back at the first.
 */

typedef struct {
    u32 link;          /* physical address of the next descriptor        */
    u32 buffer;        /* physical address of the data                   */
    u32 control0;
    u32 control1;
    u32 status0;
    u32 status1;
    u32 reserved[2];
} __attribute__((packed)) ath_desc_t;

/* Transmit control zero: the frame length in the low bits. */
#define ATH_TXC0_LENGTH        0x00000FFF
#define ATH_TXC0_TX_INTERRUPT  (1u << 23)

/* Transmit control one: the buffer length, and whether more follows. */
#define ATH_TXC1_BUF_LEN       0x00000FFF
#define ATH_TXC1_MORE          (1u << 12)

/* Transmit status zero. */
#define ATH_TXS0_DONE          (1u << 0)
#define ATH_TXS0_OK            (1u << 1)
#define ATH_TXS0_EXCESSIVE     (1u << 3)

/* Receive control one: how big the buffer is. */
#define ATH_RXC1_BUF_LEN       0x00000FFF

/* Receive status zero: what came in. */
#define ATH_RXS0_DONE          (1u << 0)
#define ATH_RXS0_OK            (1u << 1)
#define ATH_RXS0_CRC_ERROR     (1u << 2)
#define ATH_RXS0_DECRYPT_ERROR (1u << 3)
#define ATH_RXS0_LENGTH        0x0FFF0000
#define ATH_RXS0_LENGTH_S      16

/* Receive status one: the signal strength, as a value above the noise floor. */
#define ATH_RXS1_RSSI          0x000000FF

#endif
