/* The native desktop renderer. Widgets submit painting records; framebuffer
 * pixels and composition remain in VRAM. CPU surfaces are immutable image
 * sources while being uploaded, not a shadow of a GPU render target.
 * Single GUI/event thread, matching libgui's existing surface/clip contract. */
#include "gui.h"
#include "../../include/kestrel/gpu2d.h"
#include "../../include/kestrel/gpu3d.h"

struct gui_gpu_surface {
    uint64_t handle;
    unsigned pitch;
    uint64_t depth_handle;
    unsigned depth_pitch;
    bool invalid;
};
static struct {
    struct gui_gpu_surface *target;
    uint64_t source;
    kg2d_command_t commands[KG2D_MAX_COMMANDS];
    unsigned count;
    rect_t clip, bounds;
    bool failed;
} batch;
static uint64_t scratch_handle;
static unsigned char *scratch;
static unsigned scratch_used;
static uint64_t retired_batches, retired_frames;
/* Reserve most of the kernel's 32 surface slots for windows, not icon assets. */
static struct { surface_t **owner; uint64_t used; } icon_cache[8];
static uint64_t icon_clock;
#define SCRATCH_BYTES KG2D_TRANSFER_MAX

static intptr_t request_status(kg2d_request_t *r) {
    r->version = KG2D_ABI;
    intptr_t status;
    unsigned tries = 0;
    do {
        status = syscall6(SYS_GPU, GPUOP_SURFACE, (intptr_t)r, sizeof *r, 0, 0, 0);
        if (status == -EBUSY) sleep_ms(1);
    } while (status == -EBUSY && ++tries < 1000);
    if (status >= 0) return status;
    if (!batch.failed) {
        char note[256];
        snprintf(note, sizeof note, "GPU desktop operation %u failed (%lld): handle=%llx source=%llx xy=%d,%d size=%ux%u count=%u offset=%llu bytes=%llu; no CPU fallback",
                 r->operation, (long long)status, (unsigned long long)r->handle,
                 (unsigned long long)r->source, r->x, r->y, r->width, r->height,
                 r->count, (unsigned long long)r->offset, (unsigned long long)r->bytes);
        log_write(3, "gui-gpu", note);
    }
    /* Optional window/resource allocation exhaustion must not stop already
     * working surfaces. A failed render/transfer, however, poisons the frame. */
    if(r->operation!=KG2D_CREATE && r->operation!=KG2D_DESTROY) batch.failed = true;
    return status;
}

static bool request(kg2d_request_t *r) {
    return request_status(r) >= 0;
}

static bool icon_evict_oldest(uint64_t protect_a, uint64_t protect_b, bool last_allowed);
static bool attach_surface(surface_t *s, bool cache_replacement);

/* Only a positively classified resource shortage justifies cache reclamation.
 * Protect caller-held image/target handles across the allocation and bound the
 * retry count even if a future cache implementation repopulates an entry. */
static bool allocate_surface(kg2d_request_t *r, uint64_t protect_a, uint64_t protect_b,
                             bool cache_replacement) {
    if (batch.failed) return false;
    for (unsigned attempt=0; ; attempt++) {
        intptr_t status=request_status(r);
        if (status>=0) {
            if (attempt) {
                char note[112];
                snprintf(note,sizeof note,"reclaimed %u cached icon surface(s); %ux%u GPU allocation succeeded",
                         attempt,r->width,r->height);
                log_write(1,"gui-gpu",note);
            }
            return true;
        }
        if (status!=-ENOMEM || attempt==8 || !icon_evict_oldest(protect_a,protect_b,cache_replacement))
            return false;
    }
}

bool gui_gpu_flush(void) {
    if (batch.failed) return false;
    if (!batch.count) return true;
    if (batch.target->invalid) return false;
    if (batch.source == scratch_handle && scratch_used) {
        kg2d_request_t upload = { .operation=KG2D_UPLOAD, .handle=scratch_handle,
            .data=(uint64_t)(uintptr_t)scratch, .bytes=(scratch_used+3u)&~3u };
        if (!request(&upload)) return false;
    }
    kg2d_request_t r = { .operation=KG2D_DRAW, .handle=batch.target->handle,
        .source=batch.source, .data=(uint64_t)(uintptr_t)batch.commands,
        .count=batch.count, .x=batch.bounds.x, .y=batch.bounds.y,
        .width=batch.bounds.w, .height=batch.bounds.h };
    if (!request(&r)) return false;
    retired_batches++;
    batch.count = 0;
    batch.target = NULL;
    scratch_used = 0;
    return true;
}

static surface_t *create_gpu_surface(int w,int h,bool cache_replacement) {
    if(w<=0 || h<=0)return NULL;
    surface_t *s=calloc(1,sizeof *s);
    if(!s)return NULL;
    s->width=w;s->height=h;s->stride=w;s->owns_pixels=true;
    if(!attach_surface(s,cache_replacement)){free(s);return NULL;}
    surface_reset_clip(s);
    return s;
}

surface_t *surface_create_target(int w,int h,bool gpu) {
    return gpu?create_gpu_surface(w,h,false):surface_create(w,h);
}

