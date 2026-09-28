/* app_taskmgr.c - what this machine is doing right now.
 *
 * Five resources down the left, each with a small graph that keeps moving; the
 * one selected gets the large graph and the numbers on the right.  A second
 * tab lists the processes, sorted by whichever column was clicked.
 *
 * Three things about measuring a computer are easy to get wrong, and all three
 * are decided here rather than in the kernel:
 *
 *   A percentage needs an interval.  The kernel counts milliseconds spent busy
 *   and milliseconds spent idle since the machine was switched on, and nothing
 *   else; those totals answer "what has this machine been doing on average
 *   since it booted", which is never the question anyone is asking.  The
 *   number people want is the difference between two readings divided by the
 *   time between them, and the interval belongs to whoever is asking - so this
 *   window keeps the previous reading and does the subtraction itself.
 *
 *   Process time does not sum to machine time.  The idle thread is a process
 *   and accrues time exactly like the others, so adding up every process
 *   always comes to a hundred per cent whatever the machine is doing.  Busy
 *   versus idle is a separate measurement and is the only one that means
 *   anything on its own.
 *
 *   A graph of a single sample is not a graph.  Every resource keeps a history
 *   of its last readings and draws all of them, because the shape over the
 *   last minute says things a single number cannot: whether the machine is
 *   loaded or was loaded, whether something is periodic, whether a spike was
 *   one spike or many.
 *
 * On the graphics card there is a fourth thing worth stating plainly rather
 * than dressing up.  A card's utilisation is not readable from outside it: the
 * number Windows shows comes from the vendor's own firmware running on the
 * card and reporting back.  On the cards this system drives through a
 * framebuffer, there is no such firmware running and no honest number to show,
 * so the graphics panel shows what IS known - which card, how much memory, how
 * it is being driven, what it would need to be driven properly - and says that
 * utilisation is not one of the things that can be known yet.  A made-up
 * percentage would look better and mean nothing.
 */
#include "desktop.h"

/* ------------------------------------------------------------ the resources */

typedef enum {
    RES_CPU, RES_MEMORY, RES_DISK, RES_NETWORK, RES_GPU, RES_COUNT
} resource_t;

static const char *res_names[RES_COUNT] = {
    "CPU", "Memory", "Disk", "Network", "GPU"
};

/* How many readings each graph remembers.  At one reading every half second
 * this is a minute of history, which is long enough to see a pattern and short
 * enough that the graph still responds to what is happening now. */
#define HISTORY 120

typedef struct {
    float    v[HISTORY];      /* 0 to 1, oldest first                        */
    int      count;
    float    peak;            /* what the top of the graph means             */
    bool     peak_fixed;      /* true for things that are a share of a whole */
} series_t;

#define TASKMGR_MAX_GPUS 8
typedef struct {
    kgpuinfo_t info;
    series_t engine[4], memory, summary;
    bool engine_valid[4], memory_valid;
} gpu_history_t;

typedef enum { TAB_PERF, TAB_PROCS, TAB_COUNT } tab_t;

#define MAX_PROCS 64

typedef struct {
    tab_t      tab;
    resource_t selected;

    series_t   graph[RES_COUNT];

    /* The last reading, so the next one can be turned into a rate. */
    ksysinfo_t info, prev_info;
    uint64_t   mem[6];
    uint64_t   net_rx, net_tx, prev_net_rx, prev_net_tx;
    uint64_t   taken_ms, prev_taken_ms;
    bool       have_prev;

    /* What the last reading came out as, in the units each one is shown in. */
    float      cpu_pct;
    bool       cpu_usage_valid;
    float      mem_pct;
    uint64_t   disk_bps;
    uint64_t   net_bps;

    kgpuinfo_t gpu;
    bool       have_gpu;
    gpu_history_t gpus[TASKMGR_MAX_GPUS];
    unsigned gpu_count, selected_gpu, gpu_order[TASKMGR_MAX_GPUS];
    int side_scroll;
    knetinfo_t nic;
    bool       have_nic;

    kprocinfo_t procs[MAX_PROCS];
    uint64_t    proc_cpu_prev[MAX_PROCS];
    int         proc_prev_pid[MAX_PROCS];
    int         proc_prev_count;
    float       proc_pct[MAX_PROCS];
    int         proc_count;
    int         sort_column;          /* 0 name, 1 pid, 2 cpu, 3 memory      */
    bool        sort_desc;
    int         scroll;
    int         detail_scroll, detail_height;
    kcpuinfo_t   cpus[256];
    unsigned    cpu_records;
} taskmgr_t;

/* --------------------------------------------------------------- geometry */

/* Every one of these is a length on screen, so every one of them scales.
 *
 * They did not, and the text drawn against them did.  At twice the interface
 * size a row of the list on the left was still 62 pixels apart while its label
 * had grown to fill more than that, so each row was painted over the one below
 * it - "3%" across "Memory", "0 B" across "Network".  The tab strip was still
 * 38 pixels tall with a rule along the bottom, and the rule went through the
 * word "Performance" rather than under it.
 *
 * Scaling the constants themselves fixes every use at once, and leaves the
 * numbers reading as the sizes somebody chose while looking at a screen. */
#define TAB_H      (38 * gui_scale())
#define TAB_PAD    (10 * gui_scale())   /* from the left edge of the window  */
#define TAB_TEXT_PAD (14 * gui_scale()) /* either side of the label          */
#define TAB_GAP    (4 * gui_scale())    /* between one tab and the next      */
#define SIDE_W     (186 * gui_scale())
#define GRAPH_W    (68 * gui_scale())   /* the sparkline at the right        */
#define ROW_H      (26 * gui_scale())
#define HEAD_H     (28 * gui_scale())

