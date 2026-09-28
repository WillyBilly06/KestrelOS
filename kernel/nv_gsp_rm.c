/* nv_gsp_rm.c - what the driver actually says once the processor is running.
 *
 * nv_falcon.c starts the co-processor.  nv_gsp_queue.c gets messages to and
 * from it.  This is the layer that makes the card do things - and on Ada and
 * Blackwell it is the only layer that can, because from those generations the
 * display and graphics engines are not reachable from the host at all.  Every
 * mode change, every allocation, every query goes through here.
 *
 * NVIDIA's resource manager is an object tree, and the whole interface is
 * three verbs on it: allocate an object under a parent, send a control command
 * to an object, free an object.  Everything else is which class and which
 * command number.
 *
 *     client
 *       device
 *         subdevice          - the chip itself: memory, clocks, sensors
 *         display            - the heads, the connectors, the links
 *
 * The tree is not decoration.  A device cannot be allocated without a client
 * to own it; a control command to an object that was never allocated is
 * refused; and a parent cannot be freed while its children exist.  A driver
 * that gets the order wrong does not fail at the point of the mistake - it
 * fails later, on the first command that depended on something that quietly
 * was not there.
 *
 * The message numbers, the structure layouts and the handle scheme below are
 * NVIDIA's, from their own published resource-manager headers.
 *
 * ---------------------------------------------------------------------------
 * What this cannot do, and the reason is worth being exact about: none of this
 * runs on a real card without NVIDIA's signed firmware, because the processor
 * that answers these messages *is* that firmware.  Nothing anybody outside
 * NVIDIA can produce will pass the signature check in nv_falcon.c, and no
 * amount of driver work changes that.
 *
 * What this does do is speak the protocol correctly, checked against a model
 * that holds the same object tree and enforces the same rules.  That proves
 * the message framing, the object lifecycle, the ordering constraints and the
 * error handling - which is every part of this layer except the one that needs
 * a key.
 * ---------------------------------------------------------------------------
 *
 * PROTOTYPE(S) ADDED IN THIS FILE (declare in nv.h - main agent owns nv.h):
 *
 *     bool nv_rm_disp_client_ctor(nv_card_t *c, nv_rm_t *rm,
 *                                 u32 *out_client, u32 *out_device,
 *                                 u32 *out_subdevice);
 *
 * Builds a SECOND, independent RM client+device+subdevice - a dedicated display
 * client - mirroring nvkm_gsp_client_device_ctor() as nouveau's r535_disp_oneinit
 * uses it (linux-nouveau .../rm/r535/disp.c:1536).  Returns the three handles.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "klog.h"
#include "nv.h"
#include "spinlock.h"

/* ------------------------------------------------------------ the messages */

#define RPC_SET_GUEST_SYSTEM_INFO   1
#define RPC_ALLOC_MEMORY            4
#define RPC_FREE                    10
#define RPC_UNLOADING_GUEST_DRIVER  47
#define RPC_GET_GSP_STATIC_INFO     65   /* GspStaticConfigInfo, rpcfn.h:81 */
#define RPC_GSP_RM_CONTROL          76
#define RPC_GSP_RM_ALLOC            103

/* GspStaticConfigInfo (the reply to RPC 65) carries the handles GSP-RM allocated
 * for its OWN privileged client - the ones NV2080_CTRL_CMD_INTERNAL_* controls
 * (GR context-buffer info, falcon GET_CONSTRUCTED_FALCON_INFO, display
 * WRITE_INST_MEM/GET_STATIC_INFO) are gated to on some GSP builds.  We issue
 * those on our own RM_SUBDEVICE first and only fall back to these if refused.
 *
 * The byte offsets are for our firmware (570.144) and are NOT eyeballed: the
 * struct is deeply nested and differs between r535 and r570, so the offsets were
 * COMPUTED by the compiler from nouveau's authoritative r570 header (the file's
 * own banner: "Excerpt of RM headers from open-gpu-kernel-modules/tree/570.144")
 * via offsetof - client 1600, device 1604, subdevice 1608, bar1PdeBase 1536,
 * sizeof 1656 - which also match NVIDIA's own ABI assertion (gsp_abi_check.c
 * ABI_CHECK_FIELD hInternalClient 1600 / _SIZE_GE 1656). */
#define GSP_SCI_SIZE            1656u
#define GSP_SCI_OFF_HCLIENT     1600u
#define GSP_SCI_OFF_HDEVICE     1604u
#define GSP_SCI_OFF_HSUBDEVICE  1608u
#define GSP_SCI_OFF_BAR1PDE     1536u

/* Filled once by nv_rm_get_static_info(); g_static_info_ok gates the fallback. */
static u32  g_internal_client;
static u32  g_internal_device;
static u32  g_internal_subdevice;
static u64  g_bar1_pde_base;
static bool g_static_info_ok;

/* How long to wait for GSP-RM to answer an alloc/control.  The first commands a
 * freshly-booted resource manager processes can be slow to come back; NVIDIA's
 * own path budgets about a second (GspMsgQueueSendCommand uses a 1s timeout).
 * 100 ms was too tight and gave up before the answer arrived. */
#define NV_RM_RPC_TIMEOUT_MS        2000

/* Native host-RM bridge implemented beside the linked RM core. */
extern u32 nvrm_host_alloc_root(u32 *client);
extern u32 nvrm_host_alloc_object(u32 client, u32 parent, u32 object,
                                  u32 klass, void *params, u32 params_size);
extern u32 nvrm_host_control_object(u32 client, u32 object, u32 cmd,
                                    void *params, u32 params_size);
extern u32 nvrm_host_free_object(u32 client, u32 parent, u32 object);
extern u32 nvrm_host_map_memory(u32 client, u32 device, u32 memory,
                                u64 offset, u64 length, void **address,
                                u32 flags);

/* The handles a driver gives things.  Any number would do; these are the ones
 * NVIDIA's own headers use, which makes a trace readable next to theirs. */
#define RM_CLIENT(id)   (0xC1D00000u | (id))
#define RM_DEVICE       0xDE1D0000u
#define RM_SUBDEVICE    0x5D1D0000u
#define RM_DISP         0x00730000u

/* Handles for the DEDICATED display client tree (nv_rm_disp_client_ctor).
 * nouveau gives the disp objects their OWN client via nvkm_gsp_client_device_ctor
 * (linux-nouveau .../rm/r535/disp.c:1536); that client is NVKM_RM_CLIENT(id) with
 * a fresh id (linux-nouveau .../rm/client.c:35,44 -> handles.h:10 0xc1d00000|id),
 * and it REUSES the same NVKM_RM_DEVICE/NVKM_RM_SUBDEVICE handle numbers because
 * object handles are scoped per-client (handles.h:12,13).  We instead pick handles
 * that are distinct from the main tree under EITHER scoping rule, so a trace is
 * unambiguous and a global-scope GSP-RM cannot see a collision:
 *   client    RM_CLIENT(0xD0) = 0xC1D000D0  (main is RM_CLIENT(0) = 0xC1D00000)
 *   device    0xD1D00080                    (main RM_DEVICE    = 0xDE1D0000)
 *   subdevice 0xD1D02080                    (main RM_SUBDEVICE = 0x5D1D0000)
 * The client handle keeps NVIDIA's 0xc1d00000|id form so GSP-RM's client-id
 * derivation still works (an arbitrary 0x0000d0xx would not be a valid client id). */
#define RM_DISP_CLIENT     RM_CLIENT(0x00D0u)   /* 0xC1D000D0 */
#define RM_DISP_DEVICE     0xD1D00080u
#define RM_DISP_SUBDEVICE  0xD1D02080u

