[CmdletBinding()]
param([switch]$SkipHostTests)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$image = Join-Path $root 'out\kestrelos.img'
$gpuImage = Join-Path $root 'out\kestrelos-gputest.img'
$gpuKernel = Join-Path $root 'out\gputest-kernel.elf'
$gpuInitrd = Join-Path $root 'out\gputest-initrd.kar'
$gpuLoader = Join-Path $root 'out\gputest-loader.efi'

function Restore-EnvironmentValue([string]$Name, [object]$Value) {
    if ($null -eq $Value) {
        Remove-Item -LiteralPath "Env:$Name" -ErrorAction SilentlyContinue
    } else {
        Set-Item -LiteralPath "Env:$Name" -Value ([string]$Value)
    }
}

$oldDefault = [Environment]::GetEnvironmentVariable('KESTREL_DEFAULT_ENTRY', 'Process')
$oldDisplay = [Environment]::GetEnvironmentVariable('KESTREL_DISPLAYMODE', 'Process')
$oldCmdline = [Environment]::GetEnvironmentVariable('KESTREL_CMDLINE', 'Process')

Push-Location $root
try {
    if ($SkipHostTests) {
        Write-Warning 'Host tests explicitly skipped. This candidate has no runtime validation.'
    } else {
    & python (Join-Path $root 'tools\test_nvrm_shared_telemetry.py') --run
    if ($LASTEXITCODE -ne 0) { throw "shared telemetry regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_nvkms_display_evidence.py')
    if ($LASTEXITCODE -ne 0) { throw "display evidence regression exited $LASTEXITCODE" }
    foreach ($check in @('test_nvkms_output_mode.py', 'test_output_mode_syscall.py', 'test_output_mode_settings.py', 'test_rtw89_security.py', 'test_rtw89_fwdl_transport.py', 'test_rtw89_fwdl_preinit.py', 'test_rtw89_fwdl_stop.py', 'test_rtw89_device_routing.py', 'test_rtw89_rf_io.py', 'test_rtw89_radio_tables.py', 'test_rtw89_radio_gain_power.py', 'test_rtw89_radio_admission.py', 'test_rtw89_mac_runtime.py')) {
        & python (Join-Path $root "tools\$check")
        if ($LASTEXITCODE -ne 0) { throw "$check exited $LASTEXITCODE" }
    }
    & python (Join-Path $root 'tools\test_nvrm_dp_link.py')
    if ($LASTEXITCODE -ne 0) { throw "DP host readback regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_rtw89_packet_memory.py')
    if ($LASTEXITCODE -ne 0) { throw "Wi-Fi packet-memory regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_rtw89_cpu_restart.py')
    if ($LASTEXITCODE -ne 0) { throw "Wi-Fi CPU restart regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_gui_text_overlap_clip.py')
    if ($LASTEXITCODE -ne 0) { throw "clipped text diagnostics regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_settings_wifi_status.py')
    if ($LASTEXITCODE -ne 0) { throw "Wi-Fi scan status regression exited $LASTEXITCODE" }
    # This codec diagnostic scaffold only compiles/links unless --run is given.
    & python (Join-Path $root 'tools\test_nvenc_timeout_status.py')
    if ($LASTEXITCODE -ne 0) { throw "NVENC timeout status compile check exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_process_vm_lock.py')
    if ($LASTEXITCODE -ne 0) { throw "VM metadata concurrency regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_process_memory.py')
    if ($LASTEXITCODE -ne 0) { throw "memory syscall regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_libc_heap_threads.py')
    if ($LASTEXITCODE -ne 0) { throw "libc heap concurrency regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_libc_thread_slots.py')
    if ($LASTEXITCODE -ne 0) { throw "libc thread startup regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_linux_thread_lifecycle.py')
    if ($LASTEXITCODE -ne 0) { throw "Linux thread lifecycle regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_vmm_tid_word.py')
    if ($LASTEXITCODE -ne 0) { throw "TID word VM permission regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_linux_wait_abi.py')
    if ($LASTEXITCODE -ne 0) { throw "Linux wait/time ABI regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_thread_runtime_metadata.py')
    if ($LASTEXITCODE -ne 0) { throw "TLS/metadata syscall regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_nvkms_timers.py')
    if ($LASTEXITCODE -ne 0) { throw "NVKMS timer lifecycle regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_process_stop.py')
    if ($LASTEXITCODE -ne 0) { throw "process stop lifecycle regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_kernel_entry_lifecycle.py')
    if ($LASTEXITCODE -ne 0) { throw "kernel entry lifecycle regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_blocking_io_stop.py')
    if ($LASTEXITCODE -ne 0) { throw "blocking I/O stop regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_process_fds.py')
    if ($LASTEXITCODE -ne 0) { throw "descriptor lifetime concurrency regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_vfs_user_io.py')
    if ($LASTEXITCODE -ne 0) { throw "user-buffer I/O lifetime regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_vfs_user_ioctl.py')
    if ($LASTEXITCODE -ne 0) { throw "device-control user-buffer regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_audio_io_stop.py')
    if ($LASTEXITCODE -ne 0) { throw "audio I/O stop regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_pipe_concurrency.py')
    if ($LASTEXITCODE -ne 0) { throw "pipe concurrency regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_nvrm_sync.py')
    if ($LASTEXITCODE -ne 0) { throw "RM host synchronization regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_nvrm_workqueue.py')
    if ($LASTEXITCODE -ne 0) { throw "RM work-queue regression exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\test_nvrm_timers.py')
    if ($LASTEXITCODE -ne 0) { throw "RM timer lifetime regression exited $LASTEXITCODE" }
    # Fail before replacing snapshots if codec ABI, programmable admission,
    # actual upload/launcher/failure-gate or syscall snapshot checks regress. These are host
    # checks, not a substitute for native encode/decode validation on the USB.
    foreach ($check in @('test_scheduler_boot_progress.py','test_scheduler_device_wait.py','test_cpu_topology.py','test_taskmgr_graph.py','test_taskmgr_gpu.py','test_gpu_telemetry_validity.py','test_framebuffer_dispatch.py','test_display_configuration_syscall.py','test_display_prepare_syscall.py','test_nvkms_prepare.py','test_nvkms_hotplug.py','test_nvkms_edid_binding.py','test_vulkan_mesh.py')) {
        & python (Join-Path $root "tools\$check")
        if ($LASTEXITCODE -ne 0) { throw "$check exited $LASTEXITCODE" }
    }
    foreach ($check in @('test_build_dependencies.py', 'test_rtw89_firmware.py', 'test_btusb_startup.py', 'test_btusb_boot_model.py', 'test_xhci_service_startup.py', 'test_platform_startup.py', 'test_usbaudio_formats.py', 'test_usbhid_startup.py', 'verify_nv_nvenc.py', 'test_nvenc_cfb7_abi.py', 'test_nvenc_submission.py', 'test_nvenc_output.py', 'test_nvenc_h264_stream.py',
                           'test_nvdec_submission.py', 'test_h264_decode.py', 'test_nvdec_h264_context.py', 'test_nv_codec_wait.py', 'test_nv_codec_notifier.py', 'test_nv_channel_fault.py',
                           'test_nv_video_layout.py', 'test_nvdec_h264_job.py', 'test_nvdec_user_decode.py',
                            'test_nvdec_user_lifetime.py', 'test_nvdec_user_vmm.py', 'test_nvdec_reference.py', 'test_gpu_video_syscall.py', 'test_video_app.py',
                           'test_nvenc_roundtrip.py', 'test_nvenc_ipcm_capacity.py', 'test_video_vram_setup.py',
                           'test_nvrm_userd_policy.py', 'test_nvrm_logging.py', 'test_nv_surface_shader.py', 'test_nv_surfaces.py', 'test_gpu_surface_syscall.py',
                          'test_nv_dispatch_layout.py', 'test_nv_surface_dispatch.py', 'test_nv_geometry_snapshot.py', 'test_window_hidden_paint.py', 'test_gpu_user_access.py', 'test_scheduler_idle_wake.py',
                          'test_gl_gpu_vm.py', 'test_nv_shader_raster.py',
                             'test_gl_shader_gpu.py', 'test_gl_shader_tiles.py', 'test_gl_frame_timing.py', 'test_desktop_api_lifecycle.py', 'test_gl_fixed_gpu.py', 'test_gui_gpu_backend.py',
                          'test_nvkms_display_discovery.py', 'test_display_inventory.py', 'test_display_arrangement.py', 'test_nv_surface_present.py', 'test_ui_controls.py', 'test_display_gpu_startup.py', 'test_desktop_hover_damage.py', 'test_window_frame_pacing.py',
                          'test_window_title_hover.py', 'test_window_animation_damage.py', 'test_window_background.py', 'test_gl_gpu.py', 'test_framebuffer_mapping.py', 'test_window_geometry.py', 'test_nvkms_config_builder.py', 'test_settings_navigation.py', 'test_settings_appearance.py', 'test_settings_sound_scale.py', 'test_ui_text_bounds.py')) {
        & python (Join-Path $root "tools\$check")
        if ($LASTEXITCODE -ne 0) { throw "$check exited $LASTEXITCODE" }
    }
    }
    # Prevent a stale reference set from making the first verification compare
    # this build against artifacts from an older GPU-test image.
    Remove-Item -LiteralPath $gpuKernel, $gpuInitrd, $gpuLoader -Force -ErrorAction SilentlyContinue

    $env:KESTREL_DEFAULT_ENTRY = 'gputest'
    $env:KESTREL_DISPLAYMODE = 'extend'
    $env:KESTREL_CMDLINE = ''

    & python (Join-Path $root 'build.py') all
    if ($LASTEXITCODE -ne 0) { throw "GPU-test build exited $LASTEXITCODE" }
    Copy-Item -LiteralPath $image -Destination $gpuImage -Force

    & python (Join-Path $root 'tools\verify_gputest_image.py')
    if ($LASTEXITCODE -ne 0) { throw "GPU-test image verifier exited $LASTEXITCODE" }

    # Snapshot the exact non-reproducible build artifacts embedded in the
    # dedicated image before rebuilding the ordinary main image.
    Copy-Item -LiteralPath (Join-Path $root 'build\kernel\kernel.elf') -Destination $gpuKernel -Force
    Copy-Item -LiteralPath (Join-Path $root 'build\initrd.kar') -Destination $gpuInitrd -Force
    Copy-Item -LiteralPath (Join-Path $root 'build\boot\BOOTX64.EFI') -Destination $gpuLoader -Force
} finally {
    Restore-EnvironmentValue 'KESTREL_DEFAULT_ENTRY' $oldDefault
    Restore-EnvironmentValue 'KESTREL_DISPLAYMODE' $oldDisplay
    Restore-EnvironmentValue 'KESTREL_CMDLINE' $oldCmdline
    Pop-Location
}

# Leave the ordinary image and Windows installer payload on their clean main
# configuration.  The dedicated image copied above is intentionally retained.
Push-Location $root
try {
    & python (Join-Path $root 'build.py') all
    if ($LASTEXITCODE -ne 0) { throw "main-image rebuild exited $LASTEXITCODE" }
    & python (Join-Path $root 'tools\verify_gputest_image.py')
    if ($LASTEXITCODE -ne 0) { throw "retained GPU-test image verifier exited $LASTEXITCODE" }
} finally {
    Pop-Location
}

if ($SkipHostTests) {
    Write-Host 'BUILD_GPUTEST_IMAGE_INTEGRITY_PASS_HOST_TESTS_SKIPPED'
} else {
    Write-Host 'BUILD_GPUTEST_IMAGE_PASS'
}
