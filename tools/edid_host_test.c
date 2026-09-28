/* edid_host_test.c - parse the user's FOUR REAL monitors' EDIDs off-target.
 *
 * The EDID is where "what resolutions?" (#6) and "what refresh rates?" (#5)
 * are answered - a misread descriptor offers a mode the panel lacks, or hides
 * the 240 Hz it has.  These are the exact bytes read from the four monitors on
 * the user's desk (all 2560x1440, ranges to 180/240 Hz); the expectations were
 * computed by an INDEPENDENT EDID parser, so this checks kernel/edid.c against
 * real data and a second implementation at once.  See kestrelos-edid-parser.
 *
 * Build + run (from the repo root):
 *   clang -std=c11 -Wall -Wextra -DEDID_HOST_TEST -I kernel
 *         tools/edid_host_test.c kernel/edid.c -o edid_test  &&  ./edid_test
 */
#ifndef EDID_HOST_TEST
#define EDID_HOST_TEST
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#include "edid.h"

static int fails=0,total=0;
static void ck(const char*w,int c){total++;if(!c){fails++;printf("    FAIL: %s\n",w);}else printf("    ok:   %s\n",w);}

static const u8 acer_x27u[] = {0,255,255,255,255,255,255,0,4,114,165,16,93,1,144,97,19,36,1,4,181,59,33,120,59,238,5,175,78,60,183,37,14,80,84,191,239,0,113,79,129,64,129,128,129,192,149,0,179,0,209,192,1,1,86,94,0,160,160,160,41,80,48,32,53,0,75,74,33,0,0,26,0,0,0,253,13,48,240,255,255,130,1,10,32,32,32,32,32,32,0,0,0,252,0,88,50,55,85,32,87,49,10,32,32,32,32,32,0,0,0,255,0,56,54,49,57,48,48,49,53,68,51,86,48,49,2,126,2,3,57,241,73,144,1,3,4,17,19,31,32,63,35,9,7,7,131,1,0,0,226,0,127,227,5,192,0,230,6,7,1,98,75,0,116,26,0,0,3,1,48,240,0,128,0,0,0,0,240,0,0,0,0,0,0,112,194,0,160,160,160,85,80,48,32,53,0,75,74,33,0,0,26,152,218,128,160,112,56,41,64,48,32,53,0,75,74,33,0,0,26,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,192,112,18,121,3,0,3,1,40,247,178,1,4,255,9,63,1,87,128,43,0,159,5,170,0,15,0,20,0,151,218,0,4,127,7,159,0,47,0,31,0,55,4,40,0,9,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,133,144};
static const u8 acer_xv271u[] = {0,255,255,255,255,255,255,0,4,114,36,12,1,1,1,1,36,33,1,4,181,60,34,120,59,162,85,172,80,70,171,38,12,80,84,191,239,128,209,192,179,0,149,0,129,128,113,64,129,192,129,64,169,64,106,94,0,160,160,160,41,80,48,32,53,0,85,80,33,0,0,26,0,0,0,253,0,48,180,255,255,80,1,10,32,32,32,32,32,32,0,0,0,252,0,88,86,50,55,49,85,32,77,51,10,32,32,32,0,0,0,255,0,49,51,51,54,48,53,70,49,68,51,76,73,74,2,203,2,3,51,241,77,31,1,2,3,4,5,16,17,18,19,20,63,77,35,9,7,7,131,1,0,0,109,26,0,0,2,1,48,180,0,0,0,0,0,0,227,5,192,0,230,6,7,1,97,97,33,212,188,0,160,160,160,41,80,48,32,53,0,85,80,33,0,0,26,92,157,0,160,160,160,41,80,48,32,53,0,85,80,33,0,0,26,28,130,128,136,112,56,45,64,24,32,53,0,85,80,33,0,0,26,152,226,0,160,160,160,41,80,48,32,53,0,85,80,33,0,0,26,0,0,0,0,134,112,18,121,0,0,3,1,60,163,3,1,4,255,9,159,0,47,128,31,0,159,5,40,0,2,128,4,0,224,9,1,4,255,9,159,0,47,128,31,0,159,5,31,0,2,128,4,0,180,25,1,4,255,9,159,0,47,128,31,0,159,5,32,0,2,128,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,134,144};
static const u8 aoc_q27g4zd[] = {0,255,255,255,255,255,255,0,5,227,33,180,102,44,0,0,13,35,1,4,181,59,33,120,63,185,245,175,77,61,182,36,14,80,84,191,239,0,209,192,129,128,49,104,49,124,69,104,69,124,1,1,1,1,131,102,0,160,160,160,168,80,48,32,53,0,78,78,33,0,0,30,0,0,0,255,0,50,79,77,82,51,74,65,48,49,49,51,54,54,0,0,0,252,0,81,50,55,71,52,90,68,10,32,32,32,32,32,0,0,0,253,12,48,240,105,105,120,1,10,32,32,32,32,32,32,2,103,2,3,60,241,76,1,3,5,20,4,19,31,18,2,17,144,63,35,9,7,7,131,1,0,0,226,0,234,227,5,227,1,230,6,7,1,98,75,0,116,26,0,0,3,1,48,240,0,32,0,0,0,0,240,0,0,0,0,0,0,7,246,0,160,160,160,168,80,48,32,53,0,78,78,33,0,0,26,112,194,0,160,160,160,85,80,48,32,53,0,78,78,33,0,0,30,112,160,0,160,160,160,70,80,48,32,53,0,78,78,33,0,0,30,0,0,0,0,0,0,0,0,0,0,0,0,0,84,112,18,121,3,0,3,1,60,40,178,1,132,255,9,63,1,47,128,31,0,159,5,167,0,2,0,4,0,95,56,1,4,255,9,159,0,47,128,31,0,159,5,29,0,2,0,4,0,151,252,0,4,255,9,105,0,7,128,31,0,159,5,29,0,2,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,31,144};
static const u8 msi_mag272q[] = {0,255,255,255,255,255,255,0,13,196,248,60,1,1,1,1,38,35,1,4,181,61,34,120,235,185,245,175,77,61,182,36,14,80,84,37,79,0,59,128,129,128,149,0,179,0,209,192,209,252,1,1,1,1,86,94,0,160,160,160,41,80,48,32,53,0,94,88,33,0,0,26,0,0,0,253,12,48,240,135,135,106,1,10,32,32,32,32,32,32,0,0,0,252,0,77,65,71,32,50,55,50,81,32,88,50,52,10,0,0,0,255,0,10,32,32,32,32,32,32,32,32,32,32,32,32,2,172,2,3,54,97,70,3,18,4,47,144,63,35,9,7,7,131,1,0,0,226,0,234,116,26,0,0,3,1,48,240,0,128,0,0,0,0,240,0,0,0,0,0,0,227,5,192,1,230,6,5,1,99,68,2,111,194,0,160,160,160,85,80,48,32,53,0,94,88,33,0,0,26,68,172,128,160,112,56,98,64,48,32,53,0,94,88,33,0,0,26,234,236,128,160,112,56,135,64,48,32,53,0,94,88,33,0,0,26,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,64,112,18,121,3,0,3,1,60,8,236,0,4,255,9,159,0,47,128,31,0,159,5,102,0,2,0,4,0,49,44,1,4,255,9,159,0,47,128,31,0,159,5,129,0,2,0,4,0,202,156,1,4,255,9,159,0,47,128,31,0,159,5,178,0,2,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,119,144};

