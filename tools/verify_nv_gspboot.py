#!/usr/bin/env python3
"""verify_nv_gspboot.py - check the GSP-boot Falcon/RISC-V register offsets and
bit positions in kernel/nv.h + nv_gsp.c against NVIDIA's dev_*.h headers.

These are the registers the Blackwell GSP boot reads/writes: the Falcon HWCFG2
(whose RISCV_BR_PRIV_LOCKDOWN bit the boot polls to clear), the mailboxes, the
WPR2 window (the card's record of accepting an image), the RISC-V CPUCTL.  A
wrong offset or bit here makes the boot watch the wrong thing and hang - the
silent failure the "verify transcribed constants" memory is about.  It already
caught one: HWCFG2 lockdown was bit 11 (RISCV_PL3_DISABLE) where NVIDIA puts
RISCV_BR_PRIV_LOCKDOWN at 13.

    python tools/verify_nv_gspboot.py   (needs network; offline -> skips)
"""
import os, re, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NVH = open(os.path.join(ROOT, "kernel", "nv.h"), encoding="utf-8").read()
GSPC = open(os.path.join(ROOT, "kernel", "nv_gsp.c"), encoding="utf-8").read()
B = "https://raw.githubusercontent.com/NVIDIA/open-gpu-kernel-modules/main/src/common/inc/swref/published/"
HDRS = {
    "falcon": B + "hopper/gh100/dev_falcon_v4.h",
    "fb":     B + "hopper/gh100/dev_fb.h",
    "riscv":  B + "blackwell/gb202/dev_riscv_pri.h",
}

# our symbol (searched in nv.h then nv_gsp.c) -> (header key, header symbol)
OFFSETS = [
    ("NV_PFALCON_FALCON_HWCFG2", "falcon", "NV_PFALCON_FALCON_HWCFG2"),
    ("NV_PFALCON_MAILBOX0",      "falcon", "NV_PFALCON_FALCON_MAILBOX0"),
    ("NV_PFALCON_MAILBOX1",      "falcon", "NV_PFALCON_FALCON_MAILBOX1"),
    ("NV_PRISCV_CPUCTL",         "riscv",  "NV_PRISCV_RISCV_CPUCTL"),
    ("NV_PFB_PRI_MMU_WPR2_ADDR_LO", "fb",  "NV_PFB_PRI_MMU_WPR2_ADDR_LO"),
    ("NV_PFB_PRI_MMU_WPR2_ADDR_HI", "fb",  "NV_PFB_PRI_MMU_WPR2_ADDR_HI"),
]
# our bit-shift macro -> (header key, header field with a hi:lo range)
BITS = [
    ("NV_PFALCON_HWCFG2_LOCKDOWN", "falcon",
     "NV_PFALCON_FALCON_HWCFG2_RISCV_BR_PRIV_LOCKDOWN"),
]


def our_hex(sym):
    m = re.search(r"#define\s+" + re.escape(sym) + r"\s+\(?\s*(0x[0-9a-fA-F]+|\d+)",
                  NVH + "\n" + GSPC)
    return int(m.group(1), 0) if m else None


def our_shift(sym):
    m = re.search(r"#define\s+" + re.escape(sym) + r"\s+\(1u?\s*<<\s*(\d+)\)", NVH)
    return int(m.group(1)) if m else None


def hdr_hex(text, sym):
    m = re.search(r"#define\s+" + re.escape(sym) + r"\s+\(?\s*(0x[0-9a-fA-F]+|\d+)\)?",
                  text)
    return int(m.group(1), 0) if m else None


def hdr_bit(text, sym):
    m = re.search(r"#define\s+" + re.escape(sym) + r"\s+(\d+):(\d+)", text)
    return int(m.group(1)) if m else None   # single-bit field: hi == lo


def main():
    hdr = {}
    for k, u in HDRS.items():
        try:
            hdr[k] = urllib.request.urlopen(
                urllib.request.Request(u, headers={"User-Agent": "curl/8"}),
                timeout=25).read().decode("utf-8", "replace")
        except Exception as e:
            print("could not fetch %s (%s); nothing verified" % (u, e)); return 0

    checked = passed = 0; fails = []
    for ours, key, nv in OFFSETS:
        ov, hv = our_hex(ours), hdr_hex(hdr[key], nv)
        if ov is None or hv is None:
            print("  ?  %-32s (ours=%s header=%s)" % (ours, ov, hv)); continue
        checked += 1
        if ov == hv:
            passed += 1; print("  OK %-32s 0x%x == %s" % (ours, ov, nv))
        else:
            fails.append((ours, ov, nv, hv))
            print("  !! %-32s ours 0x%x  %s 0x%x" % (ours, ov, nv, hv))
    for ours, key, nv in BITS:
        os_, hb = our_shift(ours), hdr_bit(hdr[key], nv)
        if os_ is None or hb is None:
            print("  ?  %-32s bit (ours=%s header=%s)" % (ours, os_, hb)); continue
        checked += 1
        if os_ == hb:
            passed += 1; print("  OK %-32s bit %d == %s" % (ours, os_, nv))
        else:
            fails.append((ours, os_, nv, hb))
            print("  !! %-32s ours bit %d  %s bit %d" % (ours, os_, nv, hb))

    print("\n%d GSP-boot constants checked against NVIDIA dev headers, %d matched"
          % (checked, passed))
    if fails:
        print("MISMATCHES: %d" % len(fails))
        for n, o, nm, h in fails: print("  %s: ours %s vs %s %s" % (n, o, nm, h))
        return 1
    print("ALL MATCH")
    return 0


if __name__ == "__main__":
    sys.exit(main())