static bool attach_surface(surface_t *s, bool cache_replacement) {
    if (!s || s->width <= 0 || s->height <= 0 || batch.failed) return false;
    if (s->gpu) return !s->gpu->invalid;
    if (!s->owns_pixels) return false;
    struct gui_gpu_surface *g = calloc(1, sizeof *g);
    if (!g) return false;
    kg2d_request_t r = { .operation=KG2D_CREATE, .width=s->width, .height=s->height };
    if (!allocate_surface(&r,0,0,cache_replacement)) { free(g); return false; }
    g->handle = r.handle; g->pitch = r.pitch;
    /* Attach only newly created/cleared render targets. Existing CPU images
     * must be uploaded through the image-source path, never silently discarded. */
    s->gpu = g;
    free(s->pixels);
    s->pixels = NULL; /* no CPU framebuffer exists for a GPU render target */
    return true;
}

bool gui_gpu_attach(surface_t *s) {
    return attach_surface(s,false);
}

static bool detach_surface(surface_t *s) {
    if (!s || !s->gpu) return true;
    bool ok=gui_gpu_flush();
    if (s->gpu->depth_handle) {
        kg2d_request_t depth = {.operation=KG2D_DESTROY,.handle=s->gpu->depth_handle};
        if (!request(&depth)) ok=false;
    }
    kg2d_request_t r = { .operation=KG2D_DESTROY, .handle=s->gpu->handle };
    if (!request(&r)) ok=false; /* driver quarantines storage if a fence failed */
    if (batch.target == s->gpu) { batch.count=0; batch.target=NULL; }
    free(s->gpu); s->gpu=NULL;
    for(unsigned i=0;i<8;i++)if(icon_cache[i].owner && *icon_cache[i].owner==s) {
        *icon_cache[i].owner=NULL;icon_cache[i].owner=NULL;
    }
    return ok;
}

void gui_gpu_detach(surface_t *s) {
    (void)detach_surface(s);
}

static bool icon_evict_oldest(uint64_t protect_a, uint64_t protect_b, bool last_allowed) {
    unsigned oldest=8,resident=0;
    for (unsigned i=0;i<8;i++) {
        surface_t *s=icon_cache[i].owner?*icon_cache[i].owner:NULL;
        if (!s || !s->gpu) continue;
        resident++;
        if (s->gpu->handle==protect_a || s->gpu->handle==protect_b) continue;
        if (oldest==8 || icon_cache[i].used<icon_cache[oldest].used) oldest=i;
    }
    /* Window/work-buffer growth must not consume the last interchangeable
     * icon slot. Only a replacement icon can borrow that final slot. */
    if (oldest==8 || (!last_allowed && resident<=1) || !gui_gpu_flush()) return false;
    surface_t *s=*icon_cache[oldest].owner;
    bool released=detach_surface(s);
    surface_destroy(s); /* metadata only after detach; no second GPU destroy */
    return released; /* Do not retry when destruction was not acknowledged. */
}

static bool readback(const surface_t *s, colour_t *destination, unsigned stride) {
    if(s->gpu->invalid)return false;
    if(!gui_gpu_flush())return false;
    unsigned pitch=s->gpu->pitch;
    unsigned rows=KG2D_TRANSFER_MAX/pitch;
    if(!rows)return false;
    unsigned char *pixels=malloc((size_t)rows*pitch);
    if(!pixels)return false;
    for(unsigned y=0;y<(unsigned)s->height;y+=rows) {
        unsigned n=(unsigned)s->height-y;if(n>rows)n=rows;
        kg2d_request_t r={.operation=KG2D_DOWNLOAD,.handle=s->gpu->handle,
            .offset=(uint64_t)y*pitch,.bytes=(uint64_t)n*pitch,
            .data=(uint64_t)(uintptr_t)pixels};
        if(!request(&r)){free(pixels);return false;}
        for(unsigned row=0;row<n;row++)
            memcpy(destination+(size_t)(y+row)*stride,pixels+(size_t)row*pitch,(size_t)s->width*4);
    }
    free(pixels);
    return true;
}

bool gui_gpu_colour_readback(const surface_t *s, colour_t *pixels,
                             unsigned stride, size_t capacity) {
    if(!s || !s->gpu || !s->owns_pixels || !pixels || s->width<=0 || s->height<=0 ||
       stride<(unsigned)s->width ||
       (uint64_t)(unsigned)(s->height-1)*stride+(unsigned)s->width>capacity)
        return false;
    return readback(s,pixels,stride);
}

/* The depth allocation belongs to the render target, so closing/resizing the
 * window releases both under the same fenced surface-lifetime contract. */