/* One entry in the list down the left: how far apart they sit, and how tall
 * each is.  The gap between the two is what stops them touching. */
#define SIDE_STRIDE (62 * gui_scale())
#define SIDE_ITEM_H (58 * gui_scale())

static const char *tab_name(int i) {
    return (i == TAB_PERF) ? "Performance" : "Processes";
}

/* Where a tab sits, measured from its own text rather than assumed.
 *
 * These used to be a fixed 112 pixels on a 116 pixel stride, which is wider
 * than "Processes" and narrower than "Performance" - so the longer label ran
 * out of its own tab and into the one beside it, and the two words were drawn
 * on top of each other.  A label is as wide as it is; the tab has to follow it
 * rather than the other way round.
 */
static rect_t tab_rect(int i) {
    int x = TAB_PAD;
    for (int k = 0; k < i; k++)
        x += gui_text_width(FONT_UI, tab_name(k)) + TAB_TEXT_PAD * 2 + TAB_GAP;
    int w = gui_text_width(FONT_UI, tab_name(i)) + TAB_TEXT_PAD * 2;
    return rect_make(x, 6 * gui_scale(), w, TAB_H - 12 * gui_scale());
}
static rect_t body_rect(surface_t *s) {
    return rect_make(0, TAB_H, s->width, s->height - TAB_H);
}
static rect_t side_rect(surface_t *s, int i) {
    const int k = gui_scale();
    int y=i<RES_GPU ? i*SIDE_STRIDE : RES_GPU*SIDE_STRIDE+(i-RES_GPU)*84*k;
    return rect_make(8 * k, TAB_H + 8 * k + y,
                     SIDE_W - 16 * k, i<RES_GPU?SIDE_ITEM_H:80*k);
}
static rect_t scrolled_side_rect(taskmgr_t *t,surface_t *s,int i) {
    rect_t r=side_rect(s,i);r.y-=t->side_scroll;return r;
}
static int side_count(taskmgr_t *t) {return RES_GPU+(t->gpu_count?t->gpu_count:1);}
static int side_height(taskmgr_t *t,surface_t *s) {
    rect_t last=side_rect(s,side_count(t)-1);return last.y+last.h-TAB_H+8*gui_scale();
}
static rect_t detail_rect(surface_t *s) {
    return rect_make(SIDE_W, TAB_H, s->width - SIDE_W, s->height - TAB_H);
}

/* ------------------------------------------------------------- the history */

static void series_push(series_t *g, float value, float peak) {
    if (value < 0) value = 0;
    if (g->count < HISTORY) {
        g->v[g->count++] = value;
    } else {
        for (int i = 1; i < HISTORY; i++) g->v[i - 1] = g->v[i];
        g->v[HISTORY - 1] = value;
    }

    if (g->peak_fixed) { g->peak = peak; return; }

    /* For a rate there is no natural full scale, so the graph scales to the
     * largest reading it has kept.  Recomputing it from the history rather
     * than remembering a high-water mark means the scale comes back down after
     * a burst, instead of flattening everything that follows it forever. */
    float top = 0;
    for (int i = 0; i < g->count; i++) if (g->v[i] > top) top = g->v[i];
    g->peak = top > 0 ? top : 1;
}

/* Keep a time slot for an unavailable reading, without inventing a zero or
 * joining a line across the missing interval. */
static void gpu_series_push(series_t *s,float value,bool valid,float peak) {
    s->peak_fixed=true;series_push(s,valid?value:0,peak);
    if(!valid)s->v[s->count-1]=-1;
}
static bool same_gpu(const kgpuinfo_t *a,const kgpuinfo_t *b) {
    return a->pci_vendor==b->pci_vendor && a->pci_device==b->pci_device &&
        a->bus==b->bus && a->slot==b->slot && a->func==b->func;
}
static void sample_gpus(taskmgr_t *t) {
    unsigned count=0,boot=0;
    for(;count<TASKMGR_MAX_GPUS;count++) {
        kgpuinfo_t info;
        if(enum_gpu(count,&info)!=0)break;
        gpu_history_t *h=&t->gpus[count];
        if(!same_gpu(&h->info,&info))memset(h,0,sizeof *h);
        h->info=info;
        if(info.boot_display)boot=count;
        int maximum=-1;
        for(unsigned e=0;e<4;e++) {
            int pct=info.engine_percent[e];
            h->engine_valid[e]=info.engines_sampled && pct>=0 && pct<=100;
            gpu_series_push(&h->engine[e],(float)pct/100.0f,h->engine_valid[e],1);
            if(h->engine_valid[e] && pct>maximum)maximum=pct;
        }
        gpu_series_push(&h->summary,(float)maximum/100.0f,maximum>=0,1);
        /* This counter covers NVIDIA allocations tracked by Kestrel only;
         * it is not the complete RM/firmware residency or a shared RAM budget. */
        h->memory_valid=info.pci_vendor==0x10de && info.driver_version[0] &&
            info.vram_exact && info.vram_bytes && info.vram_used<=info.vram_bytes;
        gpu_series_push(&h->memory,(float)info.vram_used,h->memory_valid,
                        info.vram_bytes?(float)info.vram_bytes:1);
    }
    for(unsigned i=count;i<t->gpu_count;i++)memset(&t->gpus[i],0,sizeof t->gpus[i]);
    if(!t->have_gpu || t->selected_gpu>=count)t->selected_gpu=boot;
    t->gpu_count=count;t->have_gpu=count!=0;
    if(count) {
        unsigned n=0;t->gpu_order[n++]=boot;
        for(unsigned i=0;i<count;i++)if(i!=boot)t->gpu_order[n++]=i;
        t->gpu=t->gpus[t->selected_gpu].info;
    } else memset(&t->gpu,0,sizeof t->gpu);
}

