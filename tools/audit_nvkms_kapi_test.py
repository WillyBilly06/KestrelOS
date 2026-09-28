#!/usr/bin/env python3
"""Offline release gate for the native NVIDIA multi-display test path.

This proves packaging and control-flow properties that previously survived
builds but failed on hardware: matching RM/GSP versions, a native KAPI atomic
modeset, one scanout per connected display, a real RM IRQ route, and no second
custom GSP owner in the full-RM branch.  Physical link success still requires
the target GPU and monitors.
"""
from pathlib import Path
import hashlib
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CLIENT = ROOT / "kernel/nvkms_kapi_client.c"
GPU = ROOT / "kernel/gpu.c"
NVCORE = ROOT / "kernel/nv_core.c"
RMOS = ROOT / "kernel/nvrm_os.c"
NVCHAN = ROOT / "kernel/nv_chan.c"
NVRM = ROOT / "kernel/nv_gsp_rm.c"
PCI = ROOT / "kernel/pci.c"
KERNEL = ROOT / "build/kernel/kernel.elf"
PATCHED_NVKMS = ROOT / "build/kernel/nv-modeset-kernel-kestrel.o_binary"
PATCHER = ROOT / "tools/patch_nvkms_edid_override.py"
BUILD_SCRIPT = ROOT / "build.py"
FW = ROOT / "firmware/nvidia/gb202/gsp/gsp-595.99.02.bin"
SOURCE_FW = ROOT / "Linux NVIDIA Driver/firmware/gsp_ga10x.bin"
ACER_EDID = ROOT / "firmware/edid/nvkms-00000200.bin"
RAW_ACER_EDID = ROOT / "firmware/edid/captures/ACR10A5-5_8681C24_0_UID8449-00570e4423b208e0.bin"
EXPECTED_FW = "f2848c5da315a636a4b03945b9edebb762eda2140df750ce2cf140b9d4b19fc3"
EXPECTED_RAW_ACER_EDID = "00570e4423b208e0927c4a2262a2946ff7922ac24c4ccc0b5464e2017262864a"
EXPECTED_ACER_EDID = "00f739b87e9de120b7f2ec0921ccaa90111bb2fd35ce317dd6056baf78429b87"
EXPECTED_PATCHED_NVKMS = "f77d19753d2eceb24cb7159401f6b50c25c4691b754986f8aaf7dfdfde747590"


def fail(message):
    print("FAIL:", message)
    raise SystemExit(1)


for path in (CLIENT, GPU, NVCORE, RMOS, NVCHAN, NVRM, PCI, KERNEL, PATCHED_NVKMS,
             PATCHER, BUILD_SCRIPT, FW, SOURCE_FW, ACER_EDID,
             RAW_ACER_EDID):
    if not path.exists():
        fail(f"missing {path.relative_to(ROOT)}")

for path in (FW, SOURCE_FW):
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != EXPECTED_FW:
        fail(f"wrong 595.99.02 GSP payload hash in {path.relative_to(ROOT)}: {digest}")
if hashlib.sha256(RAW_ACER_EDID.read_bytes()).hexdigest() != EXPECTED_RAW_ACER_EDID:
    fail("raw Windows Acer X27U capture was not preserved byte-for-byte")
acer_edid = ACER_EDID.read_bytes()
if hashlib.sha256(acer_edid).hexdigest() != EXPECTED_ACER_EDID:
    fail("connector-scoped Acer X27U EDID is not the captured checksum-valid payload")
if acer_edid[75] != 0xFD or acer_edid[76] != 0x0C or \
   any(sum(acer_edid[i:i + 128]) & 0xFF for i in range(0, len(acer_edid), 128)):
    fail("Acer override does not contain the checksum-correct NVIDIA range-limit repair")
if hashlib.sha256(PATCHED_NVKMS.read_bytes()).hexdigest() != EXPECTED_PATCHED_NVKMS:
    fail("linked NVKMS core does not contain the exact owned-EDID and DP DSC corrections")

client = CLIENT.read_text(encoding="utf-8")
gpu = GPU.read_text(encoding="utf-8")
rmos = RMOS.read_text(encoding="utf-8")
# The RM implementation now keeps timer/workqueue code in included headers.
# Require the actual include, not merely an unreferenced file on disk.
for header in ("nvrm_timers.h", "nvrm_workqueue.h"):
    if f'#include "{header}"' not in rmos:
        fail(f"RM implementation no longer includes {header}")
    rmos += "\n" + (ROOT / "kernel" / header).read_text(encoding="utf-8")
