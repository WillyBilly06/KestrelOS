#!/usr/bin/env python3
"""verify_nv_fsp.py - check the FSP (security-processor) register offsets in
kernel/nv.h against NVIDIA's dev_fsp_pri.h.

On Blackwell the GSP is booted through the FSP: the driver writes a chain-of-
trust message into the FSP's embedded-memory channel (EMEMC/EMEMD) and rings a
queue (QUEUE_HEAD/TAIL, MSGQ_HEAD/TAIL).  A wrong offset writes the boot
message into the wrong registers and the security processor never sees it - the
boot silently never starts.  It already caught one: EMEMC/EMEMD were at
BASE+0xAC0 where NVIDIA puts them at BASE+0x2AC0 (0x008F2AC0), 0x2000 low.

    python tools/verify_nv_fsp.py       (needs network; offline -> skips)
"""
import os, re, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NVH = open(os.path.join(ROOT, "kernel", "nv.h"), encoding="utf-8").read()
URL = ("https://raw.githubusercontent.com/NVIDIA/open-gpu-kernel-modules/main/"
       "src/common/inc/swref/published/hopper/gh100/dev_fsp_pri.h")

# our macro -> header symbol (both are per-index register bases).
REGS = ["QUEUE_HEAD", "QUEUE_TAIL", "MSGQ_HEAD", "MSGQ_TAIL", "EMEMC", "EMEMD"]


def our_base(sym):
    """Evaluate NV_PFSP_<sym>'s base address: substitute NV_PFSP_BASE and drop
    the per-index term, leaving the constant base."""
    base = int(re.search(r"#define\s+NV_PFSP_BASE\s+\(?(0x[0-9a-fA-F]+)", NVH).group(1), 0)
    m = re.search(r"#define\s+NV_PFSP_" + sym + r"\s*\([^)]*\)\s*(.+)", NVH)
    if not m:
        return None
    expr = m.group(1)
    expr = expr.replace("NV_PFSP_BASE", str(base))
    # strip the per-index term: + (i)*8 / + (port)*8
    expr = re.sub(r"\+\s*\([A-Za-z_]+\)\s*\*\s*8u?", "", expr)
    expr = expr.replace("u", "").strip().rstrip(";").strip()
    # keep only the parenthesised value if present
    mm = re.search(r"\(([^()]*)\)", expr)
    if mm:
        expr = mm.group(1)
    try:
        return eval(expr, {"__builtins__": {}}, {})
    except Exception:
        return None


def hdr_base(text, sym):
    m = re.search(r"#define\s+NV_PFSP_" + sym + r"\s*\([^)]*\)\s*\(?\s*(0x[0-9a-fA-F]+)", text)
    return int(m.group(1), 0) if m else None


def hdr_bit(text, sym):
    m = re.search(r"#define\s+NV_PFSP_EMEMC_" + sym + r"\s+(\d+):(\d+)", text)
    return int(m.group(1)) if m else None


def our_shift(sym):
    m = re.search(r"#define\s+NV_PFSP_EMEMC_" + sym + r"\s+\(1u?\s*<<\s*(\d+)\)", NVH)
    return int(m.group(1)) if m else None


def main():
    try:
        text = urllib.request.urlopen(
            urllib.request.Request(URL, headers={"User-Agent": "curl/8"}),
            timeout=25).read().decode("utf-8", "replace")
    except Exception as e:
        print("could not fetch dev_fsp_pri.h (%s); nothing verified" % e); return 0

    checked = passed = 0; fails = []
    for sym in REGS:
        ov, hv = our_base(sym), hdr_base(text, sym)
        if ov is None or hv is None:
            print("  ?  NV_PFSP_%-12s (ours=%s header=%s)" % (sym, hex(ov) if ov else None, hex(hv) if hv else None))
            continue
        checked += 1
        if ov == hv:
            passed += 1; print("  OK NV_PFSP_%-12s 0x%08x" % (sym, ov))
        else:
            fails.append((sym, ov, hv)); print("  !! NV_PFSP_%-12s ours 0x%08x  header 0x%08x" % (sym, ov, hv))
    # EMEMC auto-increment bits: our WRITE=AINCW, READ=AINCR
    for ours_bit, nv in (("WRITE", "AINCW"), ("READ", "AINCR")):
        os_, hb = our_shift(ours_bit), hdr_bit(text, nv)
        if os_ is None or hb is None:
            print("  ?  EMEMC_%s bit (ours=%s header=%s)" % (ours_bit, os_, hb)); continue
        checked += 1
        if os_ == hb:
            passed += 1; print("  OK EMEMC_%-6s bit %d == AINC%s" % (ours_bit, os_, ours_bit[0]))
        else:
            fails.append(("EMEMC_" + ours_bit, os_, hb)); print("  !! EMEMC_%s ours bit %d  %s bit %d" % (ours_bit, os_, nv, hb))

    print("\n%d FSP constants checked against dev_fsp_pri.h, %d matched" % (checked, passed))
    if fails:
        print("MISMATCHES: %d" % len(fails))
        for f in fails: print("  %s: ours %s vs header %s" % (f[0], hex(f[1]) if f[1] > 100 else f[1], hex(f[2]) if f[2] > 100 else f[2]))
        return 1
    print("ALL MATCH")
    return 0


if __name__ == "__main__":
    sys.exit(main())