/* ------------------------------------------------------------- the readings */

static void sample(taskmgr_t *t) {
    t->cpu_usage_valid = false;
    t->prev_info = t->info;
    t->prev_taken_ms = t->taken_ms;
    t->prev_net_rx = t->net_rx;
    t->prev_net_tx = t->net_tx;

    sysinfo(&t->info);
    meminfo(t->mem);
    t->taken_ms = uptime_ms();
    t->cpu_records=0;
    while(t->cpu_records<256 && enum_cpu(t->cpu_records,&t->cpus[t->cpu_records])==0)
        t->cpu_records++;

    /* Every interface added together: the question "how much network is this
     * machine using" is not about one card. */
    t->net_rx = t->net_tx = 0;
    t->have_nic = false;
    for (uint32_t i = 0; i < 8; i++) {
        knetinfo_t n;
        if (enum_net(i, &n) != 0) break;
        t->net_rx += n.rx_bytes;
        t->net_tx += n.tx_bytes;
        if (!t->have_nic || (n.link_up && !t->nic.link_up)) { t->nic = n; t->have_nic = true; }
    }

    sample_gpus(t);

    /* Memory is a share of a whole, so its graph is always scaled to the whole
     * - a memory graph that rescaled itself would make a machine using two per
     * cent look identical to one that is nearly full. */
    uint64_t total = t->info.mem_total;
    uint64_t used = total > t->info.mem_free ? total - t->info.mem_free : 0;
    t->mem_pct = total ? (float)used / (float)total : 0;
    t->graph[RES_MEMORY].peak_fixed = true;
    series_push(&t->graph[RES_MEMORY], t->mem_pct, 1.0f);

    if (!t->have_prev) { t->have_prev = true; return; }

    uint64_t dt = t->taken_ms - t->prev_taken_ms;
    if (!dt) return;

    /* CPU: the share of the interval that was not idle.  Both counters move,
     * so the denominator is what they moved by together and not the wall
     * clock - which is what makes this right on a machine with more than one
     * processor, where the two can add up to more than the time that passed. */
    if(t->info.cpu_time_valid && t->prev_info.cpu_time_valid &&
       t->info.cpu_busy_ms>=t->prev_info.cpu_busy_ms &&
       t->info.cpu_idle_ms>=t->prev_info.cpu_idle_ms) {
        uint64_t busy = t->info.cpu_busy_ms - t->prev_info.cpu_busy_ms;
        uint64_t idle = t->info.cpu_idle_ms - t->prev_info.cpu_idle_ms;
        uint64_t ticks = busy + idle;
        t->cpu_usage_valid = ticks != 0;
        t->cpu_pct = ticks ? (float)busy / (float)ticks : 0;
    }
    t->graph[RES_CPU].peak_fixed = true;
    gpu_series_push(&t->graph[RES_CPU], t->cpu_pct, t->cpu_usage_valid, 1.0f);

    uint64_t disk = (t->info.disk_read_bytes - t->prev_info.disk_read_bytes) +
                    (t->info.disk_write_bytes - t->prev_info.disk_write_bytes);
    t->disk_bps = disk * 1000 / dt;
    series_push(&t->graph[RES_DISK], (float)t->disk_bps, 0);

    uint64_t net = (t->net_rx - t->prev_net_rx) + (t->net_tx - t->prev_net_tx);
    t->net_bps = net * 1000 / dt;
    series_push(&t->graph[RES_NETWORK], (float)t->net_bps, 0);

    /* A missing sample is not an idle engine. Only graph a measured window. */
    if(t->have_gpu && t->gpu.engines_sampled && t->gpu.engine_percent[0]>=0) {
        t->graph[RES_GPU].peak_fixed=true;
        series_push(&t->graph[RES_GPU],(float)t->gpu.engine_percent[0]/100.0f,1.0f);
    }
}

/* Processes, and how much of the interval each one used. */
static void sample_procs(taskmgr_t *t) {
    /* What the previous reading said, kept by process number so that a process
     * appearing or exiting does not make every other row wrong. */
    uint64_t was[MAX_PROCS];
    int was_pid[MAX_PROCS];
    int was_count = t->proc_prev_count;
    for (int i = 0; i < was_count && i < MAX_PROCS; i++) {
        was[i] = t->proc_cpu_prev[i];
        was_pid[i] = t->proc_prev_pid[i];
    }

    t->proc_count = 0;
    for (uint32_t i = 0; i < MAX_PROCS; i++) {
        kprocinfo_t p;
        if (proclist(i, &p) != 0) break;
        t->procs[t->proc_count++] = p;
    }

    uint64_t dt = t->taken_ms - t->prev_taken_ms;
    for (int i = 0; i < t->proc_count; i++) {
        uint64_t before = 0;
        bool seen = false;
        for (int j = 0; j < was_count; j++)
            if (was_pid[j] == t->procs[i].pid) { before = was[j]; seen = true; break; }

        uint64_t used = (seen && t->procs[i].cpu_ms >= before)
                        ? t->procs[i].cpu_ms - before : 0;
        /* A process's share of machine capacity, matching the aggregate graph.
         * AP batch work is currently kernel-worker time, not process charges. */
        uint64_t capacity = dt * (t->info.cpu_running ? t->info.cpu_running : 1u);
        t->proc_pct[i] = (capacity && seen) ? (float)used / (float)capacity : 0;
        if (t->proc_pct[i] > 1) t->proc_pct[i] = 1;
    }

    for (int i = 0; i < t->proc_count; i++) {
        t->proc_cpu_prev[i] = t->procs[i].cpu_ms;
        t->proc_prev_pid[i] = t->procs[i].pid;
    }
    t->proc_prev_count = t->proc_count;
}

