#!/usr/bin/env python3
"""Actual WM run loop and input-wait fallback; simulated clock, not FPS proof."""
import re
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    src = (ROOT/'user/libgui/window.c').read_text()
    display = (ROOT/'user/libgui/display.c').read_text()
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef struct { int input_fd; } display_t;
typedef struct { bool animations_on,running,animating,drawing_continuously;display_t *display; } wm_t;
static unsigned pumps,waits,paints,retires,slept,ioctls;
static unsigned sleep_ms_total;
static bool ioctl_fail;
static int waits_seen[16];
static void sleep_ms(int ms){assert(ms>0);slept++;sleep_ms_total+=(unsigned)ms;}
static int ioctl(int fd,int command,uint32_t *ms){
    assert(fd==7&&command==3);ioctls++;waits_seen[waits++]=(int)*ms;
    if(ioctl_fail)return -1;
    *ms=0;return 0;
}
''' + function(display, 'display_wait_input') + r'''
static void repaint(wm_t *wm){assert(wm->animations_on);paints++;}
static void stage_retire_if_idle(wm_t *wm){assert(!wm->running);retires++;}
static bool wm_has_damage(wm_t *wm){(void)wm;return pumps==5;}
static bool wm_pump(wm_t *wm){
    pumps++;
    wm->drawing_continuously=pumps<=2;
    wm->animating=pumps==2||pumps==3; // continuous takes precedence
    if(pumps==6)wm->running=false;
    return true;
}
'''
    for name in ('FRAME_MS', 'TICK_MS'):
        code += re.search(r'^#define '+name+r'\s+[^\n]+', src, re.M)[0]+'\n'
    code += function(src, 'wm_run') + r'''
int main(void){
    display_t d={7};wm_t wm={.running=true,.display=&d};
    wm_run(&wm);
    assert(pumps==6&&paints==1&&retires==1&&waits==4&&!slept);
    assert(waits_seen[0]==0&&waits_seen[1]==0); // no delay appended to either 3D frame
    assert(waits_seen[2]==16&&waits_seen[3]==120); // animation/idle semantics preserved
    // Failed event device never turns a zero-time poll into a busy-spin fallback.
    ioctl_fail=true;display_wait_input(&d,0);assert(slept==1&&sleep_ms_total==1);
    display_wait_input(&d,16);assert(slept==2&&sleep_ms_total==17);
    puts("PASS actual WM frame pacing: continuous frames add no 16ms wait; animation/idle/input/damage/shutdown and failed-event-device backoff preserved");
}
'''
    run_test(code,'window_frame_pacing')


if __name__ == '__main__':
    main()
