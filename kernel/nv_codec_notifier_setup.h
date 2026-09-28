/* Included after native channel types and host mapping declarations.
 * Official references: kernel_channel.c kchannelGetNotifierInfo,
 * kernel_rc_notification.c krcErrorWriteNotifier_CPU, nvos.h allocation ABI.
 * Scope intentionally excludes the already-working desktop and golden GR. */
static u32 nv_codec_notifier_setup(nv_channel_t *ch, int idx) {
    static struct {
        bool attempted;
        u32 client;
        volatile nv_error_notification_t *cpu;
    } slots[CH_COUNT];
    if (!ch->rm->host_api || (idx != CH_NVENC && idx != CH_NVDEC)) return 0;
    const u32 handle = 0x00430000u + (u32)idx;
    if (!slots[idx].attempted) {
        slots[idx].attempted = true;
        slots[idx].client = ch->rm->client;
        mem_alloc_params_t mp = {0};
        mp.owner = ch->rm->client;
        mp.type = 13u; /* NVOS32_TYPE_NOTIFIER */
        mp.attr = (1u << 23) | (1u << 25) | (2u << 27);
        /* 4 KiB, PCI/system memory, physically contiguous, uncached. */
        mp.flags = NVOS32_ALLOC_FLAGS_ALIGN_FORCE;
        mp.size = mp.alignment = 4096;
        if (!nv_rm_alloc(ch->card, ch->rm, RM_DEVICE, handle,
                         0x3eu /* NV01_MEMORY_SYSTEM */, &mp, sizeof mp)) {
            kwarn("nv-chan", "%s: error notifier allocation unavailable (%u)",
                  ch->name, nv_last_alloc_status);
            return 0;
        }
        void *cpu = NULL;
        u32 status = nvrm_host_map_memory(ch->rm->client, RM_SUBDEVICE,
                                          handle, 0, 4096, &cpu, 0);
        if (status || !cpu) {
            kwarn("nv-chan", "%s: error notifier mapping unavailable (%u)", ch->name, status);
            /* A partial/non-null failed mapping has uncertain ownership. Keep
             * it allocated; at most one 4 KiB page per codec for this boot. */
            if (!cpu && !nv_rm_free(ch->card, ch->rm, RM_DEVICE, handle))
                kwarn("nv-chan", "%s: unused error notifier retained after free failure", ch->name);
            return 0;
        }
        memset(cpu, 0, 4096); /* Initialize once, BEFORE channel publication. */
        __asm__ volatile("sfence" ::: "memory");
        slots[idx].cpu = (volatile nv_error_notification_t *)cpu;
    }
    if (slots[idx].client != ch->rm->client || !slots[idx].cpu) return 0;
    /* Never clear/recycle after publication, even on a failed channel open.
     * Native codec channels have boot lifetime; RM owns their dependencies. */
    ch->error_notifier = slots[idx].cpu;
    kinfo("nv-chan", "%s: host-RM error notifier handle=%#x CPU=%p (boot lifetime)",
          ch->name, handle, (const void *)ch->error_notifier);
    return handle;
}