struct expect { const u8*edid; u32 len; const char*name; const char*man; u16 prod; int w,h; u8 vmin,vmax; u32 maxr, anyr; int anyw,anyh; };
/* maxr = the fastest millihertz seen at the native resolution across base,
 * CTA and DisplayID blocks, computed the way edid.c does it: pclk_khz*1e6 / total_pixels with
 * INTEGER (truncating) division - not rounded.  The independent parser first
 * rounded and reported 144002 / 119998 here; the 1-mHz gap was the rounding,
 * not a parser bug, and truncation is what both should agree on. */
static const struct expect monitors[] = {
  { acer_x27u, sizeof acer_x27u, "acer_x27u", "ACR", 0x10a5, 2560, 1440, 48, 240, 239999, 239999, 2560, 1440 },
  { acer_xv271u, sizeof acer_xv271u, "acer_xv271u", "ACR", 0x0c24, 2560, 1440, 48, 180, 179997, 179997, 2560, 1440 },
  { aoc_q27g4zd, sizeof aoc_q27g4zd, "aoc_q27g4zd", "AOC", 0xb421, 2560, 1440, 48, 240, 240000, 240000, 2560, 1440 },
  { msi_mag272q, sizeof msi_mag272q, "msi_mag272q", "CND", 0x3cf8, 2560, 1440, 48, 240, 239969, 239988, 1920, 1080 },
};

