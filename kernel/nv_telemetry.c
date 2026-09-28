/* nv_telemetry.c - what the card is doing right now.
 *
 * A graphics card has several engines that work independently: one that draws,
 * one or more that only move memory, and separate ones that encode and decode
 * video.  A system monitor shows a row per engine because they genuinely run
 * at once - a video call can have the decoder busy while nothing is drawn at
 * all - and a single "GPU usage" figure hides the thing somebody opened the
 * window to see.
 *
 * Modern host-RM cards use NVIDIA's firmware perfmon sample ring. The legacy
 * register sampler below is retained for older/model-backed paths only.
 * Firmware FB bandwidth must not be presented as Copy-engine activity.
 *
 * The drawing engine can be watched without any firmware: it has a status
 * register with a busy bit, at the same place it has been since Fermi.  This
 * file samples that often and reports the share of recent samples that found
 * it busy.  That is sampled, not measured - over a second of steady work it
 * converges on the truth, and it will under-report a burst shorter than the
 * gap between samples - but it is a real reading of a real signal.
 *
 * On the legacy path, copy/encode/decode usage is unavailable. The host-RM
 * path instead binds its successfully opened engines and reads GR/NVENC/NVDEC
 * firmware counters. A firmware error, a frozen timestamp or missing samples
 * remains unavailable, not an invented zero. A
 * monitor reading 0% because nothing was measured looks exactly like an idle
 * engine, and there is no way for somebody looking at it to tell the
 * difference - which is what makes the guess worse than the gap.
 */
#include "kernel.h"
#include "klog.h"
#include "nv.h"
#include "time.h"
#include "proc.h"
#include "cpu.h"
#include "nvrm_telemetry.h"

/* How many samples make up the window a percentage is computed over.  At the
 * sampling interval this is about a second: short enough to feel live, long
 * enough not to flicker between nothing and everything. */
#define WINDOW 64

typedef struct {
    u8  busy[WINDOW];
    u8  valid[WINDOW];
    u32 next;
    u32 taken;
    u64 sampled_ms;
} history_t;

static history_t history[NV_MAX_CARDS];

/* Firmware owns modern engine counters. Setup/RM queries use the render guard;
 * steady-state reads snapshot the boot-lifetime RUSD mapping without that guard.
 * Rendering must not starve monitoring of the very work being measured.
 * Copy remains unknown: FB bandwidth is NOT Copy-engine utilization. */
typedef struct {
    nv_card_t *card;
    u32 client, device, subdevice, last_status, source;
    u64 timestamp, changed_ms, temperature_timestamp, temperature_changed_ms;
    u64 shared_timestamp, shared_changed_ms;
    int temperature_c;
    bool temperature_fresh;
    int percent[NV_ENGINE_COUNT];
    bool bound, started, have_fresh, reported;
} host_history_t;
static host_history_t host_history[NV_MAX_CARDS];

