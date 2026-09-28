"""Production per-CPU collector scaffold; --compile-only never runs its model."""
from pathlib import Path
import sys
from test_gpu_stable_candidate import run_test

ROOT = Path(__file__).resolve().parents[1]
src = (ROOT / 'kernel/cputopo.c').read_text()
src = '\n'.join(x for x in src.splitlines() if not x.startswith('#include'))
header = (ROOT / 'kernel/cpu.h').read_text()
types = header[header.index('#define CPU_MAX_CACHES'):header.index('extern cpu_topology_t')]
abi = (ROOT / 'include/kestrel/syscall.h').read_text()
abi = abi[abi.index('#define KCPU_FREQ_EFFECTIVE_VALID'):abi.index('} kcpuinfo_t;')+len('} kcpuinfo_t;')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define ARRAY_LEN(x) (sizeof(x)/sizeof(*(x)))
#define ACPI_MAX_CPUS 64
#define kinfo(...) ((void)0)
#define kwarn(...) ((void)0)
static void strlcpy(char *d,const char *s,size_t n){snprintf(d,n,"%s",s);}
static struct {u32 apic_id,flags;} g_acpi_cpus[64];
static int g_acpi_cpu_count;
static u32 mock_apic, mock_kind, mock_threads=1,mock_slot,mock_nominal=3700;
static bool mock_hypervisor, mock_aperf=true,mock_invariant=true,mock_crystal=true;
static u64 g_uptime_ms,mock_cycles,mock_mperf,mock_tsc,mock_reference_us;
static bool mock_timer_calibrated=true;
static unsigned msr_reads;
static u64 timer_cycles_per_us(void){return mock_timer_calibrated?1000:0;}
static u64 timer_now_us(void){return mock_reference_us;}
static u64 rdtsc(void){return mock_tsc;}
static u32 cpu_current_slot(void){return mock_slot;}
static bool irq_save(void){return false;}
static void irq_restore(bool on){assert(!on);}
static u64 rdmsr(u32 msr){assert(msr==0xe8||msr==0xe7);msr_reads++;return msr==0xe8?mock_cycles:mock_mperf;}
static void cpuid_raw(u32 leaf,u32 sub,u32 *a,u32 *b,u32 *c,u32 *d){
    *a=*b=*c=*d=0;
    if(leaf==0){*a=0x1f;*b=0x756e6547;*d=0x49656e69;*c=0x6c65746e;}
    if(leaf==1){*b=mock_apic<<24;*c=mock_hypervisor?1u<<31:0;}
    if(leaf==6)*c=mock_aperf;
    if(leaf==7)*d=1u<<15;
    if(leaf==0x1a)*a=mock_kind<<24;
    if(leaf==0x15 && mock_crystal){*a=1;*b=150;*c=24000000;} // 3600MHz TSC, NOT 3700MHz MPERF
    if(leaf==0x16){*a=mock_nominal;*b=5500;*c=100;}
    if(leaf==0x80000000)*a=0x80000007;
    if(leaf==0x80000007 && mock_invariant)*d=1u<<8;
    if(leaf==0x1f || leaf==0xb){
        *d=mock_apic;
        if(sub==0){*a=1;*b=mock_threads;*c=1u<<8;}
        if(sub==1){*a=7;*b=24;*c=2u<<8;}
    }
    if(leaf==4 && sub<3){
        u32 sharing=sub==2?128:mock_kind==0x40?2:8;
        u32 bytes=sub==0?32768:sub==1?2097152:37748736;
        *a=(sub==0?1:3)|((sub+1)<<5)|((sharing-1)<<14);
        *b=63;*c=bytes/64-1;
    }
}
'''
tests = r'''
static void reset(void){
    memset(logical,0,sizeof logical);memset(&g_topo,0,sizeof g_topo);
    memset(g_acpi_cpus,0,sizeof g_acpi_cpus);g_acpi_cpu_count=0;
    refined=false;mock_hypervisor=false;mock_aperf=true;
    mock_threads=1;mock_kind=0x40;mock_apic=0;
    g_uptime_ms=mock_cycles=mock_mperf=mock_tsc=0;msr_reads=mock_slot=0;
    mock_reference_us=1;mock_timer_calibrated=true;
    mock_nominal=3700;mock_invariant=mock_crystal=true;
}
static void capture(unsigned slot,unsigned apic,unsigned kind,unsigned threads){
    mock_slot=slot;mock_apic=apic;mock_kind=kind;mock_threads=threads;cpu_topology_capture(slot);
}
int main(void){
    reset();
    for(unsigned i=0;i<48;i++){g_acpi_cpus[i].apic_id=i;g_acpi_cpus[i].flags=(i%2)==0;}
    g_acpi_cpu_count=48;cpu_topology_init();assert(g_topo.threads==24);
    for(unsigned i=0;i<24;i++)capture(i,2*i,i<8?0x40:0x20,1);
    cpu_topology_refine();
    assert(g_topo.cores==24 && g_topo.perf_cores==8 && g_topo.eff_cores==16);
    assert(cpu_cache_total(1)==12ull*32768); // 8 P private + 4 E groups
    assert(cpu_cache_total(2)==12ull*2097152);
    assert(cpu_cache_total(3)==37748736);
    kcpuinfo_t r;assert(cpu_logical_info(23,&r) && r.apic_id==46 && r.kind==2);
    assert(!cpu_logical_info(24,&r));
    _Static_assert(sizeof(kcpuinfo_t)==40,"CPU enumeration ABI remains 40 bytes");
    g_uptime_ms=100;mock_tsc=360000000;mock_cycles=420000000;mock_mperf=370000000;
    cpu_frequency_sample(23);assert(cpu_logical_info(23,&r));
    assert(r.frequency_valid==3 && r.effective_mhz==4200 && r.active_mhz==4200);
    assert(logical[23].tsc_khz==3600000 && logical[23].mperf_mhz==3700);
    g_uptime_ms=200;mock_tsc+=360000000; // entirely idle: effective zero, active unknown
    cpu_frequency_sample(23);assert(cpu_logical_info(23,&r));
    assert(r.frequency_valid==KCPU_FREQ_EFFECTIVE_VALID && !r.effective_mhz && !r.active_mhz);
    g_uptime_ms=300;mock_tsc+=360000000;mock_cycles+=50000000;mock_mperf+=37000000;
    cpu_frequency_sample(23);assert(cpu_logical_info(23,&r));
    assert(r.frequency_valid==3 && r.effective_mhz==500 && r.active_mhz==5000); // 10% active
    unsigned reads=msr_reads;mock_slot=0;g_uptime_ms=400;
    cpu_frequency_sample(23);assert(msr_reads==reads);mock_slot=23; // not caller-selected affinity
    g_uptime_ms=1400;mock_reference_us+=1000001;assert(cpu_logical_info(23,&r) && !r.frequency_valid);
    g_uptime_ms=1500;mock_tsc=5400000000ull;mock_cycles=2;cpu_frequency_sample(23);
    assert(cpu_logical_info(23,&r) && !r.frequency_valid); // counter reset
    reset();g_topo.hybrid=true;g_topo.threads=28;
    for(unsigned i=0;i<16;i++)capture(i,i,0x40,2);
    for(unsigned i=0;i<12;i++)capture(16+i,16+2*i,0x20,1);
    cpu_topology_refine();
    assert(g_topo.cores==20 && g_topo.perf_cores==8 && g_topo.eff_cores==12);
    // SMT threads do not multiply private caches; E groups may be sparse.
    assert(cpu_cache_total(2)==11ull*2097152);
    reset();g_topo.hybrid=true;g_topo.threads=4;g_topo.cores=4;
    capture(0,0,0x40,1);capture(1,2,0x20,1);cpu_topology_refine();
    assert(!g_topo.per_core_kinds && !g_topo.perf_cores && g_topo.cores==4);
    reset();mock_hypervisor=true;capture(0,0,0x40,1);assert(msr_reads==0);
    reset();mock_aperf=false;capture(0,0,0x40,1);assert(msr_reads==0);
    reset();mock_invariant=false;capture(0,0,0x40,1);assert(msr_reads==0);
    reset();mock_crystal=false;capture(0,0,0x40,1);assert(logical[0].tsc_khz==1000000);
    reset();mock_nominal=0;capture(0,0,0x40,1);
    g_acpi_cpu_count=1;g_acpi_cpus[0].flags=1;
    g_uptime_ms=100;mock_tsc=360000000;mock_cycles=400000000;mock_mperf=370000000;
    cpu_frequency_sample(0);assert(cpu_logical_info(0,&r));
    assert(r.frequency_valid==KCPU_FREQ_EFFECTIVE_VALID && r.effective_mhz==4000 && !r.active_mhz);
    reset();capture(0,0,0x40,1);mock_nominal=3000;capture(1,2,0x20,1);
    assert(logical[0].mperf_mhz==3700 && logical[1].mperf_mhz==3000); // local CPUID, not BSP copy
    mock_slot=0;reads=msr_reads;cpu_topology_capture(2);assert(msr_reads==reads && !logical[2].answered);
    // A masked BSP timer must not prevent another CPU's due sample.
    reset();capture(0,0,0x40,1);g_acpi_cpu_count=1;g_acpi_cpus[0].flags=1;
    g_uptime_ms=10;mock_tsc=360000000;mock_cycles=420000000;mock_mperf=370000000;
    cpu_frequency_sample(0);assert(cpu_logical_info(0,&r));
    assert(r.frequency_valid==3&&r.active_mhz==4200&&r.effective_mhz==4200);
    reads=msr_reads;cpu_frequency_sample(0);assert(reads==msr_reads); // same reference is not due
    mock_tsc+=360000000;mock_cycles+=500000000;mock_mperf+=370000000;
    cpu_frequency_sample(0);assert(g_uptime_ms==10&&cpu_logical_info(0,&r));
    assert(r.active_mhz==5000&&r.effective_mhz==5000);
    // Freshness must expire even if the BSP has delivered no new timer IRQ.
    mock_reference_us+=1000001;
    assert(g_uptime_ms==10&&cpu_logical_info(0,&r)&&!r.frequency_valid);
    mock_reference_us=0;assert(cpu_logical_info(0,&r)&&!r.frequency_valid); // backwards reference
    mock_reference_us=2000000;
    mock_tsc+=3ull*3600000000;mock_cycles+=1000;mock_mperf+=1000;
    cpu_frequency_sample(0);assert(cpu_logical_info(0,&r)&&!r.frequency_valid); // gap too long
    mock_tsc+=360000000;mock_cycles+=430000000;mock_mperf+=370000000;
    cpu_frequency_sample(0);assert(cpu_logical_info(0,&r)&&r.active_mhz==4300); // rebase then recover
    mock_timer_calibrated=false;
    assert(cpu_logical_info(0,&r)&&!r.frequency_valid); // no reference: do not fall back to stopped ticks
    mock_tsc+=360000000;mock_cycles+=430000000;mock_mperf+=370000000;
    cpu_frequency_sample(0);mock_timer_calibrated=true;
    assert(cpu_logical_info(0,&r)&&!r.frequency_valid); // uncalibrated publication stays invalid
    mock_tsc+=360000000;mock_cycles+=430000000;mock_mperf+=370000000;
    cpu_frequency_sample(0);assert(cpu_logical_info(0,&r)&&r.active_mhz==4300);
    puts("CPU topology PASS: disabled entries, affinity, SMT, hybrid, shared caches, partial startup, frequency validity");
}
'''
if __name__ == '__main__':
    run_test(prefix + types + abi + src + tests, 'cpu-topology',compile_only='--compile-only' in sys.argv)
