/* nv_gsp_init.c - the two messages GSP-RM consumes during its own startup.
 *
 * NVIDIA R570's kgspQueueAsyncInitRpcs_IMPL and Nouveau's r535_gsp_oneinit()
 * agree on both the order and the reply policy:
 *
 *   72  GSP_SET_SYSTEM_INFO   (NOSEQ)
 *   73  SET_REGISTRY          (NOSEQ)
 *
 * They are present in the command ring before the GSP is bootstrapped.  This
 * is deliberately separate from the FSP chain-of-trust transport: FSP only
 * authenticates and starts the image; these records are host configuration
 * that GSP-RM reads while creating OBJGPU.
 */
#include "kernel.h"
#include "klog.h"
#include "mm.h"
#include "pci.h"
#include "proc.h"
#include "nv.h"

#define RPC_GSP_SET_SYSTEM_INFO 72u
#define RPC_SET_REGISTRY        73u

/* R570 GspSystemInfo is a naturally aligned, 928-byte x86-64 ABI structure.
 * Only fields the host can state authoritatively are populated; NVIDIA and
 * Nouveau allocate it zeroed and likewise leave the rest zero.  Keeping the
 * published offsets explicit avoids importing hundreds of unrelated RM/ACPI
 * types merely to describe zero-filled space. */
#define SYSTEM_INFO_BYTES                  928u
#define SI_GPU_PHYS_ADDR                     0u
#define SI_GPU_PHYS_FB_ADDR                  8u
#define SI_GPU_PHYS_INST_ADDR               16u
#define SI_DOMAIN_BUS_DEVICE_FUNC           32u
#define SI_MAX_USER_VA                      72u
#define SI_PCI_CONFIG_MIRROR_BASE           80u
#define SI_PCI_CONFIG_MIRROR_SIZE           84u
#define SI_PCI_DEVICE_ID                    88u
#define SI_PCI_SUBDEVICE_ID                 92u
#define SI_PCI_REVISION_ID                  96u
#define SI_FLR_SUPPORTED                   125u
#define SI_64BIT_BAR0_SUPPORTED            126u
#define SI_IS_PRIMARY                      896u
#define SI_PRESERVE_VIDEO_MEMORY           908u
#define SI_HOST_PAGE_SIZE                  920u

/* GB20x inherits GH100's endpoint configuration mirror.  Nouveau r570 uses
 * DRF_LO/H I(NV_EP_PCFGM), whose published range is 0x92000..0x92fff. */
#define GB20X_PCI_CONFIG_MIRROR_BASE 0x00092000u
#define GB20X_PCI_CONFIG_MIRROR_SIZE 0x00001000u

_Static_assert(SI_HOST_PAGE_SIZE + sizeof(u64) == SYSTEM_INFO_BYTES,
               "the final R570 system-info field must end at byte 928");

static void put32(u8 *p, u32 at, u32 value) { memcpy(p + at, &value, sizeof value); }
static void put64(u8 *p, u32 at, u64 value) { memcpy(p + at, &value, sizeof value); }
static u32 get32(const u8 *p, u32 at) { u32 v; memcpy(&v, p + at, sizeof v); return v; }
static u64 get64(const u8 *p, u32 at) { u64 v; memcpy(&v, p + at, sizeof v); return v; }

static void build_system_info(const nv_card_t *c, const pci_dev_t *d,
                              bool flr_supported, u64 boot_fb,
                              u8 out[SYSTEM_INFO_BYTES]) {
    memset(out, 0, SYSTEM_INFO_BYTES);

    put64(out, SI_GPU_PHYS_ADDR, d->bar[0]);
    put64(out, SI_GPU_PHYS_FB_ADDR, d->bar[1]);
    put64(out, SI_GPU_PHYS_INST_ADDR, d->bar[2]);
    /* Linux pci_dev_id(), used by Nouveau here, is bus:devfn without the PCI
     * segment despite the RM field's historical name. */
    put64(out, SI_DOMAIN_BUS_DEVICE_FUNC,
          ((u64)d->bus << 8) | ((u64)d->slot << 3) | d->func);
    put64(out, SI_MAX_USER_VA, USER_STACK_TOP);
    put32(out, SI_PCI_CONFIG_MIRROR_BASE, GB20X_PCI_CONFIG_MIRROR_BASE);
    put32(out, SI_PCI_CONFIG_MIRROR_SIZE, GB20X_PCI_CONFIG_MIRROR_SIZE);
    put32(out, SI_PCI_DEVICE_ID, ((u32)d->device << 16) | d->vendor);
    put32(out, SI_PCI_SUBDEVICE_ID,
          ((u32)d->subsys_device << 16) | d->subsys_vendor);
    put32(out, SI_PCI_REVISION_ID, d->revision);
    out[SI_FLR_SUPPORTED] = flr_supported ? 1 : 0;
    out[SI_64BIT_BAR0_SUPPORTED] = d->bar_is_64[0] ? 1 : 0;
    out[SI_IS_PRIMARY] = c && c->vram_base && boot_fb >= c->vram_base &&
                         boot_fb < c->vram_base + c->vram_aperture;
    out[SI_PRESERVE_VIDEO_MEMORY] = 0;
    put64(out, SI_HOST_PAGE_SIZE, PAGE_SIZE);
}

