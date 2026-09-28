# GPU compatibility requirement and implementation gaps

The requested product scope is AMD and NVIDIA, across old and new generations,
including multiple installed adapters and mixed-vendor configurations. The
Intel/RTX 5070 trial machine is a test target, not the compatibility boundary.
Identifying a PCI device, retaining firmware scanout, and providing native
hardware acceleration are different support levels. Do not present any one as
proof of the others. Hardware features must be exposed per adapter and per
engine; absent hardware features cannot be supplied by changing a device name.

## Source audit, 2026-09-17

| Area | Current implementation | Remaining work |
| --- | --- | --- |
| Inventory | `kernel/gpu.c` inventories up to four display-class PCI devices | Explicit handling of capacity limits and lifecycle changes |
| Firmware display | Boot framebuffer ownership is matched to a PCI aperture | Not a universal boot/display path, especially without usable GOP |
| Screen acceleration routing | PCI-keyed immutable registrations carry driver contexts; framebuffer handoff explicitly selects one desktop target | Per-surface/multi-target routing, removal lifecycle and cross-adapter composition |
| NVIDIA screen backend | Dispatch checks selected card against the live copy-channel owner and published NVKMS device | Move underlying driver resources out of singleton state |
| NVIDIA native runtime | Channel/display paths retain shared state, including one NVKMS runtime framebuffer | Isolate RM, channels, allocations, fences, display resources and failures by adapter |
| NVIDIA generations | Command-class table spans several generations, but table entries are not execution proof | Validate generation-specific initialization, command/shader formats, firmware and rendering paths |
| AMD | `amd_bring_up()` stops before accelerated engines; no AMD registration in the generic screen acceleration interface | Implement generation-appropriate firmware/engine initialization, queues, memory management and rendering |
| Codec capability | Existing video work is NVIDIA-specific | Separate per-vendor/per-generation engine and format capabilities; do not advertise NVENC/NVDEC as AMD interfaces |

The supplied NVIDIA Linux driver is a reference for its supported NVIDIA
hardware, not an AMD driver or evidence that this OS supports every GPU.

## Changes made during this audit

- NVIDIA inventory reads no longer enlarge a short register BAR to reach the
  memory-size registers. An unknown BAR size now leaves MMIO identification
  disabled rather than assuming a 16 MiB aperture; PCI identification remains.
- `nv_classes_for()` no longer substitutes GeForce Blackwell classes for an
  unknown newer family, or truncates a wider chip ID into a known family.
- Existing class mappings, including GB203 and GB205, are unchanged.
- Removed the claim that a firmware framebuffer works on every machine.

`tools/test_gpu_variant_safety.py` executes the production lookup, aperture-size
helper and bounded register reader in host memory. It covers 65,538 chip IDs,
14 aperture sizes and seven register-read boundary cases. It is neither a GPU
emulator nor a hardware compatibility test.

These identification changes do **not** implement AMD acceleration, make the
NVIDIA runtime multi-device, or establish support for
any additional generation. Other initialization and firmware-selection paths
also need a generation/bounds audit; the identification fix is not a global
MMIO-safety certification.

## Explicit accelerator routing

The follow-up changes remove first-registration-wins screen dispatch:

- Registrations use PCI bus/slot/function keys and stable per-device contexts.
  Registration does not select a screen. Conflicting registrations cannot
  overwrite a live context. Capacity exhaustion is reported.
- VMware selects its registration when adopting its framebuffer. NVKMS records
  the host card when allocating its display device, checks that it matches the
  scanout channel, and selects that registration at desktop handoff.
- Fill, copy, cursor and capability calls use one selected binding. Failure or
  unavailable readiness does not fall through to another GPU's framebuffer.
- A stale NVKMS ready flag cannot override a failed copy channel, and a channel
  owned by another registered card cannot lend that card's readiness to the
  selected device. Abandoning the framebuffer clears the accelerator target.

`tools/test_gpu_accel_routing.py` compiles the production registry, dispatch,
NVIDIA adapter callbacks, channel-owner accessor and NVKMS ownership predicate.
It checks registration order, bus/slot/function identity, duplicate/conflicting
registrations, capacity, missing callbacks, readiness loss, argument/context
forwarding, failure propagation, stale display readiness and GPU-owner mismatch.
The framebuffer-handoff call sites additionally have source-order assertions.
Both routing and variant-safety tests are included in `tools/run_host_tests.py`.

