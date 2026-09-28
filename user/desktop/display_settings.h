/* Native output timing controls. Live Apply retains the logical desktop size:
 * NVIDIA scales its current surface to a validated physical output timing.
 * Full canvas/topology replacement remains a separate transaction. */
#ifndef KESTREL_DESKTOP_DISPLAY_SETTINGS_H
#define KESTREL_DESKTOP_DISPLAY_SETTINGS_H
typedef struct {
    kdisplay_output_t outputs[KDISPLAY_MAX_OUTPUTS];
    kdisplay_mode_t modes[KDISPLAY_MAX_MODES];
    uint32_t mode_indices[KDISPLAY_MAX_MODES]; /* driver indices survive UI sorting */
    int output_count, selected_output, mode_count;
    int resolution[KDISPLAY_MAX_MODES], resolution_count, selected_resolution;
    int rates[KDISPLAY_MAX_MODES], rate_count, selected_rate;
    int resolution_scroll, rate_scroll;
    rect_t output_rect[KDISPLAY_MAX_OUTPUTS], resolution_rect, rate_rect;
    int visible_rows, row_height;
    bool dragging_resolution, dragging_rate;
    rect_t viewport, page_bar, group_rect[3];
    int page_scroll, page_height, requested_group;
    bool dragging_page;
    bool inventory_pending;
    rect_t apply_rect, keep_rect, revert_rect;
    uint64_t mode_token, mode_deadline;
    char mode_status[160];
} display_settings_t;

static void ds_format_hz(uint32_t rate, char *out, size_t n) {
    if (!(rate % 1000)) snprintf(out, n, "%u Hz", rate / 1000);
    else snprintf(out, n, "%u.%03u Hz", rate / 1000, rate % 1000);
}

static void ds_rates(display_settings_t *d) {
    d->rate_count = 0; d->selected_rate = -1; d->rate_scroll = 0;
    if (d->selected_resolution < 0 || d->selected_resolution >= d->resolution_count) return;
    kdisplay_mode_t *r = &d->modes[d->resolution[d->selected_resolution]];
    for (int i = 0; i < d->mode_count; i++) {
        kdisplay_mode_t *m = &d->modes[i];
        if (m->width != r->width || m->height != r->height) continue;
        if (m->flags & KDISPLAY_MODE_CURRENT) d->selected_rate = d->rate_count;
        d->rates[d->rate_count++] = i;
    }
}

static void ds_select_output(display_settings_t *d, int index) {
    if (index < 0 || index >= d->output_count) return;
    d->selected_output = index; d->mode_count = d->resolution_count = 0;
    d->selected_resolution = -1; d->resolution_scroll = 0;
    d->dragging_resolution = d->dragging_rate = false;
    ds_rates(d);
    if(d->outputs[index].flags&KDISPLAY_STALE){d->inventory_pending=true;return;}
    for (uint32_t i = 0; i < KDISPLAY_MAX_MODES; i++) {
        kdisplay_mode_t m;
        if (enum_display_output_mode((uint32_t)index, i, &m) < 0) break;
        if (!m.width || !m.height || !m.refresh_millihz) continue;
        d->mode_indices[d->mode_count]=i;
        d->modes[d->mode_count++] = m;
    }
    kdisplay_output_t after;
    if(enum_display_output((uint32_t)index,&after)<0 ||
       after.output_id!=d->outputs[index].output_id ||
       after.connector_id!=d->outputs[index].connector_id ||
       after.reserved!=d->outputs[index].reserved || (after.flags&KDISPLAY_STALE)){
        d->mode_count=0;d->inventory_pending=true;return;
    }
    /* Descending resolution and exact refresh, with progressive before
     * interlaced when all other fields match. No rate rounding/deduplication. */
    for (int i = 1; i < d->mode_count; i++) {
        kdisplay_mode_t key = d->modes[i]; int j = i - 1;
        uint32_t original_index=d->mode_indices[i];
        while (j >= 0) {
            kdisplay_mode_t *m = &d->modes[j];
            uint64_t a = (uint64_t)key.width * key.height;
            uint64_t b = (uint64_t)m->width * m->height;
            bool before = a > b || (a == b && (key.width > m->width ||
                (key.width == m->width && (key.refresh_millihz > m->refresh_millihz ||
                (key.refresh_millihz == m->refresh_millihz &&
                 !(key.flags & KDISPLAY_MODE_INTERLACED) && (m->flags & KDISPLAY_MODE_INTERLACED))))));
            if (!before) break;
            d->modes[j + 1] = *m;
            d->mode_indices[j+1]=d->mode_indices[j];j--;
        }
        d->modes[j + 1] = key;
        d->mode_indices[j+1]=original_index;
    }
    for (int i = 0; i < d->mode_count; i++) {
        kdisplay_mode_t *m = &d->modes[i];
        if (!i || m->width != d->modes[i-1].width || m->height != d->modes[i-1].height)
            d->resolution[d->resolution_count++] = i;
        if (m->flags & KDISPLAY_MODE_CURRENT) d->selected_resolution = d->resolution_count - 1;
    }
    if (d->selected_resolution < 0 && d->resolution_count) d->selected_resolution = 0;
    ds_rates(d);
}

