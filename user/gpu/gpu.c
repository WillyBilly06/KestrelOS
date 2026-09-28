/* gpu - what the graphics driver found, and a way to check it.
 *
 *   gpu                what is in the machine and what the driver read from it
 *   gpu selftest       drive the driver against a model of a card
 *   gpu shader-test    native vertex/fragment VM readback proof (real GPU)
 */
#include "kestrel.h"
#include "../libgl/gpuvm.h"

static void print_cards(void) {
    kgpuinfo_t g;
    int count = 0;

    for (uint32_t i = 0; enum_gpu(i, &g) == 0; i++) {
        count++;
        printf("\n" A_BOLD "%s" A_RESET "\n", g.name);
        printf("  %-14s %s\n", "Architecture", g.arch[0] ? g.arch : "unknown");
        printf("  %-14s %04x:%04x at %02x:%02x.%u\n", "PCI",
               g.pci_vendor, g.pci_device, g.bus, g.slot, g.func);

        if (g.vram_bytes) {
            char size[24];
            format_size(size, sizeof size, g.vram_bytes);
            printf("  %-14s %s%s\n", "Memory", size,
                   g.vram_exact ? "" : " (at least)");
        }
        if (g.boot_display) printf("  %-14s yes\n", "Boot display");
        if (g.note[0]) printf("  %-14s %s\n", "Note", g.note);

        if (g.firmware_needed) {
            printf("  %-14s %s%s\n", "Firmware", g.firmware_name,
                   g.firmware_present ? "" : "  " A_YELLOW "MISSING" A_RESET);
        }

        /* What can be drawn with, and what executes it.
         *
         * Deliberately not phrased as a DirectX version: DirectX is
         * Microsoft's, and what this has is its own implementation of those
         * interfaces.  Naming the interfaces offered is the true answer to
         * the question somebody means when they ask. */
        if (g.graphics_apis[0])
            printf("  %-14s %s\n", "Graphics APIs", g.graphics_apis);
        if (g.renderer[0])
            printf("  %-14s %s\n", "Drawn by", g.renderer);

        if (g.driver_version[0]) {
            printf("  %-14s %s%s%s\n", "Driver", g.driver_version,
                   g.driver_date[0] ? ", built " : "", g.driver_date);
        }

        /* What the card is doing, one row per engine.
         *
         * These are separate because they genuinely run at once - a video call
         * has the decoder busy while nothing is drawn at all - and a single
         * figure hides the thing somebody ran this to see.
         *
         * The two negative cases are printed as words rather than numbers, and
         * the distinction is the whole point of the row.  An engine that is not
         * fitted and an engine whose activity this driver cannot read would
         * both show as 0%, and 0% reads as idle - which is true of neither. */
        static const char *engine[4] = { "3D", "Copy", "Video encode",
                                         "Video decode" };
        bool any = false;
        for (int e = 0; e < 4; e++) if (g.engine_percent[e] != -1) any = true;

        if (any) {
            printf("\n  " A_BOLD "Engines" A_RESET);
            if (!g.engines_sampled)
                printf("  (no window collected yet)");
            printf("\n");

            for (int e = 0; e < 4; e++) {
                if (g.engine_percent[e] == -1) continue;   /* not fitted */
                if (g.engine_percent[e] == -2)
                    printf("    %-14s fitted; this driver cannot read its "
                           "activity\n", engine[e]);
                else
                    printf("    %-14s %d%%\n", engine[e], g.engine_percent[e]);
            }
        }
    }

    if (!count) printf("No graphics card was found on the bus.\n");
}

/* What the driver itself read out of the card, which is more than the bus can
 * say: the card's own description of itself, its sensors and its monitors. */