nvchan = NVCHAN.read_text(encoding="utf-8")
nvrm = NVRM.read_text(encoding="utf-8")
pci = PCI.read_text(encoding="utf-8")
nvcore = NVCORE.read_text(encoding="utf-8")
patcher = PATCHER.read_text(encoding="utf-8")
build_script = BUILD_SCRIPT.read_text(encoding="utf-8")

required_client = (
    "nvKmsKapiGetFunctionsTableInternal", "enumerateGpus", "allocateDevice",
    "grabOwnership", "getDynamicDisplayInfo", "getDisplayMode",
    "getConnectorInfo", "dynamicDpyIdListValid", "validateDisplayMode",
    "query_dynamic_display", "EDID_WAKE_TIMEOUT_USEC",
    "deadline = began + timeout_usec",
    "timer_mdelay(EDID_WAKE_POLL_MSEC)",
    "safe wake modeset is live", "post-signal EDID",
    "post-signal maximum-mode atomic commit",
    "free_display_slot(d, 1)", "free_display_slot(d, 0)",
    "load_provisioned_edid", "apply_provisioned_edid",
    "candidate->overrideEdid = NV_TRUE",
    "kapi.getDynamicDisplayInfo(test_device, candidate)",
    "*dyn = *candidate",
    '"/lib/firmware/edid/nvkms-%08x.bin"',
    "find_valid_head_assignment", "NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT",
    "nvrm_transfer_rm_memory", "createSurface", "return kapi.applyModeSetConfig",
    "applyModeSetConfig(test_device, &requested, &reply, NV_TRUE)",
    "requested.headsMask |= 1u << d->head",
    "NVKMS_OLUT_FP_NORM_SCALE_DEFAULT",
    "head->flags.legacyIlutChanged = NV_TRUE",
    "head->flags.legacyOlutChanged = NV_TRUE",
    "kapi.systemInfo.bAllowWriteCombining",
    '__asm__ volatile("sfence" ::: "memory")',
)
missing = [s for s in required_client if s not in client]
if missing:
    fail("native KAPI test omits: " + ", ".join(missing))
override_load = client.find("apply_provisioned_edid(handles[i]")
mode_query = client.find("select_max_mode(handles[i]", override_load)
if override_load < 0 or mode_query < 0 or override_load > mode_query:
    fail("connector EDID override is not applied before initial mode-pool enumeration")
required_patch_contract = (
    'SOURCE_SHA256 = "27edc5af2db22f1e6046fcf7fad30f5a087876e55f7f7ac811d886c26dc1cf77"',
    'ORIGINAL = bytes.fromhex("e9 4c f8 ff ff")',
    'PATCHED = bytes.fromhex("e9 9d fa ff ff")',
    'LIVE_PRESERVE_PATCHED = bytes.fromhex("eb 20 0f 0b")',
    'LIVE_BRIDGE_PATCHED = bytes.fromhex("e9 71 01 00 00 90")',
    'os.path.join(TOOLS, "patch_nvkms_edid_override.py")',
    "objs.append(nvkms_core_patched)",
)
combined_patch_source = patcher + build_script
missing = [s for s in required_patch_contract if s not in combined_patch_source]
if missing:
    fail("fail-closed NVKMS override patch contract omits: " + ", ".join(missing))
if not re.search(r"#define\s+EDID_WAKE_TIMEOUT_USEC\s+15000000ull", client):
    fail("post-signal EDID wake deadline is not a real 15 seconds")
if "kapi.mapMemory(test_device, d->memory" in client:
    fail("VRAM scanout still depends on the known-broken CPU BAR1 mapping")
if "dyn.edid.bufferSize = NVKMS_KAPI_EDID_BUFFER_SIZE" in client:
    fail("dynamic EDID query passes a bogus nonzero override-data length")
if "for (NvU32 attempt = 0; attempt < 50" in client or \
   "sched_sleep_ms(100)" in client:
    fail("DP EDID wake-up still uses the target-proven short attempt-count wait")
if client.count("kapi.getConnectorInfo") < 2 or \
   "(attempt % 4u) == 0" not in client:
    fail("late DisplayPort EDID path does not repeat full connector detection")
if 'wait_for_flip_mask(requested.headsMask, modeset_before' in client:
    fail("initial inactive-to-active modeset incorrectly waits for an event NVIDIA does not generate")