static void ds_load(display_settings_t *d) {
    d->requested_group = -1;
    uint32_t previous = d->output_count && d->selected_output >= 0 && d->selected_output<d->output_count ?
        d->outputs[d->selected_output].output_id : 0;
    kdisplay_output_t snapshot[KDISPLAY_MAX_OUTPUTS];
    int selected=0,count=0;
    bool pending=false;
    for (uint32_t i = 0; i < KDISPLAY_MAX_OUTPUTS; i++) {
        kdisplay_output_t out;
        if (enum_display_output(i, &out) < 0) break;
        if (out.version != KDISPLAY_INFO_VERSION) break;
        if(i && out.reserved!=snapshot[0].reserved)goto stale;
        snapshot[count++]=out;
        pending|=!!(out.flags&KDISPLAY_STALE);
        if ((previous && out.output_id == previous) || (!previous && (out.flags & KDISPLAY_PRIMARY)))
            selected = (int)i;
    }
    /* Recheck the epoch after enumerating all outputs, not just each selected
     * mode list. A removal can change ordinals midway through the loop. */
    if(count){
        kdisplay_output_t after;
        if(enum_display_output(0,&after)<0 || after.reserved!=snapshot[0].reserved ||
           after.output_id!=snapshot[0].output_id)goto stale;
        pending|=!!(after.flags&KDISPLAY_STALE);
    }
    d->output_count=count;d->inventory_pending=pending;
    memcpy(d->outputs,snapshot,(size_t)count*sizeof snapshot[0]);
    d->mode_count=d->resolution_count=d->rate_count=0;
    d->selected_resolution=d->selected_rate=-1;
    if (d->output_count) ds_select_output(d, selected);
    return;
stale:
    d->inventory_pending=true;
    d->mode_count=d->resolution_count=d->rate_count=0;
    d->selected_resolution=d->selected_rate=-1;
}

static bool ds_refresh(display_settings_t *d){
    kdisplay_output_t first;
    if(enum_display_output(0,&first)<0)return false;
    if(d->output_count && first.reserved==d->outputs[0].reserved &&
       first.flags==d->outputs[0].flags && first.output_id==d->outputs[0].output_id &&
       !d->inventory_pending)return false;
    ds_load(d);return true;
}

static bool ds_can_apply(const display_settings_t *d){
    if(d->mode_token || d->inventory_pending || d->selected_output<0 ||
       d->selected_output>=d->output_count || d->selected_rate<0 || d->selected_rate>=d->rate_count)return false;
    const kdisplay_output_t *o=&d->outputs[d->selected_output];
    return (o->flags&(KDISPLAY_CONNECTED|KDISPLAY_ENABLED))==(KDISPLAY_CONNECTED|KDISPLAY_ENABLED) &&
           !(o->flags&(KDISPLAY_STALE|KDISPLAY_DETECTED_ONLY));
}
static void ds_finish_mode(display_settings_t *d,bool keep){
    int rc=fb_finish_output_mode(d->mode_token,keep),error=errno;
    if(rc==0 || error==ENOENT){
        d->mode_token=0;
        strlcpy(d->mode_status,rc==0?(keep?"Output timing kept.":"Previous output timing restored."):
            "Timing confirmation expired; refresh the current output state.",sizeof d->mode_status);
        ds_load(d);
    }else snprintf(d->mode_status,sizeof d->mode_status,"Timing %s failed (%d); refresh or retry Revert.",keep?"confirmation":"rollback",error);
}
static void ds_tick(display_settings_t *d){
    if(d->mode_token && uptime_ms()>=d->mode_deadline){
        ds_finish_mode(d,false);
        if(d->mode_token)d->mode_deadline=uptime_ms()+1000;
    }
    ds_refresh(d);
}

