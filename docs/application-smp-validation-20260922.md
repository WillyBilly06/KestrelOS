# Application SMP: implemented and CPU-VM verified

This is an engineering record of SMP validation.
The physical USB and existing release images were left unchanged.

## Result

The scheduler now executes application user code on multiple CPUs. APs migrate
syscall/exception continuations to the bootstrap processor (BSP), where legacy
kernel and driver handlers remain serialized. After those handlers and any
local interrupt acknowledgement finish, return-to-user continuations can be
scheduled on idle application APs again.

One AP remains in the restricted kernel-job pool. Other eligible APs leave that
pool at a protected batch boundary, enter a private idle stack, and join the
application scheduler with a local 1 kHz timer. In-flight promotion reservations
prevent simultaneous admissions from consuming the last restricted worker.
This is not a fully parallel kernel or complete POSIX/Linux compatibility.
Two-CPU systems still have only one application CPU under this reservation policy.

## Bugs caught before physical testing

- The initial AP syscall invariant incorrectly expected the parked idle thread
  to own the running CPU. It must be unowned while the user thread runs.
- The first real VM boot advertised two application CPUs but sampled user work
  only on CPU 0 (`APIC mask=0x1`). Preempted kernel continuations remained
  BSP-only and there was no re-admission point after kernel return. The new
  return boundary fixes that demonstrated issue.

The failed run is retained at `out/smp-qemu-l9fr3xm2/`. It is not counted as a pass.

## Executed integration tests

`tools/test_smp_qemu.py` creates a unique private GPT image, initrd, firmware
variable store and log directory. Its minimal guest has no desktop or NVIDIA
firmware. Command line `nogpu`, emulated VGA/NVMe, no network, no passthrough.
It uses QEMU TCG with multiple host threads. It does not use a physical USB,
canonical release image, NVENC, NVDEC, or a physical GPU.

`tests/smp/user_parallel.c` is packaged as `/bin/init` only in this test image.
The final guest runs 32 native threads and checks:

- CPUID APIC IDs sampled in actual user code on every reported application CPU;
- exact arithmetic checksums and 16,384 atomic work increments;
- distinct thread IDs and per-thread GS state across syscalls/sleep/migration;
- concurrent 16 KiB heap allocations, retained contents and freeing;
- the existing Linux clone/futex four-thread shared-counter workload;
- eight subprocess exits with live spinning sibling threads, followed by wait.

Observing every CPU is this particular stress workload's acceptance condition,
not a promise that every ordinary program will occupy every CPU. No CPU-only
test here measures GPU frame time, physical hybrid-core clocks or native codecs.

| Run directory | vCPUs | Application CPUs / reserved workers | User APIC mask | Result |
| --- | ---: | ---: | --- | --- |
| `out/smp-qemu-8ampwtz9` | 4 | 2 / 2 | `0x3` | First active-path pass, earlier four-thread guest |
| `out/smp-qemu-7xw6jp9c` | 4 | 3 / 1 | `0x7` | Scalable admission pass, 16-thread guest |
| `out/smp-qemu-3y8_o142` | 8 | 7 / 1 | `0x7f` | Scalable admission pass, 16-thread guest |
| `out/smp-qemu-o79bw_bg` | 24 | 23 / 1 | `0x7fffff` | 32-thread and heap checks passed |
| `out/smp-qemu-y71u76ou` | 24 | 23 / 1 | `0x7fffff` | Repeat passed |
| `out/smp-qemu-11vxd7w1` | 24 | 23 / 1 | `0x7fffff` | Final rebuilt kernel passed |

Final kernel: `build/kernel/kernel.elf`, 20,714,760 bytes, SHA-256
`c8d66df3acbc9fc8ab38d033866fc3946834718bdab73730decc1941c007d2fe`.
Final CPU-test guest SHA-256:
`9abb4d2c99c0609bd80ba718bcce82be5ba7bd9486df16276cfc469ee92e2e75`.
Each run preserves its own kernel copy, manifest, serial log and result.

The GPT-6 Sol scheduler worker also passed 11 host regressions covering idle
wakes/stacks, entry/return affinity, promotion reservations, device waits, boot
progress, process stop, kernel-entry lifecycle, Linux threads and futexes.
The parent separately reran return-affinity and reservation checks. Host tests
mock context switching; the QEMU tests above execute the actual assembly path.

## Other requested work: status, not completion claims

- The resident GPU launch optimization and combined geometry-metadata readback
  remain in this kernel. Launch-packet, QMD-field and 1,365 metadata-layout
  checks passed. Native speed gains and physical SM utilization are unmeasured.
  Rendering already dispatches GPU grids; small geometry batches cannot be
  assumed to occupy every SM. See `gpu-inline-launch-20260922.md`.
- NVENC's source-backed history/RC-work corrections are included, but no native
  successful encode has been observed. The 114 Linux/three Windows instruction
  anchors and 19 method definitions still pass static checks. The latest native
  failure predates this corrected candidate. Windows reference success is not
  proof of Kestrel success. See `nvenc-cqp-work-state-20260918.md` and
  `nvenc-sdk-slice-comparison-20260922.md`.
- Acer physical image visibility remains unverified. The DSC threshold fix is
  included; a passing static NVKMS audit does not establish sink output.

No physical image was flashed. No native or VM codec test was run.
