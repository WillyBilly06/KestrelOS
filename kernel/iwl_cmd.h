/* iwl_cmd.h - the shapes an Intel Wi-Fi device and its microcode agree on.
 *
 * Once the microcode is running, the driver stops talking to the radio and
 * starts talking to the microcode.  Everything goes through two rings in host
 * memory that the device reads and writes by DMA: a queue of descriptors
 * pointing at commands, and a queue of pages the device fills with whatever it
 * wants to say back.
 *
 * The ring mechanics below are the stable part of this interface - they have
 * been the same shape across every generation of the family.  What sits inside
 * a command's payload is not: those structures are versioned by the firmware
 * API and change with each release.  That distinction is worth keeping in
 * mind, and is why this file separates the two.
 */
#ifndef KESTREL_IWL_CMD_H
#define KESTREL_IWL_CMD_H

#include "kernel.h"

/* --------------------------------------------------------------- the rings */

#define IWL_TFD_QUEUE_SIZE  256      /* descriptors per transmit queue */
#define IWL_NUM_OF_TBS      20       /* buffers one descriptor can point at */
#define IWL_RX_QUEUE_SIZE   256
#define IWL_RX_PAGE_SIZE    4096
#define IWL_CMD_QUEUE       9        /* the queue commands go down */
#define IWL_DATA_QUEUE      0
#define IWL_MAX_QUEUES      2        /* only the two this driver uses */
#define IWL_CMD_BUFFER      512      /* enough for every command sent here */

/* One buffer the device should read: a physical address split across a
 * thirty-two bit low half and a sixteen-bit field holding the top four bits of
 * the address and the twelve-bit length. */
typedef struct {
    u32 lo;
    u16 hi_n_len;
} __attribute__((packed)) iwl_tfd_tb_t;

/* A transmit frame descriptor.  The count is a byte on its own so the device
 * can be told how many buffers to gather without reading the rest. */
typedef struct {
    u8           reserved[3];
    u8           num_tbs;
    iwl_tfd_tb_t tbs[IWL_NUM_OF_TBS];
    u32          pad;
} __attribute__((packed)) iwl_tfd_t;      /* 128 bytes exactly */

/* Where the device has got to in the receive ring.  It writes this by DMA
 * rather than making the driver read a register, which is what lets the
 * receive path run without touching the bus. */
typedef struct {
    u16 closed_rb_num;
    u16 closed_fr_num;
    u16 finished_rb_num;
    u16 finished_fr_num;
    u32 unused;
} __attribute__((packed)) iwl_rb_status_t;

/* ------------------------------------------------------------ the messages */

/* Every command and every notification begins with this.  The sequence number
 * is what pairs a response with the command that asked for it. */
typedef struct {
    u8  cmd;
    u8  group_id;
    u16 sequence;
} __attribute__((packed)) iwl_cmd_header_t;

typedef struct {
    u32               len_n_flags;
    iwl_cmd_header_t  hdr;
    u8                data[];
} __attribute__((packed)) iwl_rx_packet_t;

#define IWL_RX_PACKET_LEN(p)     (((p)->len_n_flags & 0x3FFF) - sizeof(iwl_cmd_header_t))
#define IWL_SEQ_TO_INDEX(seq)    ((seq) & 0xFF)
#define IWL_SEQ_TO_QUEUE(seq)    (((seq) >> 8) & 0x1F)
#define IWL_MAKE_SEQ(queue, idx) (u16)(((queue) << 8) | ((idx) & 0xFF))

/* The commands in the legacy group - the ones every generation has. */
#define IWL_CMD_ALIVE               0x01
#define IWL_CMD_INIT_COMPLETE       0x04
#define IWL_CMD_PHY_CONTEXT         0x08
#define IWL_CMD_SCAN_REQ_UMAC       0x0D
#define IWL_CMD_SCAN_COMPLETE_UMAC  0x0F
#define IWL_CMD_ADD_STA             0x18
#define IWL_CMD_TX                  0x1C
#define IWL_CMD_MAC_CONTEXT         0x28
#define IWL_CMD_TIME_EVENT          0x29
#define IWL_CMD_BINDING_CONTEXT     0x2B
#define IWL_CMD_PHY_CONFIGURATION   0x6A
#define IWL_CMD_NVM_ACCESS          0x88
#define IWL_CMD_RX_PHY              0xC0
#define IWL_CMD_RX_MPDU             0xC1
#define IWL_CMD_TX_RESPONSE         0x1C
#define IWL_CMD_ECHO                0xF0    /* answered by returning the payload */

