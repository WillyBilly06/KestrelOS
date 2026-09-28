#!/usr/bin/env python3
"""verify_nv_2d.py - check the NV902D (2D engine) method offsets in kernel/nv.h
against NVIDIA's own class/cl902d.h.

The 2D engine is what accelerates the desktop: nv_2d_fill / nv_2d_copy build
pushbuffer streams of these methods to fill and blit rectangles.  A method at
the wrong offset makes the engine interpret the following data as some other
piece of state - the source surface programmed where nothing reads it, say - so
the blit reads garbage and the desktop corrupts, invisible until real silicon.
The offsets are hand-transcribed, and this project's history says every
hand-transcribed table has been wrong at least once (see the memory note
"verify transcribed constants"), so diff them against the source.

    python tools/verify_nv_2d.py        (needs network; offline -> skips)

Exit 0 if every NV902D_* offset that the header also defines matches, 1 on any
mismatch.
"""
import os, re, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OURS = os.path.join(ROOT, "kernel", "nv.h")
URL = ("https://raw.githubusercontent.com/NVIDIA/open-gpu-kernel-modules/"
       "main/src/common/sdk/nvidia/inc/class/cl902d.h")


def defines(text):
    """All NV902D_* method offsets #defined in `text`, as {name: int}.

    Keeps only address-like values (a bare number, optionally parenthesised) -
    the header also defines field sub-values (shifts/masks written as
    a:b ranges or enum names) which are not method offsets and would not
    compare.  A method offset is a plain hex/decimal literal on its own."""
    out = {}
    for m in re.finditer(r"#define\s+(NV902D_[A-Z0-9_]+)\s+\(?\s*"
                         r"(0x[0-9a-fA-F]+|\d+)\s*\)?\s*$", text, re.M):
        name, val = m.group(1), m.group(2)
        out[name] = int(val, 16) if val.lower().startswith("0x") else int(val)
    return out


def main():
    ours = defines(open(OURS, encoding="utf-8").read())
    # Only the method-offset names (values below 0x10000, i.e. register
    # offsets), not the format/layout/operation ENUM values we also define.
    ours = {k: v for k, v in ours.items()
            if v < 0x10000 and not k.startswith((
                "NV902D_FORMAT", "NV902D_LAYOUT", "NV902D_OPERATION",
                "NV902D_PRIM_MODE"))}

    try:
        with urllib.request.urlopen(URL, timeout=20) as r:
            ref = defines(r.read().decode("utf-8", "replace"))
    except Exception as e:
        print("could not fetch cl902d.h (%s); nothing verified" % e)
        return 0

    checked = passed = 0
    fails = []
    for name in sorted(ours):
        if name not in ref:
            print("  ?  %-44s 0x%04x  (not in cl902d.h)" % (name, ours[name]))
            continue
        checked += 1
        if ours[name] == ref[name]:
            passed += 1
            print("  OK %-44s 0x%04x" % (name, ours[name]))
        else:
            fails.append((name, ours[name], ref[name]))
            print("  !! %-44s ours 0x%04x  header 0x%04x" %
                  (name, ours[name], ref[name]))

    print("\n%d NV902D offsets checked against cl902d.h, %d matched" % (checked, passed))
    if fails:
        print("MISMATCHES: %d" % len(fails))
        for n, o, h in fails:
            print("  %s: change 0x%04x -> 0x%04x" % (n, o, h))
        return 1
    if checked == 0:
        print("no offsets could be compared (offline?); nothing verified")
        return 0
    print("ALL MATCH")
    return 0


if __name__ == "__main__":
    sys.exit(main())
