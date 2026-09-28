#ifndef KESTREL_NV_VIDEO_H
#define KESTREL_NV_VIDEO_H
#include "../include/kestrel/video.h"
/* Kernel-owned snapshots only; caller holds nv_render_try_begin transaction.
 * Metadata inspect may run without a live GPU. No retained user pointers. */
int nv_nvdec_decode_idr(const unsigned char *input, unsigned int bytes,
                       unsigned char *output, unsigned int capacity,
                        kvideo_request_t *info);
int nv_nvenc_encode_idr(const unsigned char *input, unsigned int bytes,
                       unsigned char *output, unsigned int capacity,
                       kvideo_request_t *info);
#endif