static bool ds_only_selection_safe(const display_settings_t *d){
    if(d->inventory_pending || !d->output_count)return false;
    for(int i=0;i<d->output_count;i++)
        if(!(d->outputs[i].flags&KDISPLAY_CONNECTED) ||
           (d->outputs[i].flags&(KDISPLAY_STALE|KDISPLAY_DETECTED_ONLY)))return false;
    return true;
}

static void ds_clamp_scroll(int *offset, int count, int visible) {
    int limit = count > visible ? count - visible : 0;
    if (*offset < 0) *offset = 0;
    if (*offset > limit) *offset = limit;
}

static void ds_paint(display_settings_t *d, surface_t *s, rect_t b, int mx, int my) {
    const int k = gui_scale(), fh = gui_font_height(FONT_UI);
    rect_t saved = surface_clip(s);
    surface_set_clip(s, rect_intersection(saved, b));
    d->viewport = b;
    d->page_height = b.h > 640*k ? b.h : 640*k;
    ds_clamp_scroll(&d->page_scroll, d->page_height, b.h);
    d->page_bar = rect_make(b.x+b.w-12*k,b.y,12*k,b.h);
    if (d->page_height > b.h) b.w -= 18*k;
    b.y -= d->page_scroll; b.h = d->page_height;
    int y = gui_page_header(s, b.x, b.y, b.w, "Displays", "Change live output timing using the monitor's validated modes.");
    int x = b.x, bh = fh + 12*k;
    for (int i = 0; i < d->output_count; i++) {
        int bw = 82*k;
        if (x > b.x && x + bw > b.x + b.w) { x = b.x; y += bh + 6*k; }
        char label[24]; snprintf(label, sizeof label, "Screen %d", i+1);
        rect_t r = rect_make(x, y, bw, bh); d->output_rect[i] = r;
        gui_button(s, r, label, rect_contains(r,mx,my), i == d->selected_output, true);
        x += bw + 6*k;
    }
    y += bh + 12*k;
    const kdisplay_output_t *o = &d->outputs[d->selected_output];
    char line[160], hz[32]; ds_format_hz(o->refresh_millihz, hz, sizeof hz);
    snprintf(line, sizeof line, "%s %s%s", o->manufacturer, o->model,
             o->flags & KDISPLAY_PRIMARY ? "  /  Primary" : "");
    gui_text_clipped(s, FONT_UI, b.x, y, b.w, line, g_theme.text_bright); y += fh + 4*k;
    if(d->inventory_pending || (o->flags&KDISPLAY_STALE))
        snprintf(line,sizeof line,"Monitor changed; refreshing capabilities...");
    else if(!(o->flags&KDISPLAY_CONNECTED))snprintf(line,sizeof line,"Disconnected");
    else if(o->flags&KDISPLAY_DETECTED_ONLY)snprintf(line,sizeof line,"Detected; live scanout reconfiguration is not available yet.");
    else snprintf(line, sizeof line, "Active signal: %u x %u at %s", o->width, o->height, hz);
    gui_text_clipped(s, FONT_UI, b.x, y, b.w, line, g_theme.text); y += fh + 4*k;
    snprintf(line, sizeof line, "%s  /  Position %d, %d  /  Rotation %u degrees",
             o->connector, o->x, o->y, o->rotation_degrees);
    gui_text_clipped(s, FONT_UI, b.x, y, b.w, line, g_theme.text_dim); y += fh + 12*k;

    int gap = 12*k, left = (b.w-gap)/2;
    gui_text_clipped(s, FONT_UI, b.x, y, left, "Resolution", g_theme.text);
    gui_text_clipped(s, FONT_UI, b.x+left+gap, y, b.w-left-gap, "Refresh rate", g_theme.text);
    y += fh + 6*k;
    d->row_height = fh + 12*k;
    int available = b.y+b.h - y - 8*fh - 110*k;
    d->visible_rows = available > 0 ? available/d->row_height : 0;
    int height = d->visible_rows*d->row_height;
    d->resolution_rect = rect_make(b.x,y,left,height);
    d->rate_rect = rect_make(b.x+left+gap,y,b.w-left-gap,height);
    ds_clamp_scroll(&d->resolution_scroll,d->resolution_count,d->visible_rows);
    ds_clamp_scroll(&d->rate_scroll,d->rate_count,d->visible_rows);
    for (int side = 0; side < 2; side++) {
        rect_t list = side ? d->rate_rect : d->resolution_rect;
        int count = side ? d->rate_count : d->resolution_count;
        int scroll = side ? d->rate_scroll : d->resolution_scroll;
        int selected = side ? d->selected_rate : d->selected_resolution;
        gui_fill(s,list,g_theme.field);
        for (int row = 0; row < d->visible_rows && row+scroll < count; row++) {
            int index = row+scroll;
            kdisplay_mode_t *m = &d->modes[side ? d->rates[index] : d->resolution[index]];
            rect_t r = rect_make(list.x,list.y+row*d->row_height,list.w-14*k,d->row_height);
            if (side) {
                ds_format_hz(m->refresh_millihz,hz,sizeof hz);
                snprintf(line,sizeof line,"%s%s%s",hz,
                         m->flags & KDISPLAY_MODE_INTERLACED ? " interlaced" : "",
                         m->flags & KDISPLAY_MODE_CURRENT ? " *" : "");
            } else snprintf(line,sizeof line,"%u x %u",m->width,m->height);
            if (index == selected) gui_fill(s,r,g_theme.selection);
            gui_text_clipped(s,FONT_UI,r.x+6*k,r.y+6*k,r.w-12*k,line,
                             index == selected ? g_theme.selection_text : g_theme.text);
        }
        gui_scrollbar(s,rect_make(list.x+list.w-12*k,list.y,12*k,list.h),scroll,d->visible_rows,count);
    }
    y += height + 8*k;
    gui_text_clipped(s,FONT_UI,b.x,y,b.w,"* Active timing. Choose resolution and rate, then Apply.",g_theme.text_dim);
    y += fh+4*k;
    gui_text_clipped(s,FONT_UI,b.x,y,b.w,"Desktop canvas stays the same size; GPU scales the output.",g_theme.text_dim);
    if (o->flags & KDISPLAY_MODES_TRUNCATED)
        gui_text_clipped(s,FONT_UI,b.x,y+fh+4*k,b.w,"The driver mode list exceeded the snapshot capacity.",g_theme.text_dim);
    y += 2*fh + 8*k;
    int action_w=(b.w-12*k)/3;
    d->apply_rect=rect_make(b.x,y,action_w,bh);
    d->keep_rect=rect_make(b.x+action_w+6*k,y,action_w,bh);
    d->revert_rect=rect_make(b.x+2*(action_w+6*k),y,action_w,bh);
    bool can_apply=ds_can_apply(d),pending=d->mode_token!=0;
    gui_button(s,d->apply_rect,"Apply",can_apply&&rect_contains(d->apply_rect,mx,my),false,can_apply);
    gui_button(s,d->keep_rect,"Keep",pending&&rect_contains(d->keep_rect,mx,my),false,pending);
    gui_button(s,d->revert_rect,"Revert",pending&&rect_contains(d->revert_rect,mx,my),false,pending);
    y+=bh+4*k;
    if(pending){
        uint64_t now=uptime_ms();
        unsigned seconds=d->mode_deadline>now?(unsigned)((d->mode_deadline-now+999)/1000):0;
        snprintf(line,sizeof line,"Keep this timing? Automatic rollback in %u seconds.",seconds);
    }else strlcpy(line,d->mode_status,sizeof line);
    gui_text_clipped(s,FONT_UI,b.x,y,b.w,line,g_theme.text);y+=fh+12*k;
    gui_text_clipped(s,FONT_UI,b.x,y,b.w,"Startup arrangement (takes effect on next boot)",g_theme.text);
    y += fh + 6*k;
    const char *groups[3] = {"Extend","Duplicate","Only selected"};
    int bw = (b.w-12*k)/3;
    for (int i = 0; i < 3; i++) {
        rect_t r = rect_make(b.x+i*(bw+6*k),y,bw,bh);
        d->group_rect[i] = r;
        bool enabled=i!=2 || ds_only_selection_safe(d);
        gui_button(s,r,groups[i],enabled&&rect_contains(r,mx,my),false,enabled);
    }
    surface_set_clip(s,saved);
    if (d->page_height > d->viewport.h)
        gui_scrollbar(s,d->page_bar,d->page_scroll,d->viewport.h,d->page_height);
}

