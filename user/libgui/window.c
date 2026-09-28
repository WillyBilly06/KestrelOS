/* window.c - the window manager and its event loop. */
#include "window.h"

/* ------------------------------------------------------------------ layout */

rect_t wm_client_rect(const window_t *w) {
    return rect_make(w->frame.x + WIN_BORDER,
                     w->frame.y + WIN_TITLE_H,
                     w->frame.w - 2 * WIN_BORDER,
                     w->frame.h - WIN_TITLE_H - WIN_BORDER);
}

/* DPI/accessibility scale is independent of window size. Maximizing provides
 * more layout space; it must not unexpectedly double/triple the font and
 * controls, change hit targets, or hide content at a size threshold. */
static int window_content_scale(const window_t *w) {
    (void)w;
    return gui_screen_scale();
}

/* Dispatch one event to a window's own handler with that window's content scale
 * in force, so the size an app paints at and the size it hit-tests at are the
 * same one.  EVERY call into w->proc goes through here - a paint or a click that
 * bypassed it would read a different scale and land in the wrong place. */
static bool win_proc(window_t *w, const wevent_t *ev) {
    if (!w || !w->proc) return false;
    int prev = gui_scale_push(window_content_scale(w));
    bool r = w->proc(w, ev);
    gui_scale_pop(prev);
    return r;
}

/* The three frame buttons, right-aligned in the title bar.  The painter lays
 * them out with the same arithmetic, so the two cannot drift apart. */
static rect_t button_rect_at(rect_t frame, int index) {
    int sc = gui_scale();
    int size = WIN_TITLE_H - 12 * sc;
    int x = frame.x + frame.w - WIN_BORDER - 7 * sc - (index + 1) * (size + 5 * sc);
    return rect_make(x, frame.y + 6 * sc, size, size);
}

static rect_t button_rect(const window_t *w, int index) {
    return button_rect_at(w->frame, index);
}

/* ------------------------------------------------------------------ z-order */

/* Remove a window from the z-order chain.  A window that was never linked in
 * must be left alone: unlinking it would clear the list's head and tail and
 * take every other window with it. */
static void unlink_window(wm_t *wm, window_t *w) {
    if (!w->linked) return;

    if (w->below) w->below->above = w->above;
    else wm->bottom = w->above;
    if (w->above) w->above->below = w->below;
    else wm->top = w->below;

    w->above = w->below = NULL;
    w->linked = false;
}

static void raise_window(wm_t *wm, window_t *w) {
    if (wm->top == w) return;
    unlink_window(wm, w);

    w->below = wm->top;
    w->above = NULL;
    if (wm->top) wm->top->above = w;
    wm->top = w;
    if (!wm->bottom) wm->bottom = w;
    w->linked = true;
}

window_t *wm_window_at(wm_t *wm, int x, int y) {
    for (window_t *w = wm->top; w; w = w->below) {
        if (!w->visible || w->state == WIN_MINIMISED) continue;
        /* One that is leaving is still drawn, but it is no longer somewhere a
         * click can land: the user has already dismissed it. */
        if (w->dying) continue;
        if (rect_contains(w->frame, x, y)) return w;
    }
    return NULL;
}

int wm_window_index(wm_t *wm, window_t *w) {
    int i = 0;
    for (window_t *p = wm->bottom; p; p = p->above, i++)
        if (p == w) return i;
    return -1;
}

window_t *wm_window_by_index(wm_t *wm, int index) {
    int i = 0;
    for (window_t *p = wm->bottom; p; p = p->above, i++)
        if (i == index) return p;
    return NULL;
}

/* A rectangle scaled about its own centre, in thousandths.  This is the shape
 * an opening window starts from and a closing one ends at. */
static rect_t scaled_about_centre(rect_t r, int scale) {
    int w = r.w * scale / ANIM_ONE;
    int h = r.h * scale / ANIM_ONE;
    return rect_make(r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h);
}

/* --------------------------------------------------------------- animation */

#define ANIM_DEFAULT_MS 180

/* A window arriving is not transparent at all.
 *
 * It used to fade up from nothing, and for most of those 180ms what showed
 * through it was whatever it had just covered - the desktop's icon labels, the
 * text of the window underneath.  Two pieces of text on top of each other are
 * unreadable rather than pretty, and that is exactly what "some words got
 * overlaid each other" turned out to be.
 *
 * Two thirds was tried first and was still legible through, which is the tell:
 * any transparency at all over text is too much.  So the arrival is carried
 * entirely by the motion - the window still grows the last little way into
 * place - and nothing ever shows through it.
 *
 * Closing is exempt and still fades away.  A window on its way out has nothing
 * left to read, and following it down reads better than having it vanish. */
#define ANIM_ARRIVE_ALPHA 255
#define ANIM_CLOSE_MS   140
#define ANIM_MOVE_MS    160

static rect_t window_visual_bounds(rect_t frame);
static bool window_paint_visible(wm_t *wm, window_t *w);

void wm_animate(wm_t *wm, window_t *w, anim_kind kind, rect_t from, rect_t to,
                int alpha_from, int alpha_to, int duration_ms) {
    if (!wm->animations_on) {
        /* Turned off: the end state is applied at once. */
        if (kind != ANIM_CLOSE) w->frame = to;
        w->anim.kind = ANIM_NONE;
        wm_damage_all(wm);
        return;
    }

    w->anim.kind = kind;
    w->anim.started_ms = uptime_ms();
    w->anim.duration_ms = duration_ms ? duration_ms : ANIM_DEFAULT_MS;
    w->anim.from = from;
    w->anim.to = to;
    w->anim.alpha_from = alpha_from;
    w->anim.alpha_to = alpha_to;
    w->anim.paint_progress = 0;
    w->anim.damage_frame = from;
    wm->animating = true;
    wm_damage_all(wm);
}

bool wm_is_animating(wm_t *wm) {
    for (window_t *w = wm->bottom; w; w = w->above)
        if (w->anim.kind != ANIM_NONE) return true;
    return false;
}

/* How far through an animation a window is, from 0 to ANIM_ONE, with the
 * easing already applied.  Returns ANIM_ONE once it is over. */
static int anim_progress(const window_t *w) {
    if (w->anim.kind == ANIM_NONE) return ANIM_ONE;
    uint64_t now = uptime_ms();
    uint64_t elapsed = now - w->anim.started_ms;
    if ((int)elapsed >= w->anim.duration_ms) return ANIM_ONE;

    int t = (int)(elapsed * ANIM_ONE / (uint64_t)w->anim.duration_ms);
    /* Opening and restoring settle; closing and minimising accelerate away,
     * which is what makes the two read as opposites rather than as the same
     * motion played backwards. */
    switch (w->anim.kind) {
    case ANIM_CLOSE:
    case ANIM_MINIMISE:  return anim_ease_in(t);
    case ANIM_GEOMETRY:  return anim_ease_in_out(t);
    default:             return anim_ease_out(t);
    }
}

/* Advance every animation.  Returns true if any is still running. */
static bool step_animations(wm_t *wm) {
    bool any = false;
    window_t *next = NULL;

    for (window_t *w = wm->bottom; w; w = next) {
        next = w->above;
        if (w->anim.kind == ANIM_NONE) continue;

        int progress = anim_progress(w);
        if (progress < ANIM_ONE) {
            /* Use exactly this position throughout the repaint. Sampling time
             * again while painting separate damaged rectangles can split a
             * moving window across positions, leaving clipped pixels/trails. */
            w->anim.paint_progress = progress;
            wm_damage(wm, window_visual_bounds(w->anim.damage_frame));
            w->anim.damage_frame = anim_lerp_rect(w->anim.from, w->anim.to, progress);
            wm_damage(wm, window_visual_bounds(w->anim.damage_frame));
            /* Allocation failure in paint_animating retains the existing
             * unscaled-frame fallback. Include that footprint as well, so a
             * failed/recovered staging allocation cannot leave pixels behind. */
            wm_damage(wm, window_visual_bounds(w->frame));
            any = true;
            continue;
        }

        /* Finished. */
        anim_kind kind = w->anim.kind;
        w->anim.kind = ANIM_NONE;

        switch (kind) {
        case ANIM_CLOSE:
            /* The window was kept alive only to be seen leaving. */
            wm_close(wm, w);
            break;
        case ANIM_MINIMISE:
            w->state = WIN_MINIMISED;
            break;
        default:
            w->frame = w->anim.to;
            w->needs_paint = true;
            break;
        }
        wm_damage_all(wm);
    }
    return any;
}

/* ------------------------------------------------------------------ damage */

/* How much bigger the union of two rectangles is than the two of them.  Two
 * rectangles are worth merging when joining them costs less than the overhead
 * of drawing a second one, and worth keeping apart when it does not. */
static long merge_waste(rect_t a, rect_t b) {
    rect_t u = rect_union(a, b);
    long joined = (long)u.w * u.h;
    long apart = (long)a.w * a.h + (long)b.w * b.h;
    return joined - apart;
}

