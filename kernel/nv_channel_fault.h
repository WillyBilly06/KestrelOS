/* Included by nv_chan.c after channel/control definitions. Error-only capture;
 * never resets/replays a channel or treats a missing query as a clean bill. */
typedef struct {
    u32 addr_hi, addr_lo, fault_type;
    char fault_string[32];
    u64 shader_va[7];
} nv_fault_info_t;
_Static_assert(sizeof(nv_fault_info_t) == 104, "NV906F MMU fault ABI");
_Static_assert(__builtin_offsetof(nv_fault_info_t, shader_va) == 48,
               "NV906F shader address alignment");

static void nv_fault_query(nv_channel_t *ch, u32 command, void *data, u32 bytes,
                            bool *valid) {
    u32 got = 0;
    bool ok = nv_rm_control(ch->card, ch->rm, ch->h_channel, command,
                            data, bytes, data, bytes, &got);
    u32 status = nv_last_control_status;
    *valid = ok && got >= bytes;
    kerr("nv-fault", "%s query=%#x RM-status=%#x reply=%u expected=%u valid=%u",
          ch->name, command, status, got, bytes, *valid);
}

static void nv_fault_shadow_path(nv_channel_t *ch, u64 va) {
    /* This is the CPU's intended mapping, not a readback of GPU VRAM/TLB. */
    const u32 shift[6] = {56, 47, 38, 29, 21, 12};
    const u32 mask[6] = {1, 511, 511, 511, 255, 511};
    u64 table = ch->vmm.root_gpu;
    for (u32 level = 0; level < 6; level++) {
        if (table < ch->vmm.pool_gpu ||
            table - ch->vmm.pool_gpu + PAGE_SIZE > (u64)ch->vmm.used * PAGE_SIZE) {
            kerr("nv-fault", "shadow VA=%#llx level=%u table=%#llx outside local table shadow",
                  (unsigned long long)va, level, (unsigned long long)table);
            break;
        }
        u32 index = (u32)(va >> shift[level]) & mask[level];
        if (level == 4) index = index * 2 + 1; /* dual PDE small-page half */
        const u64 *entries = (const u64 *)(ch->vmm.pool + table - ch->vmm.pool_gpu);
        u64 entry = entries[index];
        kerr("nv-fault", "shadow VA=%#llx level=%u table=%#llx index=%u entry=%#llx",
              (unsigned long long)va, level, (unsigned long long)table, index,
              (unsigned long long)entry);
        if (level == 5 || !(entry & 6u)) break;
        table = entry & 0x000ffffffffff000ull;
    }
}

static void nv_channel_capture_fault(nv_channel_t *ch, u32 start, u32 bytes,
                                      u32 expected) {
    static bool captured[CH_COUNT];
    u32 index;
    for (index = 0; index < CH_COUNT; index++) if (&channels[index] == ch) break;
    if (index == CH_COUNT || captured[index]) return;
    captured[index] = true; /* MMU query consumes its saved record: only once. */
    nv_error_notification_t notification = {0};
    if (ch->error_notifier) {
        bool stable = nv_error_notification_snapshot(ch->error_notifier, &notification);
        kerr("nv-fault", "%s RM-error-notifier status=%#x exception=%u engine=%#x timestamp=%#llx matching-reads=%u (diagnostic only; zero is not proof of no fault)",
              ch->name, notification.status, notification.info32, notification.info16,
              ((unsigned long long)notification.time_hi << 32) | notification.time_lo, stable);
    } else {
        kerr("nv-fault", "%s RM-error-notifier unavailable", ch->name);
    }
    kerr("nv-fault", "BEGIN channel=%#x class=%#x chid=%u runlist=%u token=%#x PUT=%u sem=%#x expected=%#x root=%#llx tables=%u/%u",
          ch->h_channel, ch->obj_class, ch->chid, ch->runlist_id,
          ch->work_submit_token, ch->gp_put, ch->sem[0], expected,
          (unsigned long long)ch->vmm.root_gpu, ch->vmm.used, ch->vmm.pages);
    /* Preserve the submitted packet even if RM subsequently recovers the engine. */
    if (start <= PUSHBUF_BYTES && bytes <= PUSHBUF_BYTES - start) {
        u32 words = bytes / 4u;
        for (u32 i = 0; i < words && i < 64u; i++)
            kerr("nv-fault", "push+%#x=%#x", start + i * 4u,
                  *(const volatile u32 *)(ch->pushbuf + start + i * 4u));
        if (words > 64u) kerr("nv-fault", "push truncated: %u of %u words saved", 64u, words);
    }
    u32 state = 0; bool valid = false;
    nv_fault_query(ch, 0xb06f010fu, &state, sizeof state, &valid);
    if (valid)
        kerr("nv-fault", "HW-state=%#x next=%u reload=%u engine-fault=%u pbdma-fault=%u acquire-fail=%u",
              state, state & 1u, (state >> 1) & 1u, (state >> 3) & 1u,
              (state >> 4) & 1u, (state >> 5) & 1u);
    u8 deferred = 0;
    nv_fault_query(ch, 0x906f0105u, &deferred, sizeof deferred, &valid);
    if (valid) kerr("nv-fault", "debugger-deferred-RC=%u (not general RC/error status)", deferred);
    nv_fault_info_t fault = {0};
    nv_fault_query(ch, 0x906f0106u, &fault, sizeof fault, &valid);
    if (valid) {
        fault.fault_string[31] = 0;
        u64 address = ((u64)fault.addr_hi << 32) | fault.addr_lo;
        kerr("nv-fault", "saved-MMU address=%#llx type=%#x text='%s' (query consumes saved record; empty is not proof of no fault)",
              (unsigned long long)address, fault.fault_type, fault.fault_string);
        for (u32 i = 0; i < 7; i++)
            if (fault.shader_va[i]) kerr("nv-fault", "shader[%u]=%#llx", i,
                                          (unsigned long long)fault.shader_va[i]);
        nv_fault_shadow_path(ch, address);
    }
    kerr("nv-fault", "END channel=%#x; submission remains quarantined; GP_GET not exposed by this snapshot",
          ch->h_channel);
}