static void print_detail(void) {
    kgpudetail_t d;
    if (gpu_detail(&d) < 0) {
        printf("\nThe graphics driver has no card to report on.\n");
        printf("Run " A_BOLD "gpu selftest" A_RESET " to drive it against a "
               "model of one.\n");
        return;
    }

    printf("\n" A_BOLD "What the driver read from the card" A_RESET);
    if (d.modelled) printf("  " A_YELLOW "(a model, not a card)" A_RESET);
    printf("\n");

    printf("  %-14s %03x, revision %u.%u\n", "Chip", d.chipset,
           d.revision >> 4, d.revision & 0xF);
    printf("  %-14s %s (%s)\n", "Architecture", d.architecture, d.codename);

    if (d.vram_bytes) {
        char size[24];
        format_size(size, sizeof size, d.vram_bytes);
        printf("  %-14s %s\n", "Memory", size);
    }
    if (d.vbios_version[0])
        printf("  %-14s %s, from %s\n", "Video BIOS", d.vbios_version, d.vbios_source);
    if (d.temperature_c > -1000)
        printf("  %-14s %d degrees\n", "Temperature", d.temperature_c);
    if (d.fan_percent >= 0)
        printf("  %-14s %d%%\n", "Fan", d.fan_percent);

    if (d.outputs) {
        printf("\n  " A_BOLD "Connectors" A_RESET "  (%u, %u with a monitor)\n",
               d.outputs, d.monitors);
        for (int i = 0; i < d.outputs && i < 8; i++)
            if (d.connector[i][0]) printf("    %s\n", d.connector[i]);
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "shader-test")) {
        printf("gpu: native shader VM matrix, fragment/discard and texture readback test.\n");
        printf("Uses the shared rendering engine; a hardware fault can interrupt the desktop.\n");
        bool passed=gl_gpu_vm_validate();
        printf("Native shader VM readback: %s. See events for the stage and status.\n",passed?"PASS":"FAIL");
        return passed?0:1;
    }
    if (argc > 1 && !strcmp(argv[1], "selftest")) {
        printf("gpu: driving the graphics driver against a model of a card.\n\n");
        printf("      Nothing available to run on has an NVIDIA card in it, so\n");
        printf("      this runs the real driver against a model that answers\n");
        printf("      the same registers, holds a video BIOS of the same shape,\n");
        printf("      and puts a monitor on one connector that answers the two\n");
        printf("      wires the driver drives itself.\n\n");
        printf("      That exercises the identification, the ROM, the connector\n");
        printf("      table, the two-wire protocol and the display channel.  It\n");
        printf("      cannot confirm the register offsets against real silicon.\n\n");

        int failures = gpu_selftest();
        if (failures < 0) {
            printf(A_RED "The driver could not be started.\n" A_RESET);
            return 1;
        }
        if (failures) {
            printf("\n" A_RED "%d check(s) failed." A_RESET "  See "
                   A_BOLD "events" A_RESET " for what.\n", failures);
            return 1;
        }
        printf("\n" A_GREEN "The driver reads the card correctly." A_RESET "\n");
        print_detail();
        return 0;
    }

    if (argc > 1 && !strcmp(argv[1], "start")) {
        printf("gpu: starting the card's own co-processor.\n\n");
        printf("      Everything this driver does today reads the card.  This\n");
        printf("      writes to it: it hands the co-processor NVIDIA's firmware\n");
        printf("      and starts it, which is the step every later one needs -\n");
        printf("      channels, the copy engine, drawing.  Without it the card\n");
        printf("      cannot be given work at all.\n\n");
        printf(A_YELLOW "      The machine's own firmware already started that\n");
        printf("      co-processor, and it is what is putting this text on the\n");
        printf("      screen.  If the card will not accept the image, it stays\n");
        printf("      halted and the display does not come back until you power\n");
        printf("      the machine off and on.  Nothing is damaged.\n" A_RESET);
        /* Whether the card being started is the one drawing this text.  If the
         * display is on the iGPU (or any other adapter), a failed start cannot
         * blank the screen, and the test is safe.  This is exactly the question
         * that decides whether to be cautious, so answer it from what the
         * driver actually found rather than leaving the reader to guess. */
        kgpuinfo_t g;
        int found_nvidia = 0, nvidia_is_boot = 0, other_boot = 0;
        char boot_name[64] = "";
        for (uint32_t i = 0; enum_gpu(i, &g) == 0; i++) {
            if (g.pci_vendor == 0x10de) {
                found_nvidia = 1;
                if (g.boot_display) nvidia_is_boot = 1;
            } else if (g.boot_display) {
                other_boot = 1;
                strlcpy(boot_name, g.name, sizeof boot_name);
            }
        }
        if (found_nvidia && !nvidia_is_boot && other_boot) {
            printf(A_GREEN "      Good news for this machine: your display is "
                   "driven by\n      %s, not the NVIDIA card. So a failed start "
                   "CANNOT\n      blank your screen - this test is safe to run "
                   "here.\n" A_RESET, boot_name);
        } else if (nvidia_is_boot) {
            printf(A_YELLOW "      On this machine the NVIDIA card is driving the "
                   "display,\n      so a failed start may blank it until a power "
                   "cycle.\n      The full log is written to "
                   "KESTREL\\KERNEL.LOG on the boot\n      disk, so the result "
                   "survives even a blank screen.\n" A_RESET);
        }

        printf("\n      This has never run against real hardware before.\n\n");

        int r = gpu_start();
        if (r == 0) {
            printf(A_GREEN "The co-processor started." A_RESET "  See "
                   A_BOLD "events" A_RESET " for what it reported.\n");
            return 0;
        }
        printf(A_RED "It did not start." A_RESET "  See " A_BOLD "events"
               A_RESET " for the control register and why.\n");
        return 1;
    }

    if (argc > 1) {
        printf("usage: gpu [selftest | start | shader-test]\n");
        return 1;
    }

    print_cards();
    print_detail();
    return 0;
}
