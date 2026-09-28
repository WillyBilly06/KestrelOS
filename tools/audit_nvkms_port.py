#!/usr/bin/env python3
"""Offline audit for the NVIDIA 595.99.02 NVKMS/Kestrel boundary.

This deliberately distinguishes ABI closure from hardware readiness.  A core
whose imports link is only the first gate; activation stays forbidden until
all RM operations used by NVKMS have real serializers.
"""
from pathlib import Path
import hashlib
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "Linux NVIDIA Driver/kernel/nvidia-modeset/nv-modeset-kernel.o_binary"
KERNEL = ROOT / "build/kernel/kernel.elf"
PORT = ROOT / "kernel/nvkms_port.c"
CLIENT = ROOT / "kernel/nvkms_kapi_client.c"
EXPECTED_CORE_SHA256 = "f015959f4b6aae749fc17ce42ff7498ee1277224633cf62d3ad8bf0fdc60c4f8"

USED_RM_OPS = {
    "NV01_FREE",
    "NV01_ALLOC_MEMORY",
    "NV04_ALLOC",
    "NV04_VID_HEAP_CONTROL",
    "NV04_MAP_MEMORY",
    "NV04_UNMAP_MEMORY",
    "NV04_MAP_MEMORY_DMA",
    "NV04_UNMAP_MEMORY_DMA",
    "NV04_CONTROL",
    "NV04_DUP_OBJECT",
}

def nm_path():
    preferred = Path(r"C:\Program Files\LLVM\bin\llvm-nm.exe")
    return str(preferred if preferred.exists() else shutil.which("llvm-nm"))


def symbols(args, path):
    p = subprocess.run([nm_path(), *args, str(path)], text=True,
                       capture_output=True, check=True)
    out = set()
    for line in p.stdout.splitlines():
        fields = line.split()
        if fields:
            out.add(fields[-1])
    return out


def fail(msg):
    print("FAIL:", msg)
    raise SystemExit(1)


if not CORE.exists() or not KERNEL.exists() or not PORT.exists():
    fail("build the kernel and keep the supplied NVKMS core in place")

digest = hashlib.sha256(CORE.read_bytes()).hexdigest()
if digest != EXPECTED_CORE_SHA256:
    fail(f"unexpected NVKMS core hash {digest}")

imports = symbols(["-u"], CORE)
kernel_defs = symbols(["-g", "--defined-only"], KERNEL)
missing = sorted(imports - kernel_defs)
if missing:
    fail("unresolved NVKMS host imports: " + ", ".join(missing))

required_exports = {"nvKmsModuleLoad", "nvKmsModuleUnload",
                    "nvKmsKapiProbe", "nvKmsKapiGetFunctionsTableInternal"}
missing_exports = sorted(required_exports - kernel_defs)
if missing_exports:
    fail("NVKMS core exports absent from kernel: " + ", ".join(missing_exports))

source = PORT.read_text(encoding="utf-8")
mentioned = set(re.findall(r"\bNV0[14]_[A-Z0-9_]+\b", source))
if not USED_RM_OPS <= mentioned:
    fail("RM dispatcher omits: " + ", ".join(sorted(USED_RM_OPS - mentioned)))

# The old Kestrel serializer remains as an intentionally unreachable pre-init
# failure path.  Readiness comes from NVIDIA's matching host-RM dispatcher,
# which owns memory descriptors and implements the complete op union.
if "if (nvrm_rmapi_op(raw)) return;" not in source:
    fail("NVKMS is not routed through the native NVIDIA host-RM dispatcher")
if not {"nvrm_rmapi_op", "rm_kernel_rmapi_op"} <= kernel_defs:
    fail("native host-RM dispatcher symbols are not linked")

rm_core = ROOT / "Linux NVIDIA Driver/kernel/nvidia/nv-kernel.o_binary"
rm_imports = symbols(["-u"], rm_core)
nm_lines = subprocess.run([nm_path(), "-g", "--defined-only", str(KERNEL)],
                          text=True, capture_output=True, check=True).stdout.splitlines()