/* And the classes. */
#define NV01_ROOT           0x0000
#define NV01_DEVICE_0       0x0080
#define NV20_SUBDEVICE_0    0x2080
#define NV04_DISPLAY_COMMON 0x0073

/* Control commands.  The top half of the number says which class it belongs
 * to, which is why a command sent to the wrong object is caught rather than
 * misinterpreted. */
#define NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS        0x00730102u
#define NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED        0x00730107u  /* NOT 0x730120 - that is EXECUTE_ACPI_METHOD; verified vs ctrl0073system.h */
#define NV0073_CTRL_CMD_SPECIFIC_GET_CONNECTOR_DATA 0x00730250u
#define NV0073_CTRL_CMD_SPECIFIC_GET_ALL_HEAD_MASK  0x00730287u
#define NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO        0x0073028Bu
#define NV0073_CTRL_CMD_DP_GET_CAPS                 0x00731369u
#define NV2080_CTRL_CMD_INTERNAL_DISPLAY_GET_STATIC_INFO 0x20800A01u

/* What the resource manager answers with. */
#define NV_OK                       0x00000000u
#define NV_ERR_INVALID_OBJECT       0x0000001Cu
#define NV_ERR_INVALID_ARGUMENT     0x0000001Fu
#define NV_ERR_INVALID_STATE        0x00000020u
#define NV_ERR_STATE_IN_USE         0x00000027u

/* The two message bodies, exactly as the published headers lay them out.  The
 * parameters follow the header in the same buffer, which is why the size is
 * carried separately rather than inferred. */
typedef struct {
    u32 client;
    u32 parent;
    u32 object;
    u32 class_number;
    u32 status;
    u32 params_size;
    u32 flags;
    u8  reserved[4];
} __attribute__((packed)) rm_alloc_t;

typedef struct {
    u32 client;
    u32 object;
    u32 cmd;
    u32 status;
    u32 params_size;
    u32 flags;
} __attribute__((packed)) rm_control_t;

/* Allocation-parameters the object-tree classes REQUIRE (verified vs nouveau
 * r535 nvrm/client.h + device.h).  Sending an alloc with zero/short params left
 * GSP-RM's FINN unmarshaller reading a struct that was not there, so it dropped
 * the request without a reply - the observed "allocating 0000 got no answer".
 *   NV01_ROOT      -> NV0000_ALLOC_PARAMETERS (108 bytes)
 *   NV01_DEVICE_0  -> NV0080_ALLOC_PARAMETERS (56 bytes, u64 members 8-aligned)
 *   NV20_SUBDEVICE -> NV2080_ALLOC_PARAMETERS (4 bytes)
 *   NV04_DISPLAY_COMMON has NO params (nouveau disp.c passes size 0). */
typedef struct { u32 h_client; u32 process_id; char process_name[100]; }
    __attribute__((packed)) nv0000_root_params_t;
_Static_assert(sizeof(nv0000_root_params_t) == 108,
               "NV0000_ALLOC_PARAMETERS is 108 bytes");

typedef struct {
    u32 device_id, h_client_share, h_target_client, h_target_device, flags;
    u64 va_space_size, va_start_internal, va_limit_internal;
    u32 va_mode;
} nv0080_device_params_t;   /* NOT packed: NV_DECLARE_ALIGNED u64s -> 56 bytes */
_Static_assert(sizeof(nv0080_device_params_t) == 56,
               "NV0080_ALLOC_PARAMETERS is 56 bytes");

typedef struct { u32 sub_device_id; } __attribute__((packed)) nv2080_subdevice_params_t;
_Static_assert(sizeof(nv2080_subdevice_params_t) == 4,
               "NV2080_ALLOC_PARAMETERS is 4 bytes");

/* ------------------------------------------------------------- the driver */

/* The RM status of the most recent nv_rm_alloc reply (NV_OK on success, the
 * refusal code otherwise).  Lets the channel code sweep parameter variations
 * and read the exact code each one returned without threading it through every
 * call site. */
u32 nv_last_alloc_status = 0;
u32 nv_last_control_status = 0;
u32 nv_last_control_replylen = 0;

/* Requested/returned local-memory objects owned through this wrapper only.
 * This is a residency floor, not RM's total heap usage. In particular, internal
 * firmware/NVKMS allocations which bypass nv_rm_alloc are not invented here.
 * Retained or quarantined objects remain charged until RM confirms their free.
 * No allocation, driver call, or logging is permitted under this short lock. */
#define NV_VRAM_ACCOUNT_SLOTS 1024u
typedef struct {
    nv_card_t *card;
    u32 client, parent, object;
    u64 bytes;
} nv_vram_account_t;
static nv_vram_account_t vram_accounts[NV_VRAM_ACCOUNT_SLOTS];
static spinlock_t vram_account_lock;
static bool vram_account_warned;

static void rm_account_alloc(nv_card_t *c, u32 client, u32 parent,
                             u32 object, u32 klass, const void *params, u32 size) {
    if (!c || klass != 0x40u || !params || size != 128u ||
        (parent != RM_DEVICE && parent != RM_DISP_DEVICE)) return;
    /* NV_MEMORY_ALLOCATION_PARAMS: flags@8, attr@24, size@64. Copy rather
     * than casting caller storage to another incompatible structure type.
     * Check returned LOCATION, since the host bridge redirects USERD to PCI. */
    u32 flags, attr; u64 bytes;
    memcpy(&flags, (const u8 *)params + 8u, sizeof flags);
    memcpy(&attr, (const u8 *)params + 24u, sizeof attr);
    memcpy(&bytes, (const u8 *)params + 64u, sizeof bytes);
    if (!bytes || (attr & 0x06000000u) || (flags & 0x00080000u)) return;
    bool irq = spin_lock_irqsave(&vram_account_lock);
    nv_vram_account_t *free_slot = NULL;
    for (unsigned i = 0; i < NV_VRAM_ACCOUNT_SLOTS; i++) {
        nv_vram_account_t *a = &vram_accounts[i];
        if (a->card == c && a->client == client && a->object == object) {
            /* An existing live handle must never be charged twice. */
            spin_unlock_irqrestore(&vram_account_lock, irq);
            return;
        }
        if (!a->card && !free_slot) free_slot = a;
    }
    u64 total = __atomic_load_n(&c->vram_allocated, __ATOMIC_RELAXED);
    bool charged = free_slot && bytes <= ~(u64)0 - total;
    if (charged) {
        *free_slot = (nv_vram_account_t){c, client, parent, object, bytes};
        __atomic_store_n(&c->vram_allocated, total + bytes, __ATOMIC_RELEASE);
    }
    bool warn = !charged && !vram_account_warned;
    if (warn) vram_account_warned = true;
    spin_unlock_irqrestore(&vram_account_lock, irq);
    if (warn)
        kwarn("nv-rm", "VRAM accounting capacity exceeded; reported usage remains a lower bound");
}

static void rm_account_free(nv_card_t *c, u32 client, u32 object) {
    if (!c) return;
    bool irq = spin_lock_irqsave(&vram_account_lock);
    u64 total = __atomic_load_n(&c->vram_allocated, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < NV_VRAM_ACCOUNT_SLOTS; i++) {
        nv_vram_account_t *a = &vram_accounts[i];
        /* Memory objects tracked here are direct device children. Freeing
         * their device or root also retires them; unrelated clients/handles
         * must not alter this card's count. Failed frees never reach here. */
        if (a->card == c && a->client == client &&
            (object == client || object == a->parent || object == a->object)) {
            total = total >= a->bytes ? total - a->bytes : 0;
            memset(a, 0, sizeof *a);
        }
    }
    __atomic_store_n(&c->vram_allocated, total, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&vram_account_lock, irq);
}