bool gui_gpu_draw3d(surface_t *s, const surface_t *texture,
                    const kg3d_command_t *commands, unsigned count, rect_t bounds) {
    if(!s || !s->gpu || s->gpu->invalid || !s->owns_pixels || !commands || !count ||
       count>KG3D_MAX_COMMANDS || (texture && (!texture->gpu || texture->gpu->invalid || texture->gpu==s->gpu)))
        return false;
    bounds=rect_intersection(bounds,surface_clip(s));
    if(rect_empty(bounds))return true;
    if(!gui_gpu_flush())return false; /* widgets precede application triangles */
    bool needs_depth=false;
    for(unsigned i=0;i<count;i++)
        needs_depth|=!!(commands[i].flags&(KG3D_DEPTH_TEST|KG3D_DEPTH_WRITE|KG3D_CLEAR_DEPTH));
    if(needs_depth && !s->gpu->depth_handle) {
        kg2d_request_t r={.operation=KG2D_CREATE,.width=s->width,.height=s->height};
        if(!allocate_surface(&r,s->gpu->handle,texture?texture->gpu->handle:0,false))return false;
        s->gpu->depth_handle=r.handle;s->gpu->depth_pitch=r.pitch;
    }
    /* Tile large viewports to obey the kernel's bounded-work contract without
     * reducing resolution or dropping commands. Each pixel keeps full order. */
    unsigned work=0;
    for(unsigned i=0;i<count;i++)work+=(commands[i].flags&KG3D_LINE)?KG3D_LINE_WORK:1u;
    unsigned rows=KG3D_MAX_WORK/((uint64_t)(unsigned)bounds.w*work);
    if(!rows)return false;
    for(unsigned y=0;y<(unsigned)bounds.h;y+=rows) {
        unsigned n=(unsigned)bounds.h-y;if(n>rows)n=rows;
        kg2d_request_t r={.operation=KG2D_DRAW3D,.handle=s->gpu->handle,
            .source=texture?texture->gpu->handle:0,.offset=s->gpu->depth_handle,
            .data=(uint64_t)(uintptr_t)commands,.count=count,
            .x=bounds.x,.y=(unsigned)bounds.y+y,.width=bounds.w,.height=n};
        if(!request(&r))return false;
        retired_batches++;
    }
    return true;
}

bool gui_gpu_target(surface_t *s, bool needs_depth, gui_gpu_target_t *out) {
    if(!s || !s->gpu || s->gpu->invalid || !s->owns_pixels || !out || !gui_gpu_flush())return false;
    if(needs_depth && !s->gpu->depth_handle){
        kg2d_request_t r={.operation=KG2D_CREATE,.width=s->width,.height=s->height};
        if(!allocate_surface(&r,s->gpu->handle,0,false))return false;
        s->gpu->depth_handle=r.handle;s->gpu->depth_pitch=r.pitch;
    }
    *out=(gui_gpu_target_t){s->gpu->handle,s->gpu->depth_handle,s->gpu->pitch,s->gpu->depth_pitch};
    return true;
}
void gui_gpu_invalidate(surface_t *s) {
    if(!s || !s->gpu)return;
    s->gpu->invalid=true;
    if(batch.target==s->gpu){batch.count=0;batch.target=NULL;scratch_used=0;}
    log_write(3,"gui-gpu","incomplete application GPU surface: suppress sampling/presentation; other canvases remain usable");
}

surface_t *gui_gpu_image(const colour_t *pixels, int width, int height) {
    if(!pixels || width<=0 || height<=0 || !gui_gpu_flush())return NULL;
    surface_t *image=surface_create_target(width,height,true);
    if(!image)return NULL;
    unsigned pitch=image->gpu->pitch,rows=KG2D_TRANSFER_MAX/pitch;
    if(rows>(unsigned)height)rows=(unsigned)height;
    unsigned char *data=rows?malloc((size_t)rows*pitch):NULL;
    bool ok=data!=NULL;
    for(unsigned y=0;ok && y<(unsigned)height;y+=rows) {
        unsigned n=(unsigned)height-y;if(n>rows)n=rows;
        memset(data,0,(size_t)n*pitch);
        for(unsigned row=0;row<n;row++)
            memcpy(data+(size_t)row*pitch,pixels+(size_t)(y+row)*width,(size_t)width*4);
        kg2d_request_t r={.operation=KG2D_UPLOAD,.handle=image->gpu->handle,
            .offset=(uint64_t)y*pitch,.data=(uint64_t)(uintptr_t)data,.bytes=(uint64_t)n*pitch};
        ok=request(&r);
    }
    free(data);
    if(!ok){surface_destroy(image);return NULL;}return image;
}

bool gui_gpu_depth_readback(const surface_t *s,float *pixels,unsigned stride) {
    if(!s || !s->gpu || s->gpu->invalid || !pixels || stride<(unsigned)s->width)return false;
    if(!s->gpu->depth_handle) {
        /* As with the original software context, depth before first clear is
         * unspecified; choose the fresh allocation's zero value consistently. */
        for(int y=0;y<s->height;y++)memset(pixels+(size_t)y*stride,0,(size_t)s->width*4);
        return true;
    }
    struct gui_gpu_surface g={.handle=s->gpu->depth_handle,.pitch=s->gpu->depth_pitch};
    surface_t view=*s;view.gpu=&g;
    return readback(&view,(colour_t *)pixels,stride);
}

bool gui_gpu_cpu_access(surface_t *s) {
    if(!s || !s->gpu)return true;
    if(!s->pixels) {
        s->pixels=calloc((size_t)s->stride*s->height,sizeof(colour_t));
        if(!s->pixels)return false;
    }
    if(!readback(s,s->pixels,s->stride))return false;
    gui_gpu_detach(s);
    log_write(2,"gui-gpu","legacy software GL target: explicit readback; desktop composition remains GPU");
    return true;
}

