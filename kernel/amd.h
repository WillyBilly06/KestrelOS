/* amd.h - what an AMD graphics card looks like from the outside.
 *
 * AMD's cards are put together very differently from NVIDIA's, and the
 * differences are not cosmetic - they change what a driver has to do first.
 *
 *   NVIDIA presents one flat sixteen-megabyte window whose blocks have sat at
 *   the same offsets since 1998.  A driver can hardcode them and be right on a
 *   card built twenty-five years later.
 *
 *   AMD does not.  The register bar is small - 256 or 512 kilobytes - and the
 *   blocks inside it move between generations.  Anything that does not fit is
 *   reached through an index/data pair.  And from Navi onward the card does
 *   not have a fixed layout at all: it carries a table in its own memory
 *   saying which blocks it has, which version each one is, and where each one
 *   sits.  A driver reads that table and then knows where everything is.
 *
 * That last part is the interesting one.  It is why a driver written before a
 * card existed can still find its way around it: the card describes itself.
 * Both halves are here - the fixed offsets that have not moved since Southern
 * Islands, and the discovery table that replaced them.
 *
 * Every number below came from AMD's own headers, named where it matters.
 */
#ifndef KESTREL_AMD_H
#define KESTREL_AMD_H

#include "kernel.h"

/* ------------------------------------------------------- reaching registers
 *
 * The register bar is far smaller than the register space.  Anything past the
 * end of the bar is reached by writing its byte offset to an index register
 * and then reading or writing a data register - two bus transactions instead
 * of one, which is why a driver uses it only when it has to.
 *
 * Offsets here are bytes.  AMD's headers give them as dword indices, so each
 * is four times the number in the header; the header's number is in the
 * comment so the two can be checked against each other.
 */
#define AMD_MM_INDEX          0x0000   /* mmMM_INDEX       dword 0x00        */
#define AMD_MM_DATA           0x0004   /* mmMM_DATA        dword 0x01        */
#define AMD_MM_INDEX_HI       0x0018   /* mmMM_INDEX_HI    dword 0x06        */
#define AMD_ROM_BASE_ADDR     0x0030   /* mmROM_BASE_ADDR  dword 0x0c        */
#define AMD_PCIE_INDEX        0x0038   /* mmPCIE_INDEX     dword 0x0e        */
#define AMD_PCIE_DATA         0x003C   /* mmPCIE_DATA      dword 0x0f        */

/* Setting this bit in MM_INDEX says the offset is a VRAM address rather than a
 * register, which is how the discovery table is read out of memory the driver
 * has no aperture onto yet. */
#define AMD_MM_INDEX_VRAM     0x80000000u

#define AMD_CONFIG_MEMSIZE    0x5428   /* mmCONFIG_MEMSIZE dword 0x150a      */
#define AMD_CONFIG_CNTL       0x5424   /* mmCONFIG_CNTL    dword 0x1509      */
#define AMD_BIOS_SCRATCH(n)   (0x1724 + (n) * 4)  /* mmBIOS_SCRATCH_0 0x5c9  */

/* --------------------------------------------------------- the co-processors
 *
 * Two of them matter.  MP0 is the security processor: it holds the root of
 * trust and every piece of firmware on the card is loaded through it.  MP1 is
 * the power management processor: it owns the clocks, the fans and the
 * voltages, and nothing else on the card is allowed to touch them.
 *
 * Both are spoken to the same way, through a bank of mailbox registers named
 * C2PMSG - "core to processor message".  The numbers below are dword offsets
 * within each block; where the block itself sits is what the discovery table
 * says, which is why they are relative.
 */
#define AMD_MP0_C2PMSG(n)     (0x0040 + (n))   /* mmMP0_SMN_C2PMSG_64 = 0x80 */
#define AMD_MP1_C2PMSG(n)     (0x0240 + (n))   /* mmMP1_SMN_C2PMSG_66 = 0x282*/