typedef struct {
    u32 name_offset;
    u8  type;
    u8  pad[3];
    u32 data;
    u32 length;
} registry_entry_t;

_Static_assert(sizeof(registry_entry_t) == 16,
               "R570 packed-registry entries are sixteen bytes");

typedef struct {
    const char *name;
    u32 value;
} registry_value_t;

static const registry_value_t required_registry[] = {
    { "RMSecBusResetEnable",   1 },
    { "RMForcePcieConfigSave", 1 },
    { "RMDevidCheckIgnore",    1 },
};

#define REGISTRY_ENTRIES 3u
#define REGISTRY_BYTES (8u + REGISTRY_ENTRIES * 16u + 20u + 22u + 19u)
_Static_assert(REGISTRY_BYTES == 117u,
               "the three required R570 registry values occupy 117 bytes");

static u32 build_registry(u8 out[REGISTRY_BYTES]) {
    memset(out, 0, REGISTRY_BYTES);
    put32(out, 0, REGISTRY_BYTES);
    put32(out, 4, REGISTRY_ENTRIES);

    registry_entry_t *entry = (registry_entry_t *)(out + 8);
    u32 string_at = 8u + REGISTRY_ENTRIES * (u32)sizeof(*entry);
    for (u32 i = 0; i < REGISTRY_ENTRIES; i++) {
        u32 len = (u32)strlen(required_registry[i].name) + 1;
        entry[i].name_offset = string_at;
        entry[i].type = 1;       /* REGISTRY_TABLE_ENTRY_TYPE_DWORD */
        entry[i].data = required_registry[i].value;
        entry[i].length = sizeof(u32);
        memcpy(out + string_at, required_registry[i].name, len);
        string_at += len;
    }
    return string_at;
}

static bool queue_built_init_rpcs(nv_gsp_queue_t *command,
                                  const u8 system_info[SYSTEM_INFO_BYTES],
                                  const u8 registry[REGISTRY_BYTES]) {
    if (!nv_gsp_rpc_enqueue_preboot(command, RPC_GSP_SET_SYSTEM_INFO,
                                    system_info, SYSTEM_INFO_BYTES))
        return false;
    if (!nv_gsp_rpc_enqueue_preboot(command, RPC_SET_REGISTRY,
                                    registry, REGISTRY_BYTES))
        return false;
    return true;
}

static pci_dev_t *card_pci(const nv_card_t *c) {
    pci_dev_t *d = NULL;
    while ((d = pci_find(0x03, 0xFF, 0xFF, d)) != NULL)
        if (d->bus == c->pci_bus && d->slot == c->pci_slot &&
            d->func == c->pci_func)
            return d;
    return NULL;
}

bool nv_gsp_queue_async_init_rpcs(nv_card_t *c, nv_gsp_queue_t *command) {
    if (!c || !command || !command->ready) return false;
    pci_dev_t *d = card_pci(c);
    if (!d) {
        kerr("nv-gsp", "cannot describe the card to GSP-RM: its PCI function "
                       "is no longer present");
        return false;
    }

    bool flr = false;
    if (d->cap_pcie) {
        u32 devcap = pci_read32(d, d->cap_pcie + 0x04);
        flr = (devcap & (1u << 28)) != 0;
    }

    u8 system_info[SYSTEM_INFO_BYTES];
    u8 registry[REGISTRY_BYTES];
    build_system_info(c, d, flr, g_boot.fb.base, system_info);
    if (build_registry(registry) != REGISTRY_BYTES) return false;

    if (!queue_built_init_rpcs(command, system_info, registry)) {
        kerr("nv-gsp", "the command ring could not hold the two asynchronous "
                       "initialisation RPCs");
        return false;
    }

    kinfo("nv-gsp", "queued R570 early-init RPCs before bootstrap: system "
                    "info (%u bytes), then registry (%u bytes), both NOSEQ",
          SYSTEM_INFO_BYTES, REGISTRY_BYTES);
    return true;
}