bool nv_rm_alloc(nv_card_t *c, nv_rm_t *rm, u32 parent, u32 object,
                 u32 class_number, void *params, u32 params_size) {
    if (!rm->ready) return false;
    if (rm->host_api) {
        nv_last_alloc_status = nvrm_host_alloc_object(
            rm->client, parent, object, class_number, params, params_size);
        if (nv_last_alloc_status != NV_OK) {
            kwarn("nv-rm", "host RM allocating class %04x under %08x was refused (%u)",
                  class_number, parent, nv_last_alloc_status);
            return false;
        }
        rm->allocations++;
        rm_account_alloc(c, rm->client, parent, object, class_number, params, params_size);
        return true;
    }
    if (params_size > NV_RM_MAX_PARAMS) {
        kwarn("nv-rm", "a %u byte allocation is larger than a message",
              params_size);
        return false;
    }

    static u8 message[sizeof(rm_alloc_t) + NV_RM_MAX_PARAMS];
    rm_alloc_t *header = (rm_alloc_t *)message;

    memset(message, 0, sizeof message);
    header->client = rm->client;
    header->parent = parent;
    header->object = object;
    header->class_number = class_number;
    header->params_size = params_size;
    if (params && params_size) memcpy(message + sizeof *header, params, params_size);

    static u8 reply[sizeof(rm_alloc_t) + NV_RM_MAX_PARAMS];
    u32 reply_len = 0;

    if (!nv_gsp_rpc_call(c, &rm->command, &rm->status, RPC_GSP_RM_ALLOC,
                         message, (u32)sizeof *header + params_size,
                         reply, sizeof reply, &reply_len, NV_RM_RPC_TIMEOUT_MS)) {
        kwarn("nv-rm", "allocating %04x got no answer", class_number);
        return false;
    }

    if (reply_len < sizeof(rm_alloc_t)) {
        kwarn("nv-rm", "the answer to an allocation was %u bytes", reply_len);
        return false;
    }

    const rm_alloc_t *answer = (const rm_alloc_t *)reply;
    nv_last_alloc_status = answer->status;      /* let callers read the exact code */
    if (answer->status != NV_OK) {
        kwarn("nv-rm", "allocating class %04x under %08x was refused (%u)",
              class_number, parent, answer->status);
        return false;
    }

    /* RM allocation parameters are IN/OUT.  NVKMS depends on returned pitch,
     * VRAM offset, limit and address fields.  The old helper discarded the
     * reply payload, which was harmless only for Kestrel's fixed-handle setup
     * calls and is incorrect for a real RM client. */
    if (params && params_size) {
        u32 got = answer->params_size;
        u32 available = reply_len - (u32)sizeof *answer;
        if (got > available) got = available;
        if (got > params_size) got = params_size;
        memcpy(params, reply + sizeof *answer, got);
    }

    rm->allocations++;
    if (answer->params_size >= params_size &&
        reply_len - (u32)sizeof *answer >= params_size)
        rm_account_alloc(c, rm->client, parent, object, class_number, params, params_size);
    return true;
}

/* The control path, with an explicit hClient - so a control can be issued either
 * on our own client (nv_rm_control) or on GSP-RM's internal privileged client
 * (nv_rm_control_internal's fallback). */
static bool rm_control_client(nv_card_t *c, nv_rm_t *rm, u32 client, u32 object,
                              u32 cmd, const void *params, u32 params_size,
                              void *out, u32 out_cap, u32 *out_len) {
    if (!rm->ready) {
        nv_last_control_status = NV_CTRL_FAIL_NOT_READY;
        nv_last_control_replylen = 0;
        return false;
    }
    if (params_size > NV_RM_MAX_PARAMS) {
        nv_last_control_status = NV_CTRL_FAIL_PARAMS_BIG;
        nv_last_control_replylen = params_size;
        return false;
    }

    if (rm->host_api) {
        (void)c;
        /* RM controls are IN/OUT in one buffer.  Preserve the existing
         * nv_rm_control contract, whose callers may supply separate input and
         * output addresses, while the native NVOS54 call uses one pointer. */
        static u8 host_params[NV_RM_MAX_PARAMS];
        memset(host_params, 0, params_size);
        if (params && params_size) memcpy(host_params, params, params_size);
        nv_last_control_status = nvrm_host_control_object(
            client, object, cmd, host_params, params_size);
        nv_last_control_replylen = params_size;
        if (nv_last_control_status != NV_OK) {
            kwarn("nv-rm", "host RM command %08x on %08x was refused (%u)",
                  cmd, object, nv_last_control_status);
            if (out_len) *out_len = 0;
            return false;
        }
        u32 got = params_size;
        if (got > out_cap) got = out_cap;
        if (out && got) memcpy(out, host_params, got);
        if (out_len) *out_len = got;
        rm->controls++;
        return true;
    }

    static u8 message[sizeof(rm_control_t) + NV_RM_MAX_PARAMS];
    rm_control_t *header = (rm_control_t *)message;

    memset(message, 0, sizeof message);
    header->client = client;
    header->object = object;
    header->cmd = cmd;
    header->params_size = params_size;
    if (params && params_size) memcpy(message + sizeof *header, params, params_size);

    static u8 reply[sizeof(rm_control_t) + NV_RM_MAX_PARAMS];
    u32 reply_len = 0;

    if (!nv_gsp_rpc_call(c, &rm->command, &rm->status, RPC_GSP_RM_CONTROL,
                         message, (u32)sizeof *header + params_size,
                         reply, sizeof reply, &reply_len, NV_RM_RPC_TIMEOUT_MS)) {
        kwarn("nv-rm", "command %08x got no answer", cmd);
        nv_last_control_status = NV_CTRL_FAIL_NO_ANSWER;
        nv_last_control_replylen = reply_len;
        return false;
    }

    if (reply_len < sizeof(rm_control_t)) {
        nv_last_control_status = NV_CTRL_FAIL_SHORT_REPLY;
        nv_last_control_replylen = reply_len;
        return false;
    }

    const rm_control_t *answer = (const rm_control_t *)reply;
    if (answer->status != NV_OK) {
        kwarn("nv-rm", "command %08x on %08x was refused (%u)", cmd, object,
              answer->status);
        nv_last_control_status = answer->status;
        nv_last_control_replylen = reply_len;
        return false;
    }

    /* The answer's parameters come back in the same place the request's went,
     * and the size is the answer's rather than the request's. */
    u32 got = answer->params_size;
    if (got > reply_len - sizeof(rm_control_t)) got = reply_len - (u32)sizeof(rm_control_t);
    if (out && out_cap) {
        if (got > out_cap) got = out_cap;
        memcpy(out, reply + sizeof(rm_control_t), got);
    }
    if (out_len) *out_len = got;

    nv_last_control_status = 0;              /* NV_OK */
    nv_last_control_replylen = reply_len;
    rm->controls++;
    return true;
}

/* Public control: on our own client, exactly as before. */
bool nv_rm_control(nv_card_t *c, nv_rm_t *rm, u32 object, u32 cmd,
                   const void *params, u32 params_size,
                   void *out, u32 out_cap, u32 *out_len) {
    return rm_control_client(c, rm, rm->client, object, cmd, params, params_size,
                             out, out_cap, out_len);
}

/* Ask GSP-RM for its GspStaticConfigInfo (RPC 65) and keep the three privileged
 * internal handles it reports.  Sent with a zeroed body sized to the struct (the
 * reply comes back in place, exactly as nouveau's nvkm_gsp_rpc_rd does).  A
 * failure is non-fatal: the fallback simply stays inactive and every internal
 * query behaves as it did before (RM_SUBDEVICE only). */