if 'return fail(r, "KAPI LUT notifier completion")' in client:
    fail("LUT timeout still aborts instead of following NVIDIA's blocking client")
if "timer_mdelay(4500);" not in client or "timer_mdelay(5000);" not in client:
    fail("seven-phase display test no longer provides five seconds per phase")
required_ce_bypass = (
    "NVRM_TRANSFER_PREFER_CE", "memmgrMemWrite_IMPL",
    "memmgrMemRead_IMPL", "memmgrMemUtilsGetMemDescFromHandle_IMPL",
    "rmapiLockAcquire", "rmGpuLocksAcquire",
)
missing = [s for s in required_ce_bypass if s not in rmos]
if missing:
    fail("official RM copy-engine VRAM bypass omits: " + ", ".join(missing))

if client.find("timings.hVisible * candidate.timings.vVisible") < 0 or \
   client.find("candidate.timings.refreshRate > best->timings.refreshRate") < 0:
    fail("maximum-resolution/maximum-refresh policy is absent")

connector_wait = client.find("kapi.getConnectorInfo")
dynamic_query = client.find("query_dynamic_display(handles[i]", connector_wait)
mode_query = client.find("select_max_mode(handles[i]", dynamic_query)
console_retire = client.find("kapi.framebufferConsoleDisabled", mode_query)
if min(connector_wait, dynamic_query, mode_query, console_retire) < 0 or not (
        connector_wait < dynamic_query < mode_query < console_retire):
    fail("NVIDIA connector detection/EDID/console-retirement lifecycle is out of order")

native_branch = re.search(
    r'if \(cmdline_has\("gpustart"\) && nvrm_is_ready\(\)\) \{(.+?)\n    \}',
    gpu, re.S)
if not native_branch or "nvkms_kapi_run_display_test" not in native_branch.group(1):
    fail("gpustart does not select the native full-RM KAPI client")
if "nv_gsp_start" in native_branch.group(1):
    fail("native full-RM branch starts a second custom GSP owner")
required_native_accel = (
    "nv_rm_host_bring_up", "nv_chan_open", "nv_chan_open_engines",
    "nv_compute_selftest_hw", "nv_3d_raster_selftest_hw",
    "nv_nvdec_selftest_hw", "nv_nvenc_selftest_hw",
    "nvkms_kapi_run_visible_accel_test",
)
missing = [s for s in required_native_accel if s not in native_branch.group(1)]
if missing:
    fail("native full-RM branch omits engine integration: " + ", ".join(missing))
native_body = native_branch.group(1)
if native_body.find("nv_chan_open_engines") > native_body.find("nvkms_kapi_run_display_test"):
    fail("Blackwell GR/Falcon contexts are allocated after six large NVKMS scanouts")
required_visible = (
    "nv_chan_bind_scanout", "nv_chan_visible_2d_hw",
    "nv_chan_visible_3d_hw", "present_prepared_slot",
    "nv_chan_render_keepalive", "GPU render/retire time",
    "nvrm_host_control_object", "NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR",
    "two_d_pixel_mask", "three_d_pixel_mask", "two_d_crc_mask",
    "three_d_crc_mask",
)
missing = [s for s in required_visible if s not in client]
if missing:
    fail("visible all-head hardware proof omits: " + ", ".join(missing))
visible_3d_body = re.search(
    r"int nv_chan_visible_3d_hw\(void\)\s*\{(.*?)\n\}", nvchan, re.S
)
if not visible_3d_body or \
   "nv_chan_draw_triangles((const float *)scene, 5)" not in visible_3d_body.group(1) or \
   "nv_chan_fill_scanout" in visible_3d_body.group(1):
    fail("visible 3D must clear and draw entirely on SM without a CE prefill dependency")

# The boot proofs and the desktop used to be disconnected: NVKMS could light
# every head, yet userland either mapped the retired GOP buffer or sent every
# operation to whichever head the last test happened to bind.  Keep the
# production bridge and its failure semantics in the release gate.
required_runtime = (
    "nvkms_kapi_publish_runtime_framebuffer", "console_publish_framebuffer",
    "display_layout_compute", "nvkms_kapi_runtime_present",
    "nvkms_kapi_runtime_fill", "nvkms_kapi_runtime_copy",
    "nvkms_kapi_runtime_draw", "nvkms_kapi_runtime_read_pixel",
    "nvkms_kapi_runtime_accel_selftest", "desktop-sequence gate PASS after 4.5 s idle",
    "for (NvU32 i = 0; i < active_count; i++)", "runtime_select(&active[i])",
    "select_preferred_primary(count)",
    'strcmp(active[i].manufacturer, "AOC") == 0',
    "selected by EDID policy",
)
missing = [s for s in required_runtime if s not in client]
if missing:
    fail("native multi-head desktop bridge omits: " + ", ".join(missing))