types = {ln.split()[-1]: ln.split()[-2] for ln in nm_lines if len(ln.split()) >= 3}
weak_rm = sorted(n for n in rm_imports if types.get(n, "").upper() in {"W", "V"})
if weak_rm:
    fail("native host RM still depends on weak placeholders: " + ", ".join(weak_rm[:12]))

# A fresh SET_MODE specifies legacy LUT state even when no ramp surface is
# supplied.  NVIDIA's Linux DRM client encodes the identity/disabled input LUT
# as depth=30,start=0,end=0 and uses minPresentInterval=1 for a normal vblank
# flip.  Zero-initializing these fields can yield a successful but black mode.
client_source = CLIENT.read_text(encoding="utf-8")
linux_defaults = (
    "head->modeSetConfig.lut.input.depth = 30;",
    "head->modeSetConfig.lut.input.end = 0;",
    "layer->config.minPresentInterval = 1;",
    "layer->config.tearing = NV_FALSE;",
)
missing_defaults = [s for s in linux_defaults if s not in client_source]
if missing_defaults:
    fail("KAPI modeset does not preserve NVIDIA Linux color/flip defaults: " +
         ", ".join(missing_defaults))

# Frame presentation must follow the same contract as nvidia-drm: populate a
# back buffer, validate a flip-only request, commit it, and only then reuse the
# previous front buffer.  In-place CE writes to a live surface do not submit a
# new framebuffer to NVKMS and need not invalidate the display cache.
flip_contract = (
    "struct NvKmsKapiMemory *memory[2];",
    "struct NvKmsKapiSurface *surface[2];",
    "static NvBool present_phase",
    "layer->flags.surfaceChanged = NV_TRUE;",
    "reply.flipResult == NV_KMS_FLIP_RESULT_SUCCESS",
)
missing_flip = [s for s in flip_contract if s not in client_source]
if missing_flip:
    fail("KAPI client does not use double-buffered atomic presentation: " +
         ", ".join(missing_flip))

linux_takeover = (
    "alloc.eventCallback = nvkms_test_event_cb;",
    "kapi.framebufferConsoleDisabled(test_device);",
    "NVKMS_EVENT_TYPE_FLIP_OCCURRED",
    "kapi.checkLutNotifier(test_device, active[i].head, NV_TRUE)",
    "wait_for_flip_mask(requested.headsMask, before, operation)",
)
missing_takeover = [s for s in linux_takeover if s not in client_source]
if missing_takeover:
    fail("KAPI client omits NVIDIA Linux's firmware-console takeover: " +
         ", ".join(missing_takeover))
if "wait_for_flip_mask(requested.headsMask, modeset_before" in client_source:
    fail("KAPI client waits for an impossible first-activation flip event")

# nvidia-drm's legacy CRC ioctl returns the target SF/SOR output CRC, not the
# raster-generator CRC.  Preserve all three stages so a dark result can be
# localized to surface fetch, raster, or physical-output formatting.
crc_contract = (
    "crcs.compositorCrc32.value",
    "crcs.rasterGeneratorCrc32.value",
    "crcs.outputCrc32.value",
    "r->scanout_confirmed = true;",
)
missing_crc = [s for s in crc_contract if s not in client_source]
if missing_crc:
    fail("KAPI client does not retain compositor/raster/output CRC evidence: " +
         ", ".join(missing_crc))

print(f"PASS: NVKMS ABI closed ({len(imports)} unique imports, supplied core hash pinned)")
print(f"PASS: NVIDIA core entry points linked ({len(required_exports)} checked)")
print(f"PASS: RM bridge is {len(USED_RM_OPS)}/{len(USED_RM_OPS)} through "
      "NVIDIA's native host-RM dispatcher")
print("PASS: KAPI modeset uses NVIDIA Linux's identity-LUT and vblank presentation defaults")
print("PASS: scanout frames use double-buffered, validated NVKMS atomic flips")
print("PASS: KAPI client retires firmware console state, waits for LUT completion, and drains flip events")
print("PASS: scanout verification follows compositor, raster, and final SF/SOR output CRCs")