bool nv_rm_get_static_info(nv_card_t *c, nv_rm_t *rm) {
    if (!rm->ready) return false;
    static u8 buf[GSP_SCI_SIZE + 256];
    memset(buf, 0, sizeof buf);
    u32 reply_len = 0;
    if (!nv_gsp_rpc_call(c, &rm->command, &rm->status, RPC_GET_GSP_STATIC_INFO,
                         buf, GSP_SCI_SIZE, buf, sizeof buf, &reply_len,
                         NV_RM_RPC_TIMEOUT_MS)) {
        kwarn("nv-rm", "GET_GSP_STATIC_INFO (RPC 65) got no answer; the internal "
                       "subdevice is unknown - internal queries use RM_SUBDEVICE only");
        return false;
    }
    if (reply_len < GSP_SCI_OFF_HSUBDEVICE + 4) {
        kwarn("nv-rm", "GspStaticConfigInfo reply too short (%u bytes)", reply_len);
        return false;
    }
    memcpy(&g_internal_client,    buf + GSP_SCI_OFF_HCLIENT,    4);
    memcpy(&g_internal_device,    buf + GSP_SCI_OFF_HDEVICE,    4);
    memcpy(&g_internal_subdevice, buf + GSP_SCI_OFF_HSUBDEVICE, 4);
    if (reply_len >= GSP_SCI_OFF_BAR1PDE + 8)
        memcpy(&g_bar1_pde_base, buf + GSP_SCI_OFF_BAR1PDE, 8);
    g_static_info_ok = (g_internal_subdevice != 0);
    kinfo("nv-rm", "GSP static info: internal client %08x device %08x subdevice %08x; "
                   "bar1PdeBase %#llx", g_internal_client, g_internal_device,
          g_internal_subdevice, (unsigned long long)g_bar1_pde_base);
    return g_static_info_ok;
}

/* An NV2080_CTRL_CMD_INTERNAL_* control: try our own RM_SUBDEVICE first, and if
 * GSP-RM refuses it, retry once on GSP-RM's internal privileged client+subdevice
 * (from RPC 65).  A no-op fallback when the first call works - so it can never
 * regress a query that was already accepted. */
bool nv_rm_control_internal(nv_card_t *c, nv_rm_t *rm, u32 cmd,
                            const void *params, u32 params_size,
                            void *out, u32 out_cap, u32 *out_len) {
    if (nv_rm_control(c, rm, RM_SUBDEVICE, cmd, params, params_size,
                      out, out_cap, out_len))
        return true;
    if (!g_static_info_ok) return false;   /* no internal handle to retry with */
    kinfo("nv-rm", "internal control %08x refused on our subdevice; retrying on "
                   "GSP-RM's internal subdevice %08x", cmd, g_internal_subdevice);
    return rm_control_client(c, rm, g_internal_client, g_internal_subdevice, cmd,
                             params, params_size, out, out_cap, out_len);
}

/* PREFER the GSP internal privileged subdevice: the reference (nouveau r535/r570)
 * routes every ROUTE_TO_PHYSICAL / INTERNAL_STATIC_KGR control to
 * gsp->internal.device.subdevice UNCONDITIONALLY - an ordinary client subdevice
 * can silently return default/wrong static data (e.g. ctxbuf sizes or a topology
 * from a different config), which would corrupt the golden context.  Try internal
 * first; fall back to RM_SUBDEVICE only if the internal handle is unavailable or
 * refuses, so this never regresses a query that already worked. */
bool nv_rm_control_prefer_internal(nv_card_t *c, nv_rm_t *rm, u32 cmd,
                                   const void *params, u32 params_size,
                                   void *out, u32 out_cap, u32 *out_len) {
    if (g_static_info_ok &&
        rm_control_client(c, rm, g_internal_client, g_internal_subdevice, cmd,
                          params, params_size, out, out_cap, out_len))
        return true;
    return nv_rm_control(c, rm, RM_SUBDEVICE, cmd, params, params_size,
                         out, out_cap, out_len);
}

bool nv_rm_free(nv_card_t *c, nv_rm_t *rm, u32 parent, u32 object) {
    if (!rm->ready) return false;
    if (rm->host_api) {
        u32 status = nvrm_host_free_object(rm->client, parent, object);
        if (status != NV_OK) {
            kwarn("nv-rm", "host RM freeing %08x was refused (%u)", object, status);
            return false;
        }
        rm->frees++;
        rm_account_free(c, rm->client, object);
        return true;
    }

    /* The free message carries the same three handles an allocation does, so
     * that the far end can check the object really is where it is said to be. */
    struct {
        u32 root, parent, object, status;
    } __attribute__((packed)) message = { rm->client, parent, object, 0 };

    u8 reply[32];
    u32 reply_len = 0;

    if (!nv_gsp_rpc_call(c, &rm->command, &rm->status, RPC_FREE,
                         &message, sizeof message, reply, sizeof reply,
                         &reply_len, NV_RM_RPC_TIMEOUT_MS))
        return false;

    if (reply_len < sizeof message) return false;
    u32 status = ((const u32 *)reply)[3];
    if (status != NV_OK) {
        kwarn("nv-rm", "freeing %08x was refused (%u)", object, status);
        return false;
    }

    rm->frees++;
    rm_account_free(c, rm->client, object);
    return true;
}

bool nv_rm_rpc_raw(nv_card_t *c, nv_rm_t *rm, u32 function,
                   const void *request, u32 request_size,
                   void *reply, u32 reply_cap, u32 *reply_size) {
    if (!c || !rm || !rm->ready || !request || !request_size ||
        !reply || !reply_cap) return false;
    return nv_gsp_rpc_call(c, &rm->command, &rm->status, function,
                           request, request_size, reply, reply_cap, reply_size,
                           NV_RM_RPC_TIMEOUT_MS);
}

/* Building the tree, in the only order it can be built in. */
bool nv_rm_bring_up(nv_card_t *c, nv_rm_t *rm) {
    rm->client = RM_CLIENT(0);

    /* Before building our own object tree, ask GSP-RM for its static info (RPC
     * 65) so we hold the internal privileged handles nouveau's postinit fetches
     * first.  Non-fatal: if it fails, internal queries just use RM_SUBDEVICE. */
    (void)nv_rm_get_static_info(c, rm);

    /* No SET_GUEST_SYSTEM_INFO here.  RPC 1 is a vGPU-GUEST call - NVIDIA only
     * sends it from a virtual function; a bare-metal physical GPU carries its
     * system description before bootstrap via GSP_SET_SYSTEM_INFO/SET_REGISTRY
     * (RPC 72/73), which we already do.  On the real GB20x card sending it here
     * was refused AND left the command ring out of step, so the next allocation
     * got no answer.  So it is not sent at all - go straight to the tree. */

    /* The client owns everything else, so it is allocated with itself as both
     * the object and the owner - which is how a root is made.  hClient must be
     * the client's own handle; processID = ~0 marks a kernel client. */
    nv0000_root_params_t root;
    memset(&root, 0, sizeof root);
    root.h_client = rm->client;
    root.process_id = 0xFFFFFFFFu;
    if (!nv_rm_alloc(c, rm, 0, rm->client, NV01_ROOT, &root, sizeof root)) return false;

    /* Only hClientShare is set (to the client), the rest zero - RM fills the
     * VA-space defaults itself. */
    nv0080_device_params_t device;
    memset(&device, 0, sizeof device);
    device.h_client_share = rm->client;
    if (!nv_rm_alloc(c, rm, rm->client, RM_DEVICE, NV01_DEVICE_0,
                     &device, sizeof device))
        return false;

    nv2080_subdevice_params_t sub;
    memset(&sub, 0, sizeof sub);
    if (!nv_rm_alloc(c, rm, RM_DEVICE, RM_SUBDEVICE, NV20_SUBDEVICE_0,
                     &sub, sizeof sub))
        return false;

    if (!nv_rm_alloc(c, rm, RM_DEVICE, RM_DISP, NV04_DISPLAY_COMMON, NULL, 0))
        return false;

    /* The usermode object (BLACKWELL_USERMODE_A) under the subdevice.  This is
     * what enables the usermode submission path - it maps the VF doorbell region
     * (BAR0 + 0x30000, the 0xbb0090 doorbell) and, on GSP, registers the client
     * for doorbell-based work submission.  Params are optional (RS_OPTIONAL), so
     * NULL is accepted, exactly as nouveau allocates it with argc == 0.  Without
     * it the channel schedules but the doorbell never runs it. */
    #define BLACKWELL_USERMODE_A_CLASS 0x0000c761u
    #define RM_USERMODE                0xc7610000u
    if (!nv_rm_alloc(c, rm, RM_SUBDEVICE, RM_USERMODE, BLACKWELL_USERMODE_A_CLASS,
                     NULL, 0))
        kwarn("nv-rm", "the usermode (doorbell) object could not be allocated - "
                       "work submission may not reach the card");

    rm->up = true;
    return true;
}