/* The security processor's bootloader answers on these. */
#define AMD_PSP_BL_CMD        AMD_MP0_C2PMSG(35)
#define AMD_PSP_BL_ARG        AMD_MP0_C2PMSG(36)
#define AMD_PSP_SOS_ALIVE     AMD_MP0_C2PMSG(81)
#define AMD_PSP_RING_LO       AMD_MP0_C2PMSG(69)
#define AMD_PSP_RING_HI       AMD_MP0_C2PMSG(70)
#define AMD_PSP_RING_SIZE     AMD_MP0_C2PMSG(71)
#define AMD_PSP_RING_WPTR     AMD_MP0_C2PMSG(67)
#define AMD_PSP_RING_CMD      AMD_MP0_C2PMSG(64)

/* A bootloader command is a bit, not a number, and bit 31 is what says the
 * bootloader has finished with the last one. */
#define AMD_PSP_BL_READY      0x80000000u
#define AMD_PSP_BL_LOAD_KDB   0x00000080u
#define AMD_PSP_BL_LOAD_SYSDRV 0x00010000u
#define AMD_PSP_BL_LOAD_SOSDRV 0x00020000u
#define AMD_PSP_RESP_FLAG     0x80000000u

/* The power processor's mailbox: message in one register, arguments in
 * another, and the answer in a third. */
#define AMD_SMU_MSG           AMD_MP1_C2PMSG(66)
#define AMD_SMU_ARG           AMD_MP1_C2PMSG(82)
#define AMD_SMU_RESP          AMD_MP1_C2PMSG(90)

/* What it answers with. */
#define AMD_SMU_RESP_NONE     0x00
#define AMD_SMU_RESP_OK       0x01
#define AMD_SMU_RESP_BUSY     0xFC
#define AMD_SMU_RESP_BAD_PREREQ 0xFD
#define AMD_SMU_RESP_UNKNOWN  0xFE
#define AMD_SMU_RESP_FAIL     0xFF

/* A few of the messages themselves.  The numbering is per-generation - it is
 * carried in the firmware's own header on a real card - so these are the ones
 * this driver asks for, and it says which generation they are from. */
#define AMD_SMU_MSG_TEST              0x01
#define AMD_SMU_MSG_GET_SMU_VERSION   0x02
#define AMD_SMU_MSG_GET_DRIVER_IF_VERSION 0x03
#define AMD_SMU_MSG_GET_METRICS_TABLE 0x06
#define AMD_SMU_MSG_SET_GFXCLK        0x0F

/* ------------------------------------------------------- the discovery table
 *
 * From Navi onward a card carries a description of itself in the last
 * sixty-four kilobytes of its own memory: which blocks it has, which version
 * of each, and the base address of each.  This is the whole reason a driver
 * can be written once and still work on silicon that did not exist when it was
 * written.
 *
 * Layout from AMD's discovery.h.
 */
#define AMD_DISCOVERY_TMR_OFFSET  (64 << 10)   /* back from the top of VRAM  */
#define AMD_BINARY_SIGNATURE      0x28211407u
#define AMD_DISCOVERY_SIGNATURE   0x53445049u  /* "IPDS"                     */

/* The hardware identifiers this driver looks for, from soc15_hw_ip.h. */
#define AMD_HWID_MP1      1
#define AMD_HWID_THM      3
#define AMD_HWID_SMUIO    4
#define AMD_HWID_GC      11
#define AMD_HWID_VCN     12
#define AMD_HWID_DCI     15
#define AMD_HWID_MMHUB   34
#define AMD_HWID_ATHUB   35
#define AMD_HWID_OSSSYS  40
#define AMD_HWID_HDP     41
#define AMD_HWID_SDMA0   42
#define AMD_HWID_NBIF   108
#define AMD_HWID_UMC    150
#define AMD_HWID_MP0    255

#define AMD_MAX_IP 48

typedef struct {
    u16 hw_id;
    u8  instance;
    u8  major, minor, revision;
    u8  bases;
    u32 base[4];        /* dword offsets, as the table gives them */
} amd_ip_t;

/* ------------------------------------------------------------- the ATOM BIOS
 *
 * AMD's video BIOS is not a table of numbers - it is a bytecode interpreter's
 * program, with data tables alongside it.  A driver needs the data tables:
 * they say what the card is called, what memory is fitted, what connectors
 * exist and which pins carry each one's two wires.
 *
 * Constants from AMD's atom.h.
 */
