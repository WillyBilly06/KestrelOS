#!/usr/bin/env python3
"""verify_all.py - run every constant verifier and summarise.

Each verify_*.py diffs a block of hand-transcribed hardware constants against
the vendor's own published headers (NVIDIA open-gpu-kernel-modules, Intel i915,
Realtek rtw89).  Run them all in one command before shipping or after any edit
that touches a register/struct/method table.  This project's history is that
EVERY such table has been wrong at least once (see the "verify transcribed
constants" memory) - three GPU-boot bugs this session alone (2D source-surface
offsets, the HWCFG2 lockdown bit, the FSP EMEM channel), each silent and each
boot-blocking, each caught only by diffing against the vendor header rather
than a self-consistent model.

Each child script needs network (it fetches the reference header); with none it
prints "nothing verified" and exits 0, so this stays green offline rather than
failing spuriously.  A real MISMATCH exits non-zero.

Usage:  python tools/verify_all.py            (all)
        python tools/verify_all.py nv          (only names containing 'nv')
"""
import os, sys, subprocess, glob

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    filt = sys.argv[1] if len(sys.argv) > 1 else ""
    # verify_rtw89.py has a different contract: it takes a directory of
    # already-downloaded reference headers as an argument (Wi-Fi, not one of
    # the GPU tables), so it is not part of this auto-fetch sweep.  Run it
    # directly with its ref dir when touching rtw89.h.
    SKIP = {"verify_all.py", "verify_rtw89.py"}
    scripts = sorted(glob.glob(os.path.join(HERE, "verify_*.py")))
    scripts = [s for s in scripts if os.path.basename(s) not in SKIP
               and (not filt or filt in os.path.basename(s))]
    if not scripts:
        print("no verify_*.py match %r" % filt); return 1

    fails, offline, ok = [], [], []
    for s in scripts:
        name = os.path.basename(s)
        r = subprocess.run([sys.executable, s], capture_output=True, text=True)
        out = (r.stdout or "") + (r.stderr or "")
        last = ""
        for line in out.splitlines():
            if line.strip():
                last = line.strip()
        # classify by the child's own verdict, not just exit code
        if "MISMATCH" in out or r.returncode != 0:
            fails.append((name, last)); tag = "FAIL"
        elif "nothing verified" in out or "offline" in out.lower() or "could not fetch" in out:
            offline.append(name); tag = "skip"
        else:
            ok.append(name); tag = " ok "
        # pull the "N ... checked, M matched" line if present
        summ = next((l.strip() for l in out.splitlines()
                     if "checked" in l and "match" in l.lower()), last)
        print("  [%s] %-24s %s" % (tag, name, summ))

    print("\n%d verified, %d skipped (offline), %d FAILED"
          % (len(ok), len(offline), len(fails)))
    for name, last in fails:
        print("  FAILED: %s -> %s" % (name, last))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