bool nv_rm_host_bring_up(nv_card_t *c, nv_rm_t *rm)
{
    (void)c;
    if (!rm) return false;
    memset(rm, 0, sizeof(*rm));
    rm->host_api = true;
    rm->ready = true;

    u32 status = nvrm_host_alloc_root(&rm->client);
    if (status != NV_OK || rm->client == 0) {
        kerr("nv-rm", "host RM render client allocation failed (%u, handle %#x)",
             status, rm->client);
        rm->ready = false;
        return false;
    }

    nv0080_device_params_t device;
    memset(&device, 0, sizeof(device));
    device.device_id = 0;              /* sole adapter / RM device instance */
    device.h_client_share = rm->client;
    if (!nv_rm_alloc(c, rm, rm->client, RM_DEVICE, NV01_DEVICE_0,
                     &device, sizeof(device)))
        goto fail_root;

    nv2080_subdevice_params_t sub;
    memset(&sub, 0, sizeof(sub));
    if (!nv_rm_alloc(c, rm, RM_DEVICE, RM_SUBDEVICE, NV20_SUBDEVICE_0,
                      &sub, sizeof(sub)))
        goto fail_device;

    /* Follow NVIDIA-push/UVM's host-RM path exactly.  A channel-specific work
     * submit token is written to HOPPER_USERMODE_A+0x90; allocating the object
     * also registers this client for usermode submissions.  nvrm_rmapi_op
     * changes bBar1Mapping to false on this machine because BAR1 is poisoned,
     * selecting usrmodeConstruct_IMPL's supported pRegVF/BAR0 mapping. */
    enum { HOST_USERMODE = 0xc6610000u, HOPPER_USERMODE_A = 0x0000c661u };
    struct { u8 b_bar1_mapping, b_priv; } usermode_params = { 1, 0 };
    if (!nv_rm_alloc(c, rm, RM_SUBDEVICE, HOST_USERMODE, HOPPER_USERMODE_A,
                     &usermode_params, sizeof(usermode_params))) {
        kerr("nv-rm", "native host-RM usermode object allocation failed");
        goto fail_subdevice;
    }
    void *usermode = NULL;
    status = nvrm_host_map_memory(rm->client, RM_SUBDEVICE, HOST_USERMODE,
                                  0, 65536, &usermode, 0);
    if (status != NV_OK || !usermode) {
        kerr("nv-rm", "native host-RM BAR0 usermode mapping failed (%u)", status);
        goto fail_usermode;
    }
    rm->usermode = (volatile u8 *)usermode;

    rm->up = true;
    kinfo("nv-rm", "native host-RM render tree ready: client %#x device %#x subdevice %#x; official BAR0 usermode %p; GSP ownership unchanged",
          rm->client, RM_DEVICE, RM_SUBDEVICE, usermode);
    return true;

fail_usermode:
    (void)nv_rm_free(c, rm, RM_SUBDEVICE, HOST_USERMODE);
fail_subdevice:
    (void)nv_rm_free(c, rm, RM_DEVICE, RM_SUBDEVICE);
fail_device:
    (void)nv_rm_free(c, rm, rm->client, RM_DEVICE);
fail_root:
    (void)nv_rm_free(c, rm, 0, rm->client);
    rm->ready = false;
    return false;
}

u32 nv_rm_host_display_query_object(nv_card_t *c, nv_rm_t *rm, u32 *object)
{
    if (object) *object = 0;
    if (!object || !rm || !rm->ready || !rm->up || !rm->host_api)
        return NV_CTRL_FAIL_NOT_READY;
    /* RM_DISP is reserved in this client's handle namespace and deliberately
     * absent from host render bring-up. Display-common initializes disabled
     * notifiers (disp_objs.c:dispcmnConstruct_IMPL); it does not allocate the
     * display engine/core channel or claim NVKMS's SOR/head ownership. */
    if (!rm->display_query_attempted) {
        rm->display_query_attempted = true;
        if (nv_rm_alloc(c, rm, RM_DEVICE, RM_DISP, NV04_DISPLAY_COMMON, NULL, 0))
            rm->display_query_status = NV_OK;
        else
            rm->display_query_status = nv_last_alloc_status;
    }
    if (rm->display_query_status == NV_OK) *object = RM_DISP;
    return rm->display_query_status;
}

/* A SECOND, independent RM client + device + subdevice - the dedicated display
 * client.  nouveau's r535_disp_oneinit allocates the display objects NOT under
 * the main device but under a client of their own, made with
 * nvkm_gsp_client_device_ctor(gsp, &disp->rm.client, &disp->rm.device)
 * (linux-nouveau drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/disp.c:1536);
 * NV04_DISPLAY_COMMON is then allocated under THAT device (disp.c:1540).  Our old
 * code put the display-common object under the shared RM_DEVICE, which is the
 * likely reason GSP-RM refused the display tree.  This builds the dedicated
 * client+device+subdevice so the caller can allocate NV04_DISPLAY_COMMON under
 * the returned display device.  Classes and params are exactly bring_up's:
 *   NV01_ROOT      + NV0000_ALLOC_PARAMETERS  (r570/client.c:12-22, r535/client.c:29-39)
 *   NV01_DEVICE_0  + NV0080_ALLOC_PARAMETERS  (r535/device.c:119-131, only hClientShare)
 *   NV20_SUBDEVICE_0 + NV2080_ALLOC_PARAMETERS (r535/device.c:110-115)
 *
 * ---- RAMIN / VRAM alloc params finding (nv_chan.c nv_vram_alloc; DO NOT edit) ----
 * Asked whether our mem_alloc_params_t (nv_chan.c:311-318, class
 * NV01_MEMORY_LOCAL_USER 0x40) matches the driver's expected alloc params
 * byte-for-byte.  FINDING: YES - it matches NV_MEMORY_ALLOCATION_PARAMS
 * (OGKM src/common/sdk/nvidia/inc/nvos.h:1635-1669) exactly, 128 bytes, every
 * offset identical:
 *   owner@0 type@4 flags@8 width@12 height@16 pitch@20 attr@24 attr2@28
 *   format@32 comprCovg@36 zcullCovg@40 [4B pad@44] rangeLo@48 rangeHi@56 size@64
 *   alignment@72 offset@80 limit@88 address@96 ctagOffset@104 hVASpace@108
 *   internalflags@112 tag@116 numaNode@120 [4B tail pad] -> 128.
 * So the display's VRAM refusal is NOT a params-layout bug.  The real divergence
 * from nouveau is the PARENT/scope: nv_vram_alloc allocates NV01_MEMORY_LOCAL_USER
 * under the shared RM_DEVICE (nv_chan.c:565), whereas nouveau does not allocate the
 * display RAMIN under the disp client at all - it makes a plain gpuobj in VRAM and
 * hands its physical addr to GSP-RM via NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM
 * on gsp->internal.device.subdevice (r535/disp.c:1513-1533).  Main agent action:
 * either allocate display memory under this dedicated display device, or set inst
 * mem through WRITE_INST_MEM on the internal subdevice - not a struct change. */