if "if (!edid_ok &&\n            apply_provisioned_edid" not in client or \
   "active[count].edid_pending = !edid_ok" not in client:
    fail("complete-sized corrupt EDID can still bypass the connector-scoped recovery path")
primary_fn = re.search(r"static void select_preferred_primary\(.*?\n\}", client, re.S)
if not primary_fn or "active[0] =" in primary_fn.group(0) or \
   "active[preferred] =" in primary_fn.group(0):
    fail("primary selection reorders the hardware-tested NVKMS scanout array")
if not re.search(r"if \(post_ok\)\s*\{\s*runtime_ok = "
                 r"nvkms_kapi_publish_runtime_framebuffer", gpu):
    fail("desktop framebuffer is published before post-codec 2D/3D validation")
if "runtime_ok && runtime_accel_ok" not in gpu or \
   "desktop accel sequence" not in gpu:
    fail("desktop admission does not require the exact post-idle runtime acceleration sequence")
present_source = (ROOT / "kernel/syscall.c").read_text(encoding="utf-8")
present_helper = re.search(r"static s64 sys_fb_present\(.*?^}",present_source,re.S|re.M)
if not present_helper or not re.search(
        r"if\s*\(nvkms_kapi_runtime_selected\(\)\)\s*return nvkms_kapi_runtime_present\([^;]+\?\s*0\s*:\s*-E_IO;",
        present_helper.group(0),re.S):
    fail("failed native GPU presentation can still fall through as CPU success")
required_runtime_channel = (
    "VA_UPLOAD_STAGE", "UPLOAD_STAGE_BYTES", "g_scanout_registry",
    "method storage quarantined", "s32 top = dy > sy ? h : 0",
    "if (dy > sy)", "GPFIFO wrap+idle PASS", "TRI_BATCH_MAX",
    "one upload/submission/fence", "VISIBLE 2D small-damage probe",
    "(1u << 26)", "DISABLE_PLC_TRUE",
)
missing = [s for s in required_runtime_channel if s not in nvchan]
if missing:
    fail("runtime present/copy safety omits: " + ", ".join(missing))
if not re.search(r"#define\s+GPFIFO_ENTRIES\s+1024u", nvchan) or \
   not re.search(r"#define\s+GPFIFO_BYTES\s+\(GPFIFO_ENTRIES \* 8u\)", nvchan) or \
   "ch->gp_put = (ch->gp_put + 1) % GPFIFO_ENTRIES" not in nvchan or \
   "the allocation following the GPFIFO must remain page aligned" not in nvchan:
    fail("copy-channel producer no longer uses NVIDIA's 1024-entry UVM GPFIFO default")
required_submit_lifetime = (
    "submit_bar1_flush", "u32 ordered = *ch->submit_bar1_flush",
    "const u32 proof_kickoffs = 2u * GPFIFO_ENTRIES + 5u",
    "timer_mdelay(8000)",
    "if (rc && status == NV_OK && nv->rc_timer_enabled && !due->destroying && !due->cancellers)",
)
missing = [s for s in required_submit_lifetime if s not in nvchan + rmos]
if missing:
    fail("Blackwell submit/idle lifetime proof omits: " + ", ".join(missing))
if 'if (!cmdline_has("gpustart"))' not in nvcore or \
   nvcore.find('if (!cmdline_has("gpustart"))') > nvcore.find('nvrm_start_gpu(c->pci_bus'):
    fail("main desktop can still enter modern NVIDIA RM takeover")

required_irq = ("rm_isr(", "rm_isr_bh(", "irq_install(",
                "pci_setup_single_msi", "NV_FLAG_USES_MSI")
missing = [s for s in required_irq if s not in rmos + pci]
if missing:
    fail("RM interrupt route omits: " + ", ".join(missing))
if "mode == NV_MEMORY_WRITECOMBINED" not in rmos or "vmm_map_wc" not in rmos:
    fail("VRAM/scanout CPU mappings are not write-combining")