The generic screen interface now also dispatches `GPUOP_CANDRAW`, triangle
drawing, image drawing and pixel readback through the selected adapter's
context. NVIDIA image scaling is still unsupported by this upload path, but
no longer falls through to VMware on a different GPU. NVIDIA's 3D availability
checks the live graphics channel, its card identity and scanout mapping rather
than treating display initialization alone as render readiness.

NVKMS ownership is exposed separately from engine readiness. Framebuffer
presentation/mapping, configuration preflight and the current NVIDIA-only
surface syscall use that ownership predicate. A failed native submission does
not become a CPU copy into its non-scanout shadow buffer. Kernel GPU enumeration
uses the selected adapter, not the historical firmware-boot adapter, to label
the renderer. Display VRAM information no longer unconditionally uses NVIDIA
card zero. This does not change the accuracy of the underlying telemetry source.

Routing tests now execute the real kernel GPU-enumeration function with separate
device/counter fixtures, and the actual NVIDIA/VMware triangle/image callbacks.
They cover a non-boot rendering GPU, graphics-channel failure, unsupported image
scaling, failed submissions, and ownership persisting through an engine fault.
Framebuffer/surface syscall tests separately cover mapping and user-copy failure
paths; their VM-copy stubs were updated to match the current chunked copy API.

The direct `GPUOP_DRAW` syscall now copies its bounded vertex batch into kernel
memory before calling an adapter. Previously it passed the validated user
pointer directly into a driver that could sleep, permitting sibling unmap or
mutation during dispatch. Full-width triangle counts are validated before any
cast; the existing 128-triangle limit is unchanged. The snapshot is freed after
the synchronous driver call, including driver failure; partial draws are not
replayed. `tools/test_gpu_triangle_syscall.py` covers all supported batch sizes,
count truncation, exact bounds, allocation failure, mapping revocation and
mutation during dispatch. It mocks the VM-copy and adapter boundaries, not GPU
execution. This does not yet audit every user-pointer path in the graphics ABI.

This is still one selected desktop target. Registration and framebuffer handoff
must be serialized with drawing quiesced; atomic pointer publication is not a
general hot-removal or in-flight resource-retirement protocol. The registry can
route different adapters, but the current NVIDIA and VMware driver internals
are not independent per-device runtimes. Offscreen surfaces/codecs remain
NVIDIA-specific; other vendor-specific cursor and mode-setting entry
points still require the same ownership migration.

The legacy `GPUOP_SHADERS` and `GPUOP_LAYOUT` entry points now also route through
the selected adapter's context, instead of calling VMware unconditionally.
Their existing bytecode/element format is explicitly tagged `GPU_PROGRAM_SVGA_DX`.
Only the VMware backend currently implements these callbacks; selecting NVIDIA,
an adapter without these operations, or no adapter returns unavailable without
touching VMware. This is not native NVIDIA/AMD shader compilation or a portable
graphics API. The programmable native surface API remains separate.

Shader metadata, both bounded bytecode streams, signature arrays and vertex
layout metadata are kernel-owned snapshots before backend entry. Failure to
copy either stream prevents dispatch and frees the allocation. Inputs may be
modified while being captured (the two streams are not an atomic user-memory
transaction), but a backend cannot retain an unmapped or subsequently modified
user pointer. `tools/test_gpu_program_syscall.py` exercises the production
snapshot helpers with VM-copy/driver spies: 512 code-length combinations,
signature count boundaries, 16 layouts, metadata/code mutation, copy/allocation
failures and backend failures. Routing tests separately exercise the production
VMware wrappers, format checks, selection, argument forwarding and no fallback.
Neither test executes a GPU shader.

## Implementation order

1. Introduce adapter-owned resources and explicit surface/scanout routing,
   preserving the existing single-card path during migration. A registry alone
   is insufficient while driver callbacks still use singleton state.
2. Publish separate capability and readiness data for each adapter's display,
   render, copy and codec engines. Unsupported and failed are distinct states.
3. Implement the missing vendor/generation backends and firmware contracts.
   Reuse common resource and presentation APIs, not another generation's raw
   register or command values.
4. Validate single-adapter, same-vendor multi-adapter and mixed-vendor systems,
   including independent failures, display ownership changes and unplug/replug.

The existing USB and cross-system trial archive were not updated by this audit.
They must not be described as universally GPU-compatible images.