static void sort_procs(taskmgr_t *t) {
    for (int i = 1; i < t->proc_count; i++) {
        kprocinfo_t p = t->procs[i];
        float pct = t->proc_pct[i];
        int j = i - 1;
        while (j >= 0) {
            int cmp;
            switch (t->sort_column) {
            case 1:  cmp = t->procs[j].pid - p.pid; break;
            case 2:  cmp = (t->proc_pct[j] > pct) - (t->proc_pct[j] < pct); break;
            case 3:  cmp = (t->procs[j].mem_bytes > p.mem_bytes) -
                           (t->procs[j].mem_bytes < p.mem_bytes); break;
            default: cmp = strcmp(t->procs[j].name, p.name); break;
            }
            if (t->sort_desc) cmp = -cmp;
            if (cmp <= 0) break;
            t->procs[j + 1] = t->procs[j];
            t->proc_pct[j + 1] = t->proc_pct[j];
            j--;
        }
        t->procs[j + 1] = p;
        t->proc_pct[j + 1] = pct;
    }
}

/* --------------------------------------------------------------- formatting */

static void bytes_str(char *out, size_t cap, uint64_t n) {
    if (n >= (1ull << 30)) snprintf(out, cap, "%llu.%llu GB",
                                    (unsigned long long)(n >> 30),
                                    (unsigned long long)((n >> 20) % 1024) * 10 / 1024);
    else if (n >= (1ull << 20)) snprintf(out, cap, "%llu.%llu MB",
                                    (unsigned long long)(n >> 20),
                                    (unsigned long long)((n >> 10) % 1024) * 10 / 1024);
    else if (n >= 1024) snprintf(out, cap, "%llu KB", (unsigned long long)(n >> 10));
    else snprintf(out, cap, "%llu B", (unsigned long long)n);
}

static void rate_str(char *out, size_t cap, uint64_t bps) {
    char b[32];
    bytes_str(b, sizeof b, bps);
    snprintf(out, cap, "%s/s", b);
}

static void pct_str(char *out, size_t cap, float f) {
    int whole = (int)(f * 100.0f + 0.5f);
    if (whole > 100) whole = 100;
    snprintf(out, cap, "%d%%", whole);
}

static colour_t res_colour(resource_t r) {
    switch (r) {
    case RES_CPU:     return RGB(0x4d, 0xa6, 0xff);
    case RES_MEMORY:  return RGB(0xa9, 0x7c, 0xff);
    case RES_DISK:    return RGB(0x4a, 0xd6, 0x91);
    case RES_NETWORK: return RGB(0xff, 0xb1, 0x4d);
    default:          return RGB(0x8a, 0x8a, 0x8a);
    }
}

/* ------------------------------------------------------------- the graphs */

static void draw_graph(surface_t *s, rect_t r, const series_t *g, colour_t c,
                       bool grid) {
    gui_fill(s, r, g_theme.field);
    gui_frame(s, r, g_theme.field_border);
    if (r.w < 8 || r.h < 8) return;

    if (grid) {
        colour_t line = colour_mix(g_theme.field, g_theme.field_border, 140);
        for (int i = 1; i < 4; i++) gui_hline(s, r.x + 1, r.y + r.h * i / 4, r.w - 2, line);
        for (int i = 1; i < 6; i++) gui_vline(s, r.x + r.w * i / 6, r.y + 1, r.h - 2, line);
    }

    if (g->count < 2 || g->peak <= 0) return;

    /* The history is drawn right-aligned, so "now" is always at the same edge
     * and the line grows leftwards as readings accumulate rather than
     * stretching to fill and rescaling under the eye. */
    int inner_w = r.w - 2, inner_h = r.h - 2;
    int prev_x = 0, prev_y = 0;
    bool have_previous=false;
    for (int i = 0; i < g->count; i++) {
        if(g->v[i]<0){have_previous=false;continue;}
        int slot = HISTORY - g->count + i;
        int x = r.x + 1 + slot * inner_w / (HISTORY - 1);
        float f = g->v[i] / g->peak;
        if (f > 1) f = 1;
        int y = r.y + 1 + inner_h - (int)(f * inner_h);

        if (have_previous) {
            gui_line(s, prev_x, prev_y, x, y, c);
            /* Filled under the line, faintly, so several graphs on one screen
             * stay tellable apart at a glance rather than only on inspection. */
            int mid = (prev_y + y) / 2;
            gui_vline(s,x,mid,r.y+r.h-1-mid,colour_mix(g_theme.field,c,40));
        }
        prev_x = x; prev_y = y;have_previous=true;
    }
}

