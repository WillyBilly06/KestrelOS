#!/usr/bin/env python3
"""Conservative direct-call reachability audit for the supplied NVRM core.

This is a prioritization tool, not a readiness proof: indirect function-table
calls cannot be recovered from ELF relocations alone.  It finds every external
host symbol referenced by the transitive direct-call graph of the RM lifecycle
and RMAPI entry points, so those primitives are ported before unrelated
NVSwitch, vGPU, and Tegra services.
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "Linux NVIDIA Driver/kernel/nvidia/nv-kernel.o_binary"
KERNEL = ROOT / "build/kernel/kernel.elf"
OBJDUMP = Path(r"C:\Program Files\LLVM\bin\llvm-objdump.exe")
NM = Path(r"C:\Program Files\LLVM\bin\llvm-nm.exe")
if not OBJDUMP.exists(): OBJDUMP = Path(shutil.which("llvm-objdump"))
if not NM.exists(): NM = Path(shutil.which("llvm-nm"))

ROOTS = {
    "rm_init_rm", "rm_shutdown_rm", "rm_init_private_state",
    "rm_free_private_state", "rm_init_adapter", "rm_disable_adapter",
    "rm_shutdown_adapter", "rm_kernel_rmapi_op", "rm_isr", "rm_isr_bh",
    "rm_isr_bh_unlocked",
}
RELOC = re.compile(r"R_X86_64_[A-Z0-9_]+\s+([^\s+-]+)")


def names(args):
    out = subprocess.run([str(NM), *args, str(CORE)], check=True,
                         capture_output=True, text=True).stdout
    return {ln.split()[-1] for ln in out.splitlines() if ln.split()}


def symbol_types(path):
    out = subprocess.run([str(NM), "-P", str(path)], check=True,
                         capture_output=True, text=True).stdout
    result = {}
    for line in out.splitlines():
        fields = line.split()
        if len(fields) >= 2:
            result[fields[0]] = fields[1]
    return result


defined = names(["--defined-only"])
undefined = names(["-u"])
seen, pending, imports = set(), set(ROOTS), set()

while pending:
    batch = sorted(pending)[:96]
    pending.difference_update(batch)
    seen.update(batch)
    out = subprocess.run(
        [str(OBJDUMP), "-dr", "--disassemble-symbols=" + ",".join(batch),
         str(CORE)], check=True, capture_output=True, text=True).stdout
    for target in RELOC.findall(out):
        if target in undefined:
            imports.add(target)
        elif target in defined and target not in seen:
            pending.add(target)

print(f"Direct-call graph: {len(seen)} internal functions from {len(ROOTS)} roots")
print(f"Reachable external host imports: {len(imports)}")
types = symbol_types(KERNEL) if KERNEL.exists() else {}
weak = sorted(name for name in imports if types.get(name, "").upper() in {"W", "V"})
strong = sorted(imports - set(weak))
print(f"Reachable strong host imports: {len(strong)}")
print(f"Reachable weak placeholders: {len(weak)}")
if "--list" in sys.argv:
    for name in strong:
        print(name)
for name in weak:
    print(name)
print("NOTE: this is a lower bound; indirect HAL/function-table edges remain gated.")
