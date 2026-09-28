/* display.c - owning the screen.
 *
 * The framebuffer is mapped once and never drawn into directly by widgets:
 * everything renders into a back buffer, and only the rectangles that actually
 * changed are copied forward.  Video memory is uncached, so a full-screen copy
 * every frame would be far slower than tracking damage.
 */
#include "gui.h"

static colour_t to_native(const kframebuffer_t *fb, colour_t c) {
    /* The palette is 0x00RRGGBB; the panel may want the channels elsewhere. */
    return ((colour_t)RGB_R(c) << fb->red_shift)
         | ((colour_t)RGB_G(c) << fb->green_shift)
         | ((colour_t)RGB_B(c) << fb->blue_shift);
}

/* True when the framebuffer already uses the layout the toolkit stores, in
 * which case presenting is a straight memcpy per row. */
static bool layout_matches(const kframebuffer_t *fb) {
    return fb->red_shift == 16 && fb->green_shift == 8 && fb->blue_shift == 0;
}

static bool display_open_backend(display_t *d, bool native_widgets) {
    memset(d, 0, sizeof *d);

    long r = syscall6(SYS_FRAMEBUFFER, (long)&d->info, 0, 0, 0, 0, 0);
    if (r < 0) {
        char note[96];
        snprintf(note,sizeof note,"desktop framebuffer acquisition failed: syscall=%ld",r);
        log_write(3,"display",note);
        /* Busy means another program has the screen - which is a different
         * problem from there being no screen, and the caller can say so. */
        errno = (r == -EBUSY) ? EBUSY : ENODEV;
        return false;
    }
    if (d->info.bpp != 32) {
        fprintf(STDERR_FD, "gui: the display is %u bits per pixel; 32 is required\n", d->info.bpp);
        return false;
    }

    /* How large one interface pixel should be on this screen, decided before
     * anything is laid out.  A caller that wants something else says so after
     * this returns. */
    gui_set_default_scale(gui_scale_for_width((int)d->info.width));

    d->screen.pixels = (colour_t *)(uintptr_t)d->info.address;
    d->screen.width  = (int)d->info.width;
    d->screen.height = (int)d->info.height;
    d->screen.stride = (int)(d->info.pitch / sizeof(colour_t));
    d->screen.owns_pixels = false;

    bool gpu=native_widgets && gpu_can_draw()==2;
    if(gpu) {
        if(!gui_gpu_validate()){errno=EIO;return false;}
        log_write(1,"gui-gpu","desktop stage: allocate full-resolution GPU backbuffer");
    }
    d->back = surface_create_target(d->screen.width, d->screen.height, gpu);
    if (!d->back) {
        log_write(3,"display","desktop backbuffer allocation failed");
        errno = gpu?EIO:ENOMEM; return false;
    }
    if (d->back->gpu) log_write(1,"gui-gpu","desktop widget raster and compositor: native GPU surfaces");

    d->input_fd = open("/dev/input", O_RDONLY);
    if (d->input_fd < 0) {
        char note[96];
        snprintf(note,sizeof note,"desktop input open failed: errno=%d",errno);
        log_write(3,"display",note);
        fprintf(STDERR_FD, "gui: cannot open /dev/input: %s\n", strerror(errno));
        surface_destroy(d->back);
        d->back = NULL;
        return false;
    }

    int pos[2] = { 0, 0 };
    d->have_mouse = syscall6(SYS_MOUSEPOS, (long)pos, 0, 0, 0, 0, 0) == 1;

    /* Is there a driver on the other end?  Asking is simply trying: a display
     * that is nobody's to update answers that it cannot. */
    d->driver_updates = fb_update(0, 0, 1, 1) == 0;

    if (d->driver_updates) {
        /* And will it draw the pointer?  Offering it the real one and seeing
         * whether it is taken is both the test and the setting-up. */
        static kcursor_t pointer;
        gui_cursor_image(pointer.pixels, &pointer.width, &pointer.height,
                         &pointer.hot_x, &pointer.hot_y);
        d->hardware_cursor = fb_set_cursor(&pointer) == 0;
    }

    if(native_widgets) log_write(1,"display","desktop startup resources ready; awaiting first presentation");
    return true;
}

bool display_open(display_t *d) { return display_open_backend(d,false); }
bool display_open_gpu(display_t *d) { return display_open_backend(d,true); }