/* One resource in the list down the left. */
static void draw_side(taskmgr_t *t, surface_t *s, int i) {
    rect_t r = scrolled_side_rect(t,s,i);
    if(i>=RES_GPU) {
        unsigned order=(unsigned)(i-RES_GPU);
        if(order>=t->gpu_count) {
            gui_text_clipped(s,FONT_UI,r.x+12,r.y+6,r.w-24,"No GPU found",g_theme.text_dim);
            return;
        }
        unsigned index=t->gpu_order[order];gpu_history_t *h=&t->gpus[index];
        bool selected=t->selected==RES_GPU && t->selected_gpu==index;
        int sc=gui_scale(),line=gui_font_height(FONT_UI);
        if(selected){gui_fill(s,r,colour_mix(g_theme.window,g_theme.accent,40));
            gui_fill(s,rect_make(r.x,r.y,3*sc,r.h),g_theme.accent);}
        char label[48],value[64];snprintf(label,sizeof label,"GPU %u",order);
        int best=-1;for(unsigned e=0;e<4;e++)if(h->engine_valid[e]&&h->info.engine_percent[e]>best)best=h->info.engine_percent[e];
        if(best>=0)snprintf(value,sizeof value,"%d%%",best);
        else snprintf(value,sizeof value,"Unavailable");
        gui_text_clipped(s,FONT_UI,r.x+10*sc,r.y+6*sc,r.w-GRAPH_W-20*sc,label,g_theme.text);
        draw_graph(s,rect_make(r.x+r.w-GRAPH_W-8*sc,r.y+6*sc,GRAPH_W,24*sc),&h->summary,res_colour(RES_GPU),false);
        gui_text_clipped(s,FONT_UI,r.x+10*sc,r.y+line+10*sc,r.w-20*sc,h->info.name,g_theme.text_dim);
        gui_text_clipped(s,FONT_UI,r.x+10*sc,r.y+2*line+14*sc,r.w-20*sc,value,g_theme.text_dim);
        return;
    }
    bool on = (t->selected == (resource_t)i);

    if (on) {
        gui_fill(s, r, colour_mix(g_theme.window, g_theme.accent, 40));
        gui_fill(s, rect_make(r.x, r.y, 3, r.h), g_theme.accent);
    }

    char value[40];
    switch (i) {
    case RES_CPU:
        if(t->cpu_usage_valid)pct_str(value, sizeof value, t->cpu_pct);
        else snprintf(value,sizeof value,"Unavailable");
        break;
    case RES_MEMORY:  pct_str(value, sizeof value, t->mem_pct); break;
    case RES_DISK:    rate_str(value, sizeof value, t->disk_bps); break;
    case RES_NETWORK: rate_str(value, sizeof value, t->net_bps); break;
    default:          snprintf(value, sizeof value, "%s",
                               t->have_gpu ? (t->gpu.accel==2 ? "accelerated" : "framebuffer") : "none"); break;
    }

    /* The graph owns the right of the row, so the words get what is left and
     * are cut to it.  Drawn without a limit, a long reading - "framebuffer" is
     * the one that showed it - runs on underneath the graph and reads as two
     * things printed on top of each other. */
    int text_w = r.w - GRAPH_W - 24;
    if (text_w < 24) text_w = 24;

    gui_text_clipped(s, FONT_UI, r.x + 12, r.y + 6, text_w, i==RES_CPU?"CPU":res_names[i],
                     on ? g_theme.text_bright : g_theme.text);
    gui_text_clipped(s, FONT_UI, r.x + 12, r.y + 8 + gui_font_height(FONT_UI),
                     text_w, value, g_theme.text_dim);

    draw_graph(s, rect_make(r.x + r.w - GRAPH_W - 8, r.y + 8, GRAPH_W, r.h - 16),
               &t->graph[i], res_colour((resource_t)i), false);
}

/* ---------------------------------------------------------- the right pane */

/* The same column problem as the System window, and this file did not use the
 * interface scale anywhere at all - so every row here was laid out for a
 * display of one size and drawn at another. */
static int field_row(surface_t *s, int x, int y, int lw, const char *k, const char *v) {
    const int sc = gui_scale();
    lw *= sc;

    int available=s->width-x-24*sc;
    if(lw>available/2)lw=available/2;
    gui_text_clipped(s, FONT_UI, x, y, lw-8*sc, k, g_theme.text_dim);
    gui_text_clipped(s, FONT_UI, x + lw, y, s->width - x - lw - 16 * sc, v,
                     g_theme.text);
    return y + gui_font_height(FONT_UI) + 7 * sc;
}

#include "taskmgr_gpu.h"