void wm_damage(wm_t *wm, rect_t r) {
    if (wm->full_redraw) return;
    if (r.w <= 0 || r.h <= 0) return;

    /* Already covered by something in the list? */
    for (int i = 0; i < wm->damage_count; i++) {
        rect_t d = wm->damage[i];
        if (r.x >= d.x && r.y >= d.y &&
            r.x + r.w <= d.x + d.w && r.y + r.h <= d.y + d.h)
            return;
    }

    /* Join it to a neighbour when that costs almost nothing - overlapping or
     * touching rectangles drawn separately would paint the shared part twice,
     * which is worse than one slightly larger rectangle. */
    for (int i = 0; i < wm->damage_count; i++) {
        if (merge_waste(wm->damage[i], r) <= 4096) {
            wm->damage[i] = rect_union(wm->damage[i], r);
            return;
        }
    }

    if (wm->damage_count < WM_MAX_DAMAGE) {
        wm->damage[wm->damage_count++] = r;
        return;
    }

    /* The list is full, so two of them have to become one.  Whichever pair
     * wastes least is the pair to join. */
    int best_a = 0, best_b = 1;
    long best = merge_waste(wm->damage[0], wm->damage[1]);
    for (int i = 0; i < wm->damage_count; i++) {
        for (int j = i + 1; j < wm->damage_count; j++) {
            long waste = merge_waste(wm->damage[i], wm->damage[j]);
            if (waste < best) { best = waste; best_a = i; best_b = j; }
        }
    }
    /* And joining the new one to an existing rectangle may be cheaper still. */
    for (int i = 0; i < wm->damage_count; i++) {
        if (merge_waste(wm->damage[i], r) < best) {
            wm->damage[i] = rect_union(wm->damage[i], r);
            return;
        }
    }

    wm->damage[best_a] = rect_union(wm->damage[best_a], wm->damage[best_b]);
    wm->damage[best_b] = wm->damage[wm->damage_count - 1];
    wm->damage_count--;
    wm->damage[wm->damage_count++] = r;
}

bool wm_has_damage(const wm_t *wm) {
    return wm->full_redraw || wm->damage_count > 0;
}

/* Allocation is the only fallible part of a geometry change. Never publish a
 * new frame/state while its canvas still belongs to the previous dimensions. */
static bool window_prepare_geometry(window_t *w, rect_t frame, surface_t **fresh) {
    *fresh = NULL;
    if (frame.w <= 2 * WIN_BORDER || frame.h <= WIN_TITLE_H + WIN_BORDER)
        return false;
    int cw = frame.w - 2 * WIN_BORDER, ch = frame.h - WIN_TITLE_H - WIN_BORDER;
    if (w->canvas && w->canvas->width == cw && w->canvas->height == ch)
        return true;
    *fresh = surface_create_target(cw, ch, w->wm->display->back->gpu != NULL);
    return *fresh != NULL;
}

static bool window_set_geometry(window_t *w, rect_t frame, rect_t restore, win_state state) {
    surface_t *fresh;
    if (!window_prepare_geometry(w, frame, &fresh)) return false;
    wm_damage(w->wm, window_visual_bounds(w->frame));
    surface_t *old = w->canvas;
    w->frame = frame;
    w->restore = restore;
    w->state = state;
    if (fresh) w->canvas = fresh;
    w->needs_paint = true;
    wm_damage(w->wm, window_visual_bounds(frame));
    if (fresh) {
        if (old) surface_destroy(old);
        wevent_t ev = { .kind = WE_RESIZE, .x = fresh->width, .y = fresh->height };
        win_proc(w, &ev);
    }
    return true;
}

/* Move to a different resolution while running.
 *
 * The screen changing size is not a repaint - it is every window potentially
 * off the edge of a smaller screen, a work area of a different shape, and a
 * back buffer that no longer matches.  All of that has to be settled before
 * anything is drawn again. */
static rect_t window_fit_area(rect_t frame, rect_t area) {
    if (frame.w > area.w) frame.w = area.w;
    if (frame.h > area.h) frame.h = area.h;
    if ((int64_t)frame.x + frame.w > (int64_t)area.x + area.w)
        frame.x = area.x + area.w - frame.w;
    if ((int64_t)frame.y + frame.h > (int64_t)area.y + area.h)
        frame.y = area.y + area.h - frame.h;
    if (frame.x < area.x) frame.x = area.x;
    if (frame.y < area.y) frame.y = area.y;
    return frame;
}

bool wm_set_mode(wm_t *wm, int width, int height) {
    if (!wm || !wm->display || !wm->display->back || width < 640 || height < 480)
        return false;
    if (wm->display->back->width == width && wm->display->back->height == height)
        return true;

    rect_t area;
    if (wm->query_work_area) area = wm->query_work_area(wm, width, height);
    else {
        /* Preserve the existing shell reservations without calling a
         * state-mutating resize notification during preparation. */
        rect_t old = wm->work_area;
        int64_t right = (int64_t)wm->display->back->width - old.x - old.w;
        int64_t bottom = (int64_t)wm->display->back->height - old.y - old.h;
        if (right < 0) right = 0;
        if (bottom < 0) bottom = 0;
        int64_t aw = (int64_t)width - old.x - right;
        int64_t ah = (int64_t)height - old.y - bottom;
        if (aw <= 0 || aw > width || ah <= 0 || ah > height) return false;
        area = rect_make(old.x, old.y, (int)aw, (int)ah);
    }
    if (area.x < 0 || area.y < 0 || area.w <= 2 * WIN_BORDER ||
        area.h <= WIN_TITLE_H + WIN_BORDER ||
        (int64_t)area.x + area.w > width || (int64_t)area.y + area.h > height)
        return false;

    struct {
        window_t *window;
        rect_t frame, restore;
        surface_t *fresh;
        win_state state;
    } plan[WM_MAX_WINDOWS];
    int count = 0;
    for (window_t *win = wm->bottom; win; win = win->above) {
        if (count == WM_MAX_WINDOWS) goto failed;
        int i = count++;
        plan[i].window = win;
        plan[i].state = win->anim.kind == ANIM_MINIMISE ? WIN_MINIMISED : win->state;
        plan[i].frame = win->state == WIN_MAXIMISED ? area : window_fit_area(win->frame, area);
        plan[i].restore = window_fit_area(win->restore, area);
        plan[i].fresh = NULL;
        if (!window_prepare_geometry(win, plan[i].frame, &plan[i].fresh)) goto failed;
    }
    if (!display_set_mode(wm->display, width, height)) goto failed;

    /* Every required canvas now exists. Publish all windows before callbacks
     * can observe the topology; no surface allocation occurs after commit. */
    wm->work_area = area;
    wm->drag = wm->resize = NULL;
    for (int i = 0; i < count; i++) {
        window_t *win = plan[i].window;
        surface_t *old = win->canvas;
        win->frame = plan[i].frame; win->restore = plan[i].restore;
        win->state = plan[i].state;
        if (win->anim.kind != ANIM_CLOSE) win->anim.kind = ANIM_NONE;
        if (plan[i].fresh) {
            win->canvas = plan[i].fresh;
            if (old) surface_destroy(old);
        }
        win->needs_paint = true;
    }
    wm->animating = wm_is_animating(wm);
    if (wm->mouse_x < 0) wm->mouse_x = 0;
    if (wm->mouse_y < 0) wm->mouse_y = 0;
    if (wm->mouse_x >= width) wm->mouse_x = width - 1;
    if (wm->mouse_y >= height) wm->mouse_y = height - 1;
    if (wm->on_resize) wm->on_resize(wm, width, height);
    for (int i = 0; i < count; i++) {
        if (!plan[i].fresh) continue;
        /* An earlier callback may have closed another window. Only notify a
         * still-linked entry; never dereference a retained freed pointer. */
        for (window_t *win = wm->bottom; win; win = win->above) {
            if (win != plan[i].window) continue;
            wevent_t ev = { .kind = WE_RESIZE, .x = win->canvas->width, .y = win->canvas->height };
            win_proc(win, &ev);
            break;
        }
    }
    wm_damage_all(wm);
    return true;

failed:
    for (int i = 0; i < count; i++)
        if (plan[i].fresh) surface_destroy(plan[i].fresh);
    return false;
}

void wm_damage_all(wm_t *wm) {
    wm->full_redraw = true;
    wm->damage_count = 0;
}

/* ------------------------------------------------ moving pixels on the card
 *
 * Dragging a window used to redraw it: every pixel of it, and of the shadow
 * and the background around it, on every one of the many mouse events a drag
 * produces, all by the processor.  But the pixels of a window being dragged do
 * not change - they move.  A display adapter can move a rectangle of the
 * screen by itself, and the two adapters this system drives both offer to.
 *
 * WHY THIS IS SAFE, given the compositor keeps its frame in a back buffer
 * that the adapter knows nothing about:
 *
 *   The screen is only ever written from the back buffer for areas that were
 *   damaged, and an area is always repainted from scratch before it is
 *   presented.  So the back buffer being out of date somewhere the adapter has
 *   moved pixels cannot reach the screen: either that area stays undamaged and
 *   is never presented, or it is damaged, and then it is repainted first.
 *
 * WHAT IS NOT MOVED, and why:
 *
 *   The rounded corners and the soft shadow are not relocated pixels - they
 *   are drawn against whatever happens to be behind them, which is different
 *   in the new place.  Only the solid interior is moved, and the border and
 *   shadow around the new position are repainted as usual.
 *
 *   Nothing is moved when another window is above this one, because the
 *   rectangle on screen would then contain that window's pixels rather than
 *   this one's, and the adapter would faithfully move those too.
 */