#define AMD_ATOM_BIOS_MAGIC       0xAA55
#define AMD_ATOM_ATI_MAGIC_PTR    0x30
#define AMD_ATOM_ATI_MAGIC        " 761295520"
#define AMD_ATOM_ROM_TABLE_PTR    0x48
#define AMD_ATOM_ROM_MAGIC        "ATOM"
#define AMD_ATOM_ROM_MAGIC_PTR    4
#define AMD_ATOM_ROM_MSG_PTR      0x10
#define AMD_ATOM_ROM_CMD_PTR      0x1E
#define AMD_ATOM_ROM_DATA_PTR     0x20

/* Which entry of the master data table is which.  These are positions in a
 * struct, so they are fixed for all time: adding a table means appending. */
#define AMD_ATOM_TABLE_FIRMWARE_INFO   4
#define AMD_ATOM_TABLE_GPIO_I2C_INFO  10
#define AMD_ATOM_TABLE_OBJECT_HEADER  22
#define AMD_ATOM_TABLE_VRAM_INFO      28

/* How an object identifier is put together, from ObjectID.h. */
#define AMD_OBJECT_ID_MASK        0x00FF
#define AMD_ENUM_ID_MASK          0x0700
#define AMD_OBJECT_TYPE_MASK      0x7000
#define AMD_OBJECT_ID_SHIFT       0
#define AMD_ENUM_ID_SHIFT         8
#define AMD_OBJECT_TYPE_SHIFT     12

#define AMD_OBJECT_TYPE_GPU        0x1
#define AMD_OBJECT_TYPE_ENCODER    0x2
#define AMD_OBJECT_TYPE_CONNECTOR  0x3
#define AMD_OBJECT_TYPE_ROUTER     0x4

/* The connectors themselves. */
#define AMD_CONNECTOR_NONE              0x00
#define AMD_CONNECTOR_SINGLE_LINK_DVI_I 0x01
#define AMD_CONNECTOR_DUAL_LINK_DVI_I   0x02
#define AMD_CONNECTOR_SINGLE_LINK_DVI_D 0x03
#define AMD_CONNECTOR_DUAL_LINK_DVI_D   0x04
#define AMD_CONNECTOR_VGA               0x05
#define AMD_CONNECTOR_HDMI_TYPE_A       0x0C
#define AMD_CONNECTOR_HDMI_TYPE_B       0x0D
#define AMD_CONNECTOR_LVDS              0x0E
#define AMD_CONNECTOR_DISPLAYPORT       0x13
#define AMD_CONNECTOR_eDP               0x14
#define AMD_CONNECTOR_USBC              0x17

/* Records hanging off an object.  The one that matters says which two wires
 * this connector's monitor answers on. */
#define AMD_ATOM_I2C_RECORD_TYPE       1
#define AMD_ATOM_HPD_INT_RECORD_TYPE   2
#define AMD_ATOM_RECORD_END_TYPE    0xFF

/* One entry of GPIO_I2C_Info: eight register indices and eight bit positions,
 * which is everything needed to drive the two wires by hand.  27 bytes, packed,
 * exactly as ATOM_GPIO_I2C_ASSIGMENT lays it out. */
typedef struct {
    u16 clk_mask_reg, clk_en_reg, clk_y_reg, clk_a_reg;
    u16 data_mask_reg, data_en_reg, data_y_reg, data_a_reg;
    u8  i2c_id;
    u8  clk_mask_shift, clk_en_shift, clk_y_shift, clk_a_shift;
    u8  data_mask_shift, data_en_shift, data_y_shift, data_a_shift;
    u8  reserved1, reserved2;
} __attribute__((packed)) amd_i2c_assignment_t;

#define AMD_MAX_I2C_BUSES 16
#define AMD_MAX_CONNECTORS 12

