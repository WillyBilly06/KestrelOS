#!/usr/bin/env python3
"""Execute native boot discovery's actual query loop with timed KAPI edges.

Models delayed DP detection, not link training or a physical Acer signal.
"""
import re
from test_gpu_stable_candidate import ROOT, run_test


def main():
    src = (ROOT / 'kernel/nvkms_kapi_client.c').read_text()
    start = src.index('static NvBool query_dynamic_display(')
    end = src.index('\n}', start) + 2
    function = src[start:end]
    macros = '\n'.join(re.findall(r'^#define (?:EDID_\w+|DP_CONNECT_TIMEOUT_USEC)\s+[^\n]+', src, re.M))
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t NvU32; typedef uint64_t NvU64; typedef bool NvBool;
typedef NvU32 NvKmsKapiDisplay; typedef NvU32 NvKmsKapiConnector;
#define NV_TRUE true
#define NV_FALSE false
struct NvKmsKapiDynamicDisplayParams {
    NvU32 handle,connected,overrideEdid,forceConnected,forceDisconnected;
    struct { unsigned char buffer[128]; NvU32 bufferSize; } edid;
};
struct NvKmsKapiConnectorInfo { unsigned unused; };
static int test_device,parsed_edid;
static NvU64 now,connection_at,edid_at,unplug_at,valid_at;
static unsigned queries,refreshes,delays;
static bool cached_connected,initially_connected,valid_edid,query_fail,refresh_fail,early_sleep;
static NvU64 timer_now_us(void) { return now; }
static void timer_mdelay(unsigned ms) { assert(ms==250); now+=early_sleep?1000:ms*1000ull; delays++; }
static bool edid_parse(const unsigned char *p,NvU32 n,int *out) {
    assert(n==128 && p[0]==0x5a && out==&parsed_edid);return valid_edid && now>=valid_at;
}
static bool dynamic(int dev,struct NvKmsKapiDynamicDisplayParams *p) {
    assert(!dev && p->handle==0x200);
    struct NvKmsKapiDynamicDisplayParams expected={.handle=0x200};
    assert(!memcmp(p,&expected,sizeof *p));queries++;
    if(query_fail)return false;
    p->connected=(initially_connected||cached_connected) && now>=connection_at && now<unplug_at;
    if(p->connected && now>=edid_at){p->edid.bufferSize=128;p->edid.buffer[0]=0x5a;}
    // Poison output-only storage: the next detect must reset it completely.
    p->forceConnected=77;p->forceDisconnected=88;return true;
}
static bool connector(int dev,NvKmsKapiConnector c,struct NvKmsKapiConnectorInfo *out) {
    assert(!dev && c==7 && !out->unused);refreshes++;
    if(refresh_fail)return false;
    if(now>=connection_at)cached_connected=true;
    return true;
}
static struct { bool (*getDynamicDisplayInfo)(int,struct NvKmsKapiDynamicDisplayParams *);
    bool (*getConnectorInfo)(int,NvKmsKapiConnector,struct NvKmsKapiConnectorInfo *); }
    kapi={dynamic,connector};
''' + macros + '\n' + function + r'''
static struct NvKmsKapiDynamicDisplayParams result;
static NvBool identity;static NvU32 waited;
static bool probe(bool dp,NvU64 deadline,NvU64 edid_timeout) {
    memset(&result,0xff,sizeof result);identity=true;waited=0xffffffff;
    return query_dynamic_display(0x200,7,dp,&result,&identity,&waited,edid_timeout,deadline);
}
static void reset(void) {
    now=0;connection_at=edid_at=valid_at=0;unplug_at=UINT64_MAX;
    queries=refreshes=delays=0;
    cached_connected=initially_connected=query_fail=refresh_fail=early_sleep=false;valid_edid=true;
}
int main(void) {
    reset();initially_connected=true;
    assert(probe(true,10000000,1000000)&&identity&&result.connected&&!waited&&!delays);
    reset();connection_at=1500000;edid_at=2500000;
    assert(probe(true,10000000,1000000)&&identity&&result.connected&&now==2500000);
    assert(waited==2500&&refreshes>=2);
    // A late link gets its own EDID window; the original one-second deadline
    // must not expire before physical detection has even succeeded.
    reset();connection_at=9000000;edid_at=10000000;
    assert(probe(true,10000000,1000000)&&identity&&now==10000000);
    reset();connection_at=UINT64_MAX;
    assert(probe(true,10000000,1000000)&&!identity&&!result.connected&&now==10000000);
    unsigned first_delays=delays;
    // Same shared deadline: additional unused DP paths add no ten-second wait.
    for(unsigned i=0;i<5;i++)assert(probe(true,10000000,1000000)&&!result.connected);
    assert(now==10000000&&delays==first_delays);
    reset();connection_at=UINT64_MAX;
    assert(probe(false,10000000,1000000)&&!result.connected&&!delays&&!refreshes);
    reset();initially_connected=true;edid_at=UINT64_MAX;
    assert(probe(true,10000000,1000000)&&result.connected&&!identity&&now==1000000);
    reset();initially_connected=true;edid_at=UINT64_MAX;unplug_at=500000;
    assert(probe(true,10000000,1000000)&&!result.connected&&!identity&&now==500000);
    reset();initially_connected=true;valid_edid=false;
    // Corrupt EDID gets the same bounded retry window as absent EDID;
    // source already implements this policy, rather than accepting it early.
    assert(probe(true,10000000,1000000)&&!identity&&now==1000000&&queries==5);
    reset();initially_connected=true;valid_at=750000;
    assert(probe(true,10000000,1000000)&&identity&&now==750000&&queries==4);
    reset();query_fail=true;assert(!probe(true,10000000,1000000)&&queries==1&&!delays);
    reset();refresh_fail=true;connection_at=UINT64_MAX;
    assert(!probe(true,10000000,1000000)&&refreshes==1);
    reset();early_sleep=true;connection_at=UINT64_MAX;
    assert(probe(true,10000000,1000000)&&now==10000000&&waited==10000&&delays==10000);
    reset();connection_at=UINT64_MAX;
    assert(probe(true,0,15000000)&&!result.connected&&!delays); // post-wake unplug is immediate
    puts("PASS actual DP discovery: delayed link/EDID, real-clock deadline, shared disconnected-port budget, no forced connection/override, fresh requests, unplug/HDMI/failure gates");
}
'''
    run_test(code, 'nvkms_display_discovery')


if __name__ == '__main__':
    main()
