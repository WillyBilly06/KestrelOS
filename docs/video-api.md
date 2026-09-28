# Hardware video API, ABI 1

This is an implemented decoder submission path, not a declaration of complete
video playback or verified NVENC support. The existing physical USB boot log at
`out/stick/_logs/20260913-153657/KERNEL.LOG` records successful native decoding
of both the neutral 64x64 fixture and the nonuniform 256x256 reference through
the application backend. This does not establish support for other codecs or
interframe playback. No new tests or VM runs were performed for the September 13
NVENC plane-layout correction.

## Application use

The C library exposes `gpu_video(kvideo_request_t *)`, declared in
`user/libc/kestrel.h`. The fixed 112-byte request is in `include/kestrel/video.h`.
The wrapper uses `SYS_GPU / GPUOP_VIDEO`; it returns zero on success, or -1 and
`errno` on failure.

The image includes a command-line utility:

```
video info input.h264
video decode input.h264 output.nv12
video encode input.nv12 output.h264 256 256
```

`info` parses metadata without decoding and does not prove GPU availability.
`decode` writes the output file only after the complete hardware frame passes
completion, status, conversion and resource-release checks. The named output
file is overwritten; use a separate output path. A file-write error can leave
a partial output file. Input and output paths spelled identically are rejected.

For direct API use:

1. Supply `version=KVIDEO_ABI`, `operation=KVIDEO_H264_INSPECT`, the input address
   and byte length, and `reserved=0`.
2. Allocate `required_bytes` of output storage returned by inspection.
3. Set `operation=KVIDEO_H264_DECODE_IDR`, `output` and `output_capacity`, and call
   again. Handle `EBUSY` with a bounded retry; there is no CPU decode fallback.
4. Consume output only after success with `phase=KVIDEO_PHASE_COMPLETE` and
   `written_bytes=required_bytes`.

The returned image is tightly packed **coded NV12**: the Y plane followed by
interleaved U/V bytes, both with row pitch `coded_width`. Display cropping is
separate metadata, in pixel units; it does not reduce allocation size. This is
not an MP4/MKV container, RGB image, or display-ready GPU surface.

## Current supported subset and limitations

- One Annex-B baseline progressive 8-bit 4:2:0 reference IDR/I slice with its SPS
  and PPS. CAVLC, POC types 0/2, valid cropping/QPs and VUI framing are parsed.
- Coded dimensions 48x64 through 4096x4096, macroblock-aligned; input at most
  16 MiB. The hardware job uses coded rather than cropped dimensions.
- No interframe DPB, CABAC/high-profile, interlaced, FMO, multi-slice or changing
  parameter-set support. These are rejected, not silently approximated.
- No general codec/graphics interoperability, NV12-to-RGB shader, video-player
  demuxer, audio synchronization, asynchronous queue or persistent video session.
- Experimental NVENC application submission now accepts one 256x256 NV12 image
  with normal intra compression at QP 26 and driver-derived quantization tables;
  it has not run on hardware. No larger encode dimensions, interframe encoding,
  or verified native encoding is claimed. Decoding does not imply encoding works.
- Host memory conversion and coherent transfer are currently synchronous and
  not a throughput-optimized zero-copy pipeline. The compressed data itself is
  decoded by NVDEC, never by a CPU entropy decoder.

## Memory and failure guarantees

User input is copied into kernel-owned storage under the shared GPU transaction.
The GPU sees only kernel-managed VRAM addresses. Decode and resource-release
failures publish no pixels to userland. After those stages succeed, output is
copied in checked chunks under the syscall's user-memory protection. A caller
unmapping its destination during that copy can receive a prefix followed by
an error; metadata copy-back can also fail. Consume output only when the syscall
returns success, not merely because a buffer or an old request contains data.
The request itself must not overlap the pixel output range. Original input may
overlap the output, because it has already been snapshotted.

