#!/usr/bin/env python3
"""Verify Kestrel's CFB7 NVENC methods and Blackwell ABI evidence.

Method offsets are inherited from NVIDIA's published C9B7 class.  The class-
specific 0xCFB70006 firmware-record magic is independently checked in the
    supplied 595.99.02 Blackwell codec binary. The separate instruction-backed
    record check covers the upload size and relocated timer, not just the magic.
"""
import os, re, sys
from test_nvenc_cfb7_abi import reference_check
from test_gpu_stable_candidate import function

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OURS = os.path.join(ROOT, "kernel", "nv_chan.c")
REF = os.path.join(ROOT, "refs", "open-gpu-doc", "classes", "video", "clc9b7.h")
LIB = os.path.join(ROOT, "Linux NVIDIA Driver", "libnvcuvid.so.595.99.02")
NAMES = [
    "SET_APPLICATION_ID", "SEMAPHORE_A", "SEMAPHORE_D", "EXECUTE",
    "SET_CONTROL_PARAMS", "SET_PICTURE_INDEX", "SET_IN_DRV_PIC_SETUP",
    "SET_OUT_ENC_STATUS", "SET_OUT_BITSTREAM", "SET_IOHISTORY",
    "SET_IN_COLOC_DATA", "SET_OUT_COLOC_DATA", "SET_OUT_REF_PIC_LUMA",
    "SET_IN_CUR_PIC", "SET_IN_CUR_PIC_CHROMA_U", "SET_IN_CUR_PIC_CHROMA_V",
    "SET_OUT_REF_PIC_CHROMA", "SET_IN_RC_DATA", "SET_IO_RC_PROCESS",
]
REFERENCE_NAMES = {"SET_IN_RC_DATA": "SET_IN_RCDATA"}


def without_comments(text):
    return re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)

def values(text, prefix):
    out = {}
    text = without_comments(text)
    for name in NAMES:
        actual = REFERENCE_NAMES.get(name, name) if prefix == "NVC9B7" else name
        definitions = re.findall(r"^[ \t]*#[ \t]*define[ \t]+%s_%s\b([^\n]*)" %
                                 (prefix, actual), text, re.M)
        if len(definitions)>1:
            raise ValueError("duplicate method definition: %s_%s" % (prefix, actual))
        if definitions:
            literal=definitions[0].strip()
            if literal.startswith('(') and literal.endswith(')'):
                literal=literal[1:-1].strip()
            m=re.fullmatch(r'0x([0-9a-fA-F]+)[uUlL]*',literal)
            if not m:
                raise ValueError("nonliteral method definition: %s_%s" % (prefix,actual))
            out[name] = int(m[1], 16)
    return out


def method_mismatches(ours_text, ref_text):
    try:
        ours, ref = values(ours_text,"NVCFB7"), values(ref_text,"NVC9B7")
    except ValueError as e:
        return [str(e)]
    errors=[]
    for name in NAMES:
        if name not in ours or name not in ref:
            errors.append("missing method %s: native=%s reference=%s" %
                          (name,name in ours,name in ref))
        elif ours[name]!=ref[name]:
            errors.append("method %s: native=%#x reference=%#x" % (name,ours[name],ref[name]))
    return errors


def context_order_mismatches(text):
    # Static source-order checks, not execution proof or a C control-flow
    # verifier. Check the active host path separately from direct-GSP fallback.
    try:
        source=without_comments(text)
        channel=function(source,"open_engine_channel")
        host=function(source,"host_bind_channel_resources")
    except (ValueError, AssertionError) as e:
        return ["missing context function: %s" % e]
    errors=[]
    bind=channel.find("if (!host_bind_channel_resources(")
    schedule=channel.find("NVA06F_CTRL_CMD_GPFIFO_SCHEDULE")
    promote=channel.find("if (!nv_flcn_promote(")
    if not (0<=bind<schedule<promote):
        errors.append("require host resource bind before schedule; direct-GSP promotion after schedule")
    elif "return -1;" not in channel[bind:schedule]:
        errors.append("host binding failure must prevent scheduling")
    steps=[host.find(token) for token in (
        "nvrm_host_get_video_falcon_context(","nv_vmm_map_kind(",
        "nv_vmm_commit(","NV2080_CTRL_CMD_GPU_PROMOTE_CTX",
        "nvrm_host_mark_context_bound(")]
    if any(i<0 for i in steps) or steps!=sorted(steps):
        errors.append("require host descriptor -> map -> commit -> promote -> mark-bound ordering")
    return errors

