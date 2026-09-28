/* cputopo.c - what the processor actually is: how many of it there are, how
 * they are arranged, how fast they run, and what they can each remember.
 *
 * The system used to answer "how many processors" with the number of entries
 * in the firmware's processor table and leave it there.  That number is the
 * count of logical processors, and reporting it as the core count is wrong on
 * every machine where the two differ - which is most of them.  It is wrong in
 * both directions, too: a processor with two threads per core reports twice
 * the cores it has, and a hybrid processor, whose performance cores and
 * efficiency cores are not the same kind of thing at all, is flattened into a
 * single number that describes neither.
 *
 * Getting it right does not require running code on every processor.  An APIC
 * identifier is not an opaque number: it is several fields packed together -
 * which package this processor is in, which core within it, which thread
 * within that - and CPUID says how wide each field is.  The firmware's table
 * lists an identifier for every logical processor in the machine, so taking
 * the widths from CPUID and the identifiers from the table and counting the
 * distinct values at each level gives the true arrangement of a machine whose
 * other processors have never been started.
 *
 * Where a number cannot be established honestly it is left at zero and the
 * caller shows nothing rather than a guess.
 */
#include "kernel.h"
#include "cpu.h"
#include "acpi.h"
#include "klog.h"
#include "smp.h"

cpu_topology_t g_topo;

/* ------------------------------------------------------------------ levels */

/* The width of each field inside an APIC identifier.
 *
 * Leaf 0x1F is the current enumeration and describes machines with dies and
 * modules that leaf 0x0B cannot; leaf 0x0B describes threads and cores and is
 * present on everything since Nehalem.  Both are laid out the same way, so the
 * same walk reads either: each subleaf names a level and says how far to shift
 * an identifier right to get past it.
 */
static bool read_level_widths(u32 leaf, u32 *smt_shift, u32 *core_shift,
                              u32 *threads_per_core) {
    u32 a, b, c, d;
    bool found = false;

    *smt_shift = 0;
    *core_shift = 0;
    *threads_per_core = 0;

    for (u32 sub = 0; sub < 16; sub++) {
        cpuid_raw(leaf, sub, &a, &b, &c, &d);

        u32 type = (c >> 8) & 0xFF;      /* 0 ends the list                  */
        if (type == 0) break;

        u32 shift = a & 0x1F;            /* bits to shift past this level    */
        u32 count = b & 0xFFFF;          /* logical processors at this level */

        if (type == 1) {                 /* SMT: threads within one core     */
            *smt_shift = shift;
            *threads_per_core = count ? count : 1;
            found = true;
        } else if (type == 2) {          /* Core: cores within one package   */
            *core_shift = shift;
            found = true;
        } else if (shift > *core_shift) {
            /* Modules, tiles and dies all sit between the core and the
             * package, so anything above the core level still has to be
             * shifted past before what remains identifies a package. */
            *core_shift = shift;
        }
    }

    if (!*threads_per_core) *threads_per_core = 1;
    return found;
}

/* ------------------------------------------------------------------ caches */

static const char *cache_kind(u32 type) {
    switch (type) {
    case 1:  return "data";
    case 2:  return "instruction";
    case 3:  return "unified";
    default: return "";
    }
}

/* Walk the cache descriptors and record one entry per cache the processor
 * actually has.  The size is not stated directly; it is the product of the
 * geometry, which is how the manuals describe it and how every other operating
 * system computes it. */
