#!/usr/bin/env python3
"""Truthful offline release gate for NVIDIA's 595.99.02 host RM core.

Weak symbols make it possible to link and inspect the complete proprietary
core while porting.  They do not make it runnable.  This audit distinguishes
typed Kestrel implementations (strong symbols) from placeholders and refuses
activation or USB imaging until no imported function is weak.
"""
from pathlib import Path
import hashlib
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "Linux NVIDIA Driver/kernel/nvidia/nv-kernel.o_binary"
KERNEL = ROOT / "build/kernel/kernel.elf"
EXPECTED_SHA256 = "b64589a3760bd2acea31ca104181ad98c1af3fdc588d251c2d1bdb55a8ccdc54"


def nm_path():
    preferred = Path(r"C:\Program Files\LLVM\bin\llvm-nm.exe")
    return str(preferred if preferred.exists() else shutil.which("llvm-nm"))


def nm(args, path):
    return subprocess.run([nm_path(), *args, str(path)], check=True,
                          text=True, capture_output=True).stdout.splitlines()


def symbol_names(lines):
    return {line.split()[-1] for line in lines if line.split()}


if not CORE.exists() or not KERNEL.exists():
    raise SystemExit("FAIL: build kernel with the supplied RM core first")
if hashlib.sha256(CORE.read_bytes()).hexdigest() != EXPECTED_SHA256:
    raise SystemExit("FAIL: supplied NVIDIA RM core hash changed")
port = (ROOT / "kernel/nvrm_os.c").read_text(encoding="utf-8")

imports = symbol_names(nm(["-u"], CORE))
defs = {}
for line in nm(["-g", "--defined-only"], KERNEL):
    fields = line.split()
    if len(fields) >= 3:
        defs[fields[-1]] = fields[-2]

unresolved = sorted(imports - defs.keys())
if unresolved:
    raise SystemExit("FAIL: unresolved NVRM imports: " + ", ".join(unresolved))

weak = sorted(name for name in imports if defs[name].upper() in {"W", "V"})
strong = sorted(imports - set(weak))
print(f"PASS: matching NVIDIA RM core linked (SHA-256 pinned, {len(imports)} imports)")
print(f"PASS: {len(strong)}/{len(imports)} host symbols have typed/strong Kestrel implementations")
if weak:
    print(f"HOLD: {len(weak)} NVRM host symbols remain weak placeholders; "
          "RM/NVKMS activation and USB imaging are forbidden")
    print("NEXT:", ", ".join(weak[:24]) + (" ..." if len(weak) > 24 else ""))
else:
    print("PASS: NVRM host ABI contains no weak placeholders")

# Linux does not hand RM a pre-mapped register aperture: RmSetupRegisters owns
# that mapping and asserts the field is initially NULL.  It does, however,
# report the EFI framebuffer when the complete surface belongs to this GPU's
# FB/IMEM BAR.  Both details are required for a clean primary-console takeover.
console_contract = (
    "nv->bars[NV_GPU_BAR_INDEX_REGS].map=NULL;",
    "nv->bars[NV_GPU_BAR_INDEX_REGS].map_u=NULL;",
    "NvU64 fb_first = g_boot.fb.base;",
    "for (NvU32 i = NV_GPU_BAR_INDEX_FB; i < NV_GPU_NUM_BARS; i++)",
    "fb_first >= bar_first && fb_last <= bar_first + bar_bytes",
    "if (base) *base = fb_first;",
    "if (size) *size = fb_bytes;",
)
missing_console = [item for item in console_contract if item not in port]
if missing_console:
    raise SystemExit("FAIL: Linux-compatible EFI console/RM BAR ownership contract missing: " +
                     ", ".join(missing_console))
print("PASS: RM owns BAR0 mapping and receives an aperture-validated EFI framebuffer")

# nvidia-push allocates its CPU-written command stream as WC system memory,
# maps it into the GPU with snooping disabled, and publishes it with sfence.
# The official Linux wrapper therefore changes every non-cached allocation to
# UC in nv_alloc_system_pages().  A WB Kestrel alias makes the first channel
# NOP invisible to the GPU and produces the real-hardware 10-second
# "Failed to initialize DMA" timeout.  Keep the cache-correct alias in the
# release gate so this cannot silently regress.
cache_contract = (
    "cache_type != NV_MEMORY_CACHED",
    "nvrm_writeback_cache_range(a->direct_cpu, bytes);",
    "a->cpu = vmm_map_mmio(phys, bytes);",
    "dma_free_pages(a->direct_cpu, a->pages);",
)
missing_cache = [item for item in cache_contract if item not in port]
if missing_cache:
    raise SystemExit("FAIL: non-snooped RM system-memory cache contract missing: " +
                     ", ".join(missing_cache))
print("PASS: non-cached RM system allocations use a flushed UC alias for non-snooped GPU DMA")

# On this GB203 the BAR1 aperture returns HOST_FB_ACK_TIMEOUT poison.  NVKMS's
# Blackwell doorbell channel cannot use its normal vidmem USERD because every
# CPU GPPut store is therefore dropped.  The official NVIDIA-push code has a
# supported system-memory USERD path; require the narrow allocation rewrite
# that selects that path without relocating ordinary local/display surfaces.
userd_contract = (
    "mem->type == 6u /* NVOS32_TYPE_DMA */",
    "mem->flags == userdFlags",
    "mem->size == mem->alignment",
    "alloc->hClass = 0x0000003eu; /* NV01_MEMORY_SYSTEM */",
    "mem->attr = (mem->attr & ~0x06000000u) | 0x02000000u;",
    "mem->flags &= ~0x00010000u;",
    "alloc->hClass == 0x0000c661u /* HOPPER_USERMODE_A */",
    "*bBar1Mapping = NV_FALSE;",
    "forcing official BAR0 usermode mapping",
)
missing_userd = [item for item in userd_contract if item not in port]
if missing_userd:
    raise SystemExit("FAIL: Blackwell NVKMS system-memory USERD workaround missing: " +
                     ", ".join(missing_userd))
print("PASS: poisoned-BAR1 NVKMS USERD and doorbell use NVIDIA's supported sysmem/BAR0 paths")

# NVDisplay 3 requires an ILUT for non-FP16 primary surfaces and uses the same
# default vidmem allocation for the fallback OLUT.  NVIDIA initializes it via
# a CPU BAR1 mapping.  Require the exact-allocation CE seed/read-back path so a
# poisoned BAR1 aperture cannot silently turn otherwise valid flips black.
lut_contract = (
    "sizeof(nvrm_evo_lut_data_t) == 0x4200",
    "__builtin_offsetof(nvrm_evo_lut_data_t, output) == 0x2100",
    "mem->owner == 0xDCBAu /* NVKMS_RM_HEAP_ID */",
    "mem->type == 0u /* NVOS32_TYPE_IMAGE */",
    "(mem->attr2 & (1u << 18)) != 0 /* NVOS32_ATTR2_ISO_YES */",
    "nvrm_unorm10_to_fp16",
    "nvrm_transfer_rm_memory(hClient, hMemory, 0, lut,",
    "nvrm_transfer_rm_memory(hClient, hMemory, 0, verify,",
    "CE identity upload and read-back verified",
)
missing_lut = [item for item in lut_contract if item not in port]
if missing_lut:
    raise SystemExit("FAIL: Blackwell NVDisplay identity-LUT CE path missing: " +
                     ", ".join(missing_lut))
print("PASS: mandatory Blackwell ILUT/OLUT is seeded and read back through CE")
