# Public source snapshot

The local development tree includes experiments, generated outputs, hardware
captures, and third-party material. This repository tracks the OS source,
selected documentation, host tests, and build tools that are useful for review.

The following local items are intentionally excluded:

- Secure Boot private keys, certificates, signatures, and signed boot files.
- GPU VBIOS and device ROM test fixtures. The ROM-specific self-test call was
  removed from this public copy of `kernel/nv_core.c`; the parser and other
  driver code remain.
- NVIDIA driver packages, object files, firmware, and open-kernel-module source
  checkouts. `build.py` still expects these user-supplied inputs for a full
  image, so `python build.py` is not a one-command build from this repository.
- Rasterized glyph headers generated from Segoe UI, Consolas, and Lucida
  Console. The font generators are included; run them only with fonts you are
  licensed to use. `tools/generate_required_fonts.ps1` records the project
  settings used to regenerate the headers locally.
- VM disk images, build outputs, logs, backups, machine-specific scripts, and
  raw hardware captures.

Verified here: `python build.py boot` produced a UEFI loader from the staged
source, and `python build.py user` built the user programs after local font
generation. Kernel/image and hardware behavior were not re-verified from this
public snapshot. The screenshot in `assets/` came from a prior local VM run.