static bool rect_covers(rect_t outer, rect_t inner);

static int accel_bits = -1;
static uint64_t adapter_moves, adapter_pixels;                   /* asked once, not every frame */

static bool adapter_can_move(void) {
    if (accel_bits < 0) {
        accel_bits = fb_accel();
        /* Said once, because "the adapter is not being used" and "the adapter
         * cannot do this" look identical from a frame counter. */
        log_write(1, "wm", accel_bits & FB_ACCEL_COPY
                  ? "the display adapter will move rectangles for the compositor"
                  : "the display adapter will not move rectangles; every pixel "
                    "is the processor's");
    }
    return (accel_bits & FB_ACCEL_COPY) != 0;
}

/* Damage everything in `outer` that `keep` does not cover, as up to four
 * rectangles.  Used to repaint what a move exposed without repainting what it
 * moved. */
static void damage_except(wm_t *wm, rect_t outer, rect_t keep) {
    rect_t hit = rect_intersection(outer, keep);
    if (rect_empty(hit)) { wm_damage(wm, outer); return; }

    if (hit.y > outer.y)
        wm_damage(wm, rect_make(outer.x, outer.y, outer.w, hit.y - outer.y));

    int below = (hit.y + hit.h);
    if (below < outer.y + outer.h)
        wm_damage(wm, rect_make(outer.x, below, outer.w,
                                outer.y + outer.h - below));

    if (hit.x > outer.x)
        wm_damage(wm, rect_make(outer.x, hit.y, hit.x - outer.x, hit.h));

    int right = (hit.x + hit.w);
    if (right < outer.x + outer.w)
        wm_damage(wm, rect_make(right, hit.y, outer.x + outer.w - right, hit.h));
}

/* Ask the adapter to move a window's interior from where it was to where it is
 * now.  Returns the rectangle it moved, or an empty one if it did not. */
static rect_t move_on_adapter(wm_t *wm, window_t *w, rect_t from, rect_t to) {
    rect_t none = rect_make(0, 0, 0, 0);
    /* Legacy FB_COPY maintains CPU shadows. GPU surfaces are moved by normal
     * resident-surface composition, never by that unrelated shadow path. */
    if (wm->display->back->gpu) return none;

    if (!adapter_can_move()) return none;
    if (wm->top != w) return none;              /* something is over it     */
    if (w->anim.kind != ANIM_NONE) return none;
    if (from.x == to.x && from.y == to.y) return none;

    rect_t src = rect_make(from.x + WIN_RADIUS, from.y + WIN_RADIUS,
                           from.w - 2 * WIN_RADIUS, from.h - 2 * WIN_RADIUS);
    rect_t dst = rect_make(to.x + WIN_RADIUS, to.y + WIN_RADIUS, src.w, src.h);

    if (src.w <= 0 || src.h <= 0) return none;

    /* Both ends have to be wholly on the screen: the adapter is given screen
     * coordinates and has no idea what is off the edge of it. */
    rect_t screen = rect_make(0, 0, wm->display->screen.width,
                              wm->display->screen.height);
    if (!rect_covers(screen, src) || !rect_covers(screen, dst)) return none;

    if (fb_copy(src.x, src.y, dst.x, dst.y, src.w, src.h) != 0) return none;

    /* FB_COPY moved the live scanout and the kernel's compositor shadow.  Keep
     * our own back buffer identical as well: the moved rectangle is excluded
     * from repaint damage below, so leaving this copy out resurrects the old
     * pixels on the next overlapping repaint. */
    surface_t *back = wm->display->back;
    if (dst.y > src.y) {
        for (int row = src.h - 1; row >= 0; row--)
            memmove(back->pixels + (size_t)(dst.y + row) * back->stride + dst.x,
                    back->pixels + (size_t)(src.y + row) * back->stride + src.x,
                    (size_t)src.w * sizeof(*back->pixels));
    } else {
        for (int row = 0; row < src.h; row++)
            memmove(back->pixels + (size_t)(dst.y + row) * back->stride + dst.x,
                    back->pixels + (size_t)(src.y + row) * back->stride + src.x,
                    (size_t)src.w * sizeof(*back->pixels));
    }

    adapter_moves++;
    adapter_pixels += (uint64_t)src.w * (uint64_t)src.h;
    return dst;
}

/* Put a window somewhere else.
 *
 * Pulled out of the mouse handler so that it is reachable by something other
 * than a person with a mouse - which matters here more than it usually would,
 * because the one path this system has for driving a real desktop cannot
 * reliably deliver a drag, and a moving window that only a person can produce
 * is a moving window nothing can check.
 */
static rect_t window_visual_bounds(rect_t frame) {
    /* Active shadow spread is scaled; include its downward offset and retain
     * the larger bound across focus changes. Match gui_soft_shadow's extent. */
    int spread=14*gui_screen_scale();
    return rect_make(frame.x-spread,frame.y-spread,
                     frame.w+2*spread,frame.h+2*spread+4);
}

static bool window_reaches_area(rect_t frame, rect_t area) {
    return rect_intersects(window_visual_bounds(frame),area);
}

void wm_move_window(wm_t *wm, window_t *w, int x, int y) {
    if (!wm || !w) return;

    rect_t was = w->frame;

    w->frame.x = x;
    w->frame.y = y;

    /* Keep the title bar reachable. */
    if (w->frame.y < 0) w->frame.y = 0;
    if (w->frame.y > wm->work_area.y + wm->work_area.h - WIN_TITLE_H)
        w->frame.y = wm->work_area.y + wm->work_area.h - WIN_TITLE_H;
    if (w->frame.x + w->frame.w < 60) w->frame.x = 60 - w->frame.w;
    if (w->frame.x > wm->display->back->width - 60)
        w->frame.x = wm->display->back->width - 60;

    if (was.x == w->frame.x && was.y == w->frame.y) return;

    /* The shadow reaches past the frame, hence the margins. */
    rect_t old_area = window_visual_bounds(was);
    rect_t new_area = window_visual_bounds(w->frame);

    /* Let the adapter move the pixels that only moved.  What it moved is then
     * left out of the damage, which is the whole saving: the processor
     * repaints the edges and what the window uncovered, and not the window. */
    rect_t moved = move_on_adapter(wm, w, was, w->frame);

    if (rect_empty(moved)) {
        wm_damage(wm, old_area);
        wm_damage(wm, new_area);
    } else {
        damage_except(wm, old_area, moved);
        damage_except(wm, new_area, moved);
    }
}

/* Whether the adapter has moved anything for the compositor, and how much.
 * The counts are for the life of the session. */
void wm_adapter_counts(uint64_t *moves, uint64_t *pixels) {
    if (moves) *moves = adapter_moves;
    if (pixels) *pixels = adapter_pixels;
}


void wm_invalidate(wm_t *wm, window_t *w) {
    w->needs_paint = true;
    /* The shadow extends a little past the frame. */
    if (window_paint_visible(wm, w))
        wm_damage(wm, window_visual_bounds(w->frame));
}

/* ------------------------------------------------------------------ create */

void wm_init(wm_t *wm, display_t *d) {
    memset(wm, 0, sizeof *wm);
    wm->display = d;
    wm->work_area = rect_make(0, 0, d->back->width, d->back->height);
    wm->mouse_x = d->back->width / 2;
    wm->mouse_y = d->back->height / 2;
    wm->running = true;
    wm->animations_on = true;
    wm_damage_all(wm);
}

window_t *wm_create(wm_t *wm, const char *title, icon_id icon, rect_t client,
                    window_proc proc, void *data) {
    if (wm->count >= WM_MAX_WINDOWS) return NULL;

    window_t *w = calloc(1, sizeof *w);
    if (!w) return NULL;

    strlcpy(w->title, title ? title : "Window", sizeof w->title);
    w->icon = icon;
    w->frame = rect_make(client.x - WIN_BORDER, client.y - WIN_TITLE_H,
                         client.w + 2 * WIN_BORDER, client.h + WIN_TITLE_H + WIN_BORDER);
    w->restore = w->frame;
    w->visible = true;
    w->resizable = true;
    w->closable = true;
    w->min_w = 220;
    w->min_h = 120;
    w->proc = proc;
    w->data = data;
    w->wm = wm;

    w->canvas = surface_create_target(client.w, client.h, wm->display->back->gpu!=NULL);
    if (!w->canvas) { free(w); return NULL; }

    raise_window(wm, w);
    wm->count++;
    wm_focus(wm, w);

    /* The whole screen, not just the newcomer.
     *
     * A new window changes more than its own rectangle: it gets a taskbar
     * button, and now that the buttons divide the row between them, every
     * existing button moves and narrows to make room.  Closing a window has
     * always damaged everything for the same reason; opening one only
     * invalidated itself, so the taskbar showed the row as it stood some
     * windows ago - six buttons while ten windows were open - until something
     * else happened to repaint it. */
    wm_damage_all(wm);

    /* It arrives by growing the last little way into place - see
     * ANIM_ARRIVE_ALPHA for why it does not fade up as well. */
    if (wm->animations_on)
        wm_animate(wm, w, ANIM_OPEN, scaled_about_centre(w->frame, 920), w->frame,
                   ANIM_ARRIVE_ALPHA, 255, ANIM_DEFAULT_MS);
    return w;
}

