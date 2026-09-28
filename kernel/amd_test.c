/* amd_test.c - running the AMD driver against a card that is not there.
 *
 * Every check below is the driver doing the thing a driver does, against a
 * model that answers the way AMD's documentation says a card answers and
 * refuses what a card would refuse.  Where the model is deliberately
 * unhelpful - an unsigned firmware image, a message whose argument arrived
 * late, a packet whose declared length is a lie - the check is that the driver
 * noticed, not that it carried on.
 */
#include "kernel.h"
#include "mm.h"
#include "pci.h"
#include "time.h"
#include "klog.h"
#include "amd.h"

static amd_card_t the_card;
static bool attached;

amd_card_t *amd_model_card(void) { return attached ? &the_card : NULL; }

bool amd_attach_model(void) {
    if (attached) return true;

    size_t size = 0;
    volatile u8 *regs = amd_model_attach(&size);
    if (!regs) return false;

    memset(&the_card, 0, sizeof the_card);
    the_card.regs = regs;
    the_card.regs_size = size;
    the_card.modelled = true;
    the_card.temperature_c = -1000;

    attached = true;
    return true;
}

/* --------------------------------------------------------------- the ring */

#define RING_WORDS 256

static int test_ring(amd_card_t *c) {
    int failures = 0;

    u64 phys = 0;
    u32 *page = dma_alloc_pages(1, &phys);
    if (!page) {
        kerr("amd-test", "no memory for a command ring");
        return 1;
    }
    memset(page, 0, PAGE_SIZE);

    /* The ring, then the two places the card writes back to, in the same page
     * so the addresses are real ones the card could reach. */
    u32 *ring_memory = page;
    volatile u32 *rptr = (volatile u32 *)(page + RING_WORDS);
    volatile u64 *fence = (volatile u64 *)(page + RING_WORDS + 4);
    u64 fence_gpu = phys + (RING_WORDS + 4) * 4;

    static amd_ring_t ring;
    if (!amd_ring_init(&ring, ring_memory, phys, RING_WORDS, rptr, fence)) {
        kerr("amd-test", "the ring would not initialise");
        return 1;
    }

    /* Where the card is told the ring got longer.  On real silicon this is an
     * offset into the doorbell aperture; here it is a register the model
     * watches, and either way it comes from where the discovery table put the
     * graphics block. */
    ring.doorbell = (c->gc_base + 0x10) * 4;
    amd_model_ring_attach(ring_memory, RING_WORDS, rptr, fence, ring.doorbell);

    /* A fence: the card writes a value when everything before it is done. */
    if (!amd_ring_emit_fence(c, &ring, fence_gpu, 0x1234ABCDu)) {
        kerr("amd-test", "a fence could not be submitted");
        failures++;
    } else if (!amd_ring_wait_fence(&ring, 0x1234ABCDu, 50)) {
        kerr("amd-test", "the fence never landed; the card was told about "
                         "work it did not see");
        failures++;
    }

    /* A call into a buffer of packets elsewhere, which is how all real work is
     * submitted. */
    if (!amd_ring_emit_indirect(c, &ring, phys + 2048, 64)) {
        kerr("amd-test", "an indirect buffer could not be submitted");
        failures++;
    }

    /* An unaligned one has to be refused: the command processor requires
     * thirty-two byte alignment and would fetch from the wrong place. */
    if (amd_ring_emit_indirect(c, &ring, phys + 2048 + 8, 64)) {
        kerr("amd-test", "an unaligned indirect buffer was accepted");
        failures++;
    }

    /* And the one that matters most.  A packet whose header declares more
     * words than were written desynchronises the ring permanently, and the
     * failure surfaces on some later packet with nothing to point at the
     * cause.  It has to be caught here and backed out. */
    u32 before = ring.write;
    amd_ring_begin(&ring, 8);
    for (int i = 0; i < 7; i++) amd_ring_write(&ring, 0);
    if (amd_ring_commit(c, &ring)) {
        kerr("amd-test", "a packet that declared 8 words and wrote 7 was "
                         "committed");
        failures++;
    } else if (ring.write != before) {
        kerr("amd-test", "the short packet was rejected but left %u words in "
                         "the ring", (ring.write - before) & (RING_WORDS - 1));
        failures++;
    }

    /* The ring has to still work afterwards, or the back-out was not clean. */
    if (!amd_ring_emit_fence(c, &ring, fence_gpu, 0x5678DEF0u) ||
        !amd_ring_wait_fence(&ring, 0x5678DEF0u, 50)) {
        kerr("amd-test", "the ring did not recover from the rejected packet");
        failures++;
    }

    /* Round the ring several times, which is where a pointer treated as bytes
     * rather than words comes apart. */
    int laps = 0;
    for (int i = 0; i < 120; i++) {
        u64 value = 0x1000u + (u64)i;
        if (!amd_ring_emit_fence(c, &ring, fence_gpu, value)) break;
        if (!amd_ring_wait_fence(&ring, value, 50)) break;
        laps++;
    }
    if (laps != 120) {
        kerr("amd-test", "only %d of 120 fences went round the ring", laps);
        failures++;
    }

    if (amd_model_ring_errors()) {
        kerr("amd-test", "the card saw %d malformed packet(s)",
             amd_model_ring_errors());
        failures++;
    }

    if (!failures)
        kinfo("amd-test", "the command ring works: %u packets accepted, %u "
                          "words, the last one opcode %02x, and a packet whose "
                          "declared length was wrong was refused and backed out",
              amd_model_ring_packets(), amd_model_ring_words(),
              amd_model_last_opcode());

    amd_model_ring_detach();
    return failures;
}

