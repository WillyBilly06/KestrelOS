/* app_gl.c - the 3D pipeline, on screen.
 *
 * A lit, textured, depth-buffered scene drawn through the OpenGL layer, so
 * that "there is a 3D renderer" is something the user can look at rather than
 * something a document claims.  It also serves as the worked example of how a
 * program on this system gets a window and renders into it.
 *
 * The counters along the bottom are the honest measure: triangles and
 * fragments actually rasterised, and CPU submission plus GPU completion time.
 * Window composition and presentation are measured separately by the desktop.
 */
#include "desktop.h"
#include "GL.h"
#include "app_gl_scene.h"
#include "math.h"

typedef enum { SCENE_CUBE, SCENE_TORUS, SCENE_TERRAIN, SCENE_SPHERE, SCENE_CONE, SCENE_CYLINDER, SCENE_COUNT } scene_t;

/* Vulkan has bounded procedural meshes; the Direct3D bridges retain their
 * cube probes. Implemented in app_gl_apis.c through each API's object model. */
typedef enum { API_GL, API_VULKAN, API_D3D9, API_D3D11, API_COUNT } api_t;

size_t api_state_size(void);
bool api_config_vulkan(void *state,unsigned shape,unsigned detail);
void api_destroy_vulkan(void *state);
void api_destroy_d3d9(void *state);
void api_destroy_d3d11(void *state);
bool api_draw_vulkan(void *state, surface_t *target, int top, int w, int h,
                     float spin, float pitch);
bool api_draw_d3d9(void *state, surface_t *target, int top, int w, int h,
                   float spin, float pitch);
bool api_draw_d3d11(void *state, surface_t *target, int top, int w, int h,
                    float spin, float pitch);

static const char *api_names[API_COUNT] = { "OpenGL", "Vulkan", "Direct3D 9",
                                            "Direct3D 11" };

typedef struct {
    gl_scene_t *renderer;
    bool       render_failed;
    float      spin;
    scene_t    scene;
    unsigned   detail;
    api_t      api;
    void      *api_state;        /* whichever API is in use owns this */
    api_t      api_state_for;
    bool       spinning;
    bool       lit;
    bool       textured;
    bool       wireframe;

    /* Dragging with the mouse turns the model. */
    bool  dragging;
    int   last_x, last_y;
    float yaw, pitch;

    uint64_t last_spin_us;
    uint64_t frame_us;
    unsigned tris, frags;
    int      fps;
    int      frames_this_second;
    uint64_t second_started;
    uint64_t perf_started, perf_total_us, perf_peak_us;
    unsigned perf_frames;
    api_t perf_api;
    scene_t perf_scene;
    int perf_w, perf_h;
    unsigned perf_flags;
} glapp_t;

static const char *scene_names[SCENE_COUNT] = { "Cube", "Torus", "Terrain", "Sphere", "Cone", "Cylinder" };

/* -------------------------------------------------------------- the frame */

static void api_release(glapp_t *a) {
    if (!a->api_state) return;
    switch (a->api_state_for) {
    case API_VULKAN: api_destroy_vulkan(a->api_state); break;
    case API_D3D9: api_destroy_d3d9(a->api_state); break;
    case API_D3D11: api_destroy_d3d11(a->api_state); break;
    default: break;
    }
    free(a->api_state);a->api_state=NULL;a->api_state_for=API_GL;
}

