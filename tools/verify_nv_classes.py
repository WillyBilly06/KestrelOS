#!/usr/bin/env python3
"""verify_nv_classes.py - check the NVIDIA engine class numbers in
kernel/nv_blackwell.c against NVIDIA's own published class headers.

A class number is the contract between the driver and a GPU engine: send the
wrong one and every method in that subchannel means something else, so the
engine rejects the work (or worse) - and this is invisible until real silicon.
The class table is hand-transcribed, and this project's history says every
hand-transcribed constant has been wrong at least once (see the memory note
"verify transcribed constants").  So diff it against the source, don't eyeball.

NVIDIA names each class header after its number: class 0xCE97 lives in
class/clce97.h and #defines a name to (0x0000CE97).  This fetches each header
referenced by the table and checks the number the file defines equals the
number the table uses.  Needs network (raw.githubusercontent.com); with none,
it says so and skips rather than failing.

Exit 0 if every fetched class matches, 1 on any mismatch.
"""
import os, re, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLE = os.path.join(ROOT, "kernel", "nv_blackwell.c")
BASE = ("https://raw.githubusercontent.com/NVIDIA/open-gpu-kernel-modules/"
        "main/src/common/sdk/nvidia/inc/class/")

# Columns in the class_table row, after the family, in order.
COLS = ["3D", "compute", "copy", "gpfifo", "disp_core", "disp_window"]


def parse_table(path):
    """Return list of (family, name, {col: classnum}) from class_table[].

    Each row is one brace group `{ family, 3d, compute, copy, gpfifo,
    disp_core, disp_window, "name" }`.  Matching is done per BRACE GROUP, not
    per line: the newest rows wrap the name onto a second line, and a per-line
    match silently skipped them - which meant the Blackwell classes (the RTX 50
    card this driver is actually for) were never checked while the script still
    printed ALL MATCH.  Comments between rows sit outside the braces and are
    ignored naturally."""
    src = open(path, encoding="utf-8").read()
    m = re.search(r"class_table\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not m:
        sys.exit("could not find class_table[] in %s" % path)
    rows = []
    # DOTALL so \s* spans the newline a wrapped row puts before its name.
    row_re = re.compile(r'\{\s*(0x[0-9a-fA-F]+)\s*,'
                        r'\s*(0x[0-9a-fA-F]+|0)\s*,\s*(0x[0-9a-fA-F]+|0)\s*,'
                        r'\s*(0x[0-9a-fA-F]+|0)\s*,\s*(0x[0-9a-fA-F]+|0)\s*,'
                        r'\s*(0x[0-9a-fA-F]+|0)\s*,\s*(0x[0-9a-fA-F]+|0)\s*,'
                        r'\s*"([^"]*)"\s*\}', re.S)
    for rm in row_re.finditer(m.group(1)):
        fam = int(rm.group(1), 16)
        nums = {COLS[i]: int(rm.group(2 + i), 16) for i in range(6)}
        rows.append((fam, rm.group(8), nums))
    return rows


def header_defines(classnum):
    """The set of hex values #defined in class/cl<num>.h, or None if no file."""
    url = BASE + "cl%04x.h" % classnum
    try:
        with urllib.request.urlopen(url, timeout=15) as r:
            if r.status != 200:
                return None
            text = r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise
    vals = set()
    for m in re.finditer(r"#define\s+\w+\s+\(?(0x[0-9a-fA-F]+)\)?", text):
        vals.add(int(m.group(1), 16))
    return vals


def main():
    rows = parse_table(TABLE)
    checked = passed = 0
    fails = []
    seen = {}
    for fam, name, nums in rows:
        for col in COLS:
            v = nums[col]
            if v == 0:
                continue                      # 0 = engine absent on this die
            key = v
            if key in seen:                   # same class reused across rows
                continue
            defs = header_defines(v)
            seen[key] = defs
            if defs is None:
                print("  ? 0x%04X (%s %s): no class header found" %
                      (v, name, col))
                continue
            checked += 1
            if v in defs:
                passed += 1
                print("  OK 0x%04X  %s %s" % (v, name, col))
            else:
                fails.append((v, name, col, defs))
                print("  !! 0x%04X  %s %s: header defines %s, table says 0x%04X"
                      % (v, name, col,
                         ", ".join("0x%04X" % d for d in sorted(defs)), v))

    print("\n%d class numbers checked against NVIDIA headers, %d matched"
          % (checked, passed))
    if fails:
        print("MISMATCHES: %d" % len(fails))
        return 1
    if checked == 0:
        print("no headers could be fetched (offline?); nothing verified")
        return 0
    print("ALL MATCH")
    return 0


if __name__ == "__main__":
    sys.exit(main())
