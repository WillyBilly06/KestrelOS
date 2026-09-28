/* Per-adapter engine and memory history. Requires the Task Manager primitives.
 * Layout coordinates are shared by painting and host geometry tests. */
typedef struct {
    rect_t engine[4], dedicated, shared;
    int metrics_y, columns;
} taskmgr_gpu_layout_t;
static taskmgr_gpu_layout_t taskmgr_gpu_layout(int x,int y,int width,int viewport_h,int sc,int font_h) {
    taskmgr_gpu_layout_t l={0};
    int gap=12*sc,label=font_h+6*sc;
    l.columns=width>=480*sc?2:1;
    int w=(width-gap*(l.columns-1))/l.columns;
    if(w<1)w=1;
    int h=(viewport_h-340*sc)/2;
    if(h<100*sc)h=100*sc;if(h>360*sc)h=360*sc;
    for(int i=0;i<4;i++)l.engine[i]=rect_make(x+(i%l.columns)*(w+gap),
        y+(i/l.columns)*(h+label+gap),w,h+label);
    int below=y+((4+l.columns-1)/l.columns)*(h+label+gap);
    l.dedicated=rect_make(x,below,width,64*sc+label);
    l.shared=rect_make(x,below+l.dedicated.h+gap,width,64*sc+label);
    l.metrics_y=l.shared.y+l.shared.h+20*sc;
    return l;
}
static void gpu_chart(surface_t *s,rect_t cell,const char *title,const char *value,
                      const series_t *history,bool valid) {
    int sc=gui_scale(),line=gui_font_height(FONT_UI);
    int value_w=cell.w/2;
    gui_text_clipped(s,FONT_UI,cell.x,cell.y,cell.w-value_w-8*sc,title,g_theme.text);
    gui_text_right(s,FONT_UI,rect_make(cell.x+cell.w-value_w,cell.y,value_w,line),value,g_theme.text_dim);
    rect_t gr=rect_make(cell.x,cell.y+line+6*sc,cell.w,cell.h-line-6*sc);
    static const series_t empty={0};
    draw_graph(s,gr,history?history:&empty,res_colour(RES_GPU),true);
    if(!valid)gui_text_centred(s,FONT_UI,gr,"Unavailable",g_theme.text_dim);
}
static int gpu_metric(surface_t *s,int x,int y,int width,const char *label,const char *value) {
    int line=gui_font_height(FONT_UI),sc=gui_scale();
    gui_text_clipped(s,FONT_UI,x,y,width,label,g_theme.text_dim);
    gui_text_clipped(s,FONT_UI,x,y+line+4*sc,width,value,g_theme.text);
    return y+2*line+18*sc;
}
static void draw_gpu_detail(taskmgr_t *t,surface_t *s) {
    rect_t d=detail_rect(s),saved=surface_clip(s);
    int sc=gui_scale(),line=gui_font_height(FONT_UI);
    int x=d.x+20*sc,width=d.w-44*sc;
    if(width<1)return;
    surface_set_clip(s,rect_intersection(saved,rect_make(d.x,d.y,d.w-24,d.h)));
    int y=d.y+16*sc-t->detail_scroll;
    if(!t->have_gpu || t->selected_gpu>=t->gpu_count) {
        gui_text_clipped(s,FONT_UI,x,y,width,"No graphics adapter found",g_theme.text_dim);
        surface_set_clip(s,saved);t->detail_height=0;return;
    }
    gpu_history_t *h=&t->gpus[t->selected_gpu];kgpuinfo_t *g=&h->info;
    unsigned order=0;for(unsigned i=0;i<t->gpu_count;i++)if(t->gpu_order[i]==t->selected_gpu)order=i;
    char label[64],value[128],used[48],capacity[48];
    snprintf(label,sizeof label,"GPU %u",order);
    gui_text_clipped(s,FONT_UI,x,y,width,label,g_theme.text_bright);y+=line+6*sc;
    gui_text_clipped(s,FONT_UI,x,y,width,g->name,g_theme.text);y+=line+16*sc;
    taskmgr_gpu_layout_t l=taskmgr_gpu_layout(x,y,width,d.h,sc,line);
    static const char *names[4]={"3D / Compute","Copy","Video Encode","Video Decode"};
    int maximum=-1;
    for(unsigned e=0;e<4;e++) {
        if(h->engine_valid[e]) {
            snprintf(value,sizeof value,"%d%%",g->engine_percent[e]);
            if(g->engine_percent[e]>maximum)maximum=g->engine_percent[e];
        } else snprintf(value,sizeof value,"Unavailable");
        gpu_chart(s,l.engine[e],names[e],value,&h->engine[e],h->engine_valid[e]);
    }
    if(h->memory_valid) {
        bytes_str(used,sizeof used,g->vram_used);bytes_str(capacity,sizeof capacity,g->vram_bytes);
        snprintf(value,sizeof value,"%s / %s",used,capacity);
    } else snprintf(value,sizeof value,"Unavailable");
    gpu_chart(s,l.dedicated,"Tracked dedicated memory",value,&h->memory,h->memory_valid);
    /* The native ABI does not currently expose a shared-residency counter or
     * budget. Never copy Windows' RAM budget or call unmeasured memory zero. */
    gpu_chart(s,l.shared,"Shared GPU memory","Unavailable",NULL,false);
    y=l.metrics_y;
    const char *keys[10]={"Utilization (sampled engines)","Dedicated memory capacity",
        "Shared memory usage / budget","Temperature","Driver version","Driver date",
        "Physical location","Rendering backend","Graphics interfaces","Memory accounting"};
    int columns=width>=660*sc?3:width>=440*sc?2:1;
    int gap=20*sc,colw=(width-(columns-1)*gap)/columns;
    int row_h=2*line+18*sc;
    for(int i=0;i<10;i++) {
        switch(i) {
        case 0:if(maximum>=0)snprintf(value,sizeof value,"%d%%",maximum);else snprintf(value,sizeof value,"Unavailable");break;
        case 1:if(g->vram_exact&&g->vram_bytes)bytes_str(value,sizeof value,g->vram_bytes);else snprintf(value,sizeof value,"Unverified / unavailable");break;
        case 2:snprintf(value,sizeof value,"Unavailable");break;
        case 3:if(g->temperature_c>-1000)snprintf(value,sizeof value,"%d C",g->temperature_c);else snprintf(value,sizeof value,"Unavailable");break;
        case 4:snprintf(value,sizeof value,"%s",g->driver_version[0]?g->driver_version:"Not reported");break;
        case 5:snprintf(value,sizeof value,"%s",g->driver_date[0]?g->driver_date:"Not reported");break;
        case 6:snprintf(value,sizeof value,"PCI %02x:%02x.%u",g->bus,g->slot,g->func);break;
        case 7:snprintf(value,sizeof value,"%s",g->renderer[0]?g->renderer:"Not reported");break;
        case 8:snprintf(value,sizeof value,"Kestrel API subsets");break;
        default:snprintf(value,sizeof value,"Tracked allocations, not total residency");break;
        }
        gpu_metric(s,x+(i%columns)*(colw+gap),y+(i/columns)*row_h,colw,keys[i],value);
    }
    y+=((10+columns-1)/columns)*row_h;
    y=gui_text_wrapped(s,FONT_UI,rect_make(x,y,width,4*line),
        "Unavailable counters are not idle engines. Dedicated history covers tracked allocations only; "
        "firmware allocations and shared RAM residency are not fully accounted for.",g_theme.text_dim);
    t->detail_height=y+t->detail_scroll-d.y+16*sc;
    surface_set_clip(s,saved);
    gui_scrollbar(s,rect_make(d.x+d.w-22,d.y,12,d.h),t->detail_scroll,d.h,t->detail_height);
}