static void draw_detail(taskmgr_t *t, surface_t *s) {
    if(t->selected==RES_GPU){draw_gpu_detail(t,s);return;}
    rect_t d = detail_rect(s);
    rect_t saved_clip=surface_clip(s);
    surface_set_clip(s,rect_intersection(saved_clip,rect_make(d.x,d.y,d.w-24,d.h)));
    int x = d.x + 20 * gui_scale(), y = d.y + 16 * gui_scale()-t->detail_scroll;
    resource_t r = t->selected;

    gui_text(s, FONT_UI, x, y, res_names[r], g_theme.text_bright);
    y += gui_font_height(FONT_UI) + 10;

    rect_t gr = rect_make(x, y, d.w - 40, 150);
    if (gr.w > 40) draw_graph(s, gr, &t->graph[r], res_colour(r), true);

    /* What the top of the graph means.  A graph without its scale is a
     * decoration - the same picture can be a machine at rest or one at its
     * limit, and only this line says which. */
    char scale[48];
    if (r == RES_CPU || r == RES_MEMORY) snprintf(scale, sizeof scale, "100%%");
    else if(r==RES_GPU)snprintf(scale,sizeof scale,"%s",t->graph[RES_GPU].count?"100% (3D)":"No usage sample");
    else rate_str(scale, sizeof scale, (uint64_t)t->graph[r].peak);
    gui_text(s, FONT_UI, gr.x + 6, gr.y + 4, scale, g_theme.text_dim);
    gui_text(s, FONT_UI, gr.x + gr.w - 60, gr.y + gr.h + 4, "now", g_theme.text_dim);
    gui_text(s, FONT_UI, gr.x, gr.y + gr.h + 4, "60 seconds ago", g_theme.text_dim);

    y = gr.y + gr.h + 8 + gui_font_height(FONT_UI) + 12;
    int lw = 170;
    char buf[128], b2[64];

    switch (r) {
    case RES_CPU:
        if(t->cpu_usage_valid)pct_str(buf, sizeof buf, t->cpu_pct);
        else snprintf(buf,sizeof buf,"Unavailable");
        y = field_row(s, x, y, lw, "CPU utilization", buf);
        y = field_row(s, x, y, lw, "Usage scope", "Scheduler + online kernel workers");
        y = field_row(s, x, y, lw, "Processor", t->info.cpu);
        snprintf(buf, sizeof buf, "%u", t->info.cpu_cores);
        y = field_row(s, x, y, lw, "Physical cores", buf);
        if(t->info.cpu_perf_cores || t->info.cpu_eff_cores)
            snprintf(buf,sizeof buf,"%u P / %u E",t->info.cpu_perf_cores,t->info.cpu_eff_cores);
        else snprintf(buf,sizeof buf,"%s",t->info.cpu_hybrid?"Not fully enumerated":"Not hybrid");
        y = field_row(s, x, y, lw, "Core types", buf);
        snprintf(buf,sizeof buf,"%u / %u online",t->info.cpu_running,t->info.cpu_threads);
        y = field_row(s, x, y, lw, "CPU threads", buf);
        if(t->info.cpu_application_threads)
            snprintf(buf,sizeof buf,"%u logical CPU%s",t->info.cpu_application_threads,
                     t->info.cpu_application_threads==1?"":"s");
        else snprintf(buf,sizeof buf,"Unavailable");
        y = field_row(s, x, y, lw, "Application scheduling", buf);
        snprintf(buf,sizeof buf,"%u",t->info.cpu_sockets);
        y = field_row(s, x, y, lw, "Sockets", buf);
        if(t->info.cpu_base_mhz)snprintf(buf,sizeof buf,"%u / %u MHz",t->info.cpu_base_mhz,t->info.cpu_max_mhz);
        else snprintf(buf,sizeof buf,"Unavailable");
        y = field_row(s, x, y, lw, "Base / max clock", buf);
        if(t->info.cpu_l1)bytes_str(buf,sizeof buf,t->info.cpu_l1);
        else snprintf(buf,sizeof buf,"Unavailable");
        y = field_row(s, x, y, lw, "L1 cache (observed)", buf);
        if(t->info.cpu_l2)bytes_str(buf,sizeof buf,t->info.cpu_l2);
        else snprintf(buf,sizeof buf,"Unavailable");
        y = field_row(s, x, y, lw, "L2 cache (observed)", buf);
        if(t->info.cpu_l3)bytes_str(buf,sizeof buf,t->info.cpu_l3);
        else snprintf(buf,sizeof buf,"Unavailable");
        y = field_row(s, x, y, lw, "L3 cache (observed)", buf);
        snprintf(buf, sizeof buf, "%u", t->info.proc_count);
        y = field_row(s, x, y, lw, "Processes", buf);
        /* And the threads inside them, which is the number that moves when a
         * program is busy rather than when one is started.  Both, because
         * neither alone says whether the machine is loaded. */
        snprintf(buf, sizeof buf, "%u", t->info.thread_count);
        y = field_row(s, x, y, lw, "Software threads", buf);
        snprintf(buf, sizeof buf, "%llu.%llu s",
                 (unsigned long long)(t->info.uptime_ms / 1000),
                 (unsigned long long)((t->info.uptime_ms % 1000) / 100));
        y = field_row(s, x, y, lw, "Up for", buf);
        y = field_row(s, x, y, lw, "Frequency samples", "Active clock / idle-inclusive MHz");
        for(unsigned i=0;i<t->cpu_records;i++) {
            kcpuinfo_t *c=&t->cpus[i];
            char label[48];
            snprintf(label,sizeof label,"CPU %u / %s / APIC %u",i,c->kind==1?"P":c->kind==2?"E":"?",c->apic_id);
            if(!c->online)snprintf(buf,sizeof buf,"Offline");
            else if(c->frequency_valid&KCPU_FREQ_ACTIVE_VALID)
                snprintf(buf,sizeof buf,"%u / %u MHz",c->active_mhz,c->effective_mhz);
            else if(c->frequency_valid&KCPU_FREQ_EFFECTIVE_VALID)
                snprintf(buf,sizeof buf,"Active unknown / %u MHz",c->effective_mhz);
            else snprintf(buf,sizeof buf,"Sample unavailable");
            y=field_row(s,x,y,lw,label,buf);
        }
        break;

    case RES_MEMORY: {
        uint64_t total = t->info.mem_total;
        uint64_t used = total > t->info.mem_free ? total - t->info.mem_free : 0;
        bytes_str(buf, sizeof buf, used);
        bytes_str(b2, sizeof b2, total);
        char both[128];
        snprintf(both, sizeof both, "%s of %s", buf, b2);
        y = field_row(s, x, y, lw, "In use", both);
        bytes_str(buf, sizeof buf, t->info.mem_free);
        y = field_row(s, x, y, lw, "Available", buf);
        bytes_str(buf, sizeof buf, t->info.heap_used);
        bytes_str(b2, sizeof b2, t->info.heap_total);
        snprintf(both, sizeof both, "%s of %s", buf, b2);
        y = field_row(s, x, y, lw, "Kernel heap", both);
        break;
    }

    case RES_DISK:
        rate_str(buf, sizeof buf, t->disk_bps);
        y = field_row(s, x, y, lw, "Throughput", buf);
        bytes_str(buf, sizeof buf, t->info.disk_read_bytes);
        y = field_row(s, x, y, lw, "Read since boot", buf);
        bytes_str(buf, sizeof buf, t->info.disk_write_bytes);
        y = field_row(s, x, y, lw, "Written since boot", buf);
        snprintf(buf, sizeof buf, "%u", t->info.block_count);
        y = field_row(s, x, y, lw, "Disks", buf);
        break;

    case RES_NETWORK:
        rate_str(buf, sizeof buf, t->net_bps);
        y = field_row(s, x, y, lw, "Throughput", buf);
        if (t->have_nic) {
            y = field_row(s, x, y, lw, "Adapter", t->nic.model);
            y = field_row(s, x, y, lw, "Link",
                          t->nic.link_up ? "up" : "down");
            bytes_str(buf, sizeof buf, t->net_rx);
            y = field_row(s, x, y, lw, "Received", buf);
            bytes_str(buf, sizeof buf, t->net_tx);
            y = field_row(s, x, y, lw, "Sent", buf);
        } else {
            y = field_row(s, x, y, lw, "Adapter", "none found");
        }
        break;


    default:
        break;
    }
    t->detail_height=y+t->detail_scroll-d.y+16;
    surface_set_clip(s,saved_clip);
    /* Keep the track clear of the window's resize-edge hit band. */
    gui_scrollbar(s,rect_make(d.x+d.w-22,d.y,12,d.h),t->detail_scroll,d.h,t->detail_height);
}