static void frame_finished(glapp_t *a, uint64_t started, int w, int h) {
    uint64_t now=uptime_us();
    a->frame_us=now>=started?now-started:0;
    a->frames_this_second++;
    uint64_t interval=now-a->second_started;
    if(interval>=1000000u) {
        a->fps=(int)((uint64_t)a->frames_this_second*1000000u/interval);
        a->frames_this_second=0;a->second_started=now;
    }
    /* Keep scene/API/viewport changes out of the same aggregate. Rendering is
     * synchronous today; this is wall time, not a GPU timestamp measurement. */
    scene_t shown=(a->api==API_GL||a->api==API_VULKAN)?a->scene:SCENE_CUBE;
    unsigned flags=a->api==API_GL?((unsigned)a->lit|((unsigned)a->textured<<1)):
                   a->api==API_VULKAN?a->detail<<8:0u;
    if(!a->perf_frames || a->perf_api!=a->api || a->perf_scene!=shown ||
       a->perf_w!=w || a->perf_h!=h || a->perf_flags!=flags) {
        a->perf_started=started;a->perf_total_us=a->perf_peak_us=0;a->perf_frames=0;
        a->perf_api=a->api;a->perf_scene=shown;a->perf_w=w;a->perf_h=h;
        a->perf_flags=flags;
    }
    a->perf_total_us+=a->frame_us;a->perf_frames++;
    if(a->frame_us>a->perf_peak_us)a->perf_peak_us=a->frame_us;
    if(now-a->perf_started>=2000000u) {
        char note[192];
        snprintf(note,sizeof note,"%s/%s %dx%d flags=%u: %u frames avg=%llu us peak=%llu us (submit+GPU wait; excludes composition/present)",
            api_names[a->perf_api],scene_names[a->perf_scene],w,h,a->perf_flags,a->perf_frames,
            (unsigned long long)(a->perf_total_us/a->perf_frames),(unsigned long long)a->perf_peak_us);
        log_write(1,"3d-perf",note);a->perf_frames=0;
    }
}

/* Render straight into the window's own canvas, using the viewport to keep the
 * scene out of the two control bars.  A separate render target would mean an
 * allocation on every resize and a full-screen copy on every frame, for nothing
 * the viewport does not already do. */
static void render(glapp_t *a, surface_t *canvas, int top, int w, int h) {
    if (a->api_state && a->api_state_for != a->api) api_release(a);
    if (w < 8 || h < 8) return;
    if (a->render_failed) return;

    if (a->api != API_GL) {
        /* Release the previous API's objects before replacing its state. */
        if (!a->api_state) a->api_state = calloc(1, api_state_size());
        if (!a->api_state) {
            a->render_failed=true;a->spinning=false;
            log_write(3,"3d-gpu","API state allocation failed; animation stopped, no CPU fallback");
            return;
        }
        if (a->api_state_for != a->api) {
            a->api_state_for = a->api;
        }

        uint64_t started = uptime_us();
        float spin = radiansf(a->spin) + a->yaw;

        glUseProgram(0);
        bool ok;
        switch (a->api) {
        case API_VULKAN: ok=api_config_vulkan(a->api_state,a->scene,a->detail) &&
                            api_draw_vulkan(a->api_state, canvas, top, w, h, spin, a->pitch); break;
        case API_D3D9:   ok=api_draw_d3d9(a->api_state, canvas, top, w, h, spin, a->pitch); break;
        default:         ok=api_draw_d3d11(a->api_state, canvas, top, w, h, spin, a->pitch); break;
        }

        glGetStats(&a->tris, &a->frags);
        GLenum error=glGetError();
        if (!ok || error!=GL_NO_ERROR) {
            a->render_failed=true;a->spinning=false;
            char note[144];
            snprintf(note,sizeof note,"%s bridge frame failed: ready=%u GL error=%#x; animation stopped, no CPU fallback",
                api_names[a->api],(unsigned)ok,(unsigned)error);
            log_write(3,"3d-gpu",note);
            return;
        }
        frame_finished(a,started,w,h);
        return;
    }

    uint64_t started = uptime_us();
    if (!gl_scene_render(&a->renderer, canvas, top, w, h, a->scene,
                         degreesf(a->yaw) + a->spin, degreesf(a->pitch),
                         a->lit, a->textured)) {
        a->render_failed = true; a->spinning = false;
        log_write(3, "3d-gpu", "desktop shader scene failed; animation stopped, no CPU fallback");
        return;
    }

    glGetStats(&a->tris, &a->frags);
    frame_finished(a,started,w,h);
}

/* ------------------------------------------------------------------ window */

#define BAR_H (bar_height())
#define BUTTON_COUNT 7

/* The bar is as tall as the text in it, not 34 pixels.
 *
 * It was 34, with the buttons inset five pixels and eight from the left, all
 * written plainly - and at twice the interface scale the font cell is around
 * forty-eight pixels, so centring a label inside a thirty-four pixel bar put
 * it at y = -6.  The system's own detector reported it:
 *
 *     text crowds text: "Lit" at 343,-6 lands on "Spinning" at 198,-6
 *
 * Above the top of the window, on every screen large enough to use scale two.
 */
