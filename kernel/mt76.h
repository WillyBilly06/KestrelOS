/* mt76.h - the shapes and registers a MediaTek Wi-Fi chip and its driver agree
 * on.  Shared with the model of the device the self-test drives the driver
 * against.
 */
#ifndef KESTREL_MT76_H
#define KESTREL_MT76_H

#include "kernel.h"

/* --------------------------------------------------------------- the rings
 *
 * MediaTek moves everything - firmware, commands, frames - over descriptor
 * rings in host memory, one pair for the microcontroller and one for data.
 * A descriptor is four words: two buffer addresses, a control word holding
 * the lengths and the done bit, and a word of information the chip fills in.
 */
typedef struct {
    u32 buf0;
    u32 ctrl;
    u32 buf1;
    u32 info;
} __attribute__((packed)) mt76_desc_t;

#define MT76_RING_SIZE   64
#define MT76_BUFFER_SIZE 2048

#define MT76_CTRL_LEN0_SHIFT  0
#define MT76_CTRL_LAST0       (1u << 15)
#define MT76_CTRL_LEN1_SHIFT  16
#define MT76_CTRL_LAST1       (1u << 31)
#define MT76_CTRL_DMA_DONE    (1u << 31)

/* The registers.  These are the offsets documented for the 7921 family; a
 * different generation moves them, which is one of the things this driver
 * cannot check without a card. */
#define MT_WFDMA_BASE       0xd4000
#define MT_WFDMA_GLO_CFG    (MT_WFDMA_BASE + 0x208)
#define MT_WFDMA_RST_DTX    (MT_WFDMA_BASE + 0x20c)

#define MT_TX_RING_BASE     (MT_WFDMA_BASE + 0x300)
#define MT_RX_RING_BASE     (MT_WFDMA_BASE + 0x500)
#define MT_RING_STRIDE      0x10
#define MT_RING_BASE_OFF    0x00
#define MT_RING_COUNT_OFF   0x04
#define MT_RING_CPU_IDX_OFF 0x08
#define MT_RING_DMA_IDX_OFF 0x0c

#define MT_TX_RING_MCU      0     /* commands and firmware go down this one  */
#define MT_TX_RING_DATA     1
#define MT_RX_RING_EVENT    0     /* the microcontroller answers on this one */
#define MT_RX_RING_DATA     1

#define MT_GLO_CFG_TX_DMA_EN (1u << 0)
#define MT_GLO_CFG_RX_DMA_EN (1u << 2)

/* Where the microcontroller reports what state it is in. */
#define MT_TOP_MISC2        0x1123c
#define MT_TOP_MISC2_FW_STATE_MASK  0x00000007
#define MT_FW_STATE_FW_DOWNLOAD  1
#define MT_FW_STATE_NORMAL_TRX   7

/* ------------------------------------------------------------- the messages
 *
 * Every message to the microcontroller carries a fixed header saying how long
 * it is, which command it is and which sequence number pairs it with its
 * answer.  MediaTek's real header is longer and mostly zero; the fields that
 * matter are these.
 */
typedef struct {
    u16 length;
    u16 pq_id;
    u8  cid;              /* the command */
    u8  pkt_type;
    u8  set_query;
    u8  seq;
    u8  uc_d2b0_rev;
    u8  ext_cid;
    u8  s2d_index;
    u8  ext_cid_ack;
    u32 reserved[5];
} __attribute__((packed)) mt76_mcu_txd_t;

typedef struct {
    u16 length;
    u16 pq_id;
    u8  cid;
    u8  pkt_type;
    u8  eid;
    u8  seq;
    u8  reserved;
    u8  ext_eid;
    u16 reserved2;
    u32 reserved3;
} __attribute__((packed)) mt76_mcu_rxd_t;

#define MT76_PKT_TYPE_CMD 2

/* The commands the download and the driver use. */
#define MCU_CMD_TARGET_ADDRESS_LEN_REQ 0x01
#define MCU_CMD_FW_START_REQ           0x02
#define MCU_CMD_INIT_ACCESS_REG        0x03
#define MCU_CMD_PATCH_START_REQ        0x05
#define MCU_CMD_PATCH_FINISH_REQ       0x07
#define MCU_CMD_PATCH_SEM_CONTROL      0x10
#define MCU_CMD_FW_SCATTER             0xEE

#define MCU_EXT_CMD_CHANNEL_SWITCH     0x08
#define MCU_EXT_CMD_STA_REC_UPDATE     0x25

#define MCU_EVENT_GENERIC              0x01
#define MCU_EVENT_RX_FRAME             0x30

#define PATCH_SEM_GET     1
#define PATCH_SEM_RELEASE 0
#define PATCH_IS_DL       2      /* somebody already loaded it */
#define PATCH_NOT_DL_SEM_SUCCESS 1

/* ------------------------------------------------------------- the firmware
 *
 * There are two files and they are not the same shape.  The patch is a header
 * with big-endian fields followed by a table of sections; the main firmware
 * puts its table at the END of the file, in a trailer, which is why it has to
 * be read backwards.
 */
#define MT76_PATCH_HEADER_SIZE 40
#define MT76_MAX_SECTIONS 8

typedef struct {
    u32 address;
    u32 length;
    u32 mode;
    const u8 *data;
} mt76_section_t;

typedef struct {
    char build_date[16];
    char platform[5];
    u32  hw_sw_version;
    u32  patch_version;
    int  sections;
    mt76_section_t section[MT76_MAX_SECTIONS];
} mt76_patch_t;

typedef struct {
    u8   chip_id;
    u8   eco_code;
    u8   regions;
    u8   format_version;
    char version[11];
    char build_date[16];
    int  sections;
    mt76_section_t section[MT76_MAX_SECTIONS];
} mt76_firmware_t;

bool   mt76_parse_patch(const u8 *data, size_t size, mt76_patch_t *out);
bool   mt76_parse_firmware(const u8 *data, size_t size, mt76_firmware_t *out);
size_t mt76_build_test_patch(u8 *out, size_t cap);
size_t mt76_build_test_firmware(u8 *out, size_t cap);

void   mt76_init(void);
bool   mt76_attach_model(void);
struct wifi_device *mt76_model_device(void);
int    mt76_drive_test(void);

#endif
