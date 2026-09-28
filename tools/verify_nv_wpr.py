#!/usr/bin/env python3
"""verify_nv_wpr.py - check the GspFwWprMeta layout in kernel/nv_gsp_boot.c
against NVIDIA's authoritative gsp_fw_wpr_meta.h.

GspFwWprMeta is the block the GSP firmware reads at boot to find the radix3 ELF,
the bootloader, the signature and the heap.  A wrong field offset points the
co-processor at the wrong memory and it never comes up - the silent hardware
failure the "verify transcribed constants" memory is about, and the riskiest
struct in the whole GSP boot (two unions, mixed field widths).  This computes
every field's byte offset from the real header - handling the two unions as the
max of their branches - and checks the offsets nv_gsp_boot.c static-asserts,
plus the total size.

    python tools/verify_nv_wpr.py       (needs network; offline -> skips)
"""
import os, re, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "kernel", "nv_gsp_boot.c")
URL = ("https://raw.githubusercontent.com/NVIDIA/open-gpu-kernel-modules/main/"
       "src/nvidia/arch/nvalloc/common/inc/gsp/gsp_fw_wpr_meta.h")
SZ = {"NvU64": 8, "NvU32": 4, "NvU16": 2, "NvU8": 1, "NvBool": 1}


def parse_fields(s):
    """(align, size, name) for each member, recursing into union/struct."""
    out = []; i = 0; n = len(s)
    while i < n:
        mm = re.match(r"\s*(union|struct)\s*\{", s[i:])
        if mm:
            depth = 0; j = i + mm.end() - 1
            while j < n:
                if s[j] == '{': depth += 1
                elif s[j] == '}':
                    depth -= 1
                    if depth == 0: break
                j += 1
            inner = s[i + mm.end():j]
            k = s.find(';', j)
            sub = parse_fields(inner)
            if mm.group(1) == "struct":
                off = 0; al = 1
                for a, z, nm in sub:
                    if off % a: off += a - (off % a)
                    off += z; al = max(al, a)
                if al and off % al: off += al - (off % al)
                out.append((al, off, "<struct>"))
            else:
                al = max((a for a, z, nm in sub), default=1)
                mx = max((z for a, z, nm in sub), default=0)
                out.append((al, mx, "<union>"))
            i = k + 1; continue
        fm = re.match(r"\s*(NvU64|NvU32|NvU16|NvU8|NvBool)\s+([A-Za-z0-9_]+)"
                      r"\s*(\[\s*(\d+)\s*\])?\s*;", s[i:])
        if fm:
            z = SZ[fm.group(1)] * (int(fm.group(4)) if fm.group(4) else 1)
            out.append((SZ[fm.group(1)], z, fm.group(2)))
            i += fm.end(); continue
        nl = s.find(';', i)
        if nl < 0: break
        i = nl + 1
    return out


def real_offsets(text):
    body = re.search(r"typedef struct\s*\{(.*?)\}\s*GspFwWprMeta", text, re.S).group(1)
    body = re.sub(r"//[^\n]*", "", body)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    offs = {}; off = 0; al = 1
    for a, z, nm in parse_fields(body):
        if off % a: off += a - (off % a)
        if not nm.startswith("<"): offs[nm] = off
        al = max(al, a); off += z
    if off % al: off += al - (off % al)
    return offs, off


def main():
    src = open(SRC, encoding="utf-8").read()
    # what nv_gsp_boot.c asserts: offsetof(GspFwWprMeta, NAME) == N, and sizeof==N
    asserts = dict((m.group(1), int(m.group(2))) for m in re.finditer(
        r"offsetof\(GspFwWprMeta,\s*([A-Za-z0-9_]+)\)\s*==\s*(\d+)", src))
    szm = re.search(r"sizeof\(GspFwWprMeta\)\s*==\s*(\d+)", src)
    want_size = int(szm.group(1)) if szm else None

    try:
        text = urllib.request.urlopen(
            urllib.request.Request(URL, headers={"User-Agent": "curl/8"}),
            timeout=25).read().decode("utf-8", "replace")
    except Exception as e:
        print("could not fetch gsp_fw_wpr_meta.h (%s); nothing verified" % e)
        return 0

    offs, size = real_offsets(text)
    checked = passed = 0
    fails = []
    for name, ours in sorted(asserts.items(), key=lambda kv: kv[1]):
        if name not in offs:
            print("  ?  %-28s (not found in header)" % name); continue
        checked += 1
        if offs[name] == ours:
            passed += 1; print("  OK %-28s @%d" % (name, ours))
        else:
            fails.append((name, ours, offs[name]))
            print("  !! %-28s ours @%d  header @%d" % (name, ours, offs[name]))
    # size
    checked += 1
    if want_size == size:
        passed += 1; print("  OK sizeof == %d" % size)
    else:
        fails.append(("sizeof", want_size, size))
        print("  !! sizeof ours %s  header %d" % (want_size, size))

    print("\n%d WprMeta offsets checked against NVIDIA's header, %d matched" % (checked, passed))
    if fails:
        print("MISMATCHES: %d" % len(fails))
        for n, o, h in fails: print("  %s: ours %s vs header %s" % (n, o, h))
        return 1
    print("ALL MATCH")
    return 0


if __name__ == "__main__":
    sys.exit(main())