static int bar_height(void) {
    return gui_font_height(FONT_UI) + 16 * gui_scale();
}

/* And a button is as wide as the widest thing that can appear in it.
 *
 * The widths used to be 118 and 96, chosen once for one font at one size.  A
 * label is measured now, which is what makes this survive both a larger
 * interface scale and a longer word - "Direct3D 11" needed a special case
 * before and needs none now, because it is simply measured like the rest.
 *
 * Each slot is sized for the widest label it can ever show, so a button does
 * not change width when what it says changes. */
static const char *slot_widest(int i) {
    switch (i) {
    case 0: return "Direct3D 11";
    case 1: return "Cylinder";
    case 2: return "Spinning";
    case 3: return "Unlit";
    case 4: return "Textured";
    case 5: return "Detail -";
    case 6: return "Detail +";
    default: return "";
    }
}

static int button_width(int i) {
    int w = gui_text_width(FONT_UI, slot_widest(i)) + 20 * gui_scale();
    int least = 48 * gui_scale();
    return w < least ? least : w;
}

static rect_t button_rect(int i, int width) {
    const int k = gui_scale();
    int x=8*k,y=5*k,bw=0;
    for (int j=0;j<=i;j++) {
        bw=button_width(j);
        if (bw>width-16*k) bw=width-16*k;
        if (x>8*k && x+bw>width-8*k) { x=8*k;y+=BAR_H; }
        if (j<i) x+=bw+6*k;
    }
    return rect_make(x,y,bw,BAR_H-10*k);
}

static int toolbar_height(int width) {
    rect_t last=button_rect(BUTTON_COUNT-1,width);
    return last.y+last.h+5*gui_scale();
}

static bool button_usable(const glapp_t *a,int i) {
    if (i==0 || i==2) return true;
    if (i==1) return a->api==API_GL || a->api==API_VULKAN;
    if (i>=5) return a->api==API_VULKAN && (i==5?a->detail>0:a->detail<3);
    return a->api==API_GL;
}

static const char *button_label(glapp_t *a, int i) {
    switch (i) {
    case 0: return api_names[a->api];
    case 1: return scene_names[a->scene];
    case 2: return a->spinning ? "Spinning" : "Still";
    case 3: return a->lit ? "Lit" : "Unlit";
    case 4: return a->textured ? "Textured" : "Plain";
    case 5: return "Detail -";
    case 6: return "Detail +";
    default: return "";
    }
}

static void paint(glapp_t *a, surface_t *s, int mx, int my) {
    int top=toolbar_height(s->width);
    int view_h = s->height - top - BAR_H*2;
    if (view_h < 8) { gui_clear(s, g_theme.window); return; }

    render(a, s, top, s->width, view_h);

    /* The control bar. */
    gui_fill(s, rect_make(0, 0, s->width, top), colour_shade(g_theme.window, 5));
    gui_hline(s, 0, top - 1, s->width, g_theme.window_border);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        rect_t r = button_rect(i,s->width);
        /* Material toggles are OpenGL-only, density is Vulkan-only, and the
         * Direct3D cube probes do not expose a nonfunctional scene selector. */
        bool usable = button_usable(a,i);
        gui_button(s, r, button_label(a, i), usable && rect_contains(r, mx, my),
                   false, usable);
    }

    /* The status line: what the renderer actually did this frame. */
    rect_t bar = rect_make(0, s->height - BAR_H*2, s->width, BAR_H*2);
    gui_fill(s, bar, colour_shade(g_theme.window, 5));
    gui_hline(s, 0, bar.y, s->width, g_theme.window_border);

    char line[160];
    if(a->render_failed)
        snprintf(line, sizeof line, "GPU scene failed; see 3d-gpu log");
    else if(s->gpu)
        snprintf(line, sizeof line, "%s   %u triangles   fragments not queried",
                 a->api == API_GL ? "GL GPU shaders" : "GPU raster / API bridge", a->tris);
    else
        snprintf(line, sizeof line, "%s CPU   %u triangles   %u fragments",
                 api_names[a->api], a->tris, a->frags);
    gui_text_clipped(s, FONT_UI, 8*gui_scale(), bar.y + 8*gui_scale(), s->width-16*gui_scale(), line, g_theme.text_dim);

    snprintf(line, sizeof line, "%llu.%03llu ms/frame   %d fps",
             (unsigned long long)(a->frame_us/1000u),
             (unsigned long long)(a->frame_us%1000u),a->fps);
    gui_text_clipped(s, FONT_UI, 8*gui_scale(), bar.y + BAR_H+8*gui_scale(), s->width-16*gui_scale(), line, g_theme.text_dim);
}

