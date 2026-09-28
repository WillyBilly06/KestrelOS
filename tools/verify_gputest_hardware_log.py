#!/usr/bin/env python3
"""Strictly accept a completed native-NVIDIA GPU-test hardware boot.

This deliberately consumes the copied USB logs rather than source/build state.
It is the final, hardware-authoritative gate after tools/grab_log.ps1 retrieves
KESTREL/gpu-boot.log and KESTREL/KERNEL.LOG from the guarded test stick.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LOG_DIR = ROOT / "out" / "stick_logs"
if len(sys.argv) == 3:
    KERNEL = Path(sys.argv[1])
    REPORT = Path(sys.argv[2])
elif len(sys.argv) == 1:
    fresh = (ROOT / "out/stick-kernel.log", ROOT / "out/stick-gpuboot.log")
    archived = (LOG_DIR / "KESTREL_KERNEL.LOG", LOG_DIR / "KESTREL_gpu-boot.log")
    pairs = [pair for pair in (fresh, archived) if all(path.is_file() for path in pair)]
    KERNEL, REPORT = max(pairs, key=lambda pair: min(path.stat().st_mtime for path in pair)) \
        if pairs else archived
else:
    print(f"usage: {Path(sys.argv[0]).name} [KERNEL.LOG gpu-boot.log]")
    raise SystemExit(2)


def fail(message: str) -> None:
    print(f"FAIL: {message}")
    failures.append(message)


def require(pattern: str, text: str, label: str, flags: int = 0) -> re.Match[str] | None:
    match = re.search(pattern, text, flags)
    if not match:
        fail(label)
    return match


failures: list[str] = []

if not REPORT.is_file() or not KERNEL.is_file():
    print(f"FAIL: retrieve both hardware logs into {LOG_DIR} first")
    raise SystemExit(1)

report = REPORT.read_text(encoding="utf-8", errors="replace")
kernel = KERNEL.read_text(encoding="utf-8", errors="replace")

topology = require(
    r"connected / active\s+\.\.\.\.\.\s+(\d+) / (\d+)",
    report,
    "missing connected/active display result",
)
active = int(topology.group(2)) if topology else 0
if active < 3:
    fail(f"only {active} active display(s); the required three-head proof did not run")

commit = require(
    r"validate / atomic commit\s+PASS / PASS \(heads (0x[0-9a-fA-F]+)\)",
    report,
    "NVKMS validation/atomic all-head commit did not pass",
)
head_mask = int(commit.group(1), 16) if commit else 0
if head_mask.bit_count() != active:
    fail(f"active count {active} does not match committed head mask {head_mask:#x}")

display_rows = re.findall(
    r"display (0x[0-9a-fA-F]+) -> head(\d+): "
    r"(\d+)x(\d+) @ (\d+)\.(\d{3}) Hz, pixel clock (\d+) Hz",
    report,
)
if len(display_rows) != active:
    fail(f"mode report contains {len(display_rows)} display row(s), expected {active}")
else:
    row_heads = {int(row[1]) for row in display_rows}
    if len(row_heads) != active:
        fail("mode report reuses a head instead of assigning one per monitor")
    for handle, head, width, height, hz, mhz, pclk in display_rows:
        if min(int(width), int(height), int(pclk)) <= 0:
            fail(f"display {handle}/head{head} has an invalid selected mode")
        print(
            f"PASS: {handle} head{head} selected {width}x{height} "
            f"@ {hz}.{mhz} Hz (pixel clock {pclk} Hz)"
        )

require(
    r"patterns / output CRC\s+\.\.\s+PASS / all-head signal changed",
    report,
    "multi-monitor pattern/output-CRC proof did not pass",
)
require(
    r"render client / copy\s+\.\.\.\s+PASS / PASS",
    report,
    "native host-RM render client/copy proof did not pass",
)
require(
    r"engine objects\s+\.\.\.\.\.\.\.\.\.\s+4/4 opened",
    report,
    "not all four render/codec engine objects opened",
)
require(
    r"compute / offscreen 3D\s+\.\s+PASS / PASS",
    report,
    "compute/offscreen 3D hardware proof did not pass",
)

for name in ("visible 2D all heads", "visible 3D all heads"):
    match = require(
        rf"{re.escape(name)}\s+\.\.\.\s+PASS "
        rf"\(draw (0x[0-9a-fA-F]+) pixel (0x[0-9a-fA-F]+) CRC (0x[0-9a-fA-F]+)\)",
        report,
        f"{name} did not pass",
    )
    if match:
        masks = tuple(int(value, 16) for value in match.groups())
        if any(value != head_mask for value in masks):
            fail(
                f"{name} masks {masks[0]:#x}/{masks[1]:#x}/{masks[2]:#x} "
                f"do not cover committed heads {head_mask:#x}"
            )

require(
    r"NVDEC / NVENC H\.264\s+\.\.\.\.\s+PASS / PASS",
    report,
    "NVDEC/NVENC H.264 hardware proofs did not both pass",
)
require(
    r"post-codec 2D / 3D\s+\.\.\.\.\.\s+PASS / PASS",
    report,
    "post-codec visible 2D/3D did not remain usable",
)
require(
    r"hardware desktop gate\s+\.\.\s+PASS",
    report,
    "hardware desktop admission gate did not pass",
)
require(
    r"desktop accel sequence\s+\.\s+PASS",
    report,
    "post-idle production desktop acceleration sequence did not pass",
)

# Detailed kernel records prevent a summary-format accident from masquerading
# as proof and establish that the accepted bytes came from the hardware paths.
kernel_requirements = {
    r"GPFIFO wrap\+idle PASS: 2053 synchronously fenced kickoffs crossed two complete revolutions and resumed after 8 seconds idle":
        "copy-channel GPFIFO did not execute across its 1023-to-0 wrap",
    r"NVENC ENCODED a valid Annex-B H\.264 stream on the real card":
        "no real-card Annex-B NVENC success record",
    r"NVDEC DECODED the I-frame on the real card":
        "no real-card NVDEC decoded-pixel success record",
    r"COMPUTE runs on the real card":
        "no real-card compute execution success record",
    r"visible all-head 2D\+3D proof PASS on heads":
        "no detailed all-head visible acceleration success record",
    r"hardware desktop observation window started; reboot in 60 seconds":
        "the validated hardware desktop did not enter its 60-second observation window",
    r"primary display is AOC .*selected by EDID policy":
        "the AOC monitor was not selected as the primary desktop by EDID identity",
    r"desktop-sequence gate PASS after 4\.5 s idle":
        "the exact post-idle GR/CAB5 desktop sequence did not pass",
    r"the card's 2D/present path returned the exact live VRAM pixel 0xff19d3ff":
        "desktop process did not verify its live CAB5 pixel",
    r"desktop damage is reaching every native NVIDIA scanout through CAB5":
        "desktop compositor never completed a native CAB5 damage present",
}
for pattern, label in kernel_requirements.items():
    require(pattern, kernel, label)

if len(re.findall(r"visible all-head 2D\+3D proof PASS on heads", kernel)) < 2:
    fail("all-head visible acceleration did not pass both before and after codecs")

desktop_at = kernel.find("hardware desktop observation window started")
if desktop_at >= 0:
    desktop_log = kernel[desktop_at:]
    for bad, label in (
        ("the engine's semaphore did not fire", "a GPU engine stopped retiring during desktop observation"),
        ("the processor is drawing every pixel of this desktop", "desktop fell back to CPU rendering"),
        ("adapter moved nothing", "desktop copy acceleration failed and fell back"),
    ):
        if bad in desktop_log:
            fail(label)

if failures:
    print(f"\n{len(failures)} hardware requirement(s) FAILED")
    raise SystemExit(1)

print(
    f"\nHARDWARE PASS: {active} displays / heads {head_mask:#x}; maximum-mode "
    "NVKMS commit, all-head patterns, copy/compute/2D/3D, NVENC/NVDEC, "
    "post-codec rendering, and hardware desktop observation all verified"
)