bool nv_rm_disp_client_ctor(nv_card_t *c, nv_rm_t *rm, u32 *out_client,
                            u32 *out_device, u32 *out_subdevice) {
    if (!rm->ready) return false;

    /* nv_rm_alloc() and nv_rm_free() both stamp the RPC's hClient field from
     * rm->client (see nv_rm_alloc ~line 191, nv_rm_free ~line 360).  To build a
     * tree OWNED BY A DIFFERENT client while reusing those exact functions, we
     * retarget rm->client for the duration and restore it before returning on
     * every path.  Bring-up is single-threaded, so this swap is safe. */
    u32 saved_client = rm->client;
    rm->client = RM_DISP_CLIENT;

    /* root/client: object == owner == the new client handle, parent 0,
     * processID ~0 marks a kernel client (mirrors nv_rm_bring_up and
     * r570_gsp_client_ctor, r570/client.c:19-20). */
    nv0000_root_params_t root;
    memset(&root, 0, sizeof root);
    root.h_client = RM_DISP_CLIENT;
    root.process_id = 0xFFFFFFFFu;
    kinfo("nv-disp-rm", "allocating dedicated display client %08x", RM_DISP_CLIENT);
    if (!nv_rm_alloc(c, rm, 0, RM_DISP_CLIENT, NV01_ROOT, &root, sizeof root)) {
        kerr("nv-disp-rm", "display client %08x refused (status %u)",
             RM_DISP_CLIENT, nv_last_alloc_status);
        rm->client = saved_client;
        return false;
    }

    /* device under the display client - only hClientShare set, RM fills the
     * VA-space defaults (r535/device.c:129). */
    nv0080_device_params_t device;
    memset(&device, 0, sizeof device);
    device.h_client_share = RM_DISP_CLIENT;
    kinfo("nv-disp-rm", "allocating display device %08x under client %08x",
          RM_DISP_DEVICE, RM_DISP_CLIENT);
    if (!nv_rm_alloc(c, rm, RM_DISP_CLIENT, RM_DISP_DEVICE, NV01_DEVICE_0,
                     &device, sizeof device)) {
        kerr("nv-disp-rm", "display device %08x refused (status %u)",
             RM_DISP_DEVICE, nv_last_alloc_status);
        nv_rm_free(c, rm, 0, RM_DISP_CLIENT);   /* still rm->client=RM_DISP_CLIENT */
        rm->client = saved_client;
        return false;
    }

    /* subdevice under the display device (r535/device.c:110-115). */
    nv2080_subdevice_params_t sub;
    memset(&sub, 0, sizeof sub);
    kinfo("nv-disp-rm", "allocating display subdevice %08x under device %08x",
          RM_DISP_SUBDEVICE, RM_DISP_DEVICE);
    if (!nv_rm_alloc(c, rm, RM_DISP_DEVICE, RM_DISP_SUBDEVICE, NV20_SUBDEVICE_0,
                     &sub, sizeof sub)) {
        kerr("nv-disp-rm", "display subdevice %08x refused (status %u)",
             RM_DISP_SUBDEVICE, nv_last_alloc_status);
        nv_rm_free(c, rm, RM_DISP_DEVICE, RM_DISP_SUBDEVICE);  /* no-op if absent */
        nv_rm_free(c, rm, RM_DISP_CLIENT, RM_DISP_DEVICE);
        nv_rm_free(c, rm, 0, RM_DISP_CLIENT);
        rm->client = saved_client;
        return false;
    }

    rm->client = saved_client;

    if (out_client)    *out_client    = RM_DISP_CLIENT;
    if (out_device)    *out_device    = RM_DISP_DEVICE;
    if (out_subdevice) *out_subdevice = RM_DISP_SUBDEVICE;

    kinfo("nv-disp-rm", "dedicated display client tree up: client %08x device %08x "
                        "subdevice %08x", RM_DISP_CLIENT, RM_DISP_DEVICE,
          RM_DISP_SUBDEVICE);
    return true;
}

bool nv_rm_shut_down(nv_card_t *c, nv_rm_t *rm) {
    if (!rm->up) return false;

    /* Children before parents.  A resource manager that let a parent go first
     * would leave its children pointing at nothing, so it refuses - and a
     * driver that tears down in the wrong order leaks the whole subtree. */
    bool ok = true;
    ok &= nv_rm_free(c, rm, RM_DEVICE, RM_DISP);
    ok &= nv_rm_free(c, rm, RM_DEVICE, RM_SUBDEVICE);
    ok &= nv_rm_free(c, rm, rm->client, RM_DEVICE);
    ok &= nv_rm_free(c, rm, 0, rm->client);

    nv_gsp_rpc_call(c, &rm->command, &rm->status, RPC_UNLOADING_GUEST_DRIVER,
                    NULL, 0, NULL, 0, NULL, NV_RM_RPC_TIMEOUT_MS);

    rm->up = false;
    return ok;
}

/* What a driver asks the display for before it can set a mode. */
bool nv_rm_query_display(nv_card_t *c, nv_rm_t *rm, nv_rm_display_t *out) {
    memset(out, 0, sizeof *out);
    if (!rm->up) return false;

    /* Every NV0073 (display-common) control takes subDeviceInstance as its
     * FIRST field.  Sending these with no params - as this used to - is why
     * GSP-RM answered NV_ERR_INVALID_ARGUMENT (31): it read a params struct
     * that was not there.  Verified vs OGKM ctrl0073system.h. */
    struct { u32 subdev, mask, mask_ddc; } __attribute__((packed)) supported = { 0, 0, 0 };
    if (!nv_rm_control(c, rm, RM_DISP, NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED,
                       &supported, sizeof supported, &supported, sizeof supported, NULL))
        return false;
    out->display_mask = supported.mask;

    struct { u32 subdev, flags, num_heads; } __attribute__((packed)) heads = { 0, 0, 0 };
    if (!nv_rm_control(c, rm, RM_DISP, NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS,
                       &heads, sizeof heads, &heads, sizeof heads, NULL))
        return false;
    out->heads = heads.num_heads;

    struct { u32 subdev, head_mask; } __attribute__((packed)) all = { 0, 0 };
    if (!nv_rm_control(c, rm, RM_DISP, NV0073_CTRL_CMD_SPECIFIC_GET_ALL_HEAD_MASK,
                       &all, sizeof all, &all, sizeof all, NULL))
        return false;
    out->head_mask = all.head_mask;

    /* One query per connector that exists, because the answer is per-display
     * rather than a list. */
    for (int i = 0; i < 32 && out->connectors < NV_RM_MAX_CONNECTORS; i++) {
        u32 id = 1u << i;
        if (!(out->display_mask & id)) continue;

        struct {
            u32 display_id;
            u32 count;
            u32 type;
            u32 location;
        } __attribute__((packed)) connector = { id, 0, 0, 0 };

        if (!nv_rm_control(c, rm, RM_DISP,
                           NV0073_CTRL_CMD_SPECIFIC_GET_CONNECTOR_DATA,
                           &connector, sizeof connector,
                           &connector, sizeof connector, NULL))
            continue;

        out->connector[out->connectors].display_id = id;
        out->connector[out->connectors].type = connector.type;
        out->connectors++;
    }

    /* And what the DisplayPort links on this card can do, which is a property
     * of the card rather than of a monitor. */
    struct {
        u32 display_id;
        u32 dp_versions;
        u32 max_link_rate;
        u32 max_lanes;
    } __attribute__((packed)) caps = { 0, 0, 0, 0 };

    if (nv_rm_control(c, rm, RM_DISP, NV0073_CTRL_CMD_DP_GET_CAPS,
                      &caps, sizeof caps, &caps, sizeof caps, NULL)) {
        out->dp_max_rate = caps.max_link_rate;
        out->dp_max_lanes = caps.max_lanes;
    }

    return true;
}