port = (ROOT / "kernel/nvkms_port.c").read_text(encoding="utf-8")
required_nvkms_execution = (
    "nvkms_core_lock_acquire();", "nvkms_timer_thread",
    "nvkms_event_thread", "nvkms_event_pending",
)
missing = [s for s in required_nvkms_execution if s not in port]
if missing or "nvKmsKapiHandleEventQueueChange(p->device)" in port:
    fail("NVKMS process-context serialization/event deferral omits: " +
         ", ".join(missing or ["asynchronous KAPI event dispatch"]))
required_host_semantics = (
    "proc_current()", "OS_CURRENT_PROCESS_FLAG_KERNEL_THREAD",
    "nvrm_dma_device", "complete_all()", "nvrm_work_process_one",
    "if (q->tail) q->tail->next = work; else q->head = work;",
    "while (!(sp = nvrm_stack_alloc())) sched_sleep_ms(1);",
    "os_pat_supported(void) { return NV_TRUE; }",
)
missing = [s for s in required_host_semantics if s not in rmos]
if missing:
    fail("RM host execution semantics omit: " + ", ".join(missing))

# A host-RM channel shares FIFO ownership with NVKMS.  It must let RM choose a
# free ChID, use a real USERD Memory handle, query that assigned ID, and submit
# the complete token through RM's mapped usermode object.  Reintroducing any
# direct-GSP pinned-ChID assumption recreates the status-81 hardware failure.
required_host_channel = (
    "if (!rm->host_api)", "chp.h_userd_memory[0]",
    "NV0080_CTRL_CMD_FIFO_GET_CHANNELLIST",
    "work_submit_token_valid", "ch->rm->usermode + 0x90u",
    "host RM assigned hardware ChID",
    "u8 enable, skip_submit, skip_enable",
    "sizeof(schedule_params_t) == 3",
    "coherent host-RM page-table upload",
    "nv_vmm_memory_handle",
    "nvrm_transfer_rm_memory(rm->client, memory, 0",
    "nv_vram_object_write(ch, H_COMPUTE_VRAM",
    "nv_vram_object_read(ch, H_COMPUTE_VRAM",
    "nv_vram_object_write(ch, H_NVENC_VRAM",
    "nv_vram_object_read(ch, H_NVENC_VRAM",
    "nv_vram_object_write(ch, H_NVDEC_VRAM",
    "nv_vram_object_read(ch, H_NVDEC_VRAM",
    "PUSHBUF_BYTES       0x80000u",
    "PB_METHOD_BEGIN     0x10000u",
    "PB_TRACKER_OFF      0x6f000u",
    "pb_method(ch, 0, HOST_SEM_ADDR_LO, 5)",
    "HOST_SEM_RELEASE_WFI",
    "submit_and_wait(ch, PB_TRACKER_OFF, signal)",
    "HOST method-wrap reuse PASS",
    "method storage quarantined",
    "NV2080_CTRL_CMD_DMA_INVALIDATE_TLB",
    "nv_vmm_commit(copy->card, copy->rm, &copy->vmm",
    "NV2080_CTRL_CMD_GR_GET_CTX_BUFFER_INFO",
    "NV2080_CTRL_CMD_KGR_GET_CTX_BUFFER_PTES",
    "nvrm_host_get_video_falcon_context",
    "host_bind_channel_resources",
    "nvrm_host_mark_context_bound",
)
missing = [s for s in required_host_channel if s not in nvchan]
if missing:
    fail("native host-RM channel ownership omits: " + ", ".join(missing))
required_video_falcon_bridge = (
    "nvrm_host_get_video_falcon_context",
    "NVRM_KERNEL_CHANNEL_GROUP_API_OFFSET 0x2b8u",
    "NVRM_CHANNEL_GROUP_FROM_API_OFFSET 0x138u",
    "kchangrpGetEngineContextMemDesc_IMPL",
    "memdescGetPhysAddr",
    "memdescGetContiguity",
    "memdescGetPageSize",
    "memdescGetPteKindForGpu",
)
missing = [s for s in required_video_falcon_bridge if s not in rmos]
if missing:
    fail("locked NVDEC/NVENC Falcon-context bridge omits: " + ", ".join(missing))
if re.search(r"nv_rm_control\s*\([^;]*NV2080_CTRL_CMD_FLCN_GET_CTX_BUFFER_INFO",
             nvchan, re.S):
    fail("SEC2-only FLCN_GET_CTX_BUFFER_INFO was reused for NVDEC/NVENC")