static bool enqueue(surface_t *s, kg2d_command_t c, uint64_t source) {
    if(s->gpu->invalid)return false;
    if (batch.failed) return false;
    rect_t clip = surface_clip(s);
    rect_t area = rect_intersection(rect_make(c.x,c.y,c.width,c.height), clip);
    if (rect_empty(area)) return true;
    rect_t bounds = batch.count ? rect_union(batch.bounds,area) : area;
    bool same_clip = !memcmp(&clip,&batch.clip,sizeof clip);
    if (batch.count && (batch.target != s->gpu || (batch.source && source && batch.source != source) ||
        !same_clip || batch.count == KG2D_MAX_COMMANDS ||
        (uint64_t)bounds.w*bounds.h*(batch.count+1) > 4000000u)) {
        if (!gui_gpu_flush()) return false;
        bounds = area;
    }
    if (!batch.count) {
        batch.target=s->gpu; batch.source=source; batch.clip=clip;
    }
    /* Solid/gradient/shape records do not sample a source. They can surround
     * masked text in the same ordered batch; the first sampled record adopts
     * its immutable source without forcing an otherwise unnecessary fence. */
    if(!batch.source)batch.source=source;
    /* Adjacent equal spans can be represented by one rectangle. This is
     * important for AA coverage and avoids a launch per generated pixel. */
    if (batch.count && c.op == KG2D_SOLID) {
        kg2d_command_t *last=&batch.commands[batch.count-1];
        if (last->op==c.op && last->colour0==c.colour0 && last->opacity==c.opacity &&
            last->y==c.y && last->height==c.height && last->x+last->width==c.x) {
            last->width+=c.width; batch.bounds=bounds; return true;
        }
    }
    batch.commands[batch.count++]=c;
    batch.bounds=bounds;
    return true;
}

bool gui_gpu_paint(surface_t *s, rect_t r, unsigned op, colour_t a, colour_t b, int alpha) {
    if (!s->gpu) return false;
    if (s->gpu->invalid) return true;
    if (alpha<=0 || rect_empty(r)) return true;
    kg2d_command_t c = { .x=r.x,.y=r.y,.width=r.w,.height=r.h,
        .op=op,.colour0=a,.colour1=b,.opacity=alpha>255?255u:(unsigned)alpha };
    enqueue(s,c,0);
    return true; /* Never silently resume CPU painting after GPU activation. */
}

bool gui_gpu_line(surface_t *s,int x0,int y0,int x1,int y1,colour_t colour) {
    if(!s->gpu)return false;
    if(s->gpu->invalid || batch.failed)return true;
    int left=x0<x1?x0:x1,top=y0<y1?y0:y1;
    int right=x0>x1?x0:x1,bottom=y0>y1?y0:y1;
    rect_t clip=surface_clip(s);
    if(rect_empty(clip) || right<clip.x || bottom<clip.y ||
       left>=(int64_t)clip.x+clip.w || top>=(int64_t)clip.y+clip.h)return true;
    int64_t width=(int64_t)right-left+1,height=(int64_t)bottom-top+1;
    /* libgui rectangles use signed-int exclusive ends. Reject unrepresentable
     * geometry explicitly rather than overflowing those clipping helpers. */
    if(width>INT32_MAX || height>INT32_MAX || right==INT32_MAX || bottom==INT32_MAX){batch.failed=true;return true;}
    kg2d_command_t c={.x=left,.y=top,.width=(int)width,.height=(int)height,
        .op=KG2D_LINE,.colour0=colour,.colour1=(x1<x0?1u:0u)|(y1<y0?2u:0u),.opacity=255};
    enqueue(s,c,0);return true;
}

bool gui_gpu_line_aa(surface_t *s,int x0,int y0,int x1,int y1,colour_t colour) {
    if(!s->gpu)return false;
    if(s->gpu->invalid || batch.failed)return true;
    if(x0==x1 || y0==y1)return gui_gpu_line(s,x0,y0,x1,y1,colour);
    int left=x0<x1?x0:x1,top=y0<y1?y0:y1;
    int right=x0>x1?x0:x1,bottom=y0>y1?y0:y1;
    int64_t dx=(int64_t)right-left,dy=(int64_t)bottom-top;
    bool steep=dy>dx;
    int64_t width=dx+1+steep,height=dy+1+!steep;
    rect_t clip=surface_clip(s);
    if(rect_empty(clip) || (int64_t)left+width<=clip.x || (int64_t)top+height<=clip.y ||
       left>=(int64_t)clip.x+clip.w || top>=(int64_t)clip.y+clip.h)return true;
    if(width>INT32_MAX || height>INT32_MAX || (int64_t)left+width>INT32_MAX ||
       (int64_t)top+height>INT32_MAX){batch.failed=true;return true;}
    kg2d_command_t c={.x=left,.y=top,.width=(int)width,.height=(int)height,
        .op=KG2D_LINE_AA,.colour0=colour,
        .colour1=(steep?1u:0u)|((x1<x0)!=(y1<y0)?2u:0u),.opacity=255};
    enqueue(s,c,0);return true;
}