static bool glapp_proc(window_t *w, const wevent_t *ev) {
    glapp_t *a = w->data;

    switch (ev->kind) {
    case WE_PAINT:
        /* The scene turns by however much time has actually passed, not by a
         * fixed step per frame: at eight frames a second a fixed step is a
         * slideshow, and at sixty it is a blur.  Turning by elapsed time makes
         * the motion look the same at any rate the machine can manage. */
        if (a->spinning) {
            uint64_t now = uptime_us();
            if (a->last_spin_us) {
                uint64_t elapsed = now - a->last_spin_us;
                if (elapsed > 200000) elapsed = 200000;   /* after a long pause */
                a->spin += 60.0f * (float)elapsed / 1000000.0f;
            }
            a->last_spin_us = now;
        } else {
            a->last_spin_us = 0;
        }
        paint(a, w->canvas, a->last_x, a->last_y);
        return false;

    case WE_TICK:
        /* The scene is drawn from the paint callback, which now runs at frame
         * rate rather than tick rate - so this only has to keep the window in
         * the right mode. */
        w->continuous = a->spinning;
        return !a->spinning;               /* still one frame per tick when still */

    case WE_MOUSE_DOWN:
        if (ev->y < toolbar_height(w->canvas->width)) {
            for (int i = 0; i < BUTTON_COUNT; i++) {
                if (!rect_contains(button_rect(i,w->canvas->width), ev->x, ev->y)) continue;
                if (!button_usable(a,i)) return false;
                switch (i) {
                case 0: a->api = (a->api + 1) % API_COUNT;
                        if(a->api==API_GL)a->scene%=3;
                        if(a->api>API_VULKAN)a->scene=SCENE_CUBE;
                        break;
                case 1: a->scene = (a->scene + 1) % (a->api==API_GL?3:SCENE_COUNT); break;
                case 2: a->spinning = !a->spinning; break;
                case 3: a->lit = !a->lit; break;
                case 4: a->textured = !a->textured; break;
                case 5: a->detail--; break;
                case 6: a->detail++; break;
                }
                return true;
            }
            return false;
        }
        a->dragging = true;
        a->last_x = ev->x;
        a->last_y = ev->y;
        return false;

    case WE_MOUSE_UP:
        a->dragging = false;
        return false;

    case WE_MOUSE_MOVE:
        a->last_x = ev->x;
        a->last_y = ev->y;
        if (a->dragging) {
            a->yaw   += (float)ev->dx * 0.01f;
            a->pitch += (float)ev->dy * 0.01f;
            return true;
        }
        return ev->y < toolbar_height(w->canvas->width);

    case WE_KEY_DOWN:
        if (ev->key == ' ') { a->spinning = !a->spinning; return true; }
        if (ev->key == 'l' || ev->key == 'L') { a->lit = !a->lit; return true; }
        if (ev->key == 't' || ev->key == 'T') { a->textured = !a->textured; return true; }
        if (ev->key == KK_LEFT)  { a->yaw -= 0.1f; return true; }
        if (ev->key == KK_RIGHT) { a->yaw += 0.1f; return true; }
        if (ev->key == KK_UP)    { a->pitch -= 0.1f; return true; }
        if (ev->key == KK_DOWN)  { a->pitch += 0.1f; return true; }
        return false;

    case WE_RESIZE:
        return true;

    case WE_CLOSE:
        gl_scene_destroy(a->renderer);
        api_release(a);
        free(a);
        return false;

    default:
        return false;
    }
}

void app_gl_launch(wm_t *wm) {
    glapp_t *a = calloc(1, sizeof *a);
    if (!a) return;

    a->spinning = true;
    a->lit = true;
    a->textured = true;
    a->pitch = -0.25f;
    a->second_started = uptime_us();

    window_t *w = desktop_new_window(wm, "3D", ICON_DISPLAY, 640, 520, glapp_proc, a);
    if (!w) { free(a); return; }
    w->min_w = 320;
    w->min_h = 240;
}