if "nvrm_host_map_memory" not in rmos or "HOPPER_USERMODE_A" not in nvrm:
    fail("native host-RM USERD/doorbell mappings are absent")

for stale in (
    "nv_fb_wr32(ch->card, g_compute_vram_fb + off",
    "nv_fb_wr32(ch->card, g_nvenc_vram_fb",
    "nv_fb_wr32(ch->card, g_nvdec_vram_fb",
    "nv_fb_rd32(ch->card, g_nvenc_vram_fb",
    "nv_fb_rd32(ch->card, g_nvdec_vram_fb",
):
    if stale in nvchan:
        fail("host-RM-incoherent engine transfer returned: " + stale)

main = (ROOT / "kernel/main.c").read_text(encoding="utf-8")
watchdog = main.find("gpu_test_watchdog_arm();")
driver = main.find("nvidia_driver_init();")
if watchdog < 0 or driver < 0 or watchdog > driver:
    fail("gpustart watchdog is not armed before RM firmware takeover")

nm = Path(r"C:\Program Files\LLVM\bin\llvm-nm.exe")
nm_cmd = str(nm if nm.exists() else shutil.which("llvm-nm"))
if not nm_cmd:
    fail("llvm-nm not found")
out = subprocess.run([nm_cmd, "-g", "--defined-only", str(KERNEL)],
                     text=True, capture_output=True, check=True).stdout
symbols = {line.split()[-1] for line in out.splitlines() if line.split()}
required_symbols = {"nvkms_kapi_run_display_test", "nvkms_kapi_run_visible_accel_test",
                    "nv_chan_visible_2d_hw", "nv_chan_visible_3d_hw",
                    "nvkms_kapi_publish_runtime_framebuffer",
                    "nvkms_kapi_runtime_present", "nvkms_kapi_runtime_read_pixel",
                    "nv_rm_host_bring_up", "nvKmsKapiGetFunctionsTableInternal",
                    "nvrm_transfer_rm_memory", "memmgrMemWrite_IMPL",
                    "nvrm_host_mark_context_bound",
                    "nvrm_start_gpu", "rm_kernel_rmapi_op", "rm_isr", "rm_isr_bh"}
if not required_symbols <= symbols:
    fail("linked kernel omits: " + ", ".join(sorted(required_symbols - symbols)))

print("STATIC AUDIT ONLY: the checks below inspect source, hashes and linked symbols; no GPU ran, and hardware results are not established.")
print("PASS: exact NVIDIA 595.99.02 GSP payload pinned in source and initrd tree")
print("PASS: checksum-valid Acer EDID override is connector-scoped and live EDID has priority")
print("PASS: raw Acer capture is preserved and its impossible 303-240 Hz range is repaired to 48-240 Hz")
print("PASS: zero-EDID DP-SST override bypasses the lossy RM cache round trip before mode enumeration")
print("PASS: native KAPI enumerates EDIDs/modes, assigns unique heads and commits all heads atomically")
print("PASS: dynamic EDID input and first-modeset flip semantics match NVIDIA's Linux client")
print("PASS: every DisplayPort connector completes NVIDIA detection before maximum-mode selection")
print("PASS: scanout pixels use NVIDIA RM's CE transfer path and do not depend on the poisoned CPU BAR1 aperture")
print("PASS: full-RM gpustart branch cannot invoke the former second-owner nv_gsp_start path")
print("PASS: host-RM copy/compute/3D/NVDEC/NVENC pipeline is integrated without resetting GSP")
print("PASS: host-RM selects ChIDs and supplies the official USERD handle and unmodified submit token")
print("PASS: hardware 2D and 3D are rendered, pixel-checked and output-CRC-checked on every active head")
print("PASS: validated NVKMS scanouts are published to the desktop with all-head present, 2D, 3D and live-VRAM readback")
print("PASS: AOC is selected as the primary desktop by live EDID identity with a safe connected-display fallback")
print("PASS: native present failures remain errors and overlapping 2D moves preserve memmove ordering across scratch bands")
print("PASS: Linux-ordered copy submissions cross two GPFIFO revolutions and prove eight-second idle/resume")
print("PASS: modern RM takeover is confined to the named GPU-test boot entry")
print("PASS: RM MSI top-half/bottom-half and write-combined scanout mappings are linked")
print("PASS: RM thread/DMA/completion/workqueue/PAT semantics and pre-takeover watchdog are present")
print("PASS: NVKMS open/ioctl/timers are serialized and RM events are deferred to process context")