static void read_caches(u32 leaf) {
    u32 a, b, c, d;

    for (u32 sub = 0; sub < 8 && g_topo.cache_count < CPU_MAX_CACHES; sub++) {
        cpuid_raw(leaf, sub, &a, &b, &c, &d);

        u32 type = a & 0x1F;
        if (type == 0) break;            /* no more caches                   */
        if (type > 3) continue;

        u32 level      = (a >> 5) & 0x7;
        u32 sharing    = ((a >> 14) & 0xFFF) + 1;
        u32 line       = (b & 0xFFF) + 1;
        u32 partitions = ((b >> 12) & 0x3FF) + 1;
        u32 ways       = ((b >> 22) & 0x3FF) + 1;
        u32 sets       = c + 1;

        cpu_cache_t *e = &g_topo.cache[g_topo.cache_count++];
        e->level   = level;
        e->type    = type;
        e->bytes   = (u64)ways * partitions * line * sets;
        e->line    = line;
        e->ways    = ways;
        e->shared  = sharing;
        strlcpy(e->kind, cache_kind(type), sizeof e->kind);
    }
}

/* ------------------------------------------------------------------- hybrid */

/* Whether this processor mixes two kinds of core, and which kind the one
 * running this code is.
 *
 * Only the processor executing CPUID can be asked what it is, so on a machine
 * whose other processors have never been started this establishes that the
 * design is hybrid and names the kind of the boot processor.  It cannot count
 * how many of each there are - that needs code running on each of them - so it
 * does not pretend to. */
static void read_hybrid(u32 max_leaf) {
    if (max_leaf < 0x1A) return;

    u32 a, b, c, d;
    cpuid_raw(7, 0, &a, &b, &c, &d);
    if (!((d >> 15) & 1)) return;         /* not a hybrid part               */

    g_topo.hybrid = true;

    cpuid_raw(0x1A, 0, &a, &b, &c, &d);
    switch ((a >> 24) & 0xFF) {
    case 0x20: strlcpy(g_topo.boot_core_kind, "efficiency",
                       sizeof g_topo.boot_core_kind); break;
    case 0x40: strlcpy(g_topo.boot_core_kind, "performance",
                       sizeof g_topo.boot_core_kind); break;
    default: break;
    }
}

/* ---------------------------------------------------------------- the whole */