static void telemetry_worker(void *arg) {
    host_history_t *h = arg;
    for (;;) {
        sched_sleep_ms(250);
        if (!h->bound) continue;
        nvrm_shared_telemetry_t shared;
        u32 shared_status = nvrm_host_peek_shared_telemetry(h->client, h->device, h->subdevice, &shared);
        if (shared_status) {
            if (!nv_render_try_begin()) continue;
            shared_status = nvrm_host_read_shared_telemetry(h->client, h->device, h->subdevice, &shared);
            nv_render_end();
        }
        if (shared.vram_bytes) {
            h->card->vram_bytes = shared.vram_bytes;
            h->card->vram_exact = true;
        }
        if (shared.temperature_timestamp > h->temperature_timestamp) {
            bool advancing = h->temperature_timestamp != 0;
            h->temperature_timestamp = shared.temperature_timestamp;
            h->temperature_changed_ms = g_uptime_ms;
            h->temperature_c = shared.temperature_c;
            h->temperature_fresh = advancing;
        }
        u32 util[3]; u64 timestamp = shared.timestamp;
        u32 source = 1, status = shared_status;
        memcpy(util, shared.utilization, sizeof util);
        /* Track RUSD progress independently of the selected fallback. Otherwise
         * a frozen RUSD sample switches back from GPUMON every other iteration
         * and repeatedly discards both sources' freshness baselines. */
        if (timestamp > h->shared_timestamp) {
            h->shared_timestamp = timestamp;h->shared_changed_ms = g_uptime_ms;
        }
        bool stalled = h->shared_timestamp &&
            (timestamp < h->shared_timestamp || g_uptime_ms - h->shared_changed_ms > 1000u);
        if (status || !timestamp || stalled) {
            /* GPUMON is still an RM command and must remain serialized. If it
             * cannot run now, retain the old timestamp (normal expiry applies),
             * not a fabricated fresh sample. Mapped RUSD reads above never wait. */
            if (!nv_render_try_begin()) continue;
            source = 2;
            status = nvrm_host_read_utilization(h->client, h->subdevice, util, &timestamp);
            nv_render_end();
        }
        if (!h->reported || status != h->last_status || source != h->source) {
            kinfo("nv-telemetry", "utilization source=%s status=%#x (GR/encode/decode; Copy unmeasured)",
                  source == 1 ? "RUSD" : "GPUMON", status);
            h->reported = true; h->last_status = status;
        }
        if (source != h->source) {
            h->source = source;h->timestamp = 0;h->have_fresh = false;
        }
        if (status) {
            bool irq = irq_save(); h->have_fresh = false; irq_restore(irq);
            sched_sleep_ms(1750); continue;
        }
        /* Do not call a frozen/historical sample live. Establish a timestamp
         * baseline, then require forward progress before publishing it. */
        if (!timestamp || timestamp <= h->timestamp) continue;
        bool irq = irq_save();
        bool advancing = h->timestamp != 0;
        h->timestamp = timestamp;
        h->changed_ms = g_uptime_ms;
        static const unsigned engine[3] = { NV_ENGINE_3D, NV_ENGINE_ENCODE, NV_ENGINE_DECODE };
        for (unsigned i = 0; i < 3; i++)
            h->percent[engine[i]] = util[i] <= 10000u ? (int)((util[i] + 50u) / 100u) : NV_ENGINE_UNMEASURED;
        h->have_fresh = advancing;
        irq_restore(irq);
    }
}

void nv_telemetry_bind_host(nv_card_t *c, u32 client, u32 device, u32 subdevice, u32 engines) {
    if (!c || c->modelled || c->index < 0 || c->index >= NV_MAX_CARDS) return;
    host_history_t *h = &host_history[c->index];
    if (h->started) return; /* boot-lifetime client; no replacement while queried */
    memset(h, 0, sizeof(*h));
    h->card = c; h->client = client; h->device = device; h->subdevice = subdevice; h->bound = true;
    c->engines_present = engines;
    for (unsigned i = 0; i < NV_ENGINE_COUNT; i++) h->percent[i] = NV_ENGINE_UNMEASURED;
}

static const char *engine_names[NV_ENGINE_COUNT] = {
    [NV_ENGINE_3D]     = "3D",
    [NV_ENGINE_COPY]   = "Copy",
    [NV_ENGINE_ENCODE] = "Video encode",
    [NV_ENGINE_DECODE] = "Video decode",
};

const char *nv_engine_name(int engine) {
    if (engine < 0 || engine >= NV_ENGINE_COUNT) return "";
    return engine_names[engine];
}

/* Take one sample of the engines this driver can watch. */
void nv_telemetry_sample(nv_card_t *c) {
    if (!c || c->index < 0 || c->index >= NV_MAX_CARDS) return;
    history_t *h = &history[c->index];

    u8 bits = 0;
    u8 valid = 0;

    if (c->engines_present & (1u << NV_ENGINE_3D)) {
        u32 status = nv_rd32(c, NV_PGRAPH_STATUS);
        /* All ones means the card has stopped answering - a sample that says
         * "busy" then would be an artefact of the card being gone. */
        if (status != 0xFFFFFFFFu && (status & 0xFFFF0000u) != 0xBADF0000u) {
            valid |= (u8)(1u << NV_ENGINE_3D);
            if(status & NV_PGRAPH_STATUS_BUSY)bits |= (u8)(1u << NV_ENGINE_3D);
        }
    }

    h->busy[h->next] = bits;
    h->valid[h->next] = valid;
    h->sampled_ms = g_uptime_ms;
    h->next = (h->next + 1) % WINDOW;
    if (h->taken < WINDOW) h->taken++;
}