/* --------------------------------------------------------------- the whole */

int amd_drive_test(void) {
    int failures = 0;

    if (!amd_attach_model()) {
        kwarn("amd-test", "the model could not be brought up");
        return 1;
    }

    amd_card_t *c = amd_model_card();
    if (!c) {
        kerr("amd-test", "the model registered no card");
        return 1;
    }

    /* What it is.  The identifier table is a courtesy; nothing below depends
     * on the card being in it. */
    amd_identify(c, amd_model_device(), 0xC8);
    if (strcmp(c->codename, "Navi 31") || strcmp(c->architecture, "RDNA 3")) {
        kerr("amd-test", "the card came out as %s (%s)", c->codename,
             c->architecture);
        failures++;
    }

    /* How much memory, which has to come first because the table that says
     * everything else sits relative to the top of it. */
    if (!amd_read_vram_size(c) || c->vram_bytes != amd_model_vram()) {
        kerr("amd-test", "memory came out as %llu MiB, expected %llu",
             (unsigned long long)(c->vram_bytes >> 20),
             (unsigned long long)(amd_model_vram() >> 20));
        failures++;
    }

    /* The card's description of itself.  This is the part that makes the
     * driver work on silicon it has never seen. */
    if (!amd_read_discovery(c)) {
        kerr("amd-test", "the card's own description of its blocks was not "
                         "read");
        failures++;
    } else if (c->ips != 9) {
        kerr("amd-test", "%d block(s) found, expected 9", c->ips);
        failures++;
    } else if (!c->gc_base || !c->mp0_base || !c->mp1_base) {
        kerr("amd-test", "the graphics block or a co-processor has no address");
        failures++;
    } else if (c->gfx_major != 11 || c->gfx_minor != 0) {
        kerr("amd-test", "the graphics block reports version %d.%d, expected "
                         "11.0", c->gfx_major, c->gfx_minor);
        failures++;
    } else {
        kinfo("amd-test", "the card described itself: %d blocks, graphics "
                          "version %d.%d, and where each one lives",
              c->ips, c->gfx_major, c->gfx_minor);
    }

    /* Its ATOM BIOS. */
    if (!amd_read_vbios(c) || !amd_parse_vbios(c)) {
        kerr("amd-test", "the ATOM image was not read");
        failures++;
    } else if (memcmp(c->vbios_version, "113-D7020100-102", 16)) {
        kerr("amd-test", "the ATOM version came out as \"%s\"", c->vbios_version);
        failures++;
    } else {
        kinfo("amd-test", "ATOM image %s, from %s", c->vbios_version,
              c->vbios_source);
    }

    /* What is on the bracket, and which wires carry each one. */
    int connectors = amd_parse_connectors(c);
    if (connectors != 4) {
        kerr("amd-test", "%d connector(s) found, expected 4", connectors);
        failures++;
    } else if (c->connector[0].connector_id != AMD_CONNECTOR_VGA ||
               c->connector[1].connector_id != AMD_CONNECTOR_HDMI_TYPE_A ||
               c->connector[2].connector_id != AMD_CONNECTOR_DISPLAYPORT ||
               c->connector[3].connector_id != AMD_CONNECTOR_DISPLAYPORT ||
               c->connector[3].enum_id != 2) {
        kerr("amd-test", "the connector list was misread");
        failures++;
    } else if (!c->connector[2].has_i2c ||
               c->connector[2].i2c_line != amd_model_monitor_line()) {
        kerr("amd-test", "the connector's two wires were not found");
        failures++;
    } else if (c->i2c_buses != 4) {
        kerr("amd-test", "%d pin assignment(s) parsed, expected 4",
             c->i2c_buses);
        failures++;
    } else {
        kinfo("amd-test", "4 connectors: VGA, HDMI and two DisplayPorts, each "
                          "with the pins that drive its two wires");
    }

    /* The monitor.  The driver drove the pins itself, one edge at a time, and
     * something on the far end answered. */
    int monitors = amd_probe_monitors(c);
    const amd_connector_t *panel = NULL;
    for (int i = 0; i < c->connectors; i++)
        if (c->connector[i].monitor_present) panel = &c->connector[i];

    if (monitors != 1 || !panel) {
        kerr("amd-test", "%d monitor(s) answered, expected 1", monitors);
        failures++;
    } else if (memcmp(panel->edid, amd_model_edid(), 128)) {
        kerr("amd-test", "the monitor's identification came back altered");
        failures++;
    } else if (panel->width != 3840 || panel->height != 2160 ||
               panel->refresh_hz < 59 || panel->refresh_hz > 61) {
        kerr("amd-test", "the monitor's mode came out as %ux%u at %u Hz",
             panel->width, panel->height, panel->refresh_hz);
        failures++;
    } else if (strcmp(panel->monitor_name, "Kestrel UHD")) {
        kerr("amd-test", "the monitor's name came out as \"%s\"",
             panel->monitor_name);
        failures++;
    } else {
        int starts = 0, acks = 0, bytes = 0;
        amd_model_i2c_report(amd_model_monitor_line(), &starts, &acks, &bytes);
        kinfo("amd-test", "a monitor answered on the wires the driver drove "
                          "itself: \"%s\", %ux%u at %u Hz, over %d start "
                          "condition(s) and %d acknowledgement(s)",
              panel->monitor_name, panel->width, panel->height,
              panel->refresh_hz, starts, acks);
    }

    /* The power processor, which owns the clocks and the sensors. */
    if (!amd_smu_bring_up(c)) {
        kerr("amd-test", "the power processor did not come up");
        failures++;
    }

    /* And its strictness: a message whose argument was not written first must
     * be refused, because a real processor would have read the last one. */
    {
        int before = amd_model_smu_out_of_order();
        amd_wr32(c, (c->mp1_base + AMD_SMU_MSG) * 4, AMD_SMU_MSG_TEST);
        u32 answer = amd_rd32(c, (c->mp1_base + AMD_SMU_RESP) * 4);
        if (amd_model_smu_out_of_order() != before + 1 ||
            answer != AMD_SMU_RESP_BAD_PREREQ) {
            kerr("amd-test", "a message sent before its argument was accepted");
            failures++;
        }
    }

    /* A message it does not know has to come back as unknown, not as success. */
    if (amd_smu_send(c, 0x7F, 0, NULL)) {
        kerr("amd-test", "the power processor accepted a message it does not "
                         "have");
        failures++;
    }

    amd_read_sensors(c);
    if (c->temperature_c != amd_model_temperature()) {
        kerr("amd-test", "the temperature came out as %d, expected %d",
             c->temperature_c, amd_model_temperature());
        failures++;
    } else if (c->gfx_clock_mhz != 2400 || c->mem_clock_mhz != 2500) {
        kerr("amd-test", "the clocks came out as %u and %u MHz",
             c->gfx_clock_mhz, c->mem_clock_mhz);
        failures++;
    } else {
        kinfo("amd-test", "the power processor reports %d degrees, %u MHz "
                          "core and %u MHz memory",
              c->temperature_c, c->gfx_clock_mhz, c->mem_clock_mhz);
    }

    /* The security processor.  The sequence is driven correctly and the image
     * is refused, because nothing outside AMD is signed by AMD - and a driver
     * that reported this as success would be reporting a capability that does
     * not exist. */
    {
        static u8 firmware[256];
        memset(firmware, 0x5A, sizeof firmware);

        /* A delta, not a total: the model is attached once and this test may
         * be run more than once against it. */
        int rejections_before = amd_model_psp_rejections();

        if (amd_psp_bootloader_load(c, AMD_PSP_BL_LOAD_SYSDRV, 0x40000000ULL, 50)) {
            kerr("amd-test", "the security processor accepted an image nobody "
                             "signed");
            failures++;
        } else if (amd_model_psp_rejections() != rejections_before + 1) {
            kerr("amd-test", "the load failed without reaching the signature "
                             "check");
            failures++;
        } else {
            kinfo("amd-test", "the security processor's boot sequence is "
                              "driven correctly and the unsigned image is "
                              "refused, which is what a card does");
        }
    }

    /* On a machine whose firmware already brought the card up, the secure OS
     * is running before the driver loads - and then the command ring on top of
     * it can be created. */
    amd_model_sos_already_running(true);
    if (!amd_psp_ring_create(c, 0x40010000ULL, 4096)) {
        kerr("amd-test", "the command ring for the security processor was not "
                         "created");
        failures++;
    }

    failures += test_ring(c);

    /* ---------------------------------------------------------------------
     * And the same thing again the way it actually happens at boot.
     *
     * Everything above drives one piece at a time, which is how a fault gets
     * named.  But the sequence that runs on a real card is a different piece
     * of code: it has its own order, its own guards, and its own decisions
     * about what to skip when a step comes back empty.  Testing the pieces
     * and not the sequence leaves the only code that will ever run on
     * silicon as the only code never run here.
     *
     * So this runs it, on a card struct with nothing filled in but the way to
     * reach the hardware, and checks it arrives where the careful version
     * arrived.
     */
    {
        static amd_card_t fresh;
        memset(&fresh, 0, sizeof fresh);
        fresh.regs = c->regs;
        fresh.regs_size = c->regs_size;
        fresh.modelled = true;
        fresh.temperature_c = -1000;
        fresh.pci_device = amd_model_device();
        fresh.pci_revision = 0xC8;

        amd_bring_up(&fresh);

        if (!fresh.codename || strcmp(fresh.codename, c->codename)) {
            kerr("amd-test", "the boot sequence identified the card as %s, "
                             "not %s", fresh.codename ? fresh.codename : "(none)",
                 c->codename);
            failures++;
        }
        if (fresh.vram_bytes != c->vram_bytes) {
            kerr("amd-test", "the boot sequence found %llu MiB, not %llu",
                 (unsigned long long)(fresh.vram_bytes >> 20),
                 (unsigned long long)(c->vram_bytes >> 20));
            failures++;
        }
        if (fresh.ips != c->ips) {
            kerr("amd-test", "the boot sequence found %d block(s), not %d",
                 fresh.ips, c->ips);
            failures++;
        }
        if (fresh.connectors != c->connectors) {
            kerr("amd-test", "the boot sequence found %d connector(s), not %d",
                 fresh.connectors, c->connectors);
            failures++;
        }
        if (!fresh.vbios_valid) {
            kerr("amd-test", "the boot sequence did not read the ATOM image");
            failures++;
        }
        /* A monitor answering is the end of a chain that starts at the ATOM
         * object table and runs through the GPIO assignment and the two
         * wires; if it arrives, everything between it and the registers
         * worked. */
        int answered = 0;
        for (int i = 0; i < fresh.connectors; i++)
            if (fresh.connector[i].edid_valid) answered++;
        if (!answered) {
            kerr("amd-test", "the boot sequence reached no monitor");
            failures++;
        } else {
            kinfo("amd-test", "the boot sequence, run start to finish the way "
                              "it runs on a card, found %s with %llu MiB, %d "
                              "block(s), %d connector(s) and a monitor on one "
                              "of them",
                  fresh.codename, (unsigned long long)(fresh.vram_bytes >> 20),
                  fresh.ips, fresh.connectors);
        }
    }

    if (!failures)
        kinfo("amd-test", "the AMD driver reads the card's own description of "
                          "itself, its ATOM tables, its connectors, its "
                          "monitor and its sensors, and drives both "
                          "co-processors and the command ring correctly");
    return failures;
}
