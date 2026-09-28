#!/usr/bin/env python3
"""Execute scan UI transitions; an unavailable radio is not an empty scan."""
from test_gpu_stable_candidate import ROOT, function, run_test
src=(ROOT/'user/desktop/app_settings.c').read_text()
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {bool radio_up;char name[16];} wifi_t;
typedef struct {bool have_wifi,wifi_on,scanning;uint64_t scan_started;wifi_t wifi;int network_count;char status[200];} settings_t;
static unsigned scans;static int result;
static uint64_t uptime_ms(void){return 123;}
static void say(settings_t*s,const char*t){snprintf(s->status,sizeof s->status,"%s",t);}
static int wifi_scan(const char*n,int timeout){assert(!strcmp(n,"wlan0")&&timeout==2500);scans++;return result;}
static void load_wifi(settings_t*s){(void)s;}
'''+function(src,'do_scan')+'\n'+function(src,'finish_scan')+r'''
int main(void){
    settings_t s={.have_wifi=true,.wifi_on=true,.wifi={.name="wlan0"}};
    do_scan(&s);assert(!s.scanning&&!scans&&strstr(s.status,"unavailable"));
    s.wifi.radio_up=true;do_scan(&s);assert(s.scanning&&s.scan_started==123);
    result=-5;finish_scan(&s);assert(!s.scanning&&scans==1&&strstr(s.status,"Scan failed"));
    result=0;finish_scan(&s);assert(!strcmp(s.status,"No networks found."));
    s.network_count=3;finish_scan(&s);assert(!strcmp(s.status,"3 network(s) found."));
    s.wifi.radio_up=false;finish_scan(&s);assert(strstr(s.status,"Scan failed"));
    puts("PASS Settings scan status: unavailable, transport failure, successful empty/result scans and lost radio");
}
'''
run_test(code,'settings_wifi_status')