/* Boot layout numbers are primary-first; native handles stay in connector
 * order. Translate instead of turning off the wrong physical monitor. */
static int ds_boot_slot(const display_settings_t *d, int selected) {
    int primary = 0;
    for (int i = 0; i < d->output_count; i++)
        if (d->outputs[i].flags & KDISPLAY_PRIMARY) { primary = i; break; }
    if (selected == primary) return 1;
    return selected < primary ? selected+2 : selected+1;
}

static bool ds_event(display_settings_t *d, const wevent_t *ev) {
    if (ev->kind == WE_MOUSE_UP) {
        d->dragging_resolution = d->dragging_rate = d->dragging_page = false; return false;
    }
    if (ev->kind == WE_MOUSE_MOVE && d->dragging_page) {
        d->page_scroll = gui_scrollbar_hit(d->page_bar,d->page_scroll,d->viewport.h,d->page_height,ev->y);
        return true;
    }
    if (!rect_contains(d->viewport,ev->x,ev->y) && !d->dragging_resolution && !d->dragging_rate)
        return false;
    if (ev->kind == WE_MOUSE_DOWN) {
        if(d->mode_token && rect_contains(d->keep_rect,ev->x,ev->y)){ds_finish_mode(d,true);return true;}
        if(d->mode_token && rect_contains(d->revert_rect,ev->x,ev->y)){ds_finish_mode(d,false);return true;}
        if(ds_can_apply(d) && rect_contains(d->apply_rect,ev->x,ev->y)){
            const kdisplay_output_t *o=&d->outputs[d->selected_output];
            kdisplay_output_mode_request_t q={o->reserved,o->output_id,o->connector_id,
                d->mode_indices[d->rates[d->selected_rate]]};
            int64_t token=fb_apply_output_mode(&q);
            if(token>0){d->mode_token=(uint64_t)token;d->mode_deadline=uptime_ms()+20000;ds_load(d);}
            else snprintf(d->mode_status,sizeof d->mode_status,"Live output timing rejected (%d); check current signal.",errno);
            return true;
        }
        if (d->page_height > d->viewport.h && rect_contains(d->page_bar,ev->x,ev->y)) {
            d->dragging_page = true;
            d->page_scroll = gui_scrollbar_hit(d->page_bar,d->page_scroll,d->viewport.h,d->page_height,ev->y);
            return true;
        }
        for (int i = 0; i < 3; i++)
            if (rect_contains(d->group_rect[i],ev->x,ev->y) && (i!=2 || ds_only_selection_safe(d))) { d->requested_group = i; return true; }
        for (int i = 0; i < d->output_count; i++)
            if (rect_contains(d->output_rect[i],ev->x,ev->y)) { ds_select_output(d,i); return true; }
    }
    for (int side = 0; side < 2; side++) {
        rect_t r = side ? d->rate_rect : d->resolution_rect;
        int count = side ? d->rate_count : d->resolution_count;
        int *scroll = side ? &d->rate_scroll : &d->resolution_scroll;
        bool *drag = side ? &d->dragging_rate : &d->dragging_resolution;
        if (!d->visible_rows || rect_empty(r)) continue;
        if (ev->kind == WE_MOUSE_WHEEL && rect_contains(r,ev->x,ev->y)) {
            int before = *scroll;
            *scroll -= ev->wheel * 3; ds_clamp_scroll(scroll,count,d->visible_rows);
            if (*scroll != before) return true;
            /* At a list boundary, wheel input continues scrolling the page. */
        }
        rect_t bar = rect_make(r.x+r.w-12*gui_scale(),r.y,12*gui_scale(),r.h);
        if ((ev->kind == WE_MOUSE_DOWN && rect_contains(bar,ev->x,ev->y)) ||
            (ev->kind == WE_MOUSE_MOVE && *drag)) {
            *drag = true;
            *scroll = gui_scrollbar_hit(bar,*scroll,d->visible_rows,count,ev->y); return true;
        }
        if (ev->kind == WE_MOUSE_DOWN && rect_contains(r,ev->x,ev->y)) {
            int row = *scroll + (ev->y-r.y)/d->row_height;
            if (row >= count) return false;
            if (side) d->selected_rate = row;
            else { d->selected_resolution = row; ds_rates(d); }
            return true;
        }
    }
    if (ev->kind == WE_MOUSE_WHEEL) {
        d->page_scroll -= ev->wheel * 36 * gui_scale();
        ds_clamp_scroll(&d->page_scroll,d->page_height,d->viewport.h);
        return true;
    }
    return false;
}
#endif