/* What the microcode says when it has started.  Only the fields this driver
 * reads are named; the structure is longer and grows with each API version,
 * which is why nothing here depends on its total size. */
typedef struct {
    u16 status;
    u16 flags;
    u8  ucode_minor, ucode_major;
    u16 id;
    u8  api_minor, api_major;
    u8  ver_subtype, ver_type, mac, opt;
    u16 reserved;
    u32 timestamp;
    u32 error_event_table_ptr;
    u32 log_event_table_ptr;
} __attribute__((packed)) iwl_alive_t;

#define IWL_ALIVE_STATUS_OK 0xCAFE

/* The device's own address, read out of its non-volatile memory. */
typedef struct {
    u16 offset;
    u16 length;
    u16 type;
    u16 status;
    u8  data[];
} __attribute__((packed)) iwl_nvm_access_resp_t;

/* ---------------------------------------------------------------- registers */

#define CSR_HW_IF_CONFIG    0x000
#define CSR_INT_COALESCING  0x004
#define CSR_INT             0x008
#define CSR_INT_MASK        0x00C
#define CSR_FH_INT_STATUS   0x010
#define CSR_RESET           0x020
#define CSR_GP_CNTRL        0x024
#define CSR_HW_REV          0x028
#define CSR_UCODE_DRV_GP1   0x054
#define CSR_UCODE_DRV_GP1_CLR 0x05C
#define CSR_DRAM_INT_TBL    0x0A0
#define CSR_MAC_SHADOW_CTRL 0x0A8

/* Interrupt bits.  The driver polls these rather than taking an interrupt,
 * which costs a little latency and removes a whole class of ordering
 * problem. */
#define CSR_INT_BIT_ALIVE   0x00000001
#define CSR_INT_BIT_WAKEUP  0x00000002
#define CSR_INT_BIT_SW_RX   0x00000008
#define CSR_INT_BIT_CT_KILL 0x00000004
#define CSR_INT_BIT_SW_ERR  0x02000000
#define CSR_INT_BIT_RF_KILL 0x08000000
#define CSR_INT_BIT_HW_ERR  0x20000000
#define CSR_INT_BIT_FH_TX   0x00000200
#define CSR_INT_BIT_FH_RX   0x80000000

/* The flow handler: the part of the device that moves the rings. */
#define FH_RSCSR_CHNL0_STTS_WPTR 0x1BC0   /* where the status block lives  */
#define FH_RSCSR_CHNL0_RBDCB_BASE 0x1BC4  /* where the page list lives     */
#define FH_RSCSR_CHNL0_WPTR      0x1BC8   /* how many pages are available  */
#define FH_MEM_RCSR_CHNL0_CONFIG 0x1C00
#define FH_MEM_CBBC_QUEUE_BASE   0x9D0    /* one word per transmit queue   */
#define FH_TCSR_CHNL_TX_CONFIG   0x1D00   /* 0x20 apart, one per channel   */
#define FH_TSSR_TX_STATUS        0x1EB0

#define FH_RCSR_RX_CONFIG_ENABLE  0x80000000
#define FH_TCSR_TX_CONFIG_ENABLE  0x80000000

/* Reaching the device's own memory and its peripheral registers. */
#define HBUS_TARG_MEM_RADDR 0x40C
#define HBUS_TARG_MEM_WADDR 0x410
#define HBUS_TARG_MEM_WDAT  0x418
#define HBUS_TARG_MEM_RDAT  0x41C
#define HBUS_TARG_PRPH_WADDR 0x444
#define HBUS_TARG_PRPH_RADDR 0x448
#define HBUS_TARG_PRPH_WDAT  0x44C
#define HBUS_TARG_PRPH_RDAT  0x450
#define HBUS_TARG_WRPTR      0x460       /* the doorbell for every queue */

#define CSR_HW_IF_NIC_READY 0x00400000
#define CSR_HW_IF_PREPARE   0x08000000

#define CSR_RESET_SW              0x00000080
#define CSR_RESET_STOP_MASTER     0x00000200
#define CSR_RESET_MASTER_DISABLED 0x00000100

#define CSR_GP_MAC_CLOCK_READY 0x00000001
#define CSR_GP_INIT_DONE       0x00000004
#define CSR_GP_MAC_ACCESS_REQ  0x00000008

#endif
