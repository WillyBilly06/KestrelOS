/* image.h - decoded pictures for a page, cached by address.  See image.c. */
#ifndef KESTREL_WEB_IMAGE_H
#define KESTREL_WEB_IMAGE_H

typedef struct {
    int width, height;
    unsigned char *rgb;     /* width*height*3 */
} web_image_t;

/* Decode the picture at `src` (resolved against the page address `base`) and
 * keep it.  Returns non-zero when a picture is available afterwards. */
int web_image_load(const char *base, const char *src);

/* The decoded picture for a resolved address, or NULL if not held. */
const web_image_t *web_image_get(const char *url);

/* Resolve, load once, and return the decoded picture (or NULL). */
const web_image_t *web_image_for(const char *base, const char *src);

/* Drop every decoded picture - called when a new page is loaded. */
void web_image_forget_all(void);

/* Advance animated pictures (GIFs) whose next frame is due at `now` (uptime ms).
 * Returns non-zero if any advanced, so the caller repaints.  A still image is
 * left alone. */
int web_image_tick(unsigned long long now);

#endif
