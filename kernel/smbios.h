/* smbios.h - the firmware's description of this machine.
 *
 * Everything here comes from tables the firmware wrote at power-on, and is
 * only as good as that firmware.  A field left empty means the tables did not
 * say, which is common and is not an error: the caller shows nothing rather
 * than inventing a plausible value.
 */
#ifndef KESTREL_SMBIOS_H
#define KESTREL_SMBIOS_H

#include "kernel.h"

#define SMBIOS_MAX_MEMORY 16

typedef struct {
    char locator[24];        /* the marking beside the slot on the board   */
    u64  bytes;              /* 0 when the slot is empty                   */
    u32  speed_mts;          /* megatransfers per second                   */
    char type[12];           /* "DDR5" and so on                           */
    char form_factor[12];    /* "DIMM", "SODIMM", ...                      */
    char maker[32];
    char part[32];
} smbios_memory_t;

typedef struct {
    bool present;
    u8   major, minor;

    char bios_vendor[48], bios_version[32], bios_date[16];
    char system_maker[48], system_product[48];
    char board_maker[48], board_product[48];
    char cpu_socket[24];
    u32  cpu_max_mhz;        /* only used when CPUID will not say          */

    smbios_memory_t memory[SMBIOS_MAX_MEMORY];
    u32  memory_count;       /* slots described, filled or not             */
    u32  memory_slots_total; /* what the board says it has                 */
    u32  memory_slots_used;
    u64  memory_bytes;
    u32  memory_speed_mts;
    char memory_kind[12];    /* "DDR5"                                     */
    char memory_form[12];    /* "DIMM"                                     */
} smbios_info_t;

extern smbios_info_t g_smbios;

void smbios_init(void);

#endif /* KESTREL_SMBIOS_H */