def main():
    ours_text = open(OURS, encoding="utf-8").read()
    if not os.path.exists(REF):
        print("FAIL official clc9b7.h missing; nothing verified (offline)")
        return 1
    errors=method_mismatches(ours_text,open(REF,encoding="utf-8").read())
    errors+=context_order_mismatches(ours_text)
    for error in errors:print("MISMATCH",error)
    bad=len(errors)
    magic = re.search(r"#define\s+NVENC_CFB7_DRV_MAGIC\s+0x([0-9a-fA-F]+)",
                      open(os.path.join(ROOT, "kernel", "nvenc_drv_h264.h"),
                           encoding="utf-8").read())
    magic_value = int(magic.group(1), 16) if magic else -1
    if not os.path.exists(LIB):
        print("FAIL 595 codec binary missing; nothing verified (offline)")
        return 1
    reference_check()
    # reference_check pins the CFB7 discriminator's exact instructions, not
    # merely the presence of a constant also used by unrelated class branches.
    if magic_value != 0xcfb70006:
        print("MISMATCH ABI magic %#x differs from audited CFB7 branch" % magic_value)
        bad += 1
    # CFB7 is not implicitly NVENC0.  Blackwell's engine list can select
    # NVENC1/2/3; open RM's msencGetEngineDescFromAllocParams routes the object
    # from engineInstance, so verify that the source no longer hardcodes zero.
    for token in ["nvenc_engine == 28u ? 1u", "nvenc_engine == 29u ? 2u",
                  "nvenc_engine == 63u ? 3u", ".engine_instance = nvenc_instance"]:
        if token not in ours_text:
            print("MISMATCH source lacks NVENC object-instance route: %s" % token)
            bad += 1
    # Retained direct-GSP fallback uses Nouveau's external-VAS protocol.
    # The active host-RM path is checked separately above; these legacy tokens
    # cannot prove host-RM resource binding or native firmware completion.
    for token in ["p.virt_address  = VA_FLCN_CTX",
                  "p.size          = size",
                  "p.entry_count   = 0u",
                  "p.engine_type   = engine_type",
                  "p.ch_id         = ch->chid",
                  "falcon PROMOTE_CTX external-VAS VA+size"]:
        if token not in ours_text:
            print("MISMATCH source lacks Linux external-VAS Falcon promotion: %s" % token)
            bad += 1
    for token, label in [
        ("NVENC_VRAM_BYTES 0xc0000u", "768-KiB NVENC allocation"),
        ("BITSTREAM_CAPACITY=0x40000", "IPCM-capable 256-KiB output capacity"),
        ("ENC_WIDTH=NVENC_H264_TEST_WIDTH, ENC_HEIGHT=NVENC_H264_TEST_HEIGHT",
         "shared encoder/decoder test dimensions (values checked by ABI harness)"),
        ("nvenc_cfb7_h264_drv_pic_setup_s *cfg", "CFB7 extended picture record"),
        ("nvenc_cfb7_h264_timer(ENC_WIDTH, ENC_HEIGHT)", "CFB7 dimension-scaled timer"),
        ("VA_NVENC_TILED", "kinded surface alias"),
        ("NV_VIDEO_PTE_KIND 0x06u", "GB202 generic-memory PTE kind"),
        ("3u | (1u << 8) | (1u << 9) | (1u << 12)", "CFB7 codec/force-picture/force-coloc/timer control"),
        ("NVENC produced bounded Annex-B IDR output", "IDR smoke-test success gate"),
    ]:
        if token not in ours_text:
            print("MISMATCH source lacks %s: %s" % (label, token)); bad += 1
    nvenc_test = ours_text[ours_text.find("static int nvenc_encode_frame_hw"):
                           ours_text.find("/* ------------------------------------------------------- NVDEC")]
    if "3u | (1u << 4)" in nvenc_test:
        print("MISMATCH NVENC control uses bit 4 instead of GPTIMER bit 12")
        bad += 1
    if bad:
        return 1
    print("%d CFB7 methods match clc9b7.h; host-RM and fallback source order checked; magic 0x%08X" % (len(NAMES),magic_value))
    print("Static source checks only; native NVENC success is NOT established")
    return 0

if __name__ == "__main__":
    sys.exit(main())