/* Change resolution without restarting.
 *
 * The firmware picked a mode before any of this ran, and for a long time that
 * was the end of it: the only way to a different one was to write it into the
 * loader's configuration and start again. With the adapter being driven, the
 * mode is something that can simply be set - so the framebuffer moves, changes
 * shape, and everything drawn against the old one has to be rebuilt.
 */
bool display_set_mode(display_t *d, int width, int height) {
    char note[160];

    if (!d->driver_updates) {
        log_write(2, "display", "no driver is running the adapter, so the mode "
                                "cannot be changed while running");
        return false;
    }
    if (width < 640 || height < 480) return false;
    if (width == d->screen.width && height == d->screen.height) return true;

    /* Prepare drawing storage before changing the hardware. An allocation
     * failure must leave both the current screen and its backbuffer intact. */
    surface_t *fresh = surface_create_target(width, height, d->back && d->back->gpu);
    if (!fresh) {
        snprintf(note, sizeof note, "no memory for a %d x %d drawing surface", width, height);
        log_write(3, "display", note);
        return false;
    }

    if (fb_set_mode((unsigned)width, (unsigned)height) < 0) {
        snprintf(note, sizeof note,
                 "the adapter refused %d x %d: %s", width, height, strerror(errno));
        log_write(2, "display", note);
        surface_destroy(fresh);
        return false;
    }

    /* Ask again: the address, the pitch and the size are all the adapter's to
     * decide, and taking the old ones on trust would draw into the wrong
     * place. */
    kframebuffer_t info;
    if (syscall6(SYS_FRAMEBUFFER, (long)&info, 0, 0, 0, 0, 0) < 0) {
        log_write(3, "display", "the new mode could not be mapped");
        surface_destroy(fresh);
        return false;
    }

    d->info = info;
    d->screen.pixels = (colour_t *)(uintptr_t)info.address;
    d->screen.width  = (int)info.width;
    d->screen.height = (int)info.height;
    d->screen.stride = (int)(info.pitch / sizeof(colour_t));

    snprintf(note, sizeof note, "now running at %d x %d, %d bytes a line",
             d->screen.width, d->screen.height, (int)info.pitch);
    log_write(1, "display", note);
    if (d->back) surface_destroy(d->back);
    d->back = fresh;

    /* The pointer belongs to the adapter and does not survive the change. */
    if (d->hardware_cursor) {
        static kcursor_t pointer;
        gui_cursor_image(pointer.pixels, &pointer.width, &pointer.height,
                         &pointer.hot_x, &pointer.hot_y);
        d->hardware_cursor = fb_set_cursor(&pointer) == 0;
    }
    return true;
}

/* Re-hand the current pointer SHAPE to the adapter.  The window manager calls
 * this when gui_set_cursor changed the shape (its hover moved onto a window
 * edge, a text field, ...), so the hardware pointer updates without redrawing
 * the frame.  A no-op when this code draws the pointer itself. */
void display_refresh_cursor(display_t *d) {
    if (!d || !d->hardware_cursor) return;
    static kcursor_t pointer;
    gui_cursor_image(pointer.pixels, &pointer.width, &pointer.height,
                     &pointer.hot_x, &pointer.hot_y);
    fb_set_cursor(&pointer);
}

void display_close(display_t *d) {
    if (d->back) surface_destroy(d->back);
    if (d->input_fd >= 0) close(d->input_fd);
    d->back = NULL;
    d->input_fd = -1;
}

/* How fast the CPU-visible framebuffer mapping accepts writes.
 *
 * Every optimisation to the compositor is worth exactly nothing if the copy
 * out to the screen is the whole cost, and whether it is depends on something
 * invisible from here: the memory type the page tables gave that mapping.
 * Write-combining gathers whole cache lines before they leave the processor;
 * uncached sends every store on its own, which is roughly twenty times slower.
 * The two are indistinguishable from the code - so this measures it.
 *
 * It is also measured two ways, because they answer different questions: one
 * long run says what the memory can do, and a row at a time says what the
 * compositor will actually get, since damage is rectangular and rows are a
 * stride apart. */
