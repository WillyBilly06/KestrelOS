/* rtw.h - the registers and shapes a Realtek Wi-Fi chip and its driver agree
 * on.  Shared with the model of the device that the self-test drives the
 * driver against.
 */
#ifndef KESTREL_RTW_H
#define KESTREL_RTW_H

#include "kernel.h"

/* ---------------------------------------------------------------- registers */

#define REG_SYS_FUNC_EN     0x0002   /* what parts of the chip are switched on */
#define FEN_CPUEN           0x0004   /* the chip's own processor               */

#define REG_MCUFWDL         0x0080   /* the firmware download control          */
#define MCUFWDL_EN          0x00000001
#define MCUFWDL_RDY         0x00000002
#define FWDL_CHKSUM_RPT     0x00000004
#define MACINI_RDY          0x00000008
#define BBINI_RDY           0x00000010
#define RFINI_RDY           0x00000020
#define WINTINI_RDY         0x00000040   /* the firmware has started */
#define CPRST               0x00800000

#define REG_FW_START_ADDRESS 0x1000  /* the window pages are written into */
#define RTW_FW_PAGE_SIZE     4096

#define REG_MACID           0x0610   /* the chip's own address */

/* The four mailboxes a command goes into, and the register whose bits say
 * which of them are still full. */
#define REG_HMETFR          0x01CC
#define REG_HMEBOX0         0x01D0
#define REG_HMEBOX0_EXT     0x01F0
#define RTW_H2C_BOX_SIZE    8
#define RTW_H2C_PAYLOAD     7        /* the box, less the command byte */

/* Where the firmware answers. */
#define REG_C2HEVT_MSG      0x01A0
#define REG_C2HEVT_CLEAR    0x01AF
/* The buffer has two states and one register says which.  Getting these the
 * right way round matters: the value at power-up is zero, so zero has to mean
 * "nothing waiting" or a driver would read one event that was never sent. */
#define C2H_EVT_EMPTY       0x00     /* nothing waiting; the host writes this
                                      * when it has finished with a message   */
#define C2H_EVT_READY       0xFF     /* the firmware has filled the buffer    */
#define RTW_C2H_MAX         14

/* The window a frame to be sent is written into. */
#define REG_TX_BUFFER       0x8000
#define RTW_TX_WINDOW       2048

/* The key table. */
#define REG_CAM_CMD         0x0670
#define REG_CAM_WRITE       0x0674
#define CAM_CMD_POLLING     0x80000000
#define CAM_CMD_WRITE       0x00010000

/* ---------------------------------------------------------------- commands */

#define H2C_SET_MEDIA_STATUS 0x01
#define H2C_SET_CHANNEL      0x02
#define H2C_SET_KEY          0x03
#define H2C_TRANSMIT         0x04

#define C2H_MSG_TX_REPORT    0x03
#define C2H_MSG_RX_FRAME     0x10

/* ---------------------------------------------------------------- firmware */

#define RTW_FW_HEADER_SIZE 32

/* The signature says which chip the file was built for.  A file for the wrong
 * one loads without complaint and then does nothing, so it is checked. */
#define RTW_SIG_8188E  0x88E1
#define RTW_SIG_8192E  0x92E1
#define RTW_SIG_8723B  0x5301
#define RTW_SIG_8812   0x8812
#define RTW_SIG_8821   0x8821
#define RTW_SIG_8822B  0x9500
#define RTW_SIG_8822C  0x9502
#define RTW_SIG_8821C  0x9501
#define RTW_SIG_8723D  0x9503

typedef struct {
    u16       signature;
    u8        category, function;
    u16       version;
    u8        subversion, subindex;
    u8        month, date, hour, minute;
    u16       header_size;
    u32       body_size;
    const u8 *body;
} rtw_fw_header_t;

bool   rtw_parse_firmware(const u8 *data, size_t size, rtw_fw_header_t *out);
size_t rtw_build_test_firmware(u8 *out, size_t cap);

void   rtw_init(void);
bool   rtw_attach_model(void);
struct wifi_device *rtw_model_device(void);

/* Drive the driver against a model of a Realtek chip: the header, the paged
 * download, the mailbox and a scan.  Needs no card. */
int    rtw_drive_test(void);

#endif