void cpu_topology_init(void) {
    u32 a, b, c, d;

    memset(&g_topo, 0, sizeof g_topo);

    cpuid_raw(0, 0, &a, &b, &c, &d);
    u32 max_leaf = a;

    /* Can this processor host virtual machines?  Intel and AMD advertise the
     * same capability in different places. */
    if (max_leaf >= 1) {
        cpuid_raw(1, 0, &a, &b, &c, &d);
        if ((c >> 5) & 1) {
            g_topo.virtualization = true;
            strlcpy(g_topo.virt_name, "VT-x", sizeof g_topo.virt_name);
        }
    }
    cpuid_raw(0x80000000, 0, &a, &b, &c, &d);
    u32 max_ext = a;
    if (max_ext >= 0x80000001) {
        cpuid_raw(0x80000001, 0, &a, &b, &c, &d);
        if ((c >> 2) & 1) {
            g_topo.virtualization = true;
            strlcpy(g_topo.virt_name, "AMD-V", sizeof g_topo.virt_name);
        }
    }

    /* The clocks the processor says it runs at.  These are the design figures,
     * not what it happens to be doing now - a modern part is rarely at its
     * base clock and the number is still the one worth showing beside it. */
    if (max_leaf >= 0x16) {
        cpuid_raw(0x16, 0, &a, &b, &c, &d);
        g_topo.base_mhz = a & 0xFFFF;
        g_topo.max_mhz  = b & 0xFFFF;
        g_topo.bus_mhz  = c & 0xFFFF;
    }

    /* The leaf exists on this processor and answers with zeroes.
     *
     * That was found by running this same derivation on a real Core Ultra:
     * leaf 0x16 is present, returns nothing, and the clock line therefore
     * never appeared - which reads as the system not knowing rather than the
     * processor not saying.  Where it answers with nothing the firmware's own
     * table is asked instead, and that is filled in later by smbios_init(). */
    if (!g_topo.base_mhz && !g_topo.max_mhz)
        g_topo.clocks_from_cpuid = false;
    else
        g_topo.clocks_from_cpuid = true;

    read_hybrid(max_leaf);

    if (max_leaf >= 4)              read_caches(4);
    else if (max_ext >= 0x8000001D) read_caches(0x8000001D);

    /* The arrangement.  Prefer the newer enumeration, fall back to the older
     * one, and if neither is there say only what the firmware's table says. */
    u32 smt_shift = 0, core_shift = 0, threads_per_core = 1;
    bool have = false;
    if (max_leaf >= 0x1F) have = read_level_widths(0x1F, &smt_shift, &core_shift, &threads_per_core);
    if (!have && max_leaf >= 0x0B) have = read_level_widths(0x0B, &smt_shift, &core_shift, &threads_per_core);

    g_topo.threads_per_core = threads_per_core;

    /* Count what the firmware listed.  Only processors it marked usable are
     * counted: a table can describe sockets that are empty and cores that are
     * fused off, and counting those would overstate the machine. */
    u32 threads = 0;
    for (int i = 0; i < g_acpi_cpu_count; i++)
        if (g_acpi_cpus[i].flags & 1) threads++;

    g_topo.threads = threads ? threads : 1;

    if (have && core_shift) {
        /* Decompose every identifier and count the distinct values at each
         * level.  A few dozen processors against a few dozen entries is small
         * enough that counting distinct values by scanning is simpler than
         * sorting, and clearer to read. */
        u32 cores = 0, packages = 0;
        u32 seen_core[ACPI_MAX_CPUS], seen_pkg[ACPI_MAX_CPUS];

        for (int i = 0; i < g_acpi_cpu_count; i++) {
            if (!(g_acpi_cpus[i].flags & 1)) continue;
            u32 id = g_acpi_cpus[i].apic_id;

            u32 core_id = id >> smt_shift;
            u32 pkg_id  = id >> core_shift;

            bool known = false;
            for (u32 j = 0; j < cores; j++) if (seen_core[j] == core_id) { known = true; break; }
            if (!known && cores < ACPI_MAX_CPUS) seen_core[cores++] = core_id;

            known = false;
            for (u32 j = 0; j < packages; j++) if (seen_pkg[j] == pkg_id) { known = true; break; }
            if (!known && packages < ACPI_MAX_CPUS) seen_pkg[packages++] = pkg_id;
        }

        g_topo.cores    = cores;
        g_topo.sockets  = packages;

        /* On a hybrid part this count can be low, and it is worth saying so
         * rather than presenting it as certain.
         *
         * The widths above come from CPUID on the BOOT processor, because this
         * runs before the other processors are started.  On a part whose
         * performance cores carry two threads and whose efficiency cores carry
         * one, the boot processor is a performance core and reports an SMT
         * width of one bit.  Shifting every identifier by that bit merges each
         * PAIR of efficiency cores into a single core - so a processor with
         * eight of one and twelve of the other is counted as fourteen cores
         * instead of twenty.
         *
         * Nothing detects that from here.  The identifiers are consistent, the
         * arithmetic is right, and the answer is wrong.
         *
         * A part with no threading anywhere - which is what the current
         * generation of these processors is - has a width of zero and is
         * counted correctly, which is why this is a caveat and not a refusal.
         *
         * The real fix is to ask each processor for its own width once they
         * are running, which is after this point in the boot. */
        /* The caveat only applies where threads actually exist.
         *
         * It used to fire on `smt_shift > 0`, and on the current generation of
         * these parts that is a false alarm: an Arrow Lake Core Ultra has no
         * threading at all, yet reports an SMT shift of one because its APIC
         * identifiers are spaced two apart.  Shifting by that bit still leaves
         * twenty distinct values for twenty cores, so the count is exact - and
         * the system was labelling it "at least 20" and warning about it.
         *
         * What actually causes the merging described above is a core that
         * carries more than one thread, which is what threads_per_core says.
         * Checked against the real processor with tools/hwprobe.c: leaf 0x1F
         * gives SMT shift 1, one thread per core, and 20 cores - which is what
         * Windows reports for the same machine. */
        g_topo.cores_uncertain = g_topo.hybrid && threads_per_core > 1;
    } else {
        /* Without the widths the identifiers cannot be taken apart, and the
         * only honest reading is that every logical processor might be a core
         * on its own. */
        g_topo.cores   = g_topo.threads;
        g_topo.sockets = 1;
    }

    if (!g_topo.cores)   g_topo.cores = 1;
    if (!g_topo.sockets) g_topo.sockets = 1;

    kinfo("cpu", "%u socket(s), %s%u core(s), %u logical processor(s)%s%s",
          g_topo.sockets,
          g_topo.cores_uncertain ? "at least " : "",
          g_topo.cores, g_topo.threads,
          g_topo.hybrid ? ", hybrid, booted on a " : "",
          g_topo.hybrid ? g_topo.boot_core_kind : "");

    if (g_topo.cores_uncertain)
        kwarn("cpu", "this processor mixes cores that thread with cores that "
                     "do not, and the count above was worked out from the one "
                     "this system started on - efficiency cores may be counted "
                     "in pairs.  The logical processor count is exact.");

    if (g_topo.base_mhz)
        kinfo("cpu", "base %u MHz, maximum %u MHz, bus %u MHz",
              g_topo.base_mhz, g_topo.max_mhz, g_topo.bus_mhz);

    for (u32 i = 0; i < g_topo.cache_count; i++) {
        cpu_cache_t *e = &g_topo.cache[i];
        kinfo("cpu", "L%u %s: %llu KiB, %u-way, %u byte lines, shared by %u",
              e->level, e->kind, (unsigned long long)(e->bytes / 1024),
              e->ways, e->line, e->shared);
    }
}