int main(void){
    for(size_t i=0;i<sizeof monitors/sizeof monitors[0];i++){
        const struct expect*e=&monitors[i];
        edid_info_t info;
        printf("[%s]\n", e->name);
        int ok = edid_parse(e->edid, e->len, &info);
        ck("parses (header + checksum good)", ok);
        if(!ok) continue;
        char b[96];
        snprintf(b,sizeof b,"manufacturer is %s",e->man);
        ck(b, strcmp(info.manufacturer,e->man)==0);
        snprintf(b,sizeof b,"product code 0x%04x",e->prod);
        ck(b, info.product_code==e->prod);
        snprintf(b,sizeof b,"native resolution %dx%d",e->w,e->h);
        ck(b, info.native.hactive==e->w && info.native.vactive==e->h);
        snprintf(b,sizeof b,"refresh range %u-%u Hz",e->vmin,e->vmax);
        ck(b, info.refresh_min_hz==e->vmin && info.refresh_max_hz==e->vmax);
        snprintf(b,sizeof b,"max refresh at native %u mHz",e->maxr);
        ck(b, info.max_refresh_mhz_at_native==e->maxr);
        ck("highest-resolution capability is retained separately",
           info.highest_resolution.hactive==e->w &&
           info.highest_resolution.vactive==e->h);
        printf("        highest refresh at any resolution: %u.%03u Hz at %ux%u\n",
               info.highest_refresh.refresh_mhz/1000,
               info.highest_refresh.refresh_mhz%1000,
               info.highest_refresh.hactive, info.highest_refresh.vactive);
        ck("highest-refresh capability is retained separately",
           info.highest_refresh.refresh_mhz==e->anyr &&
           info.highest_refresh.hactive==e->anyw &&
           info.highest_refresh.vactive==e->anyh);
        ck("highest resolution+refresh pair is an advertised mode",
           info.highest_resolution_refresh.hactive==e->w &&
           info.highest_resolution_refresh.vactive==e->h &&
           info.highest_resolution_refresh.refresh_mhz==e->maxr);
        printf("        -> %s %04x, %dx%d, %u-%u Hz, up to %u.%03u Hz at native\n",
               info.manufacturer, info.product_code, info.native.hactive,
               info.native.vactive, info.refresh_min_hz, info.refresh_max_hz,
               info.max_refresh_mhz_at_native/1000, info.max_refresh_mhz_at_native%1000);
    }
    printf("[corruption rejected]\n");
    u8 bad[128]; memcpy(bad, monitors[0].edid, 128); bad[80]^=0xFF;
    edid_info_t info;
    ck("a bad checksum is rejected", edid_parse(bad,128,&info)==0);
    u8 nothdr[128]; memset(nothdr,0x55,128);
    ck("a non-EDID block is rejected", edid_parse(nothdr,128,&info)==0);
    printf("\n%d/%d checks passed%s\n", total-fails, total, fails?"  <<< FAILURE":"  ALL GOOD");
    return fails?1:0;
}
