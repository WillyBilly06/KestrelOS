#ifndef KESTREL_NVRM_TELEMETRY_H
#define KESTREL_NVRM_TELEMETRY_H
/* Host RM boundary: percentages in hundredths, not host fence wait time.
 * Index order is GR, NVENC, NVDEC. No Copy-engine counter in this query. */
u32 nvrm_host_read_utilization(u32 client, u32 subdevice,
                              u32 utilization[3], u64 *timestamp);
/* Independent timestamped RUSD readings; unknown engines remain UINT32_MAX.
 * VRAM is physical framebuffer capacity, never BAR aperture or shared RAM. */
typedef struct {
    u32 utilization[3];
    u64 timestamp, temperature_timestamp, vram_bytes;
    int temperature_c;
} nvrm_shared_telemetry_t;
u32 nvrm_host_read_shared_telemetry(u32 client, u32 device, u32 subdevice,
                                   nvrm_shared_telemetry_t *out);
/* Snapshot an already-published boot-lifetime mapping. No RM calls, allocation,
 * render transaction or GPU submission. Setup above still needs the RM guard. */
u32 nvrm_host_peek_shared_telemetry(u32 client, u32 device, u32 subdevice,
                                   nvrm_shared_telemetry_t *out);
#endif