bool gui_gpu_rounded(surface_t *s,rect_t r,int radius,unsigned style,colour_t top,colour_t bottom) {
    if(!s->gpu)return false;
    if(s->gpu->invalid || rect_empty(r))return true;
    if(radius<0)radius=0;
    if(radius>r.w/2)radius=r.w/2;
    int max_height=style==2u || style==3u?r.h:r.h/2;
    if(radius>max_height)radius=max_height;
    if(radius>4096 || style>5u){batch.failed=true;return true;}
    kg2d_command_t c={.x=r.x,.y=r.y,.width=r.w,.height=r.h,
        .op=KG2D_ROUNDED,.colour0=top,.colour1=bottom,.opacity=255,
        .source_x=(unsigned)radius,.source_y=style};
    enqueue(s,c,0);return true;
}

bool gui_gpu_shadow(surface_t *s, rect_t r, int spread, int strength, int radius, colour_t colour) {
    if (!s->gpu) return false;
    if (s->gpu->invalid) return true;
    if (rect_empty(r) || spread<=0 || strength<=0) return true;
    if (spread>4096 || radius>4096 || r.w>INT32_MAX-2*spread ||
        r.h>INT32_MAX-2*spread-2 || r.x<INT32_MIN+spread || r.y<INT32_MIN+spread ||
        (int64_t)r.y-spread+2>INT32_MAX) {
        batch.failed=true;return true;
    }
    kg2d_command_t c={.x=r.x-spread,.y=r.y-spread+2,
        .width=r.w+2*spread,.height=r.h+2*spread+2,.op=KG2D_SHADOW,
        .colour0=colour,.opacity=strength>255?255u:(unsigned)strength,
        .source_x=(unsigned)spread,.source_y=radius>0?(unsigned)radius:0};
    enqueue(s,c,0);return true;
}

static bool ensure_scratch(uint64_t protected_handle) {
    if (scratch_handle) return true;
    scratch=calloc(1,SCRATCH_BYTES);
    if (!scratch) return false;
    kg2d_request_t r={.operation=KG2D_CREATE,.width=1024,.height=1024};
    if (!allocate_surface(&r,protected_handle,0,false)) { free(scratch);scratch=NULL;return false; }
    scratch_handle=r.handle;
    return true;
}

/* Visibility rejection precedes allocations; wide sums keep off-screen input
 * safe even when an origin plus extent would overflow the public int geometry. */
static bool gpu_rect_visible(rect_t r, rect_t clip) {
    return !rect_empty(r) && !rect_empty(clip) &&
        (int64_t)r.x + r.w > clip.x && (int64_t)clip.x + clip.w > r.x &&
        (int64_t)r.y + r.h > clip.y && (int64_t)clip.y + clip.h > r.y;
}

bool gui_gpu_mask(surface_t *s, const uint8_t *mask, int w,int h,int stride,
                   int x,int y,colour_t colour,int alpha) {
    if (!s->gpu) return false;
    if (s->gpu->invalid || batch.failed) return true;
    if (w<=0 || h<=0 || alpha<=0) return true;
    rect_t clip=surface_clip(s);
    if(!gpu_rect_visible(rect_make(x,y,w,h),clip))return true;
    uint64_t size=(uint64_t)(unsigned)w*(unsigned)h;
    if (size>SCRATCH_BYTES || stride<w || !ensure_scratch(s->gpu->handle)) { batch.failed=true;return true; }
    unsigned bytes=(unsigned)size;
    rect_t area=rect_intersection(rect_make(x,y,w,h),clip);
    rect_t bounds=batch.count?rect_union(batch.bounds,area):area;
    unsigned offset=(scratch_used+3u)&~3u;
    /* Decide every enqueue flush condition before packing a new mask. */
    if (batch.count && (batch.target!=s->gpu || (batch.source && batch.source!=scratch_handle) ||
        memcmp(&clip,&batch.clip,sizeof clip) || batch.count==KG2D_MAX_COMMANDS ||
        (uint64_t)bounds.w*bounds.h*(batch.count+1)>4000000u ||
        bytes>SCRATCH_BYTES-offset)) {
        if(!gui_gpu_flush())return true;
        offset=0;
    }
    for (int row=0;row<h;row++) memcpy(scratch+offset+(size_t)row*w,mask+(size_t)row*stride,w);
    scratch_used=offset+bytes;
    kg2d_command_t c={.x=x,.y=y,.width=w,.height=h,.op=KG2D_MASK_A8,
        .colour0=colour,.opacity=alpha>255?255u:(unsigned)alpha,.source_stride=w,
        .source_offset=offset};
    enqueue(s,c,scratch_handle);
    return true;
}