/* ------------------------------------------------------------ the processes */

static const struct { const char *name; int width; } columns[] = {
    { "Name", 220 }, { "PID", 70 }, { "CPU", 90 }, { "Memory", 120 },
};

static void draw_procs(taskmgr_t *t, surface_t *s) {
    rect_t body = body_rect(s);
    int x = 16, y = body.y + 8;

    gui_fill(s, rect_make(0, y, s->width, HEAD_H),
             colour_mix(g_theme.window, g_theme.field, 120));
    int cx = x;
    for (int i = 0; i < 4; i++) {
        char label[40];
        snprintf(label, sizeof label, "%s%s", columns[i].name,
                 t->sort_column == i ? (t->sort_desc ? "  v" : "  ^") : "");
        /* Bounded to its own column, like the names below it.
         *
         * A header carries its column's name plus a sort arrow, and the arrow
         * is added at run time - so a heading that fitted when it was written
         * can stop fitting the moment somebody sorts by it, and then it prints
         * across the next heading. */
        gui_text_clipped(s, FONT_UI, cx, y + 5, columns[i].width - 8, label,
                         t->sort_column == i ? g_theme.text_bright
                                             : g_theme.text_dim);
        cx += columns[i].width;
    }
    y += HEAD_H;
    gui_hline(s, 0, y, s->width, g_theme.field_border);
    y += 1;

    int first = t->scroll;
    for (int i = first; i < t->proc_count; i++) {
        if (y + ROW_H > body.y + body.h) break;

        if ((i - first) & 1)
            gui_fill(s, rect_make(0, y, s->width, ROW_H),
                     colour_mix(g_theme.window, g_theme.field, 60));

        /* A faint bar behind the row, as wide as that process's share of the
         * processor.  It costs nothing and turns a column of numbers into
         * something that can be read without reading. */
        if (t->proc_pct[i] > 0.01f) {
            int w = (int)(t->proc_pct[i] * (float)s->width);
            gui_fill(s, rect_make(0, y, w, ROW_H),
                     colour_mix(g_theme.window, res_colour(RES_CPU), 28));
        }

        char buf[64];
        cx = x;
        gui_text_clipped(s, FONT_UI, cx, y + 4, columns[0].width - 8,
                         t->procs[i].name, g_theme.text);
        cx += columns[0].width;

        snprintf(buf, sizeof buf, "%d", t->procs[i].pid);
        gui_text(s, FONT_UI, cx, y + 4, buf, g_theme.text_dim);
        cx += columns[1].width;

        pct_str(buf, sizeof buf, t->proc_pct[i]);
        gui_text(s, FONT_UI, cx, y + 4, buf,
                 t->proc_pct[i] > 0.01f ? g_theme.text : g_theme.text_dim);
        cx += columns[2].width;

        bytes_str(buf, sizeof buf, t->procs[i].mem_bytes);
        gui_text(s, FONT_UI, cx, y + 4, buf, g_theme.text_dim);

        y += ROW_H;
    }

    if (!t->proc_count)
        gui_text(s, FONT_UI, x, y + 8, "no processes", g_theme.text_dim);
}

/* ------------------------------------------------------------------ painting */

static void taskmgr_paint(taskmgr_t *t, surface_t *s) {
    gui_clear(s, g_theme.window);

    gui_fill(s, rect_make(0, 0, s->width, TAB_H),
             colour_mix(g_theme.window, g_theme.field, 90));
    gui_hline(s, 0, TAB_H - 1, s->width, g_theme.field_border);

    for (int i = 0; i < TAB_COUNT; i++) {
        rect_t r = tab_rect(i);
        bool on = (t->tab == (tab_t)i);
        if (on) {
            gui_round_rect_aa(s, r, 6, g_theme.window);
            gui_fill(s, rect_make(r.x + 8, r.y + r.h - 2, r.w - 16, 2), g_theme.accent);
        }
        /* Centred in its own tab, both ways.  Sitting the text at a fixed
         * offset from the top left leaves it looking dropped when the tab is
         * taller than the line. */
        gui_text_centred(s, FONT_UI, r, tab_name(i),
                         on ? g_theme.text_bright : g_theme.text_dim);
    }

    if (t->tab == TAB_PROCS) { draw_procs(t, s); return; }

    gui_fill(s, rect_make(0, TAB_H, SIDE_W, s->height - TAB_H),
             colour_mix(g_theme.window, g_theme.field, 50));
    gui_vline(s, SIDE_W - 1, TAB_H, s->height - TAB_H, g_theme.field_border);
    rect_t saved=surface_clip(s);
    surface_set_clip(s,rect_intersection(saved,rect_make(0,TAB_H,SIDE_W,s->height-TAB_H)));
    int maximum=side_height(t,s)-(s->height-TAB_H);if(maximum<0)maximum=0;
    if(t->side_scroll>maximum)t->side_scroll=maximum;
    for (int i = 0; i < side_count(t); i++) draw_side(t, s, i);
    if(maximum)gui_scrollbar(s,rect_make(SIDE_W-8*gui_scale(),TAB_H,6*gui_scale(),s->height-TAB_H),
        t->side_scroll,s->height-TAB_H,side_height(t,s));
    surface_set_clip(s,saved);

    draw_detail(t, s);
}