/* Capture once on each actual processor before it advertises itself online.
 * A work-stealing queue cannot perform per-CPU enumeration. */
#include "time.h"
#include "../include/kestrel/syscall.h"

typedef struct {
    u32 apic, core, package, kind, smt_shift, package_shift;
    cpu_cache_t caches[CPU_MAX_CACHES];
    u32 cache_count, answered;
    bool aperf_supported;
    u32 tsc_khz, mperf_mhz;
    u64 previous_aperf, previous_mperf, previous_tsc;
    u32 frequency_sequence, effective_mhz, active_mhz, frequency_valid;
    u64 sampled_ms, sampled_reference_us;
} logical_cpu_t;
static logical_cpu_t logical[ACPI_MAX_CPUS];
static bool refined;

void cpu_topology_capture(u32 slot) {
    if (slot >= ARRAY_LEN(logical) || cpu_current_slot()!=slot) return;
    logical_cpu_t *p = &logical[slot];
    u32 a,b,c,d;
    cpuid_raw(0,0,&a,&b,&c,&d);
    u32 max_leaf=a;
    bool intel=b==0x756e6547 && d==0x49656e69 && c==0x6c65746e;
    cpuid_raw(1,0,&a,&b,&c,&d);
    p->apic=b>>24;
    bool hypervisor=(c & (1u<<31))!=0;
    u32 tpc=1;
    bool topology=false;
    for (u32 attempt=0;attempt<2 && !topology;attempt++) {
        u32 leaf=attempt?0x0b:0x1f;
        if(max_leaf<leaf)continue;
        cpuid_raw(leaf,0,&a,&b,&c,&d);
        if(!(b&0xffff))continue;
        p->apic=d;
        topology=read_level_widths(leaf,&p->smt_shift,&p->package_shift,&tpc);
    }
    p->core=p->apic>>p->smt_shift;
    p->package=topology?p->apic>>p->package_shift:0;
    if(intel && max_leaf>=0x1a && g_topo.hybrid) {
        cpuid_raw(0x1a,0,&a,&b,&c,&d);
        p->kind=(a>>24)==0x40?1:(a>>24)==0x20?2:0;
    }
    u32 cache_leaf=0;
    if(intel && max_leaf>=4)cache_leaf=4;
    else {
        cpuid_raw(0x80000000,0,&a,&b,&c,&d);
        if(a>=0x8000001d)cache_leaf=0x8000001d;
    }
    for(u32 sub=0;cache_leaf && sub<CPU_MAX_CACHES;sub++) {
        cpuid_raw(cache_leaf,sub,&a,&b,&c,&d);
        u32 type=a&31;
        if(!type)break;
        if(type>3)continue;
        cpu_cache_t *e=&p->caches[p->cache_count++];
        e->type=type; e->level=(a>>5)&7;
        e->shared=((a>>14)&4095)+1;
        e->line=(b&4095)+1;e->ways=((b>>22)&1023)+1;
        e->bytes=(u64)e->line*e->ways*(((b>>12)&1023)+1)*((u64)c+1);
    }
    /* Read architectural counters only on the owning bare-metal Intel CPU.
     * Invariant TSC is a reference clock, never a claimed current core clock.
     * Prefer that CPU's complete CPUID.15 ratio/crystal over the approximate
     * boot calibration. MPERF's nominal reference is separate: Linux turbostat
     * uses CPUID.16 base_hz (not unadjusted TSC Hz) on Arrow Lake too. Query it
     * locally, never copy the BSP's g_topo clock or a HWP guaranteed P/E rate.
     * APERF/MPERF then supply this processor's measured active ratio. */
    if(intel && !hypervisor && max_leaf>=6) {
        cpuid_raw(6,0,&a,&b,&c,&d);
        bool counters=(c&1)!=0;
        cpuid_raw(0x80000000,0,&a,&b,&c,&d);
        bool invariant=false;
        if(a>=0x80000007) {
            cpuid_raw(0x80000007,0,&a,&b,&c,&d);invariant=(d&(1u<<8))!=0;
        }
        u64 khz=0;
        if(max_leaf>=0x15) {
            cpuid_raw(0x15,0,&a,&b,&c,&d);
            if(a && b && c)khz=((u64)c*b/a)/1000u;
        }
        if(!khz)khz=timer_cycles_per_us()*1000u;
        if(counters && invariant && khz>=1000u && khz<=65535000u) {
            p->tsc_khz=(u32)khz;p->aperf_supported=true;
            if(max_leaf>=0x16) {
                cpuid_raw(0x16,0,&a,&b,&c,&d);p->mperf_mhz=a&0xffffu;
            }
        }
    }
    if(p->aperf_supported) {
        p->previous_aperf=rdmsr(0xe8);
        p->previous_mperf=rdmsr(0xe7);
        p->previous_tsc=rdtsc();
    }
    __atomic_store_n(&p->answered,1,__ATOMIC_RELEASE);
}
void cpu_frequency_sample(u32 slot) {
    if(slot>=ARRAY_LEN(logical))return;
    bool irq=irq_save();
    if(cpu_current_slot()!=slot){irq_restore(irq);return;}
    logical_cpu_t *p=&logical[slot];
    if(!__atomic_load_n(&p->answered,__ATOMIC_ACQUIRE) || !p->aperf_supported){
        irq_restore(irq);return;
    }
    /* The BSP's interrupt-count clock stops while a CLI parallel batch owns
     * user mappings. Rate-limit against this CPU's invariant reference instead,
     * so AP sampling does not depend on the BSP delivering timer interrupts.
     * A backwards reference is sampled once to invalidate/rebase the pair. */
    u64 due_tsc=rdtsc();
    if(due_tsc>=p->previous_tsc &&
       due_tsc-p->previous_tsc<(u64)p->tsc_khz*100u){irq_restore(irq);return;}
    /* Keep the counter pair and reference timestamp on this CPU, with no
     * nested local sampling or scheduler migration between their reads. */
    u64 now_ms=g_uptime_ms,aperf=rdmsr(0xe8),mperf=rdmsr(0xe7),tsc=rdtsc();
    /* Preserve the ABI's uptime stamp, but do not use missed BSP timer IRQs
     * to decide freshness. This is the same calibrated reference used for
     * cross-CPU worker accounting. Without calibration, report unknown. */
    u64 reference_us=timer_cycles_per_us()?timer_now_us():0;
    u64 delta=aperf-p->previous_aperf,mdelta=mperf-p->previous_mperf;
    u64 tdelta=tsc-p->previous_tsc;
    bool monotonic=aperf>=p->previous_aperf && mperf>=p->previous_mperf && tsc>p->previous_tsc;
    p->previous_aperf=aperf;p->previous_mperf=mperf;p->previous_tsc=tsc;
    u32 effective=0,active=0,valid=0;
    if(monotonic && tdelta<=(u64)p->tsc_khz*2000u) {
        u64 elapsed=tdelta*1000u/p->tsc_khz;
        if(elapsed && delta/elapsed<65535u) {
            effective=(u32)(delta/elapsed);valid=KCPU_FREQ_EFFECTIVE_VALID;
            /* No active cycles means no measurable active clock. In particular
             * a sleeping core is not assigned its base/max clock or 0 MHz.
             * Bound delta first so the ratio multiplication cannot overflow. */
            if(mdelta && delta && p->mperf_mhz) {
                u64 mhz=(u64)p->mperf_mhz*delta/mdelta;
                if(mhz && mhz<65535u)active=(u32)mhz,valid|=KCPU_FREQ_ACTIVE_VALID;
            }
        }
    }
    __atomic_fetch_add(&p->frequency_sequence,1u,__ATOMIC_ACQ_REL);
    __atomic_store_n(&p->effective_mhz,effective,__ATOMIC_RELAXED);
    __atomic_store_n(&p->active_mhz,active,__ATOMIC_RELAXED);
    __atomic_store_n(&p->frequency_valid,valid,__ATOMIC_RELAXED);
    __atomic_store_n(&p->sampled_ms,now_ms,__ATOMIC_RELAXED);
    __atomic_store_n(&p->sampled_reference_us,reference_us,__ATOMIC_RELAXED);
    __atomic_fetch_add(&p->frequency_sequence,1u,__ATOMIC_RELEASE);
    irq_restore(irq);
}
bool cpu_logical_info(u32 index, void *out) {
    kcpuinfo_t *r=out;
    u32 enabled=0;
    for(int i=0;i<g_acpi_cpu_count;i++) {
        if(!(g_acpi_cpus[i].flags&1))continue;
        if(enabled++!=index)continue;
        memset(r,0,sizeof *r);r->apic_id=g_acpi_cpus[i].apic_id;
        for(u32 j=0;j<ARRAY_LEN(logical);j++) {
            const logical_cpu_t *p=&logical[j];
            if(!__atomic_load_n(&p->answered,__ATOMIC_ACQUIRE) || p->apic!=r->apic_id)continue;
            r->online=1;r->core_id=p->core;r->package_id=p->package;r->kind=p->kind;
            /* A bounded coherent snapshot; contention/stale data is unknown,
             * not a reason to invent a clock or wait for another CPU. */
            for(u32 tries=0;tries<8u;tries++) {
                u32 seq=__atomic_load_n(&p->frequency_sequence,__ATOMIC_ACQUIRE);
                if(seq&1u)continue;
                u64 sampled=__atomic_load_n(&p->sampled_ms,__ATOMIC_RELAXED);
                u64 reference=__atomic_load_n(&p->sampled_reference_us,__ATOMIC_RELAXED);
                u32 valid=__atomic_load_n(&p->frequency_valid,__ATOMIC_RELAXED);
                u32 effective=__atomic_load_n(&p->effective_mhz,__ATOMIC_RELAXED);
                u32 active=__atomic_load_n(&p->active_mhz,__ATOMIC_RELAXED);
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                if(seq!=__atomic_load_n(&p->frequency_sequence,__ATOMIC_RELAXED))continue;
                u64 now=timer_now_us();r->sampled_ms=sampled;
                /* A backwards/skewed reference is unknown, never unsigned
                 * wraparound or an invented fresh clock. */
                if(reference && timer_cycles_per_us() && now>=reference && now-reference<=1000000u) {
                    r->frequency_valid=valid;r->effective_mhz=effective;r->active_mhz=active;
                }
                break;
            }
            break;
        }
        return true;
    }
    return false;
}
void cpu_topology_refine(void) {
    u32 cores=0,packages=0,perf=0,eff=0,answered=0,known=0;
    for(u32 i=0;i<ARRAY_LEN(logical);i++) {
        const logical_cpu_t *p=&logical[i];
        if(!__atomic_load_n(&p->answered,__ATOMIC_ACQUIRE))continue;
        answered++;
        bool core_seen=false,pkg_seen=false;
        for(u32 j=0;j<i;j++) {
            const logical_cpu_t *q=&logical[j];
            if(!__atomic_load_n(&q->answered,__ATOMIC_ACQUIRE))continue;
            if(p->package==q->package) {
                pkg_seen=true;
                if(p->core==q->core)core_seen=true;
            }
        }
        if(!pkg_seen)packages++;
        if(core_seen)continue; /* SMT siblings are threads, not extra P-cores */
        cores++;
        if(p->kind==1){perf++;known++;}
        if(p->kind==2){eff++;known++;}
    }
    bool complete=answered==g_topo.threads;
    if(complete && cores) {
        g_topo.cores=cores;g_topo.sockets=packages;g_topo.cores_uncertain=false;
    }
    g_topo.per_core_kinds=complete && known==cores;
    g_topo.perf_cores=g_topo.per_core_kinds?perf:0;
    g_topo.eff_cores=g_topo.per_core_kinds?eff:0;
    refined=true;
    kinfo("cpu","per-processor CPUID: %u/%u logical CPUs, %u physical cores, %u P + %u E%s",
          answered,g_topo.threads,cores,perf,eff,complete?"":" (partial; missing CPUs not inferred)");
}
/* CPUID sharing is an APIC-ID address width, not a divisor of online CPUs.
 * Count each actual cache identity once, including partially populated groups. */