bool gui_gpu_glyph(surface_t *s,const unsigned char *data,int w,int h,int stride,
                    bool coverage,int scale,int x,int y,colour_t colour) {
    if(!s->gpu)return false;
    if(s->gpu->invalid || batch.failed)return true;
    if(w<=0 || h<=0 || scale<=0)return true;
    int64_t width=(int64_t)w*scale,height=(int64_t)h*scale;
    rect_t clip=surface_clip(s);
    if(rect_empty(clip) || (int64_t)x+width<=clip.x || (int64_t)y+height<=clip.y ||
       x>=(int64_t)clip.x+clip.w || y>=(int64_t)clip.y+clip.h)return true;
    uint64_t row_bytes=coverage?(unsigned)w:((uint64_t)(unsigned)w+7u)/8u;
    uint64_t bytes=(uint64_t)(unsigned)stride*(unsigned)h;
    if(!data || stride<=0 || (uint64_t)(unsigned)stride<row_bytes || bytes>SCRATCH_BYTES ||
       width>INT32_MAX || height>INT32_MAX || (int64_t)x+width>INT32_MAX ||
       (int64_t)y+height>INT32_MAX || !ensure_scratch(s->gpu->handle)) {
        batch.failed=true;return true;
    }
    rect_t r=rect_make(x,y,(int)width,(int)height),area=rect_intersection(r,clip);
    rect_t bounds=batch.count?rect_union(batch.bounds,area):area;
    unsigned offset=(scratch_used+3u)&~3u;
    if(batch.count && (batch.target!=s->gpu || (batch.source && batch.source!=scratch_handle) ||
       memcmp(&clip,&batch.clip,sizeof clip) || batch.count==KG2D_MAX_COMMANDS ||
       (uint64_t)bounds.w*bounds.h*(batch.count+1)>4000000u || bytes>SCRATCH_BYTES-offset)) {
        if(!gui_gpu_flush())return true;
        offset=0;
    }
    /* Copy the original font rows once. GPU threads unpack monochrome bits,
     * normalize near-opaque A8 coverage, scale, and blend destination pixels. */
    memcpy(scratch+offset,data,(size_t)bytes);scratch_used=offset+(unsigned)bytes;
    kg2d_command_t c={.x=x,.y=y,.width=(int)width,.height=(int)height,
        .op=KG2D_GLYPH,.colour0=colour,.colour1=coverage?0u:1u,.opacity=255,
        .source_x=(unsigned)w,.source_y=(unsigned)h,.source_stride=(unsigned)stride,.source_offset=offset};
    enqueue(s,c,scratch_handle);return true;
}

bool gui_gpu_blit(surface_t *dst,const surface_t *src,rect_t from,rect_t to,int alpha) {
    if (!dst->gpu && !src->gpu) return false;
    if((dst->gpu && dst->gpu->invalid)||(src->gpu && src->gpu->invalid))return true;
    if (alpha<=0 || rect_empty(from) || rect_empty(to)) return true;
    if(from.x<0 || from.y<0 || from.x>src->width || from.y>src->height ||
       from.w>src->width-from.x || from.h>src->height-from.y)return true;
    /* Reject invisible copies before flushing unrelated draws or allocating
     * uploads, readbacks and self-copy snapshots. Keep the original rectangles
     * for partially visible scaling so its source sampling does not change. */
    if(!gpu_rect_visible(to,surface_clip(dst)))return true;
    if(!dst->gpu) {
        /* Explicit capture into a CPU surface must snapshot live VRAM, not
         * dereference a nonexistent CPU shadow or demote the GPU source. */
        colour_t *pixels=malloc((size_t)src->width*src->height*4);
        if(!pixels)return true;
        if(readback(src,pixels,src->width)) {
            rect_t a=rect_intersection(to,surface_clip(dst));
            for(int y=a.y;y<a.y+a.h;y++)for(int x=a.x;x<a.x+a.w;x++) {
                int sx=from.x+(int)((int64_t)(x-to.x)*from.w/to.w);
                int sy=from.y+(int)((int64_t)(y-to.y)*from.h/to.h);
                colour_t *p=&dst->pixels[(size_t)y*dst->stride+x];
                *p=colour_mix(*p,pixels[(size_t)sy*src->width+sx],alpha);
            }
        }
        free(pixels);return true;
    }
    /* Distinct resident sources can share preceding destination paints.
     * enqueue() retires a pending SOURCE write on target change, and splits
     * incompatible source/clip bindings. Keep explicit retirement before
     * reusing upload scratch or creating an overlapping-copy snapshot. The
     * trailing flush below still completes sampling before returning. */
    if ((!src->gpu || src->gpu==dst->gpu) && !gui_gpu_flush()) return true;
    if (src->gpu==dst->gpu) {
        /* Overlapping moves require an immutable source snapshot on the GPU. */
        kg2d_request_t r={.operation=KG2D_CREATE,.width=from.w,.height=from.h};
        if(!allocate_surface(&r,src->gpu->handle,dst->gpu->handle,false))return true;
        struct gui_gpu_surface g={.handle=r.handle,.pitch=r.pitch};
        surface_t temporary={.width=from.w,.height=from.h,.gpu=&g};
        rect_t all=rect_make(0,0,from.w,from.h);
        gui_gpu_blit(&temporary,src,from,all,255);
        gui_gpu_blit(dst,&temporary,all,to,alpha);
        r.operation=KG2D_DESTROY;request(&r);
        return true;
    }
    uint64_t handle;
    unsigned pitch;
    bool temporary=false;
    if (src->gpu) { handle=src->gpu->handle;pitch=src->gpu->pitch; }
    else {
        /* CPU image/legacy application content is an explicit upload source,
         * not the widget/compositor render target. */
        kg2d_request_t r={.operation=KG2D_CREATE,.width=src->width,.height=src->height};
        if (!allocate_surface(&r,dst->gpu->handle,0,false)) return true;
        handle=r.handle;pitch=r.pitch;temporary=true;
        unsigned rows=SCRATCH_BYTES/pitch;
        if (!rows || !ensure_scratch(dst->gpu->handle)) { batch.failed=true;goto release; }
        for (unsigned y=0;y<(unsigned)src->height;y+=rows) {
            unsigned n=(unsigned)src->height-y;if(n>rows)n=rows;
            memset(scratch,0,(size_t)n*pitch);
            for(unsigned row=0;row<n;row++)
                memcpy(scratch+(size_t)row*pitch,src->pixels+(size_t)(y+row)*src->stride,
                       (size_t)src->width*4);
            kg2d_request_t u={.operation=KG2D_UPLOAD,.handle=handle,
                .offset=(uint64_t)y*pitch,.data=(uint64_t)(uintptr_t)scratch,.bytes=(uint64_t)n*pitch};
            if(!request(&u))goto release;
        }
    }
    {
        kg2d_command_t c={.x=to.x,.y=to.y,.width=to.w,.height=to.h,
            .op=KG2D_IMAGE_SCALED,.opacity=alpha>255?255u:(unsigned)alpha,
            .source_x=from.x,.source_y=from.y,.source_stride=pitch,
            .reserved={(unsigned)from.w,(unsigned)from.h,0,0}};
        enqueue(dst,c,handle);
        gui_gpu_flush(); /* source cannot change/disappear while queued */
    }
release:
    if(temporary){ kg2d_request_t d={.operation=KG2D_DESTROY,.handle=handle};request(&d); }
    return true;
}