typedef struct {
    u8   connector_id;       /* one of AMD_CONNECTOR_*                      */
    u8   enum_id;            /* which of that kind, when there are several  */
    u16  object_id;          /* the whole identifier, as the table gave it  */
    u16  device_tag;
    const char *name;
    bool has_i2c;
    u8   i2c_line;           /* which GPIO_I2C_Info entry drives it         */
    /* Filled in if a monitor answered. */
    bool monitor_present;
    bool edid_valid;
    u8   edid[128];
    char monitor_name[16];
    u16  width, height, refresh_hz;
} amd_connector_t;

/* ---------------------------------------------------------------- the card */

typedef struct {
    volatile u8 *regs;
    size_t       regs_size;
    u64          vram_base;
    size_t       vram_aperture;

    u16          pci_device;
    u8           pci_revision;
    const char  *codename;
    const char  *architecture;
    const char  *marketing;
    int          gfx_major, gfx_minor;    /* the graphics block's version   */

    u64          vram_bytes;
    bool         vram_exact;

    /* What the discovery table said. */
    amd_ip_t     ip[AMD_MAX_IP];
    int          ips;
    bool         discovered;
    u32          gc_base, mp0_base, mp1_base, dc_base, sdma_base;

    /* What the ATOM BIOS said. */
    u8          *vbios;
    size_t       vbios_size;
    bool         vbios_valid;
    char         vbios_version[64];
    const char  *vbios_source;
    u16          atom_data_table;
    u16          atom_cmd_table;

    amd_i2c_assignment_t i2c[AMD_MAX_I2C_BUSES];
    int          i2c_buses;

    amd_connector_t connector[AMD_MAX_CONNECTORS];
    int          connectors;

    /* What the power processor said. */
    bool         smu_ready;
    u32          smu_version;
    int          temperature_c;
    u32          gfx_clock_mhz, mem_clock_mhz;

    bool         modelled;
} amd_card_t;

/* --------------------------------------------------------------- the driver */

u32  amd_rd32(amd_card_t *c, u32 offset);
void amd_wr32(amd_card_t *c, u32 offset, u32 value);
void amd_allow_writes(bool yes);
bool amd_writes_ok(void);
u32  amd_writes_dropped(void);
/* Through the index/data pair, for anything past the end of the bar. */
u32  amd_rd32_indirect(amd_card_t *c, u32 offset);
void amd_wr32_indirect(amd_card_t *c, u32 offset, u32 value);
/* Reading the card's own memory without an aperture onto it. */
void amd_read_vram(amd_card_t *c, u64 at, void *into, size_t len);

bool amd_identify(amd_card_t *c, u16 device, u8 revision);
bool amd_read_discovery(amd_card_t *c);
const amd_ip_t *amd_find_ip(amd_card_t *c, u16 hw_id, u8 instance);
bool amd_read_vram_size(amd_card_t *c);

bool amd_read_vbios(amd_card_t *c);
bool amd_parse_vbios(amd_card_t *c);
bool amd_atom_data_table(amd_card_t *c, int index, u16 *offset, u16 *size);
int  amd_parse_connectors(amd_card_t *c);
bool amd_i2c_read_edid(amd_card_t *c, u8 line, u8 out[128]);
int  amd_probe_monitors(amd_card_t *c);

/* The power processor. */
bool amd_smu_send(amd_card_t *c, u32 message, u32 argument, u32 *reply);
bool amd_smu_bring_up(amd_card_t *c);
void amd_read_sensors(amd_card_t *c);

/* The security processor. */
bool amd_psp_wait_bootloader(amd_card_t *c, int timeout_ms);
bool amd_psp_bootloader_load(amd_card_t *c, u32 command, u64 firmware_at,
                             int timeout_ms);
bool amd_psp_ring_create(amd_card_t *c, u64 ring_at, u32 ring_bytes);

/* ------------------------------------------------------------ the PM4 ring
 *
 * Drawing on an AMD card means writing packets into a ring in memory and
 * telling the card the ring got longer.  The packet encoding has not changed
 * since Southern Islands: a type-3 packet is an opcode and a word count, and
 * everything a driver submits is built out of a handful of them.
 */