void display_measure(display_t *d, char *out, size_t cap) {
    if (d->back && d->back->gpu) {
        snprintf(out,cap,"native GPU widget painting and VRAM composition; no CPU framebuffer benchmark");
        return;
    }
    if (!d->back || !d->screen.pixels) { snprintf(out, cap, "no display"); return; }

    int rows = 300;
    if (rows > d->screen.height) rows = d->screen.height;
    int base = d->screen.height - rows;
    int full = d->screen.width;
    int narrow = full / 3;

    /* Enough repetitions that the timing is not one microsecond of noise. */
    const int passes = 8;

    /* A rectangle the full width of the screen.  When the stride is the width -
     * which it is here - every row runs straight into the next, so this is one
     * long contiguous write and write-combining has nothing to interrupt it. */
    uint64_t started = uptime_us();
    for (int pass = 0; pass < passes; pass++)
        for (int y = 0; y < rows; y++)
            blit_present_row(d->screen.pixels + (size_t)(base + y) * d->screen.stride,
                             d->back->pixels + (size_t)(base + y) * d->back->stride,
                             full);
    blit_present_done();
    uint64_t wide_us = uptime_us() - started;

    /* And a narrow one, which is the shape most damage really has: every row
     * ends and the next begins a stride away. */
    started = uptime_us();
    for (int pass = 0; pass < passes; pass++)
        for (int y = 0; y < rows; y++)
            blit_present_row(d->screen.pixels + (size_t)(base + y) * d->screen.stride + 64,
                             d->back->pixels + (size_t)(base + y) * d->back->stride + 64,
                             narrow);
    blit_present_done();
    uint64_t narrow_us = uptime_us() - started;

    unsigned long long wide_bytes = (unsigned long long)passes * rows * full * 4ULL;
    unsigned long long narrow_bytes = (unsigned long long)passes * rows * narrow * 4ULL;

    const char *memory = gpu_can_draw() == 2
                       ? "logical compositor memory"
                       : "video memory";
    snprintf(out, cap,
             "%s: %llu MB/s across the full width, %llu MB/s in "
             "%d-pixel columns (%llu and %llu ns per pixel)",
             memory,
             wide_us ? wide_bytes / wide_us : 0,
             narrow_us ? narrow_bytes / narrow_us : 0,
             narrow,
             wide_us * 1000ULL / ((unsigned long long)passes * rows * full),
             narrow_us * 1000ULL / ((unsigned long long)passes * rows * narrow));
}