/* Immutable source images stay resident. Scaling/filtering/alpha blending are
 * shader work, not thousands of one-pixel paint records per icon redraw.
 * Caller destroys the cache before replacing its registered source bitmap. */
bool gui_gpu_icon(surface_t *s, surface_t **cache, const void *pixels,
                  int w, int h, bool alpha_only, int x, int y, int size,
                  bool tinted, colour_t tint) {
    if(!s->gpu)return false;
    if(s->gpu->invalid || batch.failed || size<=0)return true;
    if(rect_empty(rect_intersection(rect_make(x,y,size,size),surface_clip(s))))return true;
    if(!cache || !pixels || w<=0 || h<=0) { batch.failed=true;return true; }
    if(!*cache) {
        unsigned slot=0;
        for(unsigned i=0;i<8;i++) {
            if(!icon_cache[i].owner){slot=i;break;}
            if(icon_cache[i].used<icon_cache[slot].used)slot=i;
        }
        if(icon_cache[slot].owner)surface_destroy(*icon_cache[slot].owner);
        surface_t *image=create_gpu_surface(w,h,true);
        if(!image){batch.failed=true;return true;}
        unsigned pitch=image->gpu->pitch, rows=KG2D_TRANSFER_MAX/pitch;
        unsigned char *data=rows?malloc((size_t)(rows<(unsigned)h?rows:(unsigned)h)*pitch):NULL;
        bool ok=data!=NULL;
        for(unsigned y0=0;ok && y0<(unsigned)h;y0+=rows) {
            unsigned n=(unsigned)h-y0;if(n>rows)n=rows;
            memset(data,0,(size_t)n*pitch);
            for(unsigned row=0;row<n;row++) {
                colour_t *out=(colour_t *)(data+(size_t)row*pitch);
                size_t at=(size_t)(y0+row)*w;
                if(alpha_only)for(int col=0;col<w;col++)out[col]=(colour_t)((const uint8_t *)pixels)[at+col]<<24;
                else memcpy(out,(const colour_t *)pixels+at,(size_t)w*4u);
            }
            kg2d_request_t r={.operation=KG2D_UPLOAD,.handle=image->gpu->handle,
                .offset=(uint64_t)y0*pitch,.data=(uint64_t)(uintptr_t)data,.bytes=(uint64_t)n*pitch};
            ok=request(&r);
        }
        free(data);
        if(!ok) { surface_destroy(image);batch.failed=true;return true; }
        *cache=image;
        icon_cache[slot].owner=cache;
    }
    for(unsigned i=0;i<8;i++)if(icon_cache[i].owner==cache)icon_cache[i].used=++icon_clock;
    kg2d_command_t c={.x=x,.y=y,.width=size,.height=size,.op=KG2D_ARGB_BILINEAR,
        .colour0=tint,.colour1=(tinted||alpha_only)?1u:0u,.opacity=255u,
        .source_stride=(*cache)->gpu->pitch,.reserved={(unsigned)w,(unsigned)h,0,0}};
    enqueue(s,c,(*cache)->gpu->handle);
    return true;
}

bool gui_gpu_present(surface_t *s,rect_t a) {
    if (!s || !s->gpu || s->gpu->invalid || !gui_gpu_flush()) return false;
    kg2d_request_t r={.operation=KG2D_PRESENT,.handle=s->gpu->handle,
        .x=a.x,.y=a.y,.width=a.w,.height=a.h};
    if (!request(&r)) return false;
    retired_frames++;
    if(retired_frames==1 || retired_frames%120==0) {
        char note[180];
        snprintf(note,sizeof note,"GPU desktop: %llu presentations, %llu retired widget/composition batches; CPU render-target buffer absent",
                 (unsigned long long)retired_frames,(unsigned long long)retired_batches);
        log_write(1,"gui-gpu",note);
    }
    return true;
}