#define AMD_PACKET_TYPE0(reg, n)  (((n) & 0x3FFF) << 16 | ((reg) >> 2))
#define AMD_PACKET_TYPE2          0x80000000u
#define AMD_PACKET_TYPE3(op, n)   ((3u << 30) | (((op) & 0xFF) << 8) | \
                                   (((n) & 0x3FFF) << 16))

#define AMD_PM4_NOP               0x10
#define AMD_PM4_SET_BASE          0x11
#define AMD_PM4_INDIRECT_BUFFER   0x3F
#define AMD_PM4_WAIT_REG_MEM      0x3C
#define AMD_PM4_WRITE_DATA        0x37
#define AMD_PM4_EVENT_WRITE       0x46
#define AMD_PM4_RELEASE_MEM       0x49
#define AMD_PM4_SET_UCONFIG_REG   0x79
#define AMD_PM4_SET_CONTEXT_REG   0x69
#define AMD_PM4_DMA_DATA         0x50

typedef struct {
    u32 *base;             /* the ring itself, in memory both sides see     */
    u64  gpu_address;
    u32  words;            /* how long it is, a power of two                */
    u32  write;            /* where the driver is up to, in words           */
    u32  doorbell;         /* which doorbell tells the card                 */
    volatile u32 *rptr_report;  /* where the card writes how far it has got */
    volatile u32 *wptr_report;
    volatile u64 *fence;   /* where a completion lands                      */
    u64  fence_next;
    bool ready;
    u32  emitted;          /* words written this packet, for the check      */
    u32  expected;
} amd_ring_t;

bool amd_ring_init(amd_ring_t *r, u32 *memory, u64 gpu_address, u32 words,
                   volatile u32 *rptr_report, volatile u64 *fence);
void amd_ring_begin(amd_ring_t *r, u32 words);
void amd_ring_write(amd_ring_t *r, u32 value);
bool amd_ring_commit(amd_card_t *c, amd_ring_t *r);
u32  amd_ring_free(amd_ring_t *r);
/* The two a driver actually builds out of the above. */
bool amd_ring_emit_fence(amd_card_t *c, amd_ring_t *r, u64 at, u64 value);
bool amd_ring_emit_indirect(amd_card_t *c, amd_ring_t *r, u64 at, u32 words);
bool amd_ring_wait_fence(amd_ring_t *r, u64 value, int timeout_ms);

/* ------------------------------------------------------------------ testing
 *
 * No AMD card is present in anything this runs on, so the driver above is run
 * against a model of one built to the same register map and the same table
 * layouts.  What that establishes and what it cannot is set out in
 * amd_model.c and repeated wherever the result is reported.
 */
volatile u8 *amd_model_attach(size_t *size_out);
bool         amd_model_present(void);
void         amd_model_sync(void);
void         amd_model_wrote(u32 offset, u32 value);
void         amd_model_read(u32 offset);
amd_card_t  *amd_model_card(void);

u16          amd_model_device(void);
u64          amd_model_vram(void);
int          amd_model_temperature(void);
const u8    *amd_model_edid(void);
u8           amd_model_monitor_line(void);
u32          amd_model_smu_messages(void);
u32          amd_model_psp_loads(void);
u32          amd_model_ring_packets(void);
u32          amd_model_ring_words(void);
u32          amd_model_last_opcode(void);

/* The card's own memory, and the parts the model deliberately refuses. */
void amd_model_ring_attach(u32 *memory, u32 words, volatile u32 *rptr,
                           volatile u64 *fence, u32 doorbell);
void amd_model_ring_detach(void);
void amd_model_sos_already_running(bool running);
int  amd_model_ring_errors(void);
int  amd_model_smu_out_of_order(void);
int  amd_model_psp_rejections(void);
void amd_model_i2c_report(int line, int *starts, int *acks, int *bytes);

/* The one entry point that runs on a real card at boot. */
void amd_driver_init(void);
/* The boot sequence itself, exposed so it can be driven against the model. */
void amd_bring_up(amd_card_t *c);

bool amd_attach_model(void);
int  amd_drive_test(void);

#endif
