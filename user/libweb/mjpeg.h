/* mjpeg.h - Motion JPEG video (a sequence of JPEG frames).  See mjpeg.c. */
#ifndef KESTREL_WEB_MJPEG_H
#define KESTREL_WEB_MJPEG_H

#ifdef MJPEG_HOST_TEST
#include <stddef.h>
#else
#include "kestrel.h"
#endif

typedef struct {
    int width, height;
    unsigned char *rgb;     /* width*height*3, malloc'd; free with mjpeg_frame_free */
} mjpeg_frame_t;

enum {
    MJPEG_OK = 0,
    MJPEG_ENOFRAME = -1,    /* no frame at that index      */
    MJPEG_EBADFRAME = -2,   /* the JPEG at it did not decode */
};

/* How many JPEG frames the stream holds. */
int  mjpeg_frame_count(const void *bytes, size_t len);

/* Decode frame `n` (0-based) to RGB.  On success free out->rgb (or use
 * mjpeg_frame_free). */
int  mjpeg_decode_frame(const void *bytes, size_t len, int n, mjpeg_frame_t *out);
void mjpeg_frame_free(mjpeg_frame_t *f);

#endif