/* Ask a window to close.  If animations are on it stays alive, and untouchable,
 * until it has finished leaving; wm_close does the real work when that ends. */
void wm_request_close(wm_t *wm, window_t *w) {
    if (!w || w->dying) return;

    if (!wm->animations_on) { wm_close(wm, w); return; }

    w->dying = true;
    if (wm->focus == w) {
        wm->focus = NULL;
        for (window_t *p = wm->top; p; p = p->below)
            if (p != w && p->visible && p->state != WIN_MINIMISED && !p->dying) {
                wm_focus(wm, p);
                break;
            }
    }
    if (wm->drag == w) wm->drag = NULL;
    if (wm->resize == w) wm->resize = NULL;

    wm_animate(wm, w, ANIM_CLOSE, w->frame, scaled_about_centre(w->frame, 880),
               255, 0, ANIM_CLOSE_MS);
}

void wm_close(wm_t *wm, window_t *w) {
    if (!w) return;

    wevent_t ev = { .kind = WE_CLOSE };
    win_proc(w, &ev);

    wm_damage(wm, window_visual_bounds(w->frame));
    unlink_window(wm, w);
    wm->count--;
    if (wm->focus == w) wm->focus = wm->top;
    if (wm->drag == w) wm->drag = NULL;
    if (wm->resize == w) wm->resize = NULL;

    surface_destroy(w->canvas);
    free(w);

    if (wm->focus) {
        wevent_t f = { .kind = WE_FOCUS };
        win_proc(wm->focus, &f);
        wm_invalidate(wm, wm->focus);
    }
    wm_damage_all(wm);
}

void wm_focus(wm_t *wm, window_t *w) {
    if (wm->focus == w) return;

    window_t *old = wm->focus;
    wm->focus = w;

    if (old) {
        wevent_t ev = { .kind = WE_BLUR };
        win_proc(old, &ev);
        wm_invalidate(wm, old);
    }
    if (w) {
        raise_window(wm, w);
        wevent_t ev = { .kind = WE_FOCUS };
        win_proc(w, &ev);
        wm_invalidate(wm, w);
    }
    wm_damage_all(wm);
}

void wm_minimise(wm_t *wm, window_t *w) {
    if (w->state == WIN_MINIMISED || w->anim.kind == ANIM_MINIMISE) return;
    if (w->state == WIN_NORMAL) w->restore = w->frame;

    if (wm->focus == w) {
        wm->focus = NULL;
        for (window_t *p = wm->top; p; p = p->below)
            if (p != w && p->visible && p->state != WIN_MINIMISED && !p->dying) {
                wm_focus(wm, p);
                break;
            }
    }

    if (!wm->animations_on) {
        w->state = WIN_MINIMISED;
        wm_damage_all(wm);
        return;
    }

    /* It collapses towards its own button on the taskbar, so the eye can
     * follow where it went - which is the whole reason to animate this one. */
    rect_t target = wm->taskbar_slot ? wm->taskbar_slot(wm, w)
                                     : rect_make(w->frame.x + w->frame.w / 2,
                                                 wm->work_area.y + wm->work_area.h, 8, 8);
    wm_animate(wm, w, ANIM_MINIMISE, w->frame, target, 255, 0, ANIM_MOVE_MS);
}

void wm_restore(wm_t *wm, window_t *w) {
    if (w->state == WIN_NORMAL) return;

    bool was_minimised = (w->state == WIN_MINIMISED);
    rect_t from = was_minimised && wm->taskbar_slot ? wm->taskbar_slot(wm, w) : w->frame;

    if (!window_set_geometry(w, w->restore, w->restore, WIN_NORMAL)) return;
    wm_focus(wm, w);

    if (wm->animations_on)
        wm_animate(wm, w, ANIM_RESTORE, from, w->frame,
                   was_minimised ? ANIM_ARRIVE_ALPHA : 255, 255, ANIM_MOVE_MS);
    else
        wm_damage_all(wm);
}

void wm_toggle_maximise(wm_t *wm, window_t *w) {
    if (!w->resizable || w->anim.kind == ANIM_GEOMETRY) return;

    rect_t from = w->frame;

    /* The canvas is resized to the destination straight away, so the window
     * lays itself out once; the animation scales that one image on the way
     * there.  Doing it the other way round would make the contents reflow at
     * every intermediate size. */
    bool restoring = w->state == WIN_MAXIMISED;
    rect_t restore = w->state == WIN_NORMAL ? w->frame : w->restore;
    if (!window_set_geometry(w, restoring ? restore : wm->work_area,
                             restore, restoring ? WIN_NORMAL : WIN_MAXIMISED)) return;

    if (wm->animations_on)
        wm_animate(wm, w, ANIM_GEOMETRY, from, w->frame, 255, 255, ANIM_MOVE_MS);
    else
        wm_damage_all(wm);
}

/* Windows-style edge snapping.
 *
 * Releasing a drag with the pointer pressed against a screen edge tiles the
 * window there: the top edge maximises, the left and right edges each take half
 * the work area.  The mechanism is the one maximise uses - resize the canvas to
 * the destination once, then let the geometry animation carry that single
 * painted image there - so a snap costs the same as a maximise and the contents
 * never reflow while it moves.
 *
 * A tiled window is held in WIN_MAXIMISED so the size it had before is
 * remembered in `restore`; dragging it away or pressing maximise brings that
 * size back, exactly as it does for a truly maximised window. */
static void snap_to_rect(wm_t *wm, window_t *w, rect_t dest) {
    rect_t from = w->frame;
    rect_t restore = w->state == WIN_NORMAL ? w->frame : w->restore;
    if (!window_set_geometry(w, dest, restore, WIN_MAXIMISED)) return;

    if (wm->animations_on)
        wm_animate(wm, w, ANIM_GEOMETRY, from, w->frame, 255, 255, ANIM_MOVE_MS);
    else
        wm_damage_all(wm);
}

/* Called when a drag ends: if the pointer is against a screen edge, snap. */
static void wm_snap_dropped(wm_t *wm, window_t *w) {
    if (!w->resizable || w->anim.kind == ANIM_GEOMETRY) return;

    rect_t a = wm->work_area;
    int thr  = 8 * gui_scale();
    int half = a.w / 2;

    if (wm->mouse_y <= a.y + thr) {
        if (w->state != WIN_MAXIMISED) snap_to_rect(wm, w, a);            /* top: full */
    } else if (wm->mouse_x <= a.x + thr) {
        snap_to_rect(wm, w, rect_make(a.x, a.y, half, a.h));             /* left half  */
    } else if (wm->mouse_x >= a.x + a.w - thr) {
        snap_to_rect(wm, w, rect_make(a.x + half, a.y, a.w - half, a.h));/* right half */
    }
}

void wm_stop(wm_t *wm) { wm->running = false; }

/* ------------------------------------------------------------------- frame */

/* Draw a window's frame and contents at `f`, which is normally its own
 * rectangle but is the origin when it is being composed off-screen for an
 * animation.  `hover` is off in that case: the pointer's position means
 * nothing in a surface that is not the screen.
 *
 * Splitting this out is what lets an animating window be drawn once at its
 * real size and then scaled, rather than laid out again at every intermediate
 * size - which would stretch the title text and misplace the buttons. */
