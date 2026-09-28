#!/usr/bin/env python3
"""verify_nv_compute.py - check the compute-launch method offsets in
kernel/nv_compute.h against NVIDIA's classes/compute/clcec0.h.

A method at the wrong offset writes to a different register of the compute
engine - a silent misfire on real silicon - so the offsets are diffed against
NVIDIA's own header rather than trusted. Needs network; offline it skips.
"""
import os, re, sys, urllib.request
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HDR = os.path.join(ROOT, "kernel", "nv_compute.h")
CHAN = os.path.join(ROOT, "kernel", "nv_chan.c")
REF = "https://raw.githubusercontent.com/NVIDIA/open-gpu-doc/master/classes/compute/clcec0.h"
NAMES = ["SET_OBJECT", "SEND_PCAS_A", "SEND_SIGNALING_PCAS_B"]

def ours():
    d = {}
    txt = open(HDR, encoding="utf-8").read()
    for n in NAMES:
        m = re.search(r"#define\s+NVCEC0_%s\s+0x([0-9a-fA-F]+)u" % n, txt)
        if m: d[n] = int(m.group(1), 16)
    return d

def main():
    try:
        txt = urllib.request.urlopen(REF, timeout=20).read().decode("utf-8","replace")
    except Exception as e:
        print("could not fetch clcec0.h (%s); nothing verified" % e); return 0
    ref = {}
    for n in NAMES:
        m = re.search(r"#define\s+NVCEC0_%s\s+0x([0-9a-fA-F]+)\b" % n, txt)
        if m: ref[n] = int(m.group(1), 16)
    o = ours()
    fails = checked = 0
    for n in NAMES:
        if n not in o or n not in ref:
            print("  ? %s missing (ours=%s ref=%s)" % (n, n in o, n in ref)); continue
        checked += 1
        if o[n] == ref[n]:
            print("  OK NVCEC0_%-24s 0x%04x" % (n, o[n]))
        else:
            fails += 1; print("  !! NVCEC0_%-24s ours 0x%04x != NVIDIA 0x%04x" % (n, o[n], ref[n]))
    chan = open(CHAN, encoding="utf-8").read()
    if "open_engine_channel(c, rm, CH_COMPUTE" in chan:
        fails += 1; print("  !! redundant dedicated compute GR context regressed")
    required = [
        "compute object 0xcec0 allocated on the 3D channel",
        "nv_channel_t *comp = &channels[CH_GFX]",
        "nv_channel_t *ch = &channels[CH_GFX]",
    ]
    for token in required:
        if token not in chan:
            fails += 1; print("  !! shared GR/compute route missing: %s" % token)
    gfx = chan.find("open_engine_channel(c, rm, CH_GFX")
    scanout = chan.find("nv_disp_prealloc_scanout(c, rm)", gfx)
    codec = chan.find("open_engine_channel(c, rm, CH_NVDEC", gfx)
    if gfx < 0 or scanout < gfx or codec < scanout:
        fails += 1; print("  !! required allocation order is not shared-GR -> scanout -> codecs")
    else:
        print("  OK shared CE97+CEC0 GR context is allocated before scanout; codecs follow")
    print("\n%d methods checked against NVIDIA, %d wrong" % (checked, fails))
    print("ALL MATCH" if not fails and checked else ("MISMATCH" if fails else "nothing checked"))
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
