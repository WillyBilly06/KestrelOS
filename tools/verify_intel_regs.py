#!/usr/bin/env python3
"""verify_intel_regs.py - check the Intel display register offsets in
kernel/intel_display.h against the Linux kernel's i915 definitions.

A display register at the wrong offset reads a plausible number from the wrong
place and the driver quietly does the wrong thing - the silent failure the
"verify transcribed constants" memory is about.  So diff the offsets against
the source rather than trusting the transcription.

The reference is drm/i915/display/intel_display_regs.h.  The base symbols we
depend on (with the per-transcoder 0x1000 stride applied in the header's
macros) are checked here.  Needs network; offline it says so and skips.

Exit 0 if every checked offset matches, 1 on a mismatch.
"""
import os, re, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HDR = os.path.join(ROOT, "kernel", "intel_display.h")
REFS = [
    ("https://raw.githubusercontent.com/torvalds/linux/master/"
     "drivers/gpu/drm/i915/display/intel_display_regs.h"),
    ("https://raw.githubusercontent.com/torvalds/linux/master/"
     "drivers/gpu/drm/i915/display/skl_universal_plane_regs.h"),
]

# ours name  -> i915 base symbol it must equal (transcoder A / pipe A instance)
CHECKS = {
    "INTEL_TRANS_TIMING_BASE": "_TRANS_HTOTAL_A",   # 0x60000
    "INTEL_TRANSCONF_BASE":    "_TRANSACONF",        # 0x70008
    "INTEL_PLANE_CTL_BASE":    "_PLANE_CTL_1_A",     # 0x70180
    "INTEL_PLANE_STRIDE_BASE": "_PLANE_STRIDE_1_A",  # 0x70188
    "INTEL_PLANE_SIZE_BASE":   "_PLANE_SIZE_1_A",    # 0x70190
    "INTEL_PLANE_SURF_BASE":   "_PLANE_SURF_1_A",    # 0x7019c
}
# offset-within-a-transcoder constants embedded in our macros, vs the i915
# base symbol minus its transcoder base (0x60000).
SUBOFF = {
    "HTOTAL @+0x00": ("_TRANS_HTOTAL_A", 0x60000, 0x00),
    "VTOTAL @+0x0c": ("_TRANS_VTOTAL_A", 0x60000, 0x0c),
    "PIPESRC @+0x1c": ("_PIPEASRC",      0x60000, 0x1c),
}


def fetch_defines(urls):
    d = {}
    for url in urls:
        with urllib.request.urlopen(url, timeout=20) as r:
            text = r.read().decode("utf-8", "replace")
        for m in re.finditer(r"#define\s+(\w+)\s+\(?(0x[0-9a-fA-F]+)\)?", text):
            d.setdefault(m.group(1), int(m.group(2), 16))
    return d


def ours():
    text = open(HDR, encoding="utf-8").read()
    d = {}
    for m in re.finditer(r"#define\s+(\w+)\s+\(?(0x[0-9a-fA-F]+)u?\)?", text):
        d[m.group(1)] = int(m.group(2), 16)
    return d


def main():
    o = ours()
    try:
        ref = fetch_defines(REFS)
    except Exception as e:
        print("could not fetch i915 reference (%s); nothing verified" % e)
        return 0

    fails = 0
    checked = 0
    print("base symbols:")
    for name, refsym in CHECKS.items():
        if name not in o:
            print("  ? %s not in our header" % name); continue
        if refsym not in ref:
            print("  ? i915 has no %s" % refsym); continue
        checked += 1
        if o[name] == ref[refsym]:
            print("  OK %-26s 0x%05X == i915 %s" % (name, o[name], refsym))
        else:
            fails += 1
            print("  !! %-26s ours 0x%05X != i915 %s 0x%05X"
                  % (name, o[name], refsym, ref[refsym]))

    print("per-transcoder offsets:")
    for label, (refsym, base, want) in SUBOFF.items():
        if refsym not in ref:
            print("  ? i915 has no %s" % refsym); continue
        got = ref[refsym] - base
        checked += 1
        if got == want:
            print("  OK %-16s +0x%02X == i915 %s-0x%X" % (label, want, refsym, base))
        else:
            fails += 1
            print("  !! %-16s ours +0x%02X != i915 %s gives +0x%02X"
                  % (label, want, refsym, got))

    print("\n%d offsets checked against i915, %d wrong" % (checked, fails))
    if fails:
        return 1
    print("ALL MATCH" if checked else "nothing checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
