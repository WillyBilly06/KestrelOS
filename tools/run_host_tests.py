#!/usr/bin/env python3
"""run_host_tests.py - build and run the off-target capability host tests.

Each tools/*_host_test.c compiles a piece of the OS with a *_HOST_TEST define so
the hardware-touching half is left out, and checks the arithmetic half (timing,
format packing, layout geometry, EDID parsing) against an INDEPENDENT
computation.  This is how a transcription slip in a register/format table is
caught on a build machine instead of silently on a screen nobody can attach.

Unlike verify_all.py (which diffs constants against vendor headers and needs
network), these need only a C compiler and run fully offline.  They prove the
computational core of the display/audio capabilities:

    audio sample+bit rate (#3), refresh options (#4), resolution/EDID (#4/#5),
    Intel iGPU modeset - scanout mapping + plane flip + timing (#5/#7),
    multi-display extend/mirror/only-other geometry (#6).

What they do NOT prove is the live MMIO/DPLL write on real silicon - that needs
the hardware.  Green here means "the numbers the driver would program are the
right numbers"; it does not mean the panel lit.

Usage:  python tools/run_host_tests.py
"""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LLVM = os.environ.get("LLVM_DIR", r"C:\Program Files\LLVM\bin")
CLANG = os.path.join(LLVM, "clang.exe") if os.name == "nt" else "clang"
OUT = os.path.join(ROOT, "out", "hosttests")

# name -> (define, extra include dirs, source files after the test .c)
TESTS = {
    "intel_modeset": ("INTEL_HOST_TEST", ["kernel"], []),                # test includes intel_display.c itself
    "hda_format":    ("HDA_FORMAT_HOST_TEST", ["kernel"], ["kernel/hda_format.c"]),
    "edid":          ("EDID_HOST_TEST", ["kernel"], ["kernel/edid.c", "kernel/edid_cea.c"]),
    "dsc":           ("", ["kernel"], ["kernel/nv_dsc_pps.c"]),
    "display_layout":("", ["kernel", "include"], ["kernel/display_layout.c"]),
    "refresh_modes": ("", ["user/libgui", "kernel"], ["user/libgui/refresh_modes.c"]),
    # #8 (run other software, ARM half): the from-scratch AArch64 interpreter
    # runs real optimiser-emitted ARM code and is checked against a native
    # compiler.  Its test .c #includes arm64.c + arm64_test.c directly, so no
    # extra sources; a host shim supplies kestrel.h (tools/armhost first on -I).
    "arm64":         ("", ["tools/armhost", "user/libarm"], []),
}

# These compile production lookup/dispatch code and use host-memory call spies.
# They do not execute GPU shaders, codec workloads, or hardware models.
PYTHON_TESTS = ("gpu_variant_safety", "gpu_accel_routing", "gpu_triangle_syscall",
                "gpu_program_syscall", "nv_present_copy", "nv_raster_prepare")


def main():
    os.makedirs(OUT, exist_ok=True)
    passed, failed = [], []
    for name, (define, incs, srcs) in TESTS.items():
        exe = os.path.join(OUT, name + (".exe" if os.name == "nt" else ""))
        cmd = [CLANG, "-std=c11", "-Wno-unused-function"]
        if define:
            cmd.append("-D" + define)
        for i in incs:
            cmd += ["-I", os.path.join(ROOT, i)]
        cmd.append(os.path.join(ROOT, "tools", name + "_host_test.c"))
        cmd += [os.path.join(ROOT, s) for s in srcs]
        cmd += ["-o", exe]
        b = subprocess.run(cmd, capture_output=True, text=True)
        if b.returncode != 0:
            print("[BUILD FAIL] %s\n%s" % (name, (b.stderr or b.stdout)[:400]))
            failed.append(name)
            continue
        r = subprocess.run([exe], capture_output=True, text=True)
        out = (r.stdout or "") + (r.stderr or "")
        # Each test prints its own verdict; treat a nonzero exit or the word
        # FAIL as failure, and require an explicit all-good line otherwise.
        good = (r.returncode == 0 and "FAIL" not in out
                and ("ALL GOOD" in out or "ALL PASS" in out or "0 failures" in out))
        verdict = [l for l in out.splitlines() if l.strip()][-1] if out.strip() else "(no output)"
        print("[%s] %-16s %s" % ("PASS" if good else "FAIL", name, verdict.strip()))
        (passed if good else failed).append(name)

    for name in PYTHON_TESTS:
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "test_" + name + ".py")],
                           capture_output=True, text=True)
        out = (r.stdout or "") + (r.stderr or "")
        good = r.returncode == 0 and any(line.startswith("PASS") for line in out.splitlines())
        print("[%s] %-16s %s" % ("PASS" if good else "FAIL", name, out.strip()))
        (passed if good else failed).append(name)

    print("\n%d passed, %d failed" % (len(passed), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
