/* jpeg.h - baseline JPEG decode, so a picture on a page is a picture and a
 * sequence of them is video.  See jpeg.c. */
#ifndef KESTREL_JPEG_H
#define KESTREL_JPEG_H

#include <stddef.h>

enum {
    JPEG_OK = 0,
    JPEG_ENOTJPEG = -1,     /* not a JPEG at all */
    JPEG_EBADDATA = -2,     /* truncated or malformed */
    JPEG_EUNSUPPORTED = -3, /* progressive/arithmetic/CMYK - refused, not guessed */
    JPEG_ENOMEM = -4
};

typedef struct {
    int width, height;
    unsigned char *rgb;     /* width*height*3, 8-bit R,G,B */
} jpeg_image_t;

/* Decode a baseline JPEG into RGB.  Returns JPEG_OK and fills `out`, or a
 * negative code and leaves it zero.  Free with jpeg_free. */
int  jpeg_decode(const void *bytes, size_t len, jpeg_image_t *out);
void jpeg_free(jpeg_image_t *img);

#endif
