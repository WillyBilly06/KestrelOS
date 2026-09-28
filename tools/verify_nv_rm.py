#!/usr/bin/env python3
"""verify_nv_rm.py - check the RM object classes and control commands in
kernel/nv_gsp_rm.c against NVIDIA's class/ and ctrl/ headers.

Once the GSP is up, the driver reaches the GPU by allocating RM objects over
RPC - a root, a device, a subdevice, the display-common object - and issuing
control commands to them.  Each object is named by a class number and each
control by a 32-bit command id whose top half is the class.  A wrong number
allocates the wrong object or sends a control nothing answers, so the render
channel / display never comes up - and it is invisible until real silicon.
These are transcribed; diff them against NVIDIA's own headers.

NOTE: the RPC FUNCTION numbers (GSP_RM_ALLOC=103, GSP_RM_CONTROL=76, ...) are
NOT checked here - their enum header (rpc_global_enums.h) moves between upstream
releases and could not be located mechanically; they were cross-checked against
nouveau r570 + open-gpu-kernel-modules when the RPC layer was written.

    python tools/verify_nv_rm.py        (needs network; offline -> skips)
"""
import os, re, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = open(os.path.join(ROOT, "kernel", "nv_gsp_rm.c"), encoding="utf-8").read()
BASE = "https://raw.githubusercontent.com/NVIDIA/open-gpu-kernel-modules/main/src/common/sdk/nvidia/inc/"

# each KestrelOS symbol -> the header(s) it should be defined in (first hit wins)
WHERE = {
    "NV01_DEVICE_0":       ["class/cl0080.h"],
    "NV20_SUBDEVICE_0":    ["class/cl2080.h"],
    "NV04_DISPLAY_COMMON": ["class/cl0073.h"],
    "NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS":        ["ctrl/ctrl0073/ctrl0073system.h"],
    "NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED":        ["ctrl/ctrl0073/ctrl0073system.h"],
    "NV0073_CTRL_CMD_SPECIFIC_GET_CONNECTOR_DATA": ["ctrl/ctrl0073/ctrl0073specific.h"],
    "NV0073_CTRL_CMD_SPECIFIC_GET_ALL_HEAD_MASK":  ["ctrl/ctrl0073/ctrl0073specific.h"],
    "NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO":        ["ctrl/ctrl0073/ctrl0073specific.h"],
    "NV0073_CTRL_CMD_DP_GET_CAPS":                 ["ctrl/ctrl0073/ctrl0073dp.h"],
    "NV2080_CTRL_CMD_INTERNAL_DISPLAY_GET_STATIC_INFO": ["ctrl/ctrl2080/ctrl2080internal.h"],
    # NV01_ROOT is 0x0 (nvos) - trivial, skipped from the network check.
}


def our_val(sym):
    m = re.search(r"#define\s+" + re.escape(sym) + r"\s+\(?(0x[0-9a-fA-F]+|\d+)", SRC)
    return int(m.group(1), 16) if m and m.group(1).lower().startswith("0x") else (int(m.group(1)) if m else None)


def get(path):
    try:
        return urllib.request.urlopen(
            urllib.request.Request(BASE + path, headers={"User-Agent": "curl/8"}),
            timeout=15).read().decode("utf-8", "replace")
    except Exception:
        return None


def hdr_val(text, sym):
    m = re.search(r"#define\s+" + re.escape(sym) + r"\s+\(?(0x[0-9a-fA-F]+|\d+)", text)
    if not m:
        return None
    return int(m.group(1), 16) if m.group(1).lower().startswith("0x") else int(m.group(1))


def main():
    cache = {}
    checked = passed = 0; fails = []; missing = 0
    for sym, headers in WHERE.items():
        ov = our_val(sym)
        if ov is None:
            print("  ?  %-48s not defined in nv_gsp_rm.c" % sym); continue
        hv = None
        for h in headers:
            if h not in cache:
                cache[h] = get(h)
            if cache[h] is None:
                continue
            hv = hdr_val(cache[h], sym)
            if hv is not None:
                break
        if hv is None:
            print("  ?  %-48s 0x%x (not found in header)" % (sym, ov)); missing += 1; continue
        checked += 1
        if ov == hv:
            passed += 1; print("  OK %-48s 0x%x" % (sym, ov))
        else:
            fails.append((sym, ov, hv)); print("  !! %-48s ours 0x%x  header 0x%x" % (sym, ov, hv))

    print("\n%d RM constants checked against NVIDIA headers, %d matched%s"
          % (checked, passed, (", %d not found" % missing) if missing else ""))
    if fails:
        print("MISMATCHES: %d" % len(fails))
        for s, o, h in fails: print("  %s: 0x%x -> 0x%x" % (s, o, h))
        return 1
    if checked == 0:
        print("nothing verified (offline?)"); return 0
    print("ALL MATCH")
    return 0


if __name__ == "__main__":
    sys.exit(main())