/* Which engines this card has at all, from the register that says which are
 * switched on.  Called once when the card comes up. */
void nv_telemetry_probe_engines(nv_card_t *c) {
    if (!c) return;
    if(c->index>=0 && c->index<NV_MAX_CARDS)memset(&history[c->index],0,sizeof history[c->index]);

    u32 enable = nv_rd32(c, NV_PMC_ENABLE);
    if (enable == 0xFFFFFFFFu) { c->engines_present = 0; return; }

    c->engines_present = 0;
    if (enable & NV_PMC_ENABLE_PGRAPH) c->engines_present |= (1u << NV_ENGINE_3D);

    /* The copy and video engines are not switched on and off through this
     * register on the parts this drives, and their presence follows from the
     * generation instead: every card new enough to be here has copy engines
     * and a video decoder, and all but the smallest have an encoder. */
    /* Fermi is where the copy and video engines became separate hardware a
     * driver can name; 0x0c0 is that generation's architecture number, and the
     * same comparison the rest of this driver uses to date a card. */
    if ((c->chipset & 0x1F0) >= 0x0c0) {
        c->engines_present |= (1u << NV_ENGINE_COPY);
        c->engines_present |= (1u << NV_ENGINE_DECODE);
        c->engines_present |= (1u << NV_ENGINE_ENCODE);
    }
}

void nv_telemetry_read(nv_card_t *c, nv_telemetry_t *out) {
    memset(out, 0, sizeof *out);
    if (!c) return;

    for (int e = 0; e < NV_ENGINE_COUNT; e++) {
        if (!(c->engines_present & (1u << e))) {
            out->engine_percent[e] = NV_ENGINE_ABSENT;
            continue;
        }
        /* Filled below only when the selected backend has a fresh reading. */
        out->engine_percent[e] = NV_ENGINE_UNMEASURED;
    }

    if (c->index >= 0 && c->index < NV_MAX_CARDS) {
        host_history_t *host = &host_history[c->index];
        if (host->bound) {
            /* First consumer starts collection after boot's render proofs.
             * The creator does not yield, so BSP process readers cannot race. */
            if (!host->started)
                host->started = kthread_create("gpu-telemetry", telemetry_worker, host) >= 0;
            if (host->have_fresh && g_uptime_ms >= host->changed_ms &&
                g_uptime_ms - host->changed_ms <= 1000u) {
                for (unsigned e = 0; e < NV_ENGINE_COUNT; e++)
                    if (c->engines_present & (1u << e)) {
                        out->engine_percent[e] = host->percent[e];
                        if (host->percent[e] >= 0) out->sampled = true;
                    }
                out->samples = out->sampled ? 1u : 0u;
            }
        } else {
        history_t *h = &history[c->index];
        u32 newest=(h->next+WINDOW-1)%WINDOW;
        if (h->taken && g_uptime_ms>=h->sampled_ms && g_uptime_ms-h->sampled_ms<=1000 &&
            (h->valid[newest] & (1u << NV_ENGINE_3D)) && (c->engines_present & (1u << NV_ENGINE_3D))) {
            u32 busy = 0, valid = 0;
            for (u32 i = 0; i < h->taken; i++)
                if(h->valid[i] & (1u << NV_ENGINE_3D)) {
                    valid++;
                    if (h->busy[i] & (1u << NV_ENGINE_3D)) busy++;
                }
            out->engine_percent[NV_ENGINE_3D] =
                (int)((busy * 100 + valid / 2) / valid);
            out->samples = valid;
            out->sampled = true;
        }
        }
    }

    out->temperature_c = c->temperature_c;
    if (c->index >= 0 && c->index < NV_MAX_CARDS && host_history[c->index].bound) {
        host_history_t *h = &host_history[c->index];
        out->temperature_c = h->temperature_fresh && g_uptime_ms >= h->temperature_changed_ms &&
            g_uptime_ms - h->temperature_changed_ms <= 1000u ? h->temperature_c : -1000;
    }
    out->fan_percent   = c->fan_percent;
    out->vram_bytes    = c->vram_bytes;

    /* What this driver has handed out itself.  It cannot see what the firmware
     * reserved before it arrived, so this is a floor and is labelled as one
     * wherever it is shown. */
    out->vram_used     = __atomic_load_n(&c->vram_allocated, __ATOMIC_ACQUIRE);
}