static void paint_frame_at(wm_t *wm, surface_t *s, window_t *w, rect_t f,
                           bool with_shadow, bool with_hover) {
    bool active = (wm->focus == w);
    int sc = gui_scale();

    /* A soft shadow, deeper under the window that has focus - which is most of
     * what tells the two apart at a glance. */
    /* The shadow's spread is a distance on screen, so it scales too - an
     * unscaled one puts a 14 pixel shadow under a window twice the size, which
     * reads as a flat sticker rather than as something above the desktop. */
    if (with_shadow)
        gui_soft_shadow(s, f, (active ? 14 : 8) * sc, active ? 90 : 55,
                        WIN_RADIUS);

    gui_round_rect_aa(s, f, WIN_RADIUS, g_theme.window);
    gui_round_frame_aa(s, f, WIN_RADIUS,
                       active ? colour_shade(g_theme.accent, -20) : g_theme.window_border);

    rect_t title = rect_make(f.x + WIN_BORDER, f.y + WIN_BORDER,
                             f.w - 2 * WIN_BORDER, WIN_TITLE_H - WIN_BORDER);
    if (active)
        gui_round_gradient_top(s, title, WIN_RADIUS - 1,
                               colour_shade(g_theme.title_active, 14), g_theme.title_active);
    else
        gui_round_top(s, title, WIN_RADIUS - 1, g_theme.title_inactive);
    gui_hline(s, title.x, title.y + title.h - 1, title.w, g_theme.window_border);

    colour_t tc = active ? g_theme.title_text : g_theme.title_text_inactive;

    /* The title's icon is sized from the bar it sits in rather than from a
     * constant.  At 16 flat it filled half a 32 pixel bar and a quarter of a
     * 64 pixel one, so the larger the display the more it looked like an
     * afterthought beside the title. */
    int icon_size = WIN_TITLE_H / 2;
    int x = title.x + 10 * sc;
    if (w->icon) {
        gui_app_icon(s, w->icon, x, title.y + (title.h - icon_size) / 2, icon_size);
        x += icon_size + 8 * sc;
    }

    int buttons = (w->closable ? 1 : 0) + (w->resizable ? 2 : 1);
    int avail = title.w - (x - title.x) - buttons * (WIN_TITLE_H - 6 * sc)
                - 10 * sc;
    gui_text_clipped(s, FONT_UI, x, title.y + (title.h - gui_font_height(FONT_UI)) / 2,
                     avail > 20 ? avail : 20, w->title, tc);

    /* Buttons, right to left: close, maximise, minimise. */
    int index = 0;
    int inset = 4 * sc;

    if (w->closable) {
        rect_t r = button_rect_at(f, index);
        index++;
        bool hover = with_hover && rect_contains(r, wm->mouse_x, wm->mouse_y);
        if (hover) gui_round_rect_aa(s, r, inset, g_theme.error);
        gui_app_icon_tinted(s, ICON_CLOSE, r.x + inset, r.y + inset, r.w - 2 * inset,
                            hover ? RGB(0xFF, 0xFF, 0xFF) : tc);
    }
    if (w->resizable) {
        rect_t r = button_rect_at(f, index);
        index++;
        bool hover = with_hover && rect_contains(r, wm->mouse_x, wm->mouse_y);
        if (hover) gui_round_rect_aa(s, r, inset, g_theme.control_hover);
        gui_app_icon_tinted(s, w->state == WIN_MAXIMISED ? ICON_RESTORE : ICON_MAXIMISE,
                            r.x + inset, r.y + inset, r.w - 2 * inset,
                            hover ? g_theme.text_bright : tc);
    }
    {
        rect_t r = button_rect_at(f, index);
        bool hover = with_hover && rect_contains(r, wm->mouse_x, wm->mouse_y);
        if (hover) gui_round_rect_aa(s, r, inset, g_theme.control_hover);
        gui_app_icon_tinted(s, ICON_MINIMISE, r.x + inset, r.y + inset, r.w - 2 * inset,
                            hover ? g_theme.text_bright : tc);
    }

    /* Client area. */
    rect_t client = rect_make(f.x + WIN_BORDER, f.y + WIN_TITLE_H,
                              f.w - 2 * WIN_BORDER, f.h - WIN_TITLE_H - WIN_BORDER);
    if (w->canvas) gui_blit(s, w->canvas, client.x, client.y);

    if (w->resizable && w->state == WIN_NORMAL) {
        /* A discreet grip in the bottom-right corner. */
        colour_t g = g_theme.window_border;
        for (int i = 0; i < 3; i++) {
            int o = (4 + i * 4) * sc;
            gui_line(s, f.x + f.w - o, f.y + f.h - 4 * sc,
                     f.x + f.w - 4 * sc, f.y + f.h - o, g);
        }
    }
}

static void paint_frame(wm_t *wm, surface_t *s, window_t *w) {
    paint_frame_at(wm, s, w, w->frame, true, true);
}

/* ------------------------------------------------------------------ hit test */

enum { HIT_NONE, HIT_TITLE, HIT_CLIENT, HIT_CLOSE, HIT_MAX, HIT_MIN, HIT_RESIZE };

static int hit_test(wm_t *wm, window_t *w, int x, int y, int *edge) {
    if (!rect_contains(w->frame, x, y)) return HIT_NONE;

    int index = 0;
    if (w->closable && rect_contains(button_rect(w, index++), x, y)) return HIT_CLOSE;
    if (w->resizable && rect_contains(button_rect(w, index++), x, y)) return HIT_MAX;
    if (rect_contains(button_rect(w, index++), x, y)) return HIT_MIN;

    if (w->resizable && w->state == WIN_NORMAL) {
        int e = 0;
        if (x < w->frame.x + WIN_RESIZE_GRIP) e |= 1;
        if (x >= w->frame.x + w->frame.w - WIN_RESIZE_GRIP) e |= 2;
        if (y < w->frame.y + WIN_RESIZE_GRIP) e |= 4;
        if (y >= w->frame.y + w->frame.h - WIN_RESIZE_GRIP) e |= 8;
        if (e) { if (edge) *edge = e; return HIT_RESIZE; }
    }

    if (y < w->frame.y + WIN_TITLE_H) return HIT_TITLE;
    return HIT_CLIENT;
}

/* ------------------------------------------------------------------ events */

static void send_client(window_t *w, wevent_kind kind, int x, int y,
                        uint32_t button, uint32_t buttons, uint32_t key, uint32_t mods,
                        int wheel, int dx, int dy) {
    if (!w || !w->proc) return;
    rect_t client = wm_client_rect(w);
    wevent_t ev = {
        .kind = kind,
        .x = x - client.x,
        .y = y - client.y,
        .dx = dx, .dy = dy,
        .wheel = wheel,
        .button = button,
        .buttons = buttons,
        .key = key,
        .mods = mods,
    };
    if (win_proc(w, &ev)) w->needs_paint = true;
}

static void handle_mouse_down(wm_t *wm, uint32_t button) {
    window_t *w = wm_window_at(wm, wm->mouse_x, wm->mouse_y);
    if (!w) {
        if (wm->focus) wm_focus(wm, NULL);
        return;
    }

    if (w != wm->focus) wm_focus(wm, w);
    else raise_window(wm, w);

    int edge = 0;
    int hit = hit_test(wm, w, wm->mouse_x, wm->mouse_y, &edge);

    switch (hit) {
    case HIT_CLOSE:
        if (button == MB_LEFT) wm_request_close(wm, w);
        return;
    case HIT_MAX:
        if (button == MB_LEFT) wm_toggle_maximise(wm, w);
        return;
    case HIT_MIN:
        if (button == MB_LEFT) wm_minimise(wm, w);
        return;
    case HIT_TITLE:
        if (button == MB_LEFT) {
            if (w->state == WIN_MAXIMISED && w->restore.w > 0) {
                /* Grabbing the title of a maximised or edge-tiled window
                 * restores its remembered size and drags that, keeping the
                 * cursor at the same fraction along the title bar - the way
                 * Windows lets you pull a maximised window back down. */
                rect_t r = w->restore;
                int off = (w->frame.w > 0)
                            ? (int)(((int64_t)wm->mouse_x - w->frame.x) * r.w / w->frame.w)
                            : r.w / 2;
                r.x = wm->mouse_x - off;
                r.y = wm->mouse_y - WIN_TITLE_H / 2;
                if (!window_set_geometry(w, r, w->restore, WIN_NORMAL)) return;
            }
            wm->drag    = w;
            wm->drag_dx = wm->mouse_x - w->frame.x;
            wm->drag_dy = wm->mouse_y - w->frame.y;
        }
        return;
    case HIT_RESIZE:
        if (button == MB_LEFT) { wm->resize = w; wm->resize_edge = edge; }
        return;
    case HIT_CLIENT:
        send_client(w, WE_MOUSE_DOWN, wm->mouse_x, wm->mouse_y, button, wm->buttons, 0, wm->mods, 0, 0, 0);
        return;
    default:
        return;
    }
}

/* The resize shape a set of edge bits (1=L 2=R 4=T 8=B) asks for. */
static cursor_kind cursor_for_edge(int edge) {
    bool l = edge & 1, r = edge & 2, t = edge & 4, b = edge & 8;
    if ((l && t) || (r && b)) return CURSOR_SIZE_NWSE;
    if ((r && t) || (l && b)) return CURSOR_SIZE_NESW;
    if (l || r) return CURSOR_SIZE_WE;
    if (t || b) return CURSOR_SIZE_NS;
    return CURSOR_ARROW;
}

static void apply_resize(wm_t *wm, int dx, int dy) {
    window_t *w = wm->resize;
    rect_t f = w->frame;

    if (wm->resize_edge & 1) { f.x += dx; f.w -= dx; }
    if (wm->resize_edge & 2) { f.w += dx; }
    if (wm->resize_edge & 4) { f.y += dy; f.h -= dy; }
    if (wm->resize_edge & 8) { f.h += dy; }

    int min_w = w->min_w + 2 * WIN_BORDER;
    int min_h = w->min_h + WIN_TITLE_H + WIN_BORDER;
    if (f.w < min_w) { if (wm->resize_edge & 1) f.x = w->frame.x + w->frame.w - min_w; f.w = min_w; }
    if (f.h < min_h) { if (wm->resize_edge & 4) f.y = w->frame.y + w->frame.h - min_h; f.h = min_h; }

    if (f.w == w->frame.w && f.h == w->frame.h && f.x == w->frame.x && f.y == w->frame.y) return;

    if (!window_set_geometry(w, f, w->restore, w->state)) return;
    wm_damage_all(wm);
}

static rect_t title_hover_rect(wm_t *wm, int x, int y) {
    window_t *w = wm_window_at(wm, x, y);
    if (w && w->anim.kind == ANIM_NONE) {
        int count = (w->closable ? 1 : 0) + (w->resizable ? 1 : 0) + 1;
        for (int i = 0; i < count; i++) {
            rect_t r = button_rect(w, i);
            if (rect_contains(r, x, y)) return r;
        }
    }
    return rect_make(0, 0, 0, 0);
}