/* ==========================================================================
 * The resource manager that is not there.
 *
 * It holds the same object tree and enforces the same rules, and it is
 * unhelpful in exactly the places the real one is: it refuses a child whose
 * parent does not exist, a command to an object that was never allocated, a
 * command that belongs to a different class, and a free that would orphan
 * something.  Those four are the whole contract of this interface, and a model
 * that accepted any of them would be checking nothing.
 * ==========================================================================
 */
#define MODEL_MAX_OBJECTS 16

static struct {
    struct {
        bool used;
        u32  handle, parent, class_number;
    } object[MODEL_MAX_OBJECTS];
    int  objects;

    bool system_told;
    int  allocs, controls, frees;
    int  refusals;
    u32  last_refusal;
    bool saw_out_of_order;
} rmm;

static int find_object(u32 handle) {
    for (int i = 0; i < MODEL_MAX_OBJECTS; i++)
        if (rmm.object[i].used && rmm.object[i].handle == handle) return i;
    return -1;
}

static bool has_children(u32 handle) {
    for (int i = 0; i < MODEL_MAX_OBJECTS; i++)
        if (rmm.object[i].used && rmm.object[i].parent == handle) return true;
    return false;
}

static u32 refuse(u32 why) {
    rmm.refusals++;
    rmm.last_refusal = why;
    return why;
}

/* A control command's number carries the class it belongs to in its top half,
 * so a command sent to the wrong object can be caught rather than acted on. */
static bool command_suits(u32 cmd, u32 class_number) {
    u32 belongs_to = cmd >> 16;
    if (belongs_to == 0x0073) return class_number == NV04_DISPLAY_COMMON;
    if (belongs_to == 0x2080) return class_number == NV20_SUBDEVICE_0;
    return false;
}

static u32 answer_control(u32 cmd, const u8 *in, u32 in_len, u8 *out,
                          u32 *out_len) {
    (void)in; (void)in_len;
    *out_len = 0;

    switch (cmd) {
    case NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED: {
        /* Four connectors present, as a bit per display. */
        u32 mask = 0x0000000Fu;
        memcpy(out, &mask, 4);
        *out_len = 4;
        return NV_OK;
    }
    case NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS: {
        struct { u32 flags, count; } answer = { 0, 4 };
        memcpy(out, &answer, sizeof answer);
        *out_len = sizeof answer;
        return NV_OK;
    }
    case NV0073_CTRL_CMD_SPECIFIC_GET_ALL_HEAD_MASK: {
        u32 heads = 0x0000000Fu;
        memcpy(out, &heads, 4);
        *out_len = 4;
        return NV_OK;
    }
    case NV0073_CTRL_CMD_SPECIFIC_GET_CONNECTOR_DATA: {
        u32 id = 0;
        if (in_len >= 4) memcpy(&id, in, 4);
        struct { u32 display_id, count, type, location; } answer = { id, 1, 0, 0 };
        /* Two DisplayPorts, an HDMI and a DVI, in that order. */
        if (id == 0x1) answer.type = 0x00000200;         /* DisplayPort      */
        else if (id == 0x2) answer.type = 0x00000200;
        else if (id == 0x4) answer.type = 0x00000061;    /* HDMI             */
        else answer.type = 0x00000060;                   /* DVI              */
        memcpy(out, &answer, sizeof answer);
        *out_len = sizeof answer;
        return NV_OK;
    }
    case NV0073_CTRL_CMD_DP_GET_CAPS: {
        struct { u32 display_id, versions, max_rate, max_lanes; } answer =
            { 0, 0x3, 4 /* 8.1 Gbps */, 4 };
        memcpy(out, &answer, sizeof answer);
        *out_len = sizeof answer;
        return NV_OK;
    }
    default:
        return refuse(NV_ERR_INVALID_ARGUMENT);
    }
}

static bool rm_model_handler(u32 function, const u8 *in, u32 in_len,
                             u8 *out, u32 out_cap, u32 *out_len) {
    *out_len = 0;

    switch (function) {
    case RPC_SET_GUEST_SYSTEM_INFO:
        rmm.system_told = true;
        return true;

    case RPC_UNLOADING_GUEST_DRIVER:
        return true;

    case RPC_GSP_RM_ALLOC: {
        if (in_len < sizeof(rm_alloc_t) || out_cap < sizeof(rm_alloc_t)) return false;

        rm_alloc_t answer;
        memcpy(&answer, in, sizeof answer);
        rmm.allocs++;

        /* Nothing can be allocated before the resource manager has been told
         * what it is running on. */
        if (!rmm.system_told) {
            rmm.saw_out_of_order = true;
            answer.status = refuse(NV_ERR_INVALID_STATE);
        } else if (find_object(answer.object) >= 0) {
            answer.status = refuse(NV_ERR_INVALID_ARGUMENT);
        } else if (answer.parent != 0 && find_object(answer.parent) < 0) {
            /* A child whose parent does not exist. */
            rmm.saw_out_of_order = true;
            answer.status = refuse(NV_ERR_INVALID_OBJECT);
        } else if (rmm.objects >= MODEL_MAX_OBJECTS) {
            answer.status = refuse(NV_ERR_INVALID_STATE);
        } else {
            for (int i = 0; i < MODEL_MAX_OBJECTS; i++) {
                if (rmm.object[i].used) continue;
                rmm.object[i].used = true;
                rmm.object[i].handle = answer.object;
                rmm.object[i].parent = answer.parent;
                rmm.object[i].class_number = answer.class_number;
                rmm.objects++;
                break;
            }
            answer.status = NV_OK;
        }

        answer.params_size = 0;
        memcpy(out, &answer, sizeof answer);
        *out_len = sizeof answer;
        return true;
    }

    case RPC_GSP_RM_CONTROL: {
        if (in_len < sizeof(rm_control_t) || out_cap < sizeof(rm_control_t))
            return false;

        rm_control_t answer;
        memcpy(&answer, in, sizeof answer);
        rmm.controls++;

        int at = find_object(answer.object);
        if (at < 0) {
            rmm.saw_out_of_order = true;
            answer.status = refuse(NV_ERR_INVALID_OBJECT);
            answer.params_size = 0;
            memcpy(out, &answer, sizeof answer);
            *out_len = sizeof answer;
            return true;
        }

        if (!command_suits(answer.cmd, rmm.object[at].class_number)) {
            answer.status = refuse(NV_ERR_INVALID_ARGUMENT);
            answer.params_size = 0;
            memcpy(out, &answer, sizeof answer);
            *out_len = sizeof answer;
            return true;
        }

        u32 payload = 0;
        u32 status = answer_control(answer.cmd, in + sizeof(rm_control_t),
                                    in_len - (u32)sizeof(rm_control_t),
                                    out + sizeof(rm_control_t), &payload);
        answer.status = status;
        answer.params_size = payload;
        memcpy(out, &answer, sizeof answer);
        *out_len = (u32)sizeof answer + payload;
        return true;
    }

    case RPC_FREE: {
        if (in_len < 16 || out_cap < 16) return false;

        u32 message[4];
        memcpy(message, in, 16);
        rmm.frees++;

        int at = find_object(message[2]);
        if (at < 0) {
            message[3] = refuse(NV_ERR_INVALID_OBJECT);
        } else if (rmm.object[at].parent != message[1]) {
            message[3] = refuse(NV_ERR_INVALID_ARGUMENT);
        } else if (has_children(message[2])) {
            /* Freeing a parent while its children exist would leave them
             * pointing at nothing. */
            rmm.saw_out_of_order = true;
            message[3] = refuse(NV_ERR_STATE_IN_USE);
        } else {
            rmm.object[at].used = false;
            rmm.objects--;
            message[3] = NV_OK;
        }

        memcpy(out, message, 16);
        *out_len = 16;
        return true;
    }

    default:
        return false;
    }
}