The job planner sizes picture, slice, coloc, history, MB-history, status and
padded NV12 planes from the parsed stream. Y and UV use different Blackwell
GOB swizzles from Mesa NIL; neutral bytes alone cannot validate either mapping.
The maximum current job consumes 113,508,352 VRAM bytes plus temporary host
buffers. Allocation failure is reported, never an invitation to use CPU decoding.

Every launch requires its fresh completion cookie and clean firmware status,
including the exact macroblock count. Firmware fields are read only after the
completion fence. The `phase`, `parse_status`, `firmware_error`, `slice_error`,
`decoded_mbs` and `error_mbs` fields distinguish failure stages.

After retired output, both aliases are unmapped and GPU page-table changes
committed before the VRAM object is freed. A timeout retains/quarantines the
allocation: an unfinished GPU may still touch it. Uncertain allocation or
teardown also quarantines the single runtime decoder slot. There is no unsafe
same-handle retry; recovery/reset remains future work. Desktop/copy/compute
resources are not repurposed by this decoder.

## Verification boundary

Host tests execute production parser, job planner, tile conversion, syscall,
native command submission function and allocation/free functions with modeled
external interfaces. The VMM capacity test uses the actual page-table builder
and independent walker. These checks cover bounds, nonuniform pixel transport,
command ordering and failure isolation, **not physical NVDEC execution**.
The existing native log additionally records successful 256x256 reference
decoding through this backend, with both complete-plane hashes matching the
independent oracle. That is evidence for this frame, not every supported size
or a throughput measurement.

## Native encoder-to-decoder integration

The boot-only NVENC round-trip check now calls this same dynamic NVDEC backend.
It no longer constructs a second hard-coded decoder picture context or keeps a
separate permanent 512 KiB round-trip allocation. It uses private host snapshots
and the backend's checked allocation, untile and release path.

After a native NVENC completion passes the fence, firmware-status, output-size
and Annex-B framing checks, the driver captures both the raw output and the exact
CFB7 picture configuration. `nvenc_h264_stream.h` produces a self-contained stream
from that pair: existing SPS/PPS must agree with the configuration and are
preserved; otherwise matching SPS/PPS are generated and the actual IDR NAL is
copied byte for byte. This is metadata packaging, not CPU encoding. No fixture
is substituted if NVENC produces nothing. Fields, high bit depth, unsupported
coding features, nonzero bottom POC delta and cropped/mismatched metadata are
rejected by this deliberately restricted native test path.

Round-trip success requires clean decoder status, exact 256x256 geometry,
98,304 output bytes, successful resource release and a full match to the native
encoder test's neutral NV12 input. A valid header alone never passes the test.
The last native encoder attempt timed out after picture submission. A subsequent
static audit corrected missing Blackwell image-plane layout selectors at CFB7
picture offset `0x208`; see `nvenc-plane-layout-20260913.md`. This correction has
not been executed on the card, and no successful native encoder-to-decoder round
trip is claimed.

A separate Windows-only reference tool, `tools/generate_nvenc_h264_fixture.py`,
uses the installed NVIDIA NVENC API and CUDA to generate an independently
encoded, nonuniform 256x256 baseline/CAVLC stream. Its original bitstream,
configuration and hashes are retained under `out/nvenc-reference-256`. That
reference validates parser compatibility with genuine encoder output, not the
Kestrel NVENC submission path. Its QP18 source image is not an exact decoded
pixel oracle because the encoding is lossy.

## Independent nonuniform native decoder check

`tools/decode_nvdec_reference.py` decoded that unchanged 5,014-byte stream through
the installed Windows dedicated CUVID/NVDEC path, with clean decode status and
all CUDA/CUVID API calls succeeding. Two fresh sessions produced identical
98,304-byte coded NV12 output. This output differs from the original lossy source
in 39,054 bytes; comparing with the pre-encode input would incorrectly fail.

