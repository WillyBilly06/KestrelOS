/* png.h - a PNG decoder to RGB.  See png.c. */
#ifndef KESTREL_WEB_PNG_H
#define KESTREL_WEB_PNG_H

#ifdef PNG_HOST_TEST
#include <stddef.h>
#else
#include "kestrel.h"
#endif

typedef struct {
    int width, height;
    unsigned char *rgb;     /* width*height*3, malloc'd; free with png_free */
    unsigned char *alpha;   /* width*height, or NULL when the image is opaque */
} png_image_t;

enum {
    PNG_OK = 0,
    PNG_ENOTPNG = -1,       /* not a PNG (bad signature)            */
    PNG_EUNSUPPORTED = -2,  /* interlaced or a depth not handled    */
    PNG_EBADDATA = -3,      /* truncated or corrupt                 */
    PNG_ENOMEM = -4,
};

/* Decode a PNG into out->rgb (RGB, 8 bits/channel).  Returns PNG_OK or a
 * negative code above.  On success free out->rgb (or call png_free). */
int  png_decode(const void *bytes, size_t len, png_image_t *out);
void png_free(png_image_t *img);

#endif
