/* image.c - the pictures on a page, kept decoded so painting is cheap.
 *
 * The layout knows where an <img> goes; this holds what goes there.  A small
 * cache keyed by the picture's address means a page that shows the same image
 * twice fetches and decodes it once, and a relayout (a resize, a scroll into a
 * reflow) does not throw the pixels away and fetch them again.
 *
 * Only the formats there is a decoder for are kept; anything else leaves the
 * placeholder the layout drew, which is the honest thing - a broken picture
 * icon says "there is a picture here I could not read", not nothing.
 */
#include "kestrel.h"
#include "web.h"
#include "jpeg.h"
#include "png.h"
#include "gif.h"
#include "mjpeg.h"
#include "image.h"

#define CACHE_MAX 24
#define URL_MAX   512

typedef struct {
    int  used;
    char url[URL_MAX];
    web_image_t img;        /* decoded rgb, or rgb==NULL if it could not be.
                             * For an animation this points at the current
                             * frame inside `anim` and must not be freed alone. */
    int  tried;             /* fetched once already, do not keep retrying   */
    int  animated;          /* holds a multi-frame anim below               */
    gif_anim_t anim;        /* the frames, when animated                    */
    int  cur;               /* which frame img.rgb currently points at      */
    unsigned long long next_ms;   /* when to advance to the next frame      */
} entry_t;

static entry_t cache[CACHE_MAX];

/* Release whatever an entry holds: an animation owns its frames; a still owns
 * its one buffer. */
static void free_entry(entry_t *e) {
    if (e->animated) { gif_anim_free(&e->anim); e->img.rgb = NULL; e->animated = 0; }
    else if (e->img.rgb) { free(e->img.rgb); e->img.rgb = NULL; }
}

static entry_t *find(const char *url) {
    for (int i = 0; i < CACHE_MAX; i++)
        if (cache[i].used && !strcmp(cache[i].url, url)) return &cache[i];
    return NULL;
}

static entry_t *make(const char *url) {
    for (int i = 0; i < CACHE_MAX; i++)
        if (!cache[i].used) {
            cache[i].used = 1;
            strlcpy(cache[i].url, url, sizeof cache[i].url);
            cache[i].img.rgb = NULL;
            cache[i].tried = 0;
            return &cache[i];
        }
    /* Full: reuse the first slot rather than grow without bound. */
    free_entry(&cache[0]);
    strlcpy(cache[0].url, url, sizeof cache[0].url);
    cache[0].tried = 0;
    return &cache[0];
}

/* Already decoded?  Returns the picture, or NULL if it is not (yet) here. */
const web_image_t *web_image_get(const char *url) {
    if (!url || !url[0]) return NULL;
    entry_t *e = find(url);
    return (e && e->img.rgb) ? &e->img : NULL;
}

/* Fetch a picture and decode it, once.  Returns true when a picture is now
 * available for web_image_get.  `base` is the page's own address, so a
 * relative src resolves against it. */
