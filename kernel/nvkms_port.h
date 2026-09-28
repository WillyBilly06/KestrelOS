#ifndef KESTREL_NVKMS_PORT_H
#define KESTREL_NVKMS_PORT_H

#include "nv.h"

/* Attach the live GSP-RM transport after its object tree is available.  This
 * does not enable NVKMS until the complete RM bridge reports ready. */
void nvkms_host_attach(nv_card_t *card, nv_rm_t *rm);
bool nvkms_host_attach_full(nv_card_t *card);
void nvkms_host_run_timers(void);
bool nvkms_host_link_ready(void);
nv_card_t *nvkms_host_card(void);

/* Native KAPI display client lives separately from the OS portability layer. */
struct nvkms_kapi_test_result;

#endif