/* -------------------------------------------------------------- the window */

static bool taskmgr_proc(window_t *w, const wevent_t *ev) {
    taskmgr_t *t = (taskmgr_t *)w->data;

    switch (ev->kind) {
    case WE_PAINT:
        taskmgr_paint(t, w->canvas);
        return false;

    case WE_RESIZE:
        /* Graphs can switch between one and two columns on resize. Start at
         * the heading instead of retaining an offset beyond the new content. */
        t->detail_scroll=0;t->side_scroll=0;
        return true;

    case WE_MOUSE_DOWN:
        for (int i = 0; i < TAB_COUNT; i++) {
            if (!rect_contains(tab_rect(i), ev->x, ev->y)) continue;
            t->tab = (tab_t)i;
            t->scroll = 0;
            t->detail_scroll=0;
            return true;
        }
        if (t->tab == TAB_PERF) {
            rect_t d=detail_rect(w->canvas);
            rect_t side_bar=rect_make(SIDE_W-8*gui_scale(),TAB_H,6*gui_scale(),d.h);
            if(rect_contains(side_bar,ev->x,ev->y)) {
                t->side_scroll=gui_scrollbar_hit(side_bar,t->side_scroll,d.h,side_height(t,w->canvas),ev->y);
                return true;
            }
            rect_t bar=rect_make(d.x+d.w-22,d.y,12,d.h);
            if(rect_contains(bar,ev->x,ev->y)) {
                t->detail_scroll=gui_scrollbar_hit(bar,t->detail_scroll,d.h,t->detail_height,ev->y);
                return true;
            }
            for (int i = 0; i < side_count(t); i++) {
                if(ev->y<TAB_H || !rect_contains(scrolled_side_rect(t,w->canvas,i),ev->x,ev->y))continue;
                t->selected = i>=RES_GPU?RES_GPU:(resource_t)i;
                if(i>=RES_GPU && (unsigned)(i-RES_GPU)<t->gpu_count) {
                    t->selected_gpu=t->gpu_order[i-RES_GPU];t->gpu=t->gpus[t->selected_gpu].info;
                }
                t->detail_scroll=0;
                return true;
            }
        } else {
            /* A click on a column heading sorts by it; clicking the one
             * already sorted by turns it round, which is what every list of
             * this shape has done for thirty years. */
            if (ev->y >= TAB_H + 8 && ev->y < TAB_H + 8 + HEAD_H) {
                int cx = 16;
                for (int i = 0; i < 4; i++) {
                    if (ev->x >= cx && ev->x < cx + columns[i].width) {
                        if (t->sort_column == i) t->sort_desc = !t->sort_desc;
                        else { t->sort_column = i; t->sort_desc = (i >= 2); }
                        sort_procs(t);
                        return true;
                    }
                    cx += columns[i].width;
                }
            }
        }
        return false;

    case WE_MOUSE_WHEEL:
        if(t->tab==TAB_PERF) {
            rect_t d=detail_rect(w->canvas);
            if(ev->x<SIDE_W) {
                int maximum=side_height(t,w->canvas)-d.h;if(maximum<0)maximum=0;
                t->side_scroll-=ev->wheel*SIDE_STRIDE;
                if(t->side_scroll<0)t->side_scroll=0;
                if(t->side_scroll>maximum)t->side_scroll=maximum;
                return true;
            }
            int maximum=t->detail_height-d.h;
            if(maximum<0)maximum=0;
            t->detail_scroll-=ev->wheel*(gui_font_height(FONT_UI)+7*gui_scale())*3;
            if(t->detail_scroll>maximum)t->detail_scroll=maximum;
            if(t->detail_scroll<0)t->detail_scroll=0;
            return true;
        }
        if (t->tab == TAB_PROCS) {
            t->scroll -= ev->wheel * 3;
            if (t->scroll < 0) t->scroll = 0;
            if (t->scroll > t->proc_count - 1) t->scroll = t->proc_count - 1;
            if (t->scroll < 0) t->scroll = 0;
            return true;
        }
        return false;

    case WE_TICK:
        sample(t);
        sample_procs(t);
        sort_procs(t);
        return true;

    case WE_CLOSE:
        free(t);
        return false;

    default:
        return false;
    }
}

void app_taskmgr_launch(wm_t *wm) {
    taskmgr_t *t = calloc(1, sizeof *t);
    if (!t) return;

    t->selected = RES_CPU;
    t->sort_column = 2;              /* busiest first, which is why anyone opens this */
    t->sort_desc = true;

    /* Two readings before the window is first painted, so the first thing
     * anyone sees is a real percentage rather than a zero that corrects itself
     * half a second later. */
    sample(t);
    sample_procs(t);

    window_t *w = desktop_new_window(wm, "Task Manager", ICON_CHIP, 980, 720,
                                     taskmgr_proc, t);
    if (!w) { free(t); return; }
    w->min_w = 560;
    w->min_h = 360;
}