bool gui_gpu_validate(void) {
    static bool passed;
    if(passed)return !batch.failed;
    /* This must pass through THIS kernel's allocation/QMD/fence/readback path,
     * not the Windows CUDA tests. It never changes a monitor or the 3D test. */
    surface_t source={.width=17,.height=13,.stride=17,.owns_pixels=true};
    surface_t destination={.width=23,.height=19,.stride=23,.owns_pixels=true};
    uint32_t pixels[23*19];
    uint8_t coverage[16];
    for(unsigned i=0;i<16;i++)coverage[i]=(uint8_t)(i*17);
    log_write(1,"gui-gpu","widget gate stage: allocate source and destination");
    bool ok=gui_gpu_attach(&source) && gui_gpu_attach(&destination);
    if(ok) {
        log_write(1,"gui-gpu","widget gate stage: gradient, mask scratch allocation/upload and composition");
        gui_gpu_paint(&source,rect_make(0,0,17,13),KG2D_GRADIENT_H,0x124578,0xdb8321,255);
        gui_gpu_mask(&source,coverage,4,4,4,3,5,0x75e912,181);
        gui_gpu_blit(&destination,&source,rect_make(0,0,17,13),rect_make(0,0,23,19),173);
        gui_gpu_line(&destination,18,14,2,6,0xfedcba);
        gui_gpu_line_aa(&destination,2,3,6,5,0x75e912);
        const unsigned char glyph_a8[]={0,127,254,255},glyph_bits[]={0xa0};
        gui_gpu_glyph(&destination,glyph_a8,2,2,2,true,2,18,1,0x1122ee);
        gui_gpu_glyph(&destination,glyph_bits,3,1,1,false,1,18,7,0x33ee55);
        gui_gpu_rounded(&destination,rect_make(0,11,9,8),4,4u,0x9251ed,0);
        gui_gpu_rounded(&destination,rect_make(13,11,9,8),4,5u,0xed5192,0);
        log_write(1,"gui-gpu","widget gate stage: flush and readback");
        ok=readback(&destination,pixels,23);
    }
    if(ok)for(int y=0;y<19;y++)for(int x=0;x<23;x++) {
        int sx=x*17/23,sy=y*13/19;
        uint32_t expected=colour_mix(0x124578,0xdb8321,sx*255/16);
        if(sx>=3&&sx<7&&sy>=5&&sy<9)
            expected=colour_mix(expected,0x75e912,coverage[(sy-5)*4+sx-3]*181/255);
        expected=colour_mix(0,expected,173);
        /* Independent explicit Bresenham pixels, including reverse-direction
         * half-step ties. This exercises the native line op, not CPU painting. */
        static const unsigned char line[][2]={{2,6},{3,7},{4,7},{5,8},{6,8},
            {7,9},{8,9},{9,10},{10,10},{11,11},{12,11},{13,12},{14,12},
            {15,13},{16,13},{17,14},{18,14}};
        for(unsigned i=0;i<sizeof line/sizeof line[0];i++)
            if(x==line[i][0] && y==line[i][1])expected=0xfedcba;
        static const unsigned char aa[][3]={{2,3,255},{3,3,128},{3,4,127},
            {4,4,255},{5,4,128},{5,5,127},{6,5,255}};
        for(unsigned i=0;i<sizeof aa/sizeof aa[0];i++)
            if(x==aa[i][0] && y==aa[i][1])expected=colour_mix(expected,0x75e912,aa[i][2]);
        if(x>=18&&x<22&&y>=1&&y<5) {
            static const unsigned char glyph_coverage[]={0,127,255,255};
            expected=colour_mix(expected,0x1122ee,glyph_coverage[(y-1)/2*2+(x-18)/2]);
        }
        if(y==7&&(x==18||x==20))expected=0x33ee55;
        if(y>=11) {
            /* Explicit original 9x8, radius-4 row masks. The hard frame's
             * top/bottom arcs keep one pixel per row, unlike a circular ring. */
            unsigned fill=y==11||y==18?0xfeu:0x1ffu;
            unsigned frame=y==11||y==18?0x92u:0x101u;
            if(x<9 && (fill&(1u<<x)))expected=0x9251ed;
            if(x>=13 && x<22 && (frame&(1u<<(x-13))))expected=0xed5192;
        }
        if(pixels[y*23+x]!=expected) {
            if(ok) {
                char note[160];
                snprintf(note,sizeof note,"widget gate first mismatch: x=%d y=%d actual=%#x expected=%#x",
                         x,y,pixels[y*23+x],expected);
                log_write(3,"gui-gpu",note);
            }
            ok=false;
        }
    }
    gui_gpu_detach(&source);gui_gpu_detach(&destination);
    passed=ok;
    if(!ok)batch.failed=true;
    log_write(ok?1:3,"gui-gpu",ok
        ?"native GPU widget gate PASS: 437 VRAM pixels, gradient + mask + scaled alpha composition + GPU hard/AA lines + scaled A8/bitmap glyphs + hard rounded fill/frame"
        :"native GPU widget gate FAILED; refusing to claim accelerated desktop");
    return ok;
}