int nv_gsp_init_rpc_selftest(void) {
    int failures = 0;
    nv_card_t c;
    pci_dev_t d;
    memset(&c, 0, sizeof c);
    memset(&d, 0, sizeof d);
    d.bus = 0x42; d.slot = 3; d.func = 2;
    d.vendor = 0x10DE; d.device = 0x2C05;
    d.subsys_vendor = 0x1458; d.subsys_device = 0x4198;
    d.revision = 0xA1;
    d.bar[0] = 0x123400000ULL;
    d.bar[1] = 0x800000000ULL;
    d.bar[2] = 0x234500000ULL;
    d.bar_is_64[0] = true;
    c.vram_base = d.bar[1];
    c.vram_aperture = 0x10000000;

    u8 system_info[SYSTEM_INFO_BYTES];
    u8 registry[REGISTRY_BYTES];
    build_system_info(&c, &d, true, d.bar[1] + 0x1000, system_info);
    u32 registry_len = build_registry(registry);

    if (get64(system_info, SI_GPU_PHYS_ADDR) != d.bar[0] ||
        get64(system_info, SI_GPU_PHYS_FB_ADDR) != d.bar[1] ||
        get64(system_info, SI_GPU_PHYS_INST_ADDR) != d.bar[2] ||
        get64(system_info, SI_DOMAIN_BUS_DEVICE_FUNC) != 0x421Au ||
        get32(system_info, SI_PCI_DEVICE_ID) != 0x2C0510DEu ||
        get32(system_info, SI_PCI_SUBDEVICE_ID) != 0x41981458u ||
        get32(system_info, SI_PCI_REVISION_ID) != 0xA1u ||
        !system_info[SI_FLR_SUPPORTED] ||
        !system_info[SI_64BIT_BAR0_SUPPORTED] ||
        !system_info[SI_IS_PRIMARY] ||
        get64(system_info, SI_HOST_PAGE_SIZE) != PAGE_SIZE) {
        kerr("nv-gsp", "the R570 system-info RPC fields are at the wrong "
                        "offsets or carry the wrong PCI identity");
        failures++;
    }

    if (registry_len != REGISTRY_BYTES ||
        get32(registry, 0) != REGISTRY_BYTES ||
        get32(registry, 4) != REGISTRY_ENTRIES) {
        kerr("nv-gsp", "the R570 registry table header is wrong");
        failures++;
    } else {
        const registry_entry_t *entry = (const registry_entry_t *)(registry + 8);
        for (u32 i = 0; i < REGISTRY_ENTRIES; i++) {
            if (entry[i].type != 1 || entry[i].data != 1 ||
                entry[i].length != sizeof(u32) ||
                entry[i].name_offset >= registry_len ||
                strcmp((const char *)registry + entry[i].name_offset,
                       required_registry[i].name)) {
                kerr("nv-gsp", "required registry entry %u is malformed", i);
                failures++;
                break;
            }
        }
    }

    /* Verify both complete RPCs land in source order and remain pending. */
    u64 phys = 0;
    u8 *rings = dma_alloc_pages(2 * NV_GSP_QUEUE_REGION_BYTES / PAGE_SIZE,
                                &phys);
    if (!rings) {
        kwarn("nv-gsp", "no memory for the early-init RPC ring test");
        return failures;
    }
    memset(rings, 0, 2 * NV_GSP_QUEUE_REGION_BYTES);
    nv_gsp_queue_t command;
    nv_gsp_cmdq_init(&command, rings, rings + NV_GSP_QUEUE_REGION_BYTES);
    if (!queue_built_init_rpcs(&command, system_info, registry)) {
        kerr("nv-gsp", "the two R570 early-init RPCs could not be staged");
        failures++;
    } else {
        const nv_gsp_rpc_header_t *first =
            (const nv_gsp_rpc_header_t *)(rings + PAGE_SIZE + 48);
        const nv_gsp_rpc_header_t *second =
            (const nv_gsp_rpc_header_t *)(rings + 2 * PAGE_SIZE + 48);
        if (*(const u32 *)(rings + 16) != 2 ||
            first->function != RPC_GSP_SET_SYSTEM_INFO ||
            first->sequence != 0 ||
            first->length != sizeof(*first) + SYSTEM_INFO_BYTES ||
            second->function != RPC_SET_REGISTRY ||
            second->sequence != 0 ||
            second->length != sizeof(*second) + REGISTRY_BYTES) {
            kerr("nv-gsp", "the R570 early-init RPCs are not pending in the "
                            "required order and NOSEQ framing");
            failures++;
        }
    }

    if (!failures)
        kinfo("nv-gsp", "the two R570 early-init RPCs are byte-laid out and "
                        "queued in NVIDIA's pre-bootstrap order");
    return failures;
}