void display_present(display_t *d, rect_t area) {
    rect_t a = rect_intersection(area, rect_make(0, 0, d->screen.width, d->screen.height));
    if (rect_empty(a)) return;
    if (d->back && d->back->gpu) {
        gui_gpu_present(d->back,a);
        return;
    }

    if (layout_matches(&d->info)) {
        /* Hand it to the system to spread across every processor.
         *
         * This copy is the largest single thing a desktop does per frame - at
         * 3840 by 2160 it moves thirty-two megabytes - and doing it from this
         * one thread leaves the rest of the machine idle while it happens.
         * The system splits it into bands, one per processor, and returns when
         * they are all done.
         *
         * A machine with nothing else running answers that it did not, and the
         * loop below does the work instead: this is an optimisation, not a
         * requirement, and a display that only ever had one processor to draw
         * with must not stop working because of it. */
        /* Whichever is actually faster on this machine, decided by timing
         * both rather than by assuming.
         *
         * Handing the copy to the system spreads it across every processor,
         * which on a large screen with many cores should win easily.  It was
         * not winning: for a long time the call was malformed and always
         * failed, so the loop below did the work and nothing said otherwise.
         * With the call fixed, this machine turned out to be a hundred times
         * SLOWER through the system than through the loop - so "spread it
         * across the processors" is not a thing to do unconditionally either.
         *
         * Both are correct.  The only question is which is quicker here, and
         * that is a question with a measurable answer, so it is measured: the
         * first few frames try each way and the faster one is kept. */
        static int      trials;
        static uint64_t us_system, px_system, us_loop, px_loop;
        static bool     use_system;

        /* The card, where there is one that will draw.
         *
         * This is the difference between a card that SHOWS the picture and a
         * card that MAKES it.  On the path where the display reads out of
         * memory this system owns, the frame already reaches the screen
         * without being copied into video memory - but the processor still
         * assembles it.  Handing the whole frame over as a picture is the step
         * that moves the last of that onto the card.
         *
         * The whole frame rather than the damaged part, because a picture has
         * to be a rectangle of its own with no gaps in it, and a damaged
         * rectangle inside a larger buffer has the rest of each row in
         * between.  Copying it out to make it contiguous would be the copy
         * this exists to avoid.
         *
         * Asked once.  A card that will not draw at start-up will not start
         * during a frame, and a syscall per frame to be told so is a syscall
         * per frame wasted. */
        static int card_state;       /* 0 unknown, 1 optional, 2 owns scanout, -1 no */
        if (!card_state) {
            int can = gpu_can_draw();
            card_state = can > 0 ? can : -1;
        }

        /* A damaged rectangle rather than the whole frame, so what has to fit
         * is what changed. */
        bool card_fits = card_state > 0 &&
                         (card_state == 2 || (a.w <= 1024 && a.h <= 1024));

        /* Measured against the processor, not assumed better than it.
         *
         * It works: the card takes the frame and draws it, and the log says so
         * the first time.  It is also, on this adapter, about two hundred
         * times slower - and the reason is structural rather than a detail to
         * be tuned away.  Handing a picture over means putting it into a
         * surface the card owns, and putting it there means sending the whole
         * surface across for every frame.  The frame then crosses the boundary
         * once to reach the card and once more to be drawn, where copying it
         * straight to the screen crosses once.
         *
         * A copy into the picture was walking pixels rather than rows, which
         * was worth fixing on its own and changed almost nothing here - which
         * is what said the cost is the crossing and not the copying.
         *
         * So it is timed and kept only if it wins.  The measurement stays
         * rather than a decision, because an adapter that can be handed a
         * picture without sending the whole surface would win, and this should
         * notice that rather than have been told the answer once by me. */
        static int      card_trials;
        static uint64_t us_card, px_card;
        static bool     use_card;
        bool card_measuring = card_fits && card_trials < 4;

        if (card_fits && (use_card || card_measuring)) {
            /* Only what changed.  The card is told where in the frame it
             * came from, so nothing has to be copied out to make it
             * contiguous first. */
            kimage_t frame;
            frame.pixels = (const unsigned int *)
                (d->back->pixels + (size_t)a.y * d->back->stride + a.x);
            frame.width  = (unsigned)a.w;
            frame.height = (unsigned)a.h;
            frame.x = a.x;
            frame.y = a.y;
            frame.draw_width  = (unsigned)a.w;
            frame.draw_height = (unsigned)a.h;
            frame.source_stride = (unsigned)d->back->stride;

            uint64_t began = card_measuring ? uptime_us() : 0;
            int drew = gpu_image(&frame);

            /* NVIDIA value 2 means the card's VRAM surface is the only visible
             * scanout.  A successful upload is therefore the required present,
             * not an optional optimisation to benchmark against stale GOP RAM. */
            if (drew == 0 && card_state == 2) {
                static bool announced_native_present;
                if (!announced_native_present) {
                    announced_native_present = true;
                    log_write(1, "display", "desktop damage is reaching every native NVIDIA scanout through CAB5");
                }
                if (d->driver_updates) fb_update(a.x, a.y, a.w, a.h);
                return;
            }

            if (drew == 0 && card_measuring) {
                us_card += uptime_us() - began;
                px_card += (uint64_t)a.w * (uint64_t)a.h;
                card_trials++;
            }

            if (drew == 0 && !card_measuring) {
                if (d->driver_updates) fb_update(a.x, a.y, a.w, a.h);
                return;
            }
            if (drew != 0 && card_state == 2) {
                /* The CPU-visible logical canvas is not the native scanout.
                 * Never silently switch to writing that shadow after an engine
                 * failure and make a frozen desktop appear successful. */
                static bool reported_native_failure;
                if (!reported_native_failure) {
                    reported_native_failure = true;
                    log_write(3, "display", "native NVIDIA presentation failed; retaining the last visible frame (no invisible CPU fallback)");
                }
                return;
            }
            if (drew != 0) card_state = -1;      /* optional adapter only */
        }

        bool measuring = trials < 8;
        bool try_system = measuring ? (trials % 2 == 0) : use_system;

        if (try_system) {
            kpresent_t q;
            q.back   = (unsigned long long)(uintptr_t)d->back->pixels;
            q.stride = (unsigned)d->back->stride;
            q.x = a.x; q.y = a.y; q.w = a.w; q.h = a.h;

            uint64_t began = measuring ? uptime_us() : 0;
            int ok = fb_present(&q);
            if (ok == 0) {
                if (measuring) {
                    /* Time and pixels kept apart and divided once at the end.
                     * Dividing per sample threw the answer away: a pass that
                     * takes a hundred microseconds over six hundred thousand
                     * pixels is a fraction of a nanosecond each, which in whole
                     * numbers is zero, and four zeroes still say nothing. */
                    us_system += uptime_us() - began;
                    px_system += (uint64_t)a.w * (uint64_t)a.h;
                    trials++;
                }
                if (d->driver_updates) fb_update(a.x, a.y, a.w, a.h);
                return;
            }
            /* It would not: stop asking. */
            if (measuring) { trials = 8; use_system = false; }
        }

        if (measuring) {
            uint64_t began = uptime_us();
            for (int y = a.y; y < a.y + a.h; y++) {
                const colour_t *src = d->back->pixels + (size_t)y * d->back->stride + a.x;
                colour_t *dst = d->screen.pixels + (size_t)y * d->screen.stride + a.x;
                blit_present_row(dst, src, a.w);
            }
            us_loop += uptime_us() - began;
            px_loop += (uint64_t)a.w * (uint64_t)a.h;
            trials++;

            if (trials >= 8) {
                /* Picoseconds a pixel, so every number has digits in it. */
                uint64_t ps_system = px_system ? us_system * 1000000 / px_system : 0;
                uint64_t ps_loop   = px_loop   ? us_loop   * 1000000 / px_loop   : 0;
                uint64_t ps_card   = px_card   ? us_card   * 1000000 / px_card   : 0;

                use_system = ps_system && ps_loop && ps_system < ps_loop;
                use_card   = ps_card && ps_loop && ps_card < ps_loop;
                if (use_card) use_system = false;

                char line[240];
                snprintf(line, sizeof line,
                         "the frame reaches the screen %s: %llu ps a pixel "
                         "drawn by the card, %llu across the processors, "
                         "%llu in one pass",
                         use_card   ? "drawn by the graphics card"
                         : use_system ? "spread across the processors"
                                      : "in one pass, which is fastest here",
                         (unsigned long long)ps_card,
                         (unsigned long long)ps_system,
                         (unsigned long long)ps_loop);
                log_write(1, "display", line);
            }

            if (d->driver_updates) fb_update(a.x, a.y, a.w, a.h);
            return;
        }

        /* Straight into video memory, which is mapped write-combining: the
         * stores here bypass the cache, because nothing will ever read these
         * pixels back and filling a write-combining buffer in one go is what
         * makes the transfer fast. */
        for (int y = a.y; y < a.y + a.h; y++) {
            const colour_t *src = d->back->pixels + (size_t)y * d->back->stride + a.x;
            colour_t *dst = d->screen.pixels + (size_t)y * d->screen.stride + a.x;
            blit_present_row(dst, src, a.w);
        }
        if (d->driver_updates) fb_update(a.x, a.y, a.w, a.h);
        return;
    }

    for (int y = a.y; y < a.y + a.h; y++) {
        const colour_t *src = d->back->pixels + (size_t)y * d->back->stride + a.x;
        colour_t *dst = d->screen.pixels + (size_t)y * d->screen.stride + a.x;
        for (int x = 0; x < a.w; x++) dst[x] = to_native(&d->info, src[x]);
    }
    if (d->driver_updates) fb_update(a.x, a.y, a.w, a.h);
}