/* --------------------------------------------------------------- self-test */

/* Drive the model's status register directly.  The model backs the card's
 * registers with ordinary memory, so setting one is a write - which keeps the
 * test honest: the sampler reads the register the same way it would on
 * hardware, and nothing about the path under test is stubbed out. */
static void model_status(nv_card_t *c, u32 value) {
    if (c->modelled && c->regs && NV_PGRAPH_STATUS + 4 <= c->regs_size)
        *(volatile u32 *)(c->regs + NV_PGRAPH_STATUS) = value;
}

int nv_telemetry_selftest(void) {
    int failures = 0;

    nv_card_t *c = nv_model_card();
    if (!c) {
        kerr("nv-telemetry", "no model card to sample");
        return 1;
    }

    memset(&history[c->index], 0, sizeof history[c->index]);
    c->engines_present = (1u << NV_ENGINE_3D) | (1u << NV_ENGINE_COPY);

    /* Busy on three samples in every four. */
    for (int i = 0; i < WINDOW; i++) {
        model_status(c, ((i % 4) != 0) ? NV_PGRAPH_STATUS_BUSY : 0);
        nv_telemetry_sample(c);
    }

    nv_telemetry_t t;
    nv_telemetry_read(c, &t);

    if (!t.sampled || t.samples != WINDOW) {
        kerr("nv-telemetry", "the window did not fill: %u sample(s)", t.samples);
        failures++;
    }
    if (t.engine_percent[NV_ENGINE_3D] != 75) {
        kerr("nv-telemetry", "the drawing engine came out at %d%%, expected 75%%",
             t.engine_percent[NV_ENGINE_3D]);
        failures++;
    }
    /* An engine that is fitted but cannot be watched must say so, and must not
     * be reported as idle: those look identical on a monitor and mean opposite
     * things. */
    if (t.engine_percent[NV_ENGINE_COPY] != NV_ENGINE_UNMEASURED) {
        kerr("nv-telemetry", "a fitted engine that cannot be measured reported "
                             "%d rather than saying so",
             t.engine_percent[NV_ENGINE_COPY]);
        failures++;
    }
    if (t.engine_percent[NV_ENGINE_DECODE] != NV_ENGINE_ABSENT) {
        kerr("nv-telemetry", "an engine this card does not have reported %d "
                             "rather than saying it is not there",
             t.engine_percent[NV_ENGINE_DECODE]);
        failures++;
    }

    /* A card that has stopped answering reads as all ones on every register,
     * and must not be counted as permanently busy. */
    memset(&history[c->index], 0, sizeof history[c->index]);
    model_status(c, 0xFFFFFFFFu);
    for (int i = 0; i < WINDOW; i++) nv_telemetry_sample(c);
    model_status(c, 0);

    nv_telemetry_read(c, &t);
    if (t.engine_percent[NV_ENGINE_3D] != 0) {
        kerr("nv-telemetry", "a card that stopped answering was read as %d%% "
                             "busy", t.engine_percent[NV_ENGINE_3D]);
        failures++;
    }

    if (!failures)
        kinfo("nv-telemetry", "the drawing engine's utilisation is sampled "
                              "correctly, an engine that cannot be measured "
                              "says so rather than reading zero, and a card "
                              "that stops answering is not counted as busy");
    return failures;
}
