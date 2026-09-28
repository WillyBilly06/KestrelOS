/* gif.h - a GIF decoder to RGB (first frame).  See gif.c. */
#ifndef KESTREL_WEB_GIF_H
#define KESTREL_WEB_GIF_H

#ifdef GIF_HOST_TEST
#include <stddef.h>
#else
#include "kestrel.h"
#endif

typedef struct {
    int width, height;
    unsigned char *rgb;     /* width*height*3, malloc'd; free with gif_free */
} gif_image_t;

/* One frame of an animation: the whole canvas composited up to this point, and
 * how long to show it. */
typedef struct {
    unsigned char *rgb;     /* width*height*3 of the animation canvas */
    int delay_ms;           /* how long to show it (0 -> a small default) */
} gif_frame_t;

typedef struct {
    int width, height;      /* the animation canvas (logical screen) size */
    int frame_count;
    int loop_count;         /* 0 = loop forever */
    gif_frame_t *frames;    /* frame_count of them */
} gif_anim_t;

enum {
    GIF_OK = 0,
    GIF_ENOTGIF = -1,
    GIF_EBADDATA = -2,
    GIF_ENOMEM = -3,
};

/* Decode a GIF's first frame into out->rgb (RGB, 8 bits/channel).  Returns
 * GIF_OK or a negative code.  On success free out->rgb (or call gif_free). */
int  gif_decode(const void *bytes, size_t len, gif_image_t *out);
void gif_free(gif_image_t *img);

/* Decode every frame of an animated GIF, each composited onto the canvas with
 * the right disposal and transparency, with its delay.  A single-frame GIF
 * comes back as one frame.  Free with gif_anim_free. */
int  gif_decode_anim(const void *bytes, size_t len, gif_anim_t *out);
void gif_anim_free(gif_anim_t *out);

#endif