void display_present_all(display_t *d) {
    display_present(d, rect_make(0, 0, d->screen.width, d->screen.height));
}

bool display_wait_event(display_t *d, kinput_event_t *ev) {
    ssize_t n = read(d->input_fd, ev, sizeof *ev);
    return n == (ssize_t)sizeof *ev;
}

/* Sleep until there is something to read, or until the timeout runs out.
 *
 * This is what lets the loop be idle and responsive at the same time: it costs
 * nothing while nothing is happening, and the moment the mouse moves the loop
 * is running again.  Polling with a fixed sleep instead puts that sleep
 * between every movement and the screen catching up. */
bool display_wait_input(display_t *d, int timeout_ms) {
    uint32_t ms = timeout_ms > 0 ? (uint32_t)timeout_ms : 0;
    if (ioctl(d->input_fd, 3, &ms) < 0) {
        sleep_ms(timeout_ms > 0 ? timeout_ms : 1);
        return false;
    }
    return ms != 0;
}

bool display_poll_event(display_t *d, kinput_event_t *ev) {
    uint32_t pending = 0;
    if (ioctl(d->input_fd, 1, &pending) < 0) return false;
    if (!pending) return false;
    return display_wait_event(d, ev);
}

/* Hand the screen back to the console while keeping the mapping, so that
 * taking it again costs nothing.  The pixels stay where they are; what changes
 * is who is allowed to write them. */
void display_give_up(display_t *d) {
    (void)d;
    syscall6(SYS_FRAMEBUFFER, 0, FB_RELEASE, 0, 0, 0, 0);
}

bool display_take(display_t *d) {
    (void)d;
    return syscall6(SYS_FRAMEBUFFER, 0, FB_REACQUIRE, 0, 0, 0, 0) == 0;
}
