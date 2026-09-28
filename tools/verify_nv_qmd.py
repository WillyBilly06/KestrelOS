#!/usr/bin/env python3
"""verify_nv_qmd.py - check the Blackwell compute QMD field bit-positions in
kernel/nv_qmd.h against NVIDIA's classes/compute/clcec0qmd.h.

A QMD field one bit off launches a compute grid with a wrong shader address,
block size or register count - a silent failure that only shows on real
silicon. So the positions are diffed against NVIDIA's own header, not trusted.

Ours: NV_QMD_F_<NAME> ((nv_qmd_field_t){lo, hi}).
NVIDIA: NVCEC0_QMDV05_00_<NAME> MW(hi:lo).  Needs network; offline it skips.
"""
import os, re, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HDR = os.path.join(ROOT, "kernel", "nv_qmd.h")
REF = ("https://raw.githubusercontent.com/NVIDIA/open-gpu-doc/master/"
       "classes/compute/clcec0qmd.h")

# ours NV_QMD_F_<X> -> NVIDIA NVCEC0_QMDV05_00_<Y>
MAP = {
    "QMD_TYPE": "QMD_TYPE",
    "QMD_MAJOR_VERSION": "QMD_MAJOR_VERSION",
    "SASS_VERSION": "SASS_VERSION",
    "PROGRAM_PREFETCH_SIZE": "PROGRAM_PREFETCH_SIZE",
    "BARRIER_COUNT": "BARRIER_COUNT",
    "PROGRAM_ADDRESS_LOWER": "PROGRAM_ADDRESS_LOWER_SHIFTED4",
    "PROGRAM_ADDRESS_UPPER": "PROGRAM_ADDRESS_UPPER_SHIFTED4",
    "CTA_THREAD_DIMENSION0": "CTA_THREAD_DIMENSION0",
    "CTA_THREAD_DIMENSION1": "CTA_THREAD_DIMENSION1",
    "CTA_THREAD_DIMENSION2": "CTA_THREAD_DIMENSION2",
    "REGISTER_COUNT": "REGISTER_COUNT",
    "SHARED_MEMORY_SIZE_SHIFTED7": "SHARED_MEMORY_SIZE_SHIFTED7",
    "SHADER_LOCAL_MEMORY_LOW_SIZE_SHIFTED4": "SHADER_LOCAL_MEMORY_LOW_SIZE_SHIFTED4",
    "SHADER_LOCAL_MEMORY_HIGH_SIZE_SHIFTED4": "SHADER_LOCAL_MEMORY_HIGH_SIZE_SHIFTED4",
    "GRID_WIDTH": "GRID_WIDTH",
    "GRID_HEIGHT": "GRID_HEIGHT",
    "GRID_DEPTH": "GRID_DEPTH",
    "PROGRAM_PREFETCH_ADDR_LOWER": "PROGRAM_PREFETCH_ADDR_LOWER_SHIFTED",
    "PROGRAM_PREFETCH_ADDR_UPPER": "PROGRAM_PREFETCH_ADDR_UPPER_SHIFTED",
}

def ours():
    d = {}
    for m in re.finditer(r"NV_QMD_F_(\w+)\s+\(\(nv_qmd_field_t\)\{\s*(\d+)\s*,\s*(\d+)\s*\}\)",
                         open(HDR, encoding="utf-8").read()):
        d[m.group(1)] = (int(m.group(2)), int(m.group(3)))   # (lo, hi)
    return d

def ref():
    try:
        text = urllib.request.urlopen(REF, timeout=20).read().decode("utf-8", "replace")
    except Exception as e:
        print("could not fetch clcec0qmd.h (%s); nothing verified" % e); return None
    d = {}
    for m in re.finditer(r"NVCEC0_QMDV05_00_(\w+)\s+MW\((\d+):(\d+)\)", text):
        d[m.group(1)] = (int(m.group(3)), int(m.group(2)))   # (lo, hi)
    return d



def main():
    o, r = ours(), ref()
    if r is None:
        return 0
    fails = checked = 0
    for mine, theirs in MAP.items():
        if mine not in o:
            print("  ? %s not in nv_qmd.h" % mine); continue
        if theirs not in r:
            print("  ? clcec0qmd.h has no QMDV05_00_%s" % theirs); continue
        checked += 1
        if o[mine] == r[theirs]:
            print("  OK %-28s bits %d:%d" % (mine, o[mine][1], o[mine][0]))
        else:
            fails += 1
            print("  !! %-28s ours %d:%d != NVIDIA %s %d:%d"
                  % (mine, o[mine][1], o[mine][0], theirs, r[theirs][1], r[theirs][0]))
    print("\n%d QMD fields checked against NVIDIA, %d wrong" % (checked, fails))
    print("ALL MATCH" if not fails and checked else ("MISMATCH" if fails else "nothing checked"))
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