static void damage_title_hover(wm_t *wm, int old_x, int old_y, int x, int y) {
    rect_t old = title_hover_rect(wm, old_x, old_y);
    rect_t now = title_hover_rect(wm, x, y);
    if (old.x == now.x && old.y == now.y && old.w == now.w && old.h == now.h) return;
    wm_damage(wm, old);
    wm_damage(wm, now);
}

static void handle_event(wm_t *wm, const kinput_event_t *ev) {
    switch (ev->type) {
    case KEV_MOUSE_MOVE: {
        int old_x = wm->mouse_x, old_y = wm->mouse_y;
        if (!wm->drag && !wm->resize)
            damage_title_hover(wm, old_x, old_y, ev->x, ev->y);
        wm->mouse_x = ev->x;
        wm->mouse_y = ev->y;
        wm->buttons = ev->buttons;

        /* Choose the pointer shape from what is under it: a window edge (or the
         * edge being dragged) gives the matching resize arrows, everything else
         * the plain arrow.  When the shape changes and the adapter draws the
         * pointer, re-hand it so the change shows without redrawing the frame. */
        {
            cursor_kind want = CURSOR_ARROW;
            if (wm->resize) {
                want = cursor_for_edge(wm->resize_edge);
            } else if (!wm->drag) {
                window_t *hw = wm_window_at(wm, wm->mouse_x, wm->mouse_y);
                int he = 0;
                if (hw && hit_test(wm, hw, wm->mouse_x, wm->mouse_y, &he) == HIT_RESIZE)
                    want = cursor_for_edge(he);
            }
            gui_set_cursor(want);
            if (gui_cursor_take_dirty()) {
                if (wm->display->hardware_cursor) display_refresh_cursor(wm->display);
                else wm_damage(wm, rect_make(old_x - 16, old_y - 16, 40, 40));
            }
        }

        if (wm->display->hardware_cursor) {
            /* The adapter draws it; telling it where is the whole cost of a
             * mouse movement, and nothing on the screen has changed. */
            fb_move_cursor(wm->mouse_x, wm->mouse_y);
        } else {
            /* Drawn into the back buffer, so both where it was and where it is
             * now have to be repainted.  The margin covers the largest cursor
             * shape and its centred hotspot, not just the 12x19 arrow. */
            wm_damage(wm, rect_make(old_x - 16, old_y - 16, 44, 44));
            wm_damage(wm, rect_make(wm->mouse_x - 16, wm->mouse_y - 16, 44, 44));
        }

        int dx = wm->mouse_x - old_x, dy = wm->mouse_y - old_y;

        if (wm->drag) {
            /* Where it was, so the desktop behind it is repainted, and where it
             * is going.  Repainting the whole screen for this - which is what
             * used to happen - means the background, every other window, every
             * shadow and the taskbar are all redrawn for a movement of a few
             * pixels, on every one of the many mouse events a drag produces.
             * The shadow reaches beyond the frame, hence the margin. */
            wm_move_window(wm, wm->drag,
                           wm->mouse_x - wm->drag_dx,
                           wm->mouse_y - wm->drag_dy);
            break;
        }
        if (wm->resize) { apply_resize(wm, dx, dy); break; }

        window_t *w = wm_window_at(wm, wm->mouse_x, wm->mouse_y);
        if (w) {
            int edge = 0;
            if (hit_test(wm, w, wm->mouse_x, wm->mouse_y, &edge) == HIT_CLIENT)
                send_client(w, WE_MOUSE_MOVE, wm->mouse_x, wm->mouse_y, 0, wm->buttons, 0, wm->mods, 0, dx, dy);
        }
        break;
    }

    case KEV_MOUSE_BUTTON:
        wm->buttons = ev->buttons;
        wm->mouse_x = ev->x;
        wm->mouse_y = ev->y;
        if (ev->pressed) {
            handle_mouse_down(wm, ev->code);
        } else {
            if (wm->drag) {
                window_t *d = wm->drag; wm->drag = NULL;
                wm_snap_dropped(wm, d);   /* Windows-style edge tiling */
                wm_damage_all(wm); break;
            }
            if (wm->resize) { wm->resize = NULL; wm_damage_all(wm); break; }
            window_t *w = wm_window_at(wm, wm->mouse_x, wm->mouse_y);
            if (w) {
                int edge = 0;
                if (hit_test(wm, w, wm->mouse_x, wm->mouse_y, &edge) == HIT_CLIENT)
                    send_client(w, WE_MOUSE_UP, wm->mouse_x, wm->mouse_y, ev->code,
                                wm->buttons, 0, wm->mods, 0, 0, 0);
            }
        }
        break;

    case KEV_MOUSE_WHEEL: {
        window_t *w = wm_window_at(wm, wm->mouse_x, wm->mouse_y);
        if (w) send_client(w, WE_MOUSE_WHEEL, wm->mouse_x, wm->mouse_y, 0, wm->buttons,
                           0, wm->mods, ev->wheel, 0, 0);
        break;
    }

    case KEV_KEY:
        wm->mods = ev->mods;
        if (ev->pressed && wm->focus)
            send_client(wm->focus, WE_KEY_DOWN, wm->mouse_x, wm->mouse_y, 0, wm->buttons,
                        ev->code, ev->mods, 0, 0, 0);
        else if (!ev->pressed && wm->focus)
            send_client(wm->focus, WE_KEY_UP, wm->mouse_x, wm->mouse_y, 0, wm->buttons,
                        ev->code, ev->mods, 0, 0, 0);
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------- paint */

/* The surface an animating window is composed on before being scaled onto the
 * screen.  One is enough: only one window is drawn at a time, and keeping it
 * between frames means an animation does not allocate. It is retired when
 * the last animation finishes, rather than retaining its peak VRAM forever. */
static surface_t *stage;

static surface_t *stage_for(rect_t frame, bool gpu) {
    int width = frame.w > 64 ? frame.w : 64;
    int height = frame.h > 64 ? frame.h : 64;
    int alloc_width = width, alloc_height = height;
    if (stage && (stage->width < frame.w || stage->height < frame.h ||
                  (stage->gpu != NULL) != gpu)) {
        /* Preserve both extents when growing within one renderer. Otherwise
         * alternating wide/tall windows replace this buffer on every frame. */
        if ((stage->gpu != NULL) == gpu) {
            if (stage->width > alloc_width) alloc_width = stage->width;
            if (stage->height > alloc_height) alloc_height = stage->height;
        }
        surface_destroy(stage);
        stage = NULL;
    }
    if (!stage) {
        stage = surface_create_target(alloc_width, alloc_height, gpu);
        /* The combined extent can exceed available VRAM or surface limits
         * even though this window fits. Retain the exact-size path then. */
        if (!stage && (alloc_width != width || alloc_height != height))
            stage = surface_create_target(width, height, gpu);
    }
    return stage;
}

static void stage_retire_if_idle(wm_t *wm) {
    if (!stage || (wm->running && wm_is_animating(wm))) return;
    /* surface_destroy flushes queued GPU work before requesting destruction;
     * the driver retains the allocation if the channel cannot retire safely. */
    surface_destroy(stage);
    stage = NULL;
}

/* Draw a window that is moving.  Rather than painting the frame at a size it
 * was never laid out for - which would stretch the title text and put the
 * buttons in the wrong place - the frame is drawn once at its real size and
 * that image is scaled.  It is also what makes an animation cost the same
 * whatever the window contains. */
static void paint_animating(wm_t *wm, surface_t *s, window_t *w) {
    int t = w->anim.paint_progress;
    rect_t where = anim_lerp_rect(w->anim.from, w->anim.to, t);
    int alpha = anim_lerp(w->anim.alpha_from, w->anim.alpha_to, t);
    if (where.w < 2 || where.h < 2 || alpha <= 2) return;

    /* Damage can belong to another monitor. Avoid rebuilding the entire
     * staging image when neither the animation/shadow nor the allocation-
     * failure fallback can contribute to this region. */
    rect_t clip = surface_clip(s);
    if (!window_reaches_area(where, clip) &&
        !window_reaches_area(w->frame, clip)) return;

    surface_t *into = stage_for(w->frame,s->gpu!=NULL);
    if (!into) { paint_frame(wm, s, w); return; }

    /* Compose the frame at its own size, at the origin. */
    rect_t real = w->frame;
    rect_t at_origin = rect_make(0, 0, real.w, real.h);

    /* The shared allocation retains its largest extents. Only the current
     * frame is sampled below, so do not clear the unused high-water padding
     * on every composition of a smaller window. gui_clear honours this clip. */
    surface_set_clip(into, at_origin);
    gui_clear(into, g_theme.window);
    paint_frame_at(wm, into, w, at_origin, false, false);
    surface_reset_clip(into);

    /* Its shadow follows it, fading with it. */
    if (alpha > 40) {
        rect_t before = surface_clip(s);
        gui_soft_shadow(s, where, 10, 55 * alpha / 255, WIN_RADIUS);
        surface_set_clip(s, before);
    }

    surface_t view = *into;
    view.width = real.w;
    view.height = real.h;
    gui_blit_scaled(s, &view, where, alpha);
}

/* How long frames are taking, and how much of the screen each one covers.
 *
 * Reported rather than guessed at: the difference between a compositor that
 * feels immediate and one that does not is a few milliseconds a frame, and
 * which few milliseconds is never where it seems. */
static uint64_t frame_total_us, frame_count, frame_pixels;
/* Where the time in a frame actually goes: the background, the windows, the
 * overlay, and the copy out to video memory. */
static uint64_t phase_back_us, phase_win_us, phase_over_us, phase_present_us;

void wm_frame_report(char *out, size_t cap) {
    if (!frame_count) { snprintf(out, cap, "no redraws yet"); return; }
    /* And what the display adapter drew rather than the processor.  Reported
     * because "is any of this on the card" was, for a long time, a question
     * this system could not answer about itself. */
    snprintf(out, cap,
             "%llu redraws, %llu us each, %llu pixels each "
             "(background %llu, windows %llu, overlay %llu, present %llu); "
             "the adapter moved %llu rectangle(s), %llu pixels",
             (unsigned long long)frame_count,
             (unsigned long long)(frame_total_us / frame_count),
             (unsigned long long)(frame_pixels / frame_count),
             (unsigned long long)(phase_back_us / frame_count),
             (unsigned long long)(phase_win_us / frame_count),
             (unsigned long long)(phase_over_us / frame_count),
             (unsigned long long)(phase_present_us / frame_count),
             (unsigned long long)adapter_moves,
             (unsigned long long)adapter_pixels);
}

void wm_frame_reset(void) {
    /* adapter_moves is deliberately not reset: it answers "has the card done
     * anything at all", which is a question about the whole session and not
     * about the last five seconds. */
    frame_total_us = frame_count = frame_pixels = 0;
    phase_back_us = phase_win_us = phase_over_us = phase_present_us = 0;
}

static bool rect_covers(rect_t outer, rect_t inner) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.w <= outer.x + outer.w &&
           inner.y + inner.h <= outer.y + outer.h;
}

/* The topmost window that covers this rectangle by itself, or NULL.
 *
 * This is worth finding because of what the measurements said.  The compositor
 * used to paint the desktop background across the whole of every damaged
 * rectangle and then paint the windows on top, and at 1024x768 that background
 * was 2159 of the 3379 microseconds a frame cost - sixty-four per cent of the
 * work, to produce pixels that the very next thing overwrote.  The largest
 * single cost in a frame was painting what could not be seen.
 *
 * When a settled window covers the rectangle, everything underneath it - the
 * background and every window below - is hidden, and none of it needs drawing.
 * That is the common case rather than a corner one: a window repainting itself
 * damages its own rectangle, which it covers by definition.
 *
 * Two things disqualify a window from covering, and both matter:
 *
 *   An animating one is blended as it moves, so what is behind it shows
 *   through and still has to be there.
 *
 *   The corners are rounded, so the frame does not reach them.  The rectangle
 *   tested is the frame inset by the corner radius on all four sides, which
 *   is entirely inside the rounded shape - conservative, and cheap enough that
 *   being exact about the corner arcs would not pay for itself.
 */
static window_t *covering_window(wm_t *wm, rect_t area) {
    for (window_t *w = wm->top; w; w = w->below) {
        if (!w->visible) continue;
        if (w->state == WIN_MINIMISED) continue;
        if (w->anim.kind != ANIM_NONE) continue;

        rect_t solid = rect_make(w->frame.x + WIN_RADIUS,
                                 w->frame.y + WIN_RADIUS,
                                 w->frame.w - 2 * WIN_RADIUS,
                                 w->frame.h - 2 * WIN_RADIUS);
        if (solid.w > 0 && solid.h > 0 && rect_covers(solid, area)) return w;
    }
    return NULL;
}

/* Would anything this window draws into this rectangle survive?
 *
 * The rectangle version above only helps when a single window covers the whole
 * of the damage.  With several windows stacked, the ones underneath were still
 * being drawn in full and then painted over, and after the background was
 * dealt with that became the largest remaining cost in a frame.
 *
 * What is tested is the frame plus a margin, because a window puts a soft
 * shadow outside its own edges and that shadow is part of what it draws.  A
 * window is only skipped when something settled and opaque above it covers all
 * of that - never merely because it is behind something.
 *
 * A window that does not reach this rectangle at all is NOT reported hidden.
 * That case is left to the caller's existing test, deliberately: an event
 * handler can ask for a repaint without anything adding damage for it, so a
 * window still gets its paint call even when this rectangle is elsewhere.
 * Skipping those would lose repaints rather than save work.
 */
static bool window_hidden_in(window_t *w, rect_t area) {
    rect_t touched = rect_intersection(
        window_visual_bounds(w->frame), area);
    if (rect_empty(touched)) return false;

    for (window_t *o = w->above; o; o = o->above) {
        if (!o->visible) continue;
        if (o->state == WIN_MINIMISED) continue;
        if (o->anim.kind != ANIM_NONE) continue;

        rect_t solid = rect_make(o->frame.x + WIN_RADIUS,
                                 o->frame.y + WIN_RADIUS,
                                 o->frame.w - 2 * WIN_RADIUS,
                                 o->frame.h - 2 * WIN_RADIUS);
        if (solid.w > 0 && solid.h > 0 && rect_covers(solid, touched)) return true;
    }
    return false;
}

/* Do not run an application's renderer just to overwrite its entire output.
 * Keep needs_paint set while covered: moving/closing the cover causes the next
 * pump to paint the latest contents before composition. Ticks and input still
 * run, and a partially visible or animating window is never throttled here.
 * This deliberately uses the conservative shadow-inclusive coverage proof;
 * several partial covers are not assumed to cover the whole window. */
static bool window_paint_visible(wm_t *wm, window_t *w) {
    if (!w->visible) return false;
    if (w->anim.kind != ANIM_NONE) return true;
    if (w->state == WIN_MINIMISED) return false;
    rect_t screen = rect_make(0, 0, wm->display->back->width,
                              wm->display->back->height);
    return window_reaches_area(w->frame, screen) && !window_hidden_in(w, screen);
}

/* The old all-or-nothing covering check misses a window repaint that includes
 * its rounded border/shadow: a few exposed edge pixels forced the entire
 * wallpaper (and its icons) to be redrawn behind an opaque 3D canvas.
 * Subtract only settled, opaque interiors. Keep the rounded corners, shadows
 * and everything behind an animation. Disjoint rectangles avoid blending an
 * icon twice; on fragmentation overflow the caller paints the original area. */
#define BACKGROUND_REGION_MAX 64
static int background_regions(wm_t *wm, rect_t area, rect_t *regions) {
    if (rect_empty(area)) return 0;
    regions[0]=area;
    int count=1;
    rect_t next[BACKGROUND_REGION_MAX];
    for (window_t *w=wm->top; w && count; w=w->below) {
        if (!w->visible || w->state==WIN_MINIMISED || w->anim.kind!=ANIM_NONE) continue;
        rect_t solid=rect_make(w->frame.x+WIN_RADIUS,w->frame.y+WIN_RADIUS,
                              w->frame.w-2*WIN_RADIUS,w->frame.h-2*WIN_RADIUS);
        if (rect_empty(solid)) continue;
        int n=0;
        for (int i=0;i<count;i++) {
            rect_t r=regions[i],cut=rect_intersection(r,solid);
            if (rect_empty(cut)) {
                if (n==BACKGROUND_REGION_MAX) return -1;
                next[n++]=r;
                continue;
            }
            rect_t part[4]={
                rect_make(r.x,r.y,r.w,cut.y-r.y),
                rect_make(r.x,cut.y+cut.h,r.w,r.y+r.h-cut.y-cut.h),
                rect_make(r.x,cut.y,cut.x-r.x,cut.h),
                rect_make(cut.x+cut.w,cut.y,r.x+r.w-cut.x-cut.w,cut.h)
            };
            for (int j=0;j<4;j++) if (!rect_empty(part[j])) {
                if (n==BACKGROUND_REGION_MAX) return -1;
                next[n++]=part[j];
            }
        }
        count=n;
        for (int i=0;i<count;i++) regions[i]=next[i];
    }
    return count;
}

static void paint_exposed_background(wm_t *wm,surface_t *s,rect_t area) {
    rect_t regions[BACKGROUND_REGION_MAX];
    int count=background_regions(wm,area,regions);
    if (count<0) { regions[0]=area;count=1; }
    for (int i=0;i<count;i++) {
        surface_set_clip(s,regions[i]);
        if (wm->paint_background) wm->paint_background(wm,s,regions[i]);
        else gui_gradient_v(s,rect_make(0,0,s->width,s->height),
                            g_theme.desktop_top,g_theme.desktop_bottom);
    }
    surface_set_clip(s,area);
}

/* Paint one rectangle into the back buffer. Presentation follows only after
 * every damaged region has been painted, so it cannot prematurely flush the
 * GPU command batch between regions. Source/target hazard flushes still apply. */
static void repaint_area(wm_t *wm, rect_t area) {
    surface_t *s = wm->display->back;
    area = rect_intersection(area, rect_make(0, 0, s->width, s->height));
    if (rect_empty(area)) return;

    uint64_t started = uptime_us();

    gui_text_frame_begin();
    surface_set_clip(s, area);

    /* Start at whatever hides the rest, and skip the background with it. */
    window_t *first = covering_window(wm, area);

    if (!first) {
        paint_exposed_background(wm,s,area);
    }

    uint64_t after_background = uptime_us();
    phase_back_us += after_background - started;

    for (window_t *w = first ? first : wm->bottom; w; w = w->above) {
        if (!w->visible) continue;
        /* A minimised window is invisible unless it is on its way there. */
        if (w->state == WIN_MINIMISED && w->anim.kind == ANIM_NONE) continue;

        /* Checked before the paint call, not after: the work an application
         * does drawing its own contents is its share of the frame, and there
         * is no point spending it on a window nobody can see. */
        if (w->anim.kind == ANIM_NONE && window_hidden_in(w, area)) continue;

        if (w->needs_paint && w->proc) {
            surface_reset_clip(w->canvas);
            wevent_t ev = { .kind = WE_PAINT };
            w->needs_paint = false;
            win_proc(w, &ev);
        }

        if (w->anim.kind != ANIM_NONE) {
            paint_animating(wm, s, w);
            surface_set_clip(s, area);
        } else if (window_reaches_area(w->frame, area)) {
            paint_frame(wm, s, w);
        }
    }

    uint64_t after_windows = uptime_us();
    phase_win_us += after_windows - after_background;

    if (wm->paint_overlay) wm->paint_overlay(wm, s, area);

    /* Only when the adapter will not do it. */
    if (!wm->display->hardware_cursor &&
        rect_intersects(rect_make(wm->mouse_x - 1, wm->mouse_y - 1, 14, 21), area))
        gui_cursor(s, wm->mouse_x, wm->mouse_y);
    surface_reset_clip(s);

    phase_over_us += uptime_us() - after_windows;
    frame_pixels += (uint64_t)area.w * (uint64_t)area.h;
}

/* Consume a bounded snapshot before callbacks run. A paint callback may queue
 * damage for the NEXT redraw; clearing the live queue after painting used to
 * discard that request, and iterating it live could extend this frame midway. */
static int take_repaint_damage(wm_t *wm, rect_t *areas) {
    surface_t *s = wm->display->back;
    rect_t bounds = rect_make(0, 0, s->width, s->height);
    int count = 0;
    if (wm->full_redraw) {
        if (!rect_empty(bounds)) areas[count++] = bounds;
    } else {
        for (int i = 0; i < wm->damage_count; i++) {
            rect_t area = rect_intersection(wm->damage[i], bounds);
            if (!rect_empty(area)) areas[count++] = area;
        }
    }
    wm->damage_count = 0;
    wm->full_redraw = false;
    return count;
}

static void repaint(wm_t *wm) {
    surface_t *s = wm->display->back;
    uint64_t repaint_start = uptime_us();
    rect_t areas[WM_MAX_DAMAGE];
    int count = take_repaint_damage(wm, areas);
    if (!count) return;
    for (int i = 0; i < count; i++) repaint_area(wm, areas[i]);
    uint64_t before_present = uptime_us();
    for (int i = 0; i < count; i++) display_present(wm->display, areas[i]);

    /* Once, at the end.  The stores that carry pixels to video memory are not
     * ordered against ordinary ones and have to be flushed before the frame
     * counts as delivered - but flushing after every rectangle drains
     * half-filled write-combining buffers over and over, which is the opposite
     * of what they are for. */
    blit_present_done();

    uint64_t now = uptime_us(), cost = now - repaint_start;
    phase_present_us += now - before_present;
    /* A frame is a complete redraw, not each damage rectangle. Include final
     * presentation/flush cost; otherwise fragmented damage inflated reported
     * FPS and divided the true per-redraw latency by the number of regions. */
    frame_total_us += cost;
    frame_count++;
    if(s->gpu) {
        static uint64_t last_report, redraws, elapsed, peak;
        redraws++;elapsed+=cost;if(cost>peak)peak=cost;
        if(now-last_report>=5000000u) {
            char note[192];
            snprintf(note,sizeof note,"GPU redraw interval: %llu redraws, average=%llu us peak=%llu us (paint+composition+present; not GPU-only time)",
                     (unsigned long long)redraws,(unsigned long long)(elapsed/redraws),(unsigned long long)peak);
            log_write(1,"wm-perf",note);
            last_report=now;redraws=elapsed=peak=0;
        }
    }
}

/* --------------------------------------------------------------- main loop */

/* How often a window is told time has passed - the caret blink, the clock, a
 * terminal checking its shell.  This does not have to be fast. */
#define TICK_MS 120
/* How long the loop sleeps when nothing is happening. */
#define IDLE_MS 10
/* Ordinary UI animation timer. Continuously rendering applications are not
 * paced by this timer: their GPU work/presentation already consumes frame time. */
#define FRAME_MS 16

/* One turn of the loop, without the sleep.  Returns true if anything
 * happened, which is what tells the caller whether it can afford to wait.
 *
 * This exists as its own function because not every program that wants
 * windows wants this loop: one running a Windows program has a message loop of
 * its own and needs to turn the manager over from inside it. */
bool wm_pump(wm_t *wm) {
    static uint64_t last_tick;
    kinput_event_t ev;
    bool busy = false;

    /* Drain everything queued before painting, so a fast mouse produces one
     * repaint rather than one per event. */
    while (display_poll_event(wm->display, &ev)) {
        busy = true;
        /* The desktop sees each event first: the taskbar and the launcher menu
         * are drawn above every window, so they must also get the chance to
         * claim clicks before the window manager routes them. */
        if (!(wm->background_event && wm->background_event(wm, &ev)))
            handle_event(wm, &ev);
        if (!wm->running) { stage_retire_if_idle(wm); return true; }
    }

    /* Anything moving is advanced before the frame is drawn, so what is drawn
     * is where things are now rather than where they were last time. */
    /* A window drawing its own moving picture gets a frame now, not at the
     * next tick. */
    bool continuous = false;
    for (window_t *w = wm->bottom; w; w = w->above) {
        if (!w->visible || !w->continuous || !w->proc) continue;
        if (w->state == WIN_MINIMISED) continue;
        if (!window_paint_visible(wm, w)) continue;
        continuous = true;
        wm_invalidate(wm, w);
    }

    /* Anything that has asked to be repainted does so once, before the frame
     * is composited - not inside the loop over damage rectangles, where a
     * window straddling two of them would paint itself twice. */
    for (window_t *w = wm->bottom; w; w = w->above) {
        if (!w->visible || !w->needs_paint || !w->proc) continue;
        if (w->state == WIN_MINIMISED && w->anim.kind == ANIM_NONE) continue;
        if (!window_paint_visible(wm, w)) continue;
        /* Apps may set needs_paint directly (e.g. asynchronous browser/store
         * content). Updating the canvas alone does not schedule composition.
         * Always damage its visible footprint, even without an input event. */
        wm_damage(wm, window_visual_bounds(w->frame));
        surface_reset_clip(w->canvas);
        wevent_t paint = { .kind = WE_PAINT };
        w->needs_paint = false; /* keep a new invalidation raised by the callback */
        win_proc(w, &paint);
    }

    bool moving = step_animations(wm);
    if (moving) {
        wm->animating = true;
        /* step_animations damaged each old/new animated footprint. */
        busy = true;
    } else if (wm->animating) {
        /* The last frame of an animation: one more repaint puts everything
         * where it finally belongs. */
        wm->animating = false;
        wm_damage_all(wm);
        busy = true;
    }

    /* Ticks drive the clock, the caret and anything an app is waiting on - a
     * terminal reading its shell's output, for instance.  They must keep
     * arriving even when nobody touches the keyboard or mouse. */
    uint64_t now = uptime_ms();
    if (now - last_tick >= TICK_MS) {
        last_tick = now;
        if (wm->on_tick) wm->on_tick(wm);
        for (window_t *w = wm->bottom; w; w = w->above) {
            if (!w->visible || w->state == WIN_MINIMISED || !w->proc) continue;
            wevent_t t = { .kind = WE_TICK };
            if (win_proc(w, &t)) wm_invalidate(wm, w);
        }
    }

    if (wm_has_damage(wm)) { repaint(wm); busy = true; }
    /* Ticks/painting can start another animation, so inspect the current list,
     * not the earlier step_animations result, before releasing the scratch. */
    stage_retire_if_idle(wm);
    wm->drawing_continuously = continuous;
    return busy;
}

void wm_run(wm_t *wm) {
    wm->animations_on = true;
    repaint(wm);
    while (wm->running) {
        wm_pump(wm);
        if (!wm->running) break;

        /* Something still to draw: go round again rather than waiting. */
        if (wm_has_damage(wm)) continue;

        /* Otherwise wait for whichever comes first - input, the next frame of
         * an animation, or the next tick.  Waiting on input rather than
         * sleeping a fixed amount is what makes the pointer keep up with the
         * hand: the loop is running again the instant the mouse moves, instead
         * of whenever the sleep happens to end. */
        /* Do not append 16 ms to every completed 3D frame. Keep processing
         * input/ticks on every pump, while a continuous canvas runs without an
         * artificial desktop frame cap. Ordinary animations and idle windows
         * retain their event-driven waits. This is not a GPU-time/FPS claim. */
        int timeout = wm->drawing_continuously ? 0
                    : wm->animating ? FRAME_MS : TICK_MS;
        display_wait_input(wm->display, timeout);
    }
    stage_retire_if_idle(wm);
}