static u32 cache_shift(u32 sharing) {
    u32 bits=0;
    while(sharing>1){sharing=(sharing+1)>>1;bits++;}
    return bits;
}
u64 cpu_cache_total(u32 level) {
    u64 total=0;
    if(!refined)return 0;
    for(u32 i=0;i<ARRAY_LEN(logical);i++) {
        const logical_cpu_t *p=&logical[i];
        if(!__atomic_load_n(&p->answered,__ATOMIC_ACQUIRE))continue;
        for(u32 n=0;n<p->cache_count;n++) {
            const cpu_cache_t *e=&p->caches[n];
            if(e->level!=level)continue;
            u32 shift=cache_shift(e->shared),id=p->apic>>shift;
            bool seen=false;
            for(u32 j=0;j<i && !seen;j++) {
                const logical_cpu_t *q=&logical[j];
                if(!__atomic_load_n(&q->answered,__ATOMIC_ACQUIRE))continue;
                for(u32 k=0;k<q->cache_count;k++) {
                    const cpu_cache_t *f=&q->caches[k];
                    if(f->level==level && f->type==e->type &&
                       cache_shift(f->shared)==shift && (q->apic>>shift)==id){seen=true;break;}
                }
            }
            if(!seen)total+=e->bytes;
        }
    }
    return total;
}