int web_image_load(const char *base, const char *src) {
    if (!src || !src[0]) return 0;

    /* Resolve src against the page's address the way following a link does:
     * parse the base, parse src against it, put the result back together. */
    char url[URL_MAX];
    web_url_t baseu, resolved;
    int have_base = base && base[0] && web_parse_url(base, NULL, &baseu);
    if (!web_parse_url(src, have_base ? &baseu : NULL, &resolved)) return 0;
    web_format_url(&resolved, url, sizeof url);

    entry_t *e = find(url);
    if (e && e->img.rgb) return 1;          /* already have it */
    if (e && e->tried) return 0;            /* tried and failed, do not spin */
    if (!e) e = make(url);
    e->tried = 1;

    web_response_t r;
    memset(&r, 0, sizeof r);
    int ok = web_fetch(url, 4, &r) && r.status == 200 && r.body && r.body_len;

    /* Decode by content, not by the name in the URL - a server may hand a
     * .jpg through a redirect, or none at all.  The signatures do not overlap,
     * so trying each decoder in turn picks the right one. */
    int got = 0;
    const char *kind = "?";

    /* Motion JPEG first, because it also starts with a JPEG SOI: more than one
     * whole JPEG frame in the stream (by a marker walk that ignores embedded
     * thumbnails) means it is a video, played like an animation.  Exactly one
     * frame falls through to the still-JPEG path below. */
    if (ok && mjpeg_frame_count(r.body, r.body_len) > 1) {
        int nf = mjpeg_frame_count(r.body, r.body_len);
        e->anim.frames = malloc(sizeof(gif_frame_t) * nf);
        if (e->anim.frames) {
            int dec = 0;
            for (int i = 0; i < nf; i++) {
                mjpeg_frame_t mf;
                if (mjpeg_decode_frame(r.body, r.body_len, i, &mf) != MJPEG_OK) continue;
                if (dec == 0) { e->anim.width = mf.width; e->anim.height = mf.height; }
                e->anim.frames[dec].rgb = mf.rgb;
                e->anim.frames[dec].delay_ms = 100;      /* 10 fps default */
                dec++;
            }
            if (dec > 1) {
                e->anim.frame_count = dec; e->anim.loop_count = 0;
                e->animated = 1; e->cur = 0;
                e->img.width = e->anim.width; e->img.height = e->anim.height;
                e->img.rgb = e->anim.frames[0].rgb;
                e->next_ms = uptime_ms() + 100;
                got = 1; kind = "MJPEG video";
            } else {
                gif_anim_free(&e->anim);             /* not really multi-frame */
            }
        }
    }

    if (!got && ok) {
        jpeg_image_t j;
        png_image_t  pn;
        if (jpeg_decode(r.body, r.body_len, &j) == JPEG_OK) {
            e->img.rgb = j.rgb; e->img.width = j.width; e->img.height = j.height;
            got = 1; kind = "JPEG";
        } else if (png_decode(r.body, r.body_len, &pn) == PNG_OK) {
            e->img.rgb = pn.rgb; e->img.width = pn.width; e->img.height = pn.height;
            got = 1; kind = "PNG";
        } else if (gif_decode_anim(r.body, r.body_len, &e->anim) == GIF_OK) {
            e->img.width = e->anim.width; e->img.height = e->anim.height;
            if (e->anim.frame_count > 1) {
                /* Animated: img.rgb tracks the current frame; the browser's
                 * tick advances it.  The anim owns the frame buffers. */
                e->animated = 1; e->cur = 0;
                e->img.rgb = e->anim.frames[0].rgb;
                e->next_ms = uptime_ms() + (unsigned)e->anim.frames[0].delay_ms;
                kind = "animated GIF";
            } else {
                /* Still: take frame 0's buffer and drop the anim wrapper. */
                e->img.rgb = e->anim.frames[0].rgb;
                e->anim.frames[0].rgb = NULL;
                gif_anim_free(&e->anim);
                kind = "GIF";
            }
            got = 1;
        }
    }
    if (got)
        printf("browser: decoded %s %dx%d from %s\n",
               kind, e->img.width, e->img.height, url);
    web_free(&r);
    return got;
}

/* Resolve, load once, and hand back the decoded picture (or NULL) - the one
 * call the layout needs per <img>. */
const web_image_t *web_image_for(const char *base, const char *src) {
    if (!src || !src[0]) return NULL;
    web_url_t baseu, resolved;
    int have_base = base && base[0] && web_parse_url(base, NULL, &baseu);
    if (!web_parse_url(src, have_base ? &baseu : NULL, &resolved)) return NULL;
    char url[URL_MAX];
    web_format_url(&resolved, url, sizeof url);
    if (!web_image_get(url)) web_image_load(base, src);
    return web_image_get(url);
}

void web_image_forget_all(void) {
    for (int i = 0; i < CACHE_MAX; i++)
        if (cache[i].used) free_entry(&cache[i]);
}

/* Advance any animated pictures whose frame is due, pointing img.rgb at the new
 * frame.  Returns non-zero if any changed (so the caller repaints).  `now` is
 * uptime in ms. */
int web_image_tick(unsigned long long now) {
    int changed = 0;
    for (int i = 0; i < CACHE_MAX; i++) {
        entry_t *e = &cache[i];
        if (!e->used || !e->animated || e->anim.frame_count < 2) continue;
        if (now < e->next_ms) continue;
        e->cur++;
        if (e->cur >= e->anim.frame_count) e->cur = 0;   /* loop */
        e->img.rgb = e->anim.frames[e->cur].rgb;
        int d = e->anim.frames[e->cur].delay_ms;
        e->next_ms = now + (unsigned)(d > 0 ? d : 100);
        changed = 1;
    }
    return changed;
}