The original decoded bytes, per-plane SHA-256 values and API provenance are
archived under `tools/fixtures/h264_nvdec_256*`. The full decoded SHA-256 is
`a782dd43fd91132e04632663728bcb984928d6f9c50507b5121faf1f78d12b3e`.

The GPU-test boot path now calls `nv_nvdec_application_selftest_hw` after the
known-good neutral decoder fixture and before native NVENC. It sends the actual
archived compressed stream through `nv_nvdec_decode_idr`, checks complete status,
geometry, byte counts and resource release, then hashes every Y/UV output byte
against that independently decoded reference. This exercises nonuniform output
and the separate Blackwell Y/UV mappings even if native NVENC fails.

Its report line is `NVDEC application IDR`; its kernel log records both actual
plane digests. It has a separate verdict and does not replace or falsely pass
the native NVENC-to-NVDEC round trip. The post-codec rendering/desktop gate is
unchanged. The existing September 13 native log records this check passing:
Y SHA-256 `523831dd2b2c30456464133af2280ad24ce7f1fdad529dfa8d6fc4847866a39c`,
UV SHA-256 `32ce92bf22547a0a31056fba3964d1b94e235bed2885bd812c3ef572f42b0c64`.
This is not a claim of full video playback, interframe or general codec support.

## Experimental application encoder

ABI 1 appends operations 2 (`KVIDEO_H264_ENCODE_PLAN`) and 3
(`KVIDEO_H264_ENCODE_IDR`) without changing the 112-byte request or decoder
operations. Old kernels reject these new operation numbers.

1. Set `coded_width=256`, `coded_height=256`, version and operation ENCODE_PLAN.
   This checks geometry and returns the output capacity bound. It does not read
   input, allocate GPU resources, or prove encoder readiness.
2. Supply exactly 98,304 bytes of tightly packed NV12 (65,536 Y bytes followed
   by 32,768 interleaved UV bytes), an output buffer of `required_bytes`, and
   operation ENCODE_IDR. No implicit resizing, RGB conversion, or software encode
   fallback occurs. Other dimensions are rejected before submission.
3. On success, consume only `written_bytes` (positive and no greater than
   `required_bytes`). Unlike decoder output, the encoded stream's actual size is
   variable; `required_bytes` remains the capacity bound. The stream includes
   matching SPS/PPS and one actual native IDR, not a substituted fixture.

The kernel snapshots caller data, tiles Y and UV independently, and uses the
same NVENC picture builder/command path as the boot fixture. Fresh completion,
firmware status, bounds and framing must pass before packaging/copy-back.
For encoding, `firmware_error` is zero for the driver's completed-picture state
2; other low two-bit states return raw state + 1. `slice_error` carries
`ucode_error_status`; these are read only after retirement. The legacy member
name `error_status` is not a zero-on-success contract for CFB7. Callers must
still require syscall success and valid output, not just zero metadata errors.
See `nvenc-completion-state-20260917.md` for Linux/Windows evidence. `decoded_mbs` and
`error_mbs` remain zero. TILE and PACKAGE phases identify host data movement and
header packaging, not CPU entropy coding.

Applications now use normal intra 16x16 coding at constant QP 26 with the NVIDIA
595 quantization defaults, documented in `nvenc-quantization-20260913.md`. This
is a lossy path; a decoded image is not expected to match the input byte-for-byte.
Only the neutral boot test retains forced IPCM and its exact-pixel comparison.
Neither this compressed path nor its quality/compression ratio has been verified
on hardware. It is not a production video encoder or a high-throughput video
session API. Work buffers
are bounded, persistent and serialized; a timeout quarantines the channel and
does not permit a same-channel retry. Application output snapshots are invalidated
before releasing the shared transaction.

The CLI opens/truncates its output only after a successful complete call. Existing
output files are replaced on success; file-write failure may leave a partial
file. No tests, native codec executions, or VM runs were performed for this API
extension. Existing decoder observations do not verify these new changes.