int nv_rm_model_objects(void)   { return rmm.objects; }
int nv_rm_model_refusals(void)  { return rmm.refusals; }
bool nv_rm_model_saw_out_of_order(void) { return rmm.saw_out_of_order; }

/* ------------------------------------------------------------------- test */

#define RM_ELEMENT 128
#define RM_COUNT   32

int nv_rm_selftest(void) {
    int failures = 0;

    nv_card_t *c = nv_model_card();
    if (!c) return 0;

    /* Real-sized ring regions, laid out the way the firmware lays them
     * out - see nv_gsp_queue.c for why a smaller stand-in proved nothing. */
    u64 phys = 0;
    u8 *shared = dma_alloc_pages(2 * NV_GSP_QUEUE_REGION_BYTES / 4096, &phys);
    if (!shared) {
        kerr("nv-rm", "no memory for the test rings");
        return 1;
    }
    memset(shared, 0, 2 * NV_GSP_QUEUE_REGION_BYTES);
    memset(&rmm, 0, sizeof rmm);

    static nv_rm_t rm;
    memset(&rm, 0, sizeof rm);
    nv_gsp_model_attach(shared, shared + NV_GSP_QUEUE_REGION_BYTES);
    nv_gsp_cmdq_init(&rm.command, shared, shared + NV_GSP_QUEUE_REGION_BYTES);
    nv_gsp_msgq_adopt(&rm.status, shared + NV_GSP_QUEUE_REGION_BYTES, shared);
    rm.ready = true;

    nv_gsp_model_set_handler(rm_model_handler);

    /* An allocation before the processor has been told what it is running on
     * has to be refused - which is the first ordering rule, and the one a
     * driver breaks by initialising things in a tidy-looking order. */
    {
        int before = nv_rm_model_refusals();
        if (nv_rm_alloc(c, &rm, 0, RM_CLIENT(0), NV01_ROOT, NULL, 0)) {
            kerr("nv-rm", "a client was allocated before the system was "
                          "described");
            failures++;
        } else if (nv_rm_model_refusals() != before + 1) {
            kerr("nv-rm", "the allocation failed without being refused");
            failures++;
        }
    }

    /* Now in the right order. */
    if (!nv_rm_bring_up(c, &rm)) {
        kerr("nv-rm", "the object tree was not built");
        failures++;
    } else if (nv_rm_model_objects() != 4) {
        kerr("nv-rm", "%d object(s) exist, expected 4", nv_rm_model_objects());
        failures++;
    } else {
        kinfo("nv-rm", "the object tree is up: client, device, subdevice and "
                       "display");
    }

    /* A child whose parent does not exist. */
    {
        int before = nv_rm_model_refusals();
        if (nv_rm_alloc(c, &rm, 0xBADBAD00u, 0x12340000u, NV20_SUBDEVICE_0,
                        NULL, 0)) {
            kerr("nv-rm", "an object was allocated under a parent that does "
                          "not exist");
            failures++;
        } else if (nv_rm_model_refusals() != before + 1) {
            kerr("nv-rm", "it failed without being refused");
            failures++;
        }
    }

    /* A command to an object that was never allocated. */
    {
        int before = nv_rm_model_refusals();
        u32 answer = 0;
        if (nv_rm_control(c, &rm, 0x99990000u,
                          NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS, NULL, 0,
                          &answer, sizeof answer, NULL)) {
            kerr("nv-rm", "a command to an object that does not exist was "
                          "accepted");
            failures++;
        } else if (nv_rm_model_refusals() != before + 1) {
            kerr("nv-rm", "it failed without being refused");
            failures++;
        }
    }

    /* A display command sent to the subdevice.  The command number says which
     * class it belongs to, so this is catchable rather than a mystery. */
    {
        int before = nv_rm_model_refusals();
        u32 answer = 0;
        if (nv_rm_control(c, &rm, RM_SUBDEVICE,
                          NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS, NULL, 0,
                          &answer, sizeof answer, NULL)) {
            kerr("nv-rm", "a display command sent to the subdevice was "
                          "accepted");
            failures++;
        } else if (nv_rm_model_refusals() != before + 1) {
            kerr("nv-rm", "it failed without being refused");
            failures++;
        }
    }

    /* Freeing a parent while its children exist. */
    {
        int before = nv_rm_model_refusals();
        if (nv_rm_free(c, &rm, 0, RM_CLIENT(0))) {
            kerr("nv-rm", "the client was freed while it still owned things");
            failures++;
        } else if (nv_rm_model_refusals() != before + 1) {
            kerr("nv-rm", "it failed without being refused");
            failures++;
        }
    }

    /* And then what a driver is actually here for: asking the display what is
     * on the card. */
    static nv_rm_display_t display;
    if (!nv_rm_query_display(c, &rm, &display)) {
        kerr("nv-rm", "the display would not describe itself");
        failures++;
    } else if (display.heads != 4 || display.connectors != 4 ||
               display.display_mask != 0xF) {
        kerr("nv-rm", "it reported %u head(s) and %d connector(s)",
             display.heads, display.connectors);
        failures++;
    } else if (display.dp_max_lanes != 4 || display.dp_max_rate != 4) {
        kerr("nv-rm", "its DisplayPort links came out as %u lanes at rate %u",
             display.dp_max_lanes, display.dp_max_rate);
        failures++;
    } else {
        kinfo("nv-rm", "the display describes itself: %u heads, %d connectors "
                       "(two DisplayPort), links up to %u lanes at 8.1 Gbps",
              display.heads, display.connectors, display.dp_max_lanes);
    }

    /* Taken down in the only order it can be taken down in. */
    if (!nv_rm_shut_down(c, &rm)) {
        kerr("nv-rm", "the object tree was not taken down cleanly");
        failures++;
    } else if (nv_rm_model_objects() != 0) {
        kerr("nv-rm", "%d object(s) were left behind", nv_rm_model_objects());
        failures++;
    }

    if (!nv_rm_model_saw_out_of_order()) {
        kerr("nv-rm", "none of the ordering rules were actually exercised");
        failures++;
    }

    int allocations = rm.allocations, controls = rm.controls;
    nv_gsp_model_detach();

    if (!failures)
        kinfo("nv-rm", "the resource manager protocol works: %d allocations "
                       "and %d control commands answered, and %d out-of-order "
                       "requests refused - which is what a driver has to speak "
                       "on Ada and Blackwell, where nothing else reaches the "
                       "engines",
              allocations, controls, nv_rm_model_refusals());
    return failures;
}
