/* nv_gsp.c - the co-processor half, for Turing and everything after it.
 *
 * From Turing onward an NVIDIA card is not driven by writing to its registers.
 * A processor on the card - the GSP - runs NVIDIA's own firmware, and the
 * driver's job becomes sending it requests and reading its replies. The
 * display engine, the graphics engine and the memory manager all sit behind
 * it. There is no sequence of register writes that substitutes: the hardware
 * checks a signature before it will run anything, and only NVIDIA holds the
 * key.
 *
 * That is why Linux's driver for these cards needs the same file, and why
 * every distribution ships it separately or not at all. It is not a gap in
 * anyone's understanding; it is a lock.
 *
 * So what this file does is the part that can be done:
 *
 *   - work out exactly which files this card needs, by architecture
 *   - say whether they are here, and where to get them if not
 *   - when they are here, check they are what they claim to be and set out the
 *     boot sequence they would go through
 *
 * What it does not do is pretend. The boot sequence below is written from the
 * published interface and has never been run against a card, because running
 * it needs firmware that cannot be redistributed and hardware that no virtual
 * machine provides. It is marked as such where it matters, and the driver
 * reports the card as running on the firmware's framebuffer rather than
 * claiming an engine it has not started.
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "klog.h"
#include "firmware.h"
#include "nv.h"
#include "nvkms_port.h"
#include "nv_fwimage.h"

/* --------------------------------------------------------------- the files
 *
 * Three of them, and they are per-architecture. The directory is named after
 * the first chip of that architecture rather than the chip in the machine,
 * which is the thing people get wrong when they go looking.
 */
static const char *gsp_directory(u32 chipset) {
    const char *dir = nv_gsp_directory(chipset);
    return dir ? dir : "ga102";
}

/* Which release, which matters as much as which directory.  A release that
 * shipped before the chip did contains no image for it, however right the
 * directory is - and 535, which the Turing-through-Ada files come from,
 * shipped a year and a half before Blackwell.  Naming it for a 5070 Ti would
 * send its owner looking for a file nobody ever made.  So the release comes
 * from the same per-architecture table the directory does. */

typedef struct {
    char       name[96];
    const char *what;
    bool       present;
    u64        size;
} gsp_file_t;

#define GSP_MAX_FILES 4
static gsp_file_t files[GSP_MAX_FILES];
static int        file_count;

/* Which files this generation's co-processor is actually started from.
 *
 * This used to ask for the same three names on every card: the firmware
 * itself, and the pair of signed images that load and unload it.  That is the
 * arrangement Turing and Ampere use, and it was written from those - but it is
 * not what ships for the newer parts, and asking for a file that does not
 * exist fails in the least useful way possible, by reporting a missing
 * firmware that nobody could have supplied because it was never published
 * under that name.
 *
 * What is actually published, checked against the firmware collection rather
 * than assumed:
 *
 *   Turing and Ampere   gsp, booter_load, booter_unload, bootloader
 *   Ada                 booter_load, booter_unload, bootloader, scrubber
 *   Hopper and Blackwell    bootloader, fmc
 *
 * The last of those is a different scheme, not a renaming: from Hopper onwards
 * the co-processor is brought up by a firmware management controller image
 * rather than by a pair of booters, so a driver looking for booters on a
 * Blackwell card is looking for something that generation does not have.
 */
static void name_files(u32 chipset) {
    const char *dir = gsp_directory(chipset);
    const char *rel = nv_gsp_release(chipset);
    u32 family = chipset & 0x1F0;

    file_count = 0;

    /* Hopper is 0x180 and Blackwell 0x1B0; both boot through the management
     * controller rather than through booters. */
    bool fmc_based = family >= 0x180;

    if (fmc_based) {
        snprintf(files[file_count].name, sizeof files[0].name,
                 "nvidia/%s/gsp/fmc-%s.bin", dir, rel);
        files[file_count].what = "the image that brings the co-processor up";
        file_count++;

        snprintf(files[file_count].name, sizeof files[0].name,
                 "nvidia/%s/gsp/bootloader-%s.bin", dir, rel);
        files[file_count].what = "and the loader it hands control to";
        file_count++;
        return;
    }

    /* Ada publishes no firmware image of its own under this directory; it is
     * started from the same pair of booters as Ampere, with a scrubber that
     * clears protected memory first. */
    bool has_own_image = family < 0x190;

    if (has_own_image) {
        snprintf(files[file_count].name, sizeof files[0].name,
                 "nvidia/%s/gsp/gsp-%s.bin", dir, rel);
        files[file_count].what = "the firmware the co-processor runs";
        file_count++;
    }

    snprintf(files[file_count].name, sizeof files[0].name,
             "nvidia/%s/gsp/booter_load-%s.bin", dir, rel);
    files[file_count].what = "the signed image that loads it into protected memory";
    file_count++;

    snprintf(files[file_count].name, sizeof files[0].name,
             "nvidia/%s/gsp/booter_unload-%s.bin", dir, rel);
    files[file_count].what = "and the one that takes it back out";
    file_count++;

    snprintf(files[file_count].name, sizeof files[0].name,
             "nvidia/%s/gsp/bootloader-%s.bin", dir, rel);
    files[file_count].what = "the loader the booter hands control to";
    file_count++;
}

/* ------------------------------------------------- what kind of processor
 *
 * "The GSP" names a job, not a design, and the silicon doing that job changed
 * once - which matters more than it sounds, because it means the boot sequence
 * in nv_falcon.c applies to exactly one generation of the cards that have a
 * GSP at all.
 *
 *   Turing puts a Falcon there.  A Falcon is NVIDIA's own small processor,
 *   the same one that has been scattered around their cards since Fermi doing
 *   small jobs, and nv_falcon.c drives it: hold in reset, wait for the memory
 *   scrubber, push code through a tagged port in blocks, set a start address,
 *   let go.
 *
 *   Ampere and everything after it - Ada, Hopper, and the Blackwell in an RTX
 *   50 card - put a RISC-V core there instead.  NVIDIA calls it Peregrine.  It
 *   lives at the same base address and the old Falcon registers are still
 *   present around it, which is the trap: a driver written for Turing can read
 *   those registers on a 5070 Ti, get plausible answers, run the whole Falcon
 *   sequence without any of it failing, and start nothing at all.  The core
 *   that would have run the code is not the core the code was pushed into.
 *
 * So the generation is named, and the Falcon sequence is refused rather than
 * attempted on the cards it does not apply to.  What this file does NOT claim
 * is the RISC-V bring-up sequence: the register offsets for it are not ones
 * this driver has verified, and writing a plausible-looking sequence out of
 * guesses would be worse than saying so, because it would look finished.
 */
typedef enum { GSP_NONE, GSP_FALCON, GSP_RISCV, GSP_RISCV_FSP } gsp_core_t;

/* Which co-processor, and - just as importantly - who is allowed to start it.
 *
 * There are three arrangements, not two.  Turing has a Falcon.  Ampere and Ada
 * have a RISC-V core the host starts itself by writing its boot registers.
 * Hopper and Blackwell have the same RISC-V core, but the card's security
 * processor owns the starting of it and those registers are locked against the
 * host - so the host posts a message asking for the boot instead.
 *
 * The families do not sort the way the marketing names do, which is why this
 * is a list rather than a threshold: Hopper (0x180) shipped between Ampere
 * (0x170) and Ada (0x190), and Hopper takes the newer route while Ada takes
 * the older one.  A simple ">= this number" test gets Ada wrong.
 */
gsp_core_t nv_gsp_core(u32 chipset) {
    u32 family = chipset & 0x1F0;
    if (family < 0x160) return GSP_NONE;        /* no GSP before Turing      */
    if (family < 0x170) return GSP_FALCON;      /* Turing                    */
    if (family == 0x180) return GSP_RISCV_FSP;  /* Hopper                    */
    if (family >= 0x1A0) return GSP_RISCV_FSP;  /* Blackwell, both kinds     */
    return GSP_RISCV;                           /* Ampere and Ada            */
}

const char *nv_gsp_core_name(u32 chipset) {
    switch (nv_gsp_core(chipset)) {
    case GSP_FALCON:     return "a Falcon";
    case GSP_RISCV:      return "a RISC-V core the host starts";
    case GSP_RISCV_FSP:  return "a RISC-V core the security processor starts";
    default:             return "nothing";
    }
}

/* ------------------------------------------------------------ the registers
 *
 * The parts of the map the boot sequence touches. Reading them is safe and
 * says something useful even when the firmware is absent: whether the
 * co-processor is already running, which it will be if the machine's own
 * firmware started it during boot.
 *
 * These are the Falcon names, and on a RISC-V GSP they still read - which is
 * why the generation is decided from the chip above rather than inferred from
 * whether these answer.
 */
#define NV_PGSP_FALCON_CPUCTL     0x110100
#define NV_PGSP_FALCON_CPUCTL_HALTED (1u << 4)
#define NV_PGSP_FALCON_MAILBOX0   0x110040
#define NV_PGSP_FALCON_MAILBOX1   0x110044
#define NV_PGSP_FALCON_HWCFG      0x110108

#define NV_PFB_PRI_MMU_WPR2_ADDR_LO 0x1FA824
#define NV_PFB_PRI_MMU_WPR2_ADDR_HI 0x1FA828

/* --------------------------------------------------------------- reporting */

/* ------------------------------------------------- what is inside the files
 *
 * The two images are packaged differently, and neither is raw.
 *
 * The bootloader is in NVIDIA's own container: a six-word header beginning
 * with 0x10de - their vendor number used as a magic - saying where the payload
 * starts and how long it is.  The values below were read out of the file for
 * this card and check out exactly: the payload's offset plus its length lands
 * on the last byte of the file.
 *
 * The management controller is an ELF image for the RISC-V core, which is why
 * it begins with the same four bytes as any other ELF.  Its sections have to
 * be walked to find the code and the data separately, because the boot ROM is
 * told where each one is rather than being handed the file.
 */
/* The container and ELF parsing live in nv_fwimage.c so they can be checked on
 * the host against the real firmware; see nv_fwimage.h.  This file uses them
 * through nv_fw_container_payload / nv_fw_is_elf / nv_fw_elf_section. */

/* Start the co-processor by writing its boot registers directly.
 *
 * THIS IS NOT THE PATH A BLACKWELL CARD TAKES.  It is kept because it is the
 * right path for Ampere and Ada, and it is not called on anything newer.
 *
 * The note that used to sit here said the difference between NVIDIA's resource
 * manager and nouveau was "conditional on something neither of them states
 * plainly, and I have not established what".  That has now been established,
 * on 2026-08-30, and the answer is that this card does not use these
 * registers at all:
 *
 *   nvkm/subdev/gsp/gb202.c binds a GB202 - the die in an RTX 5070 Ti - to
 *   gh100_gsp_init, Hopper's routine rather than Ada's.
 *
 *   nvkm/subdev/gsp/gh100.c contains no register write for this.  It calls
 *   nvkm_fsp_boot_gsp_fmc(), handing the image, its hash, its public key and
 *   its signature to the card's security processor and asking that to start
 *   the co-processor.
 *
 *   On these parts the security processor owns that job, and the registers
 *   below are locked against the host.
 *
 * That same note also claimed these offsets came "from dev_riscv_pri.h for
 * gb202".  They did not.  Blackwell's published dev_riscv_pri.h contains
 * exactly one of them - CPUCTL, at 0x388, which is correct - and none of the
 * BCR registers; those came from an earlier generation's header and were
 * described as Blackwell's.  A provenance claim that cannot be checked is
 * worse than none, because it stops the next reader checking.
 *
 * See NV_PFSP_* in nv.h for the route this card really takes.
 *
 * The order is not adjustable.  The boot ROM latches where it is to fetch from
 * when the configuration is locked, and reads the start bit after that; moving
 * either changes what the card starts, or whether it starts anything.
 */
static bool start_from_fmc(nv_card_t *c, u64 args_phys, u64 fmc_code_phys,
                           u64 fmc_data_phys, u64 manifest_phys) {
    u32 gsp = NV_FALCON_GSP;

    /* The arguments the image is started with, left in the mailboxes. */
    nv_wr32(c, gsp + NV_PFALCON_MAILBOX0, (u32)args_phys);
    nv_wr32(c, gsp + NV_PFALCON_MAILBOX1, (u32)(args_phys >> 32));

    /* Where the three pieces are, each shifted before it is written. */
    u64 code = fmc_code_phys >> NV_PRISCV_BCR_ADDR_SHIFT;
    u64 data = fmc_data_phys >> NV_PRISCV_BCR_ADDR_SHIFT;
    u64 manifest = manifest_phys >> NV_PRISCV_BCR_ADDR_SHIFT;

    nv_wr32(c, gsp + NV_PRISCV_BCR_FMCCODE_LO, (u32)code);
    nv_wr32(c, gsp + NV_PRISCV_BCR_FMCCODE_HI, (u32)(code >> 32));
    nv_wr32(c, gsp + NV_PRISCV_BCR_FMCDATA_LO, (u32)data);
    nv_wr32(c, gsp + NV_PRISCV_BCR_FMCDATA_HI, (u32)(data >> 32));
    nv_wr32(c, gsp + NV_PRISCV_BCR_PKCPARAM_LO, (u32)manifest);
    nv_wr32(c, gsp + NV_PRISCV_BCR_PKCPARAM_HI, (u32)(manifest >> 32));

    /* Fetch from system memory the processor and the card both see, and lock
     * the configuration - which is what makes the boot ROM take it. */
    nv_wr32(c, gsp + NV_PRISCV_BCR_DMACFG,
            NV_PRISCV_BCR_DMACFG_TARGET_COHERENT_SYS | NV_PRISCV_BCR_DMACFG_LOCK);

    /* And go. */
    nv_wr32(c, gsp + NV_PRISCV_CPUCTL, NV_PRISCV_CPUCTL_STARTCPU);

    /* It has started when it stops reporting itself halted.  A core that never
     * leaves that state has either been given an image it will not accept or
     * an address it cannot reach, and both look the same from here. */
    for (int i = 0; i < 2000; i++) {
        u32 ctl = nv_rd32(c, gsp + NV_PRISCV_CPUCTL);
        if (ctl == 0xFFFFFFFFu) return false;
        if (!(ctl & NV_PRISCV_CPUCTL_HALTED)) return true;
        timer_mdelay(1);
    }
    return false;
}

/* ------------------------------------------------- inside the ELF image
 *
 * The management controller is not a plain image with a header.  It is a small
 * ELF holding four named sections: the code itself, and the three things that
 * let the card check the code is NVIDIA's before it runs it.
 *
 *     image       the firmware the co-processor executes
 *     signature   over that image
 *     publickey   the key the signature is checked against
 *     hash        of the image
 *
 * The boot ROM is told where each piece is rather than being handed the file,
 * so they have to be found by name.  The names below are what is actually in
 * the file for this card - read out of it rather than assumed - and a file
 * missing any of them is not one this can start a card from.
 *
 * Finding a section by name (nv_fw_elf_section) is in nv_fwimage.c, and it
 * reads both ELF32 (the FMC here) and ELF64 (the resident manager, which is
 * 64-bit), bounds-checking every header against the file.
 */

/* Read the real firmware files, if somebody has supplied them.
 *
 * This is not a test against a model.  The bytes are whatever was put in
 * firmware/, which for this machine is what NVIDIA published - so what it
 * establishes is that the driver understands the actual packaging, which is
 * the thing that has to be right before an image can be handed to a card at
 * all.  Where no firmware has been supplied it says so and passes: a machine
 * without it is not a broken machine.
 */
int nv_gsp_firmware_selftest(void) {
    static const struct { const char *name; bool elf; } expect[] = {
        { "nvidia/gb202/gsp/fmc-570.144.bin",        true  },
        { "nvidia/gb202/gsp/bootloader-570.144.bin", false },
    };

    int failures = 0, checked = 0;

    for (size_t i = 0; i < ARRAY_LEN(expect); i++) {
        firmware_t fw;
        if (!firmware_present(expect[i].name, NULL)) continue;
        if (!firmware_load(expect[i].name, &fw)) {
            kerr("nv-gsp", "%s is there and would not load", expect[i].name);
            failures++;
            continue;
        }
        checked++;

        if (expect[i].elf) {
            if (!nv_fw_is_elf(fw.data, fw.size)) {
                kerr("nv-gsp", "%s should be an ELF image and is not",
                     expect[i].name);
                failures++;
            } else {
                /* And the four pieces the card is told where to find.  A file
                 * that is an ELF but has none of them would pass the check
                 * above and fail at the point it matters. */
                static const char *want[] = { "image", "signature",
                                              "publickey", "hash" };
                for (size_t k = 0; k < ARRAY_LEN(want); k++) {
                    const u8 *sec = NULL;
                    u32 sec_size = 0;
                    if (!nv_fw_elf_section(fw.data, fw.size, want[k], &sec, &sec_size)) {
                        kerr("nv-gsp", "%s has no \"%s\" section",
                             expect[i].name, want[k]);
                        failures++;
                    } else {
                        kdebug("nv-gsp", "  %s: %u bytes", want[k], sec_size);
                    }
                }
            }
        } else {
            const u8 *payload = NULL;
            u32 payload_size = 0;
            if (!nv_fw_container_payload(fw.data, fw.size, &payload, &payload_size)) {
                kerr("nv-gsp", "%s should be a container and does not parse as "
                               "one", expect[i].name);
                failures++;
            } else if (payload + payload_size != fw.data + fw.size) {
                /* The payload should end where the file does.  Anything else
                 * means the header was read wrongly in a way that happens to
                 * stay inside the file, which is the failure that would
                 * otherwise only appear as a card that will not start. */
                kerr("nv-gsp", "%s: its payload ends %lld bytes from the end of "
                               "the file", expect[i].name,
                     (long long)((fw.data + fw.size) - (payload + payload_size)));
                failures++;
            }
        }
        firmware_free(&fw);
    }

    if (!checked)
        kinfo("nv-gsp", "no co-processor firmware has been supplied, so there "
                        "is nothing to check the packaging of");
    else if (!failures)
        kinfo("nv-gsp", "%d supplied firmware file(s) are packaged the way this "
                        "driver reads them", checked);
    return failures;
}

bool nv_gsp_init(nv_card_t *c) {
    name_files(c->chipset);

    int have = 0;
    for (int i = 0; i < file_count; i++) {
        firmware_declare(files[i].name, "linux-firmware, or NVIDIA's driver package");
        files[i].present = firmware_present(files[i].name, &files[i].size);
        if (files[i].present) have++;
    }

    /* Whether the co-processor is already up. The machine's own firmware
     * starts it to put a picture on the screen before the operating system
     * loads, and on many machines it is still running when we arrive. */
    u32 cpuctl = nv_rd32(c, NV_PGSP_FALCON_CPUCTL);
    bool halted = (cpuctl == 0xFFFFFFFFu) || (cpuctl & NV_PGSP_FALCON_CPUCTL_HALTED);

    u32 wpr_lo = nv_rd32(c, NV_PFB_PRI_MMU_WPR2_ADDR_LO);
    u32 wpr_hi = nv_rd32(c, NV_PFB_PRI_MMU_WPR2_ADDR_HI);
    bool wpr_set = wpr_lo && wpr_hi && wpr_lo != 0xFFFFFFFFu && wpr_hi >= wpr_lo;

    kinfo("nvidia", "%s is driven through its co-processor rather than by "
                    "registers, and that co-processor is %s",
          c->codename, nv_gsp_core_name(c->chipset));

    if (nv_gsp_core(c->chipset) == GSP_RISCV ||
        nv_gsp_core(c->chipset) == GSP_RISCV_FSP)
        kinfo("nvidia", "so the Falcon boot sequence does not apply to this "
                        "card: the old registers still answer at the same "
                        "addresses, which is exactly why running it would look "
                        "like it worked and start nothing");

    if (!halted || wpr_set) {
        kinfo("nvidia", "the co-processor is already running - the machine's own "
                        "firmware started it, which is what is putting the "
                        "picture on the screen");
    }

    /* The sequence is not run from here.
     *
     * Every value in it has been checked against NVIDIA's published source,
     * but it has never been run against a card, and the first thing it does is
     * write to one that is currently displaying a picture.  Starting it unasked
     * on somebody's working machine is not a reasonable way to find out whether
     * it works.
     *
     * It is reachable deliberately instead, through nv_gsp_start() - see the
     * note there.  What is recorded here is only whether it could be. */
    if (have == file_count && file_count > 0)
        kinfo("nv-gsp", "the files needed to start the co-processor are all "
                        "present; `gpu start` will attempt it");

    /* Look inside whichever files are here.
     *
     * Naming a file and finding it is not the same as it being the thing it
     * claims to be, and the difference only shows up when it is handed to the
     * card - at which point the failure is a co-processor that will not start
     * and no way to tell why.  Reading the headers costs nothing and turns
     * that into a sentence. */
    for (int i = 0; i < file_count; i++) {
        if (!files[i].present) continue;

        firmware_t fw;
        if (!firmware_load(files[i].name, &fw)) continue;

        const u8 *payload = NULL;
        u32 payload_size = 0;

        if (nv_fw_container_payload(fw.data, fw.size, &payload, &payload_size)) {
            kinfo("nvidia", "  %s: a container, %u bytes of payload inside "
                            "%llu", files[i].name, payload_size,
                  (unsigned long long)fw.size);
        } else if (nv_fw_is_elf(fw.data, fw.size)) {
            kinfo("nvidia", "  %s: an ELF image for the co-processor, %llu bytes",
                  files[i].name, (unsigned long long)fw.size);
        } else {
            kwarn("nvidia", "  %s: %llu bytes in a form this driver does not "
                            "recognise - it is not the file it should be",
                  files[i].name, (unsigned long long)fw.size);
        }
        firmware_free(&fw);
    }

    if (have == file_count) {
        /* However many this generation needs - two on Blackwell, four on
         * Ampere.  Comparing against three was left over from when every card
         * was assumed to want the same set, and meant a Blackwell card with
         * both of its files present fell through to the "some are missing"
         * path and listed nothing as missing. */
        kinfo("nvidia", "all %d firmware file(s) this card needs are present",
              file_count);
        kwarn("nvidia", "the sequence that hands them to the card is written but "
                        "has never been run against one, so it is not started; "
                        "the display keeps working through the framebuffer the "
                        "firmware set up");
        return false;
    }

    kinfo("nvidia", "%d of %d firmware file(s) present; the display works "
                    "through the framebuffer either way", have, file_count);
    for (int i = 0; i < file_count; i++)
        if (!files[i].present)
            kinfo("nvidia", "  missing: %s - %s", files[i].name, files[i].what);

    return false;
}

/* What the tools show. */
int nv_gsp_files(int index, const char **name, const char **what, bool *present) {
    if (index < 0 || index >= file_count) return 0;
    if (name) *name = files[index].name;
    if (what) *what = files[index].what;
    if (present) *present = files[index].present;
    return file_count;
}


/* --------------------------------------------------- starting it on purpose
 *
 * Everything above describes the card; this is the one thing that changes it.
 *
 * The co-processor is normally already running when we arrive - the machine's
 * own firmware started it, and that is what is putting a picture on the screen
 * before any operating system loads.  Restarting it means taking a working
 * card away from whatever is using it and handing it an image that has never
 * been tried on real hardware.  If the image is wrong, or an address is one
 * the card cannot reach, the co-processor stays halted and the display it was
 * driving does not come back until the machine is power-cycled.
 *
 * That is a real risk and it is why this is not on the boot path.  It runs
 * when somebody types `gpu start`, having read what it says it will do.
 *
 * What succeeding here does and does not mean, because the difference matters:
 * a started co-processor is the thing every later step needs - channels,
 * the copy engine, the display path - and none of them can be attempted
 * without it.  It is not by itself rendering.  It is the door being open.
 */
/* What the last attempt to start the co-processor did, kept out of the log.
 *
 * Everything below reports itself with kinfo/kerr, and that was enough right
 * up until it mattered.  The log is a 512-entry ring, `gpu start` is run early
 * in a session, and by the time the machine is shut down and the log is
 * collected the entire attempt has been overwritten by routine chatter.  The
 * user's report was exactly this: "why doesn't it report on gpu start".
 *
 * So the few facts worth having are held in variables instead.  Deriving the
 * verdict at the moment somebody asks for it, rather than hoping a line
 * survives in a ring, is the same fix the storage side needed. */
static const char *gsp_stage = "gpu start has not been run";

/* Hardware engine self-test verdicts (0 = pass, >0 = fail, -1 = not run) so the
 * GPU boot summary can report what actually ran on the card. */
static int g_hw_compute = -1, g_hw_runtime = -1, g_hw_nvdec = -1,
           g_hw_nvenc = -1, g_hw_3d = -1;
void nv_gsp_hw_verdicts(int *compute, int *runtime, int *nvdec,
                        int *nvenc, int *d3d) {
    if (compute) *compute = g_hw_compute;
    if (runtime) *runtime = g_hw_runtime;
    if (nvdec)   *nvdec   = g_hw_nvdec;
    if (nvenc)   *nvenc   = g_hw_nvenc;
    if (d3d)     *d3d     = g_hw_3d;
}
static int  gsp_attempts;
static bool gsp_succeeded;
static u32  gsp_ctl, gsp_boot, gsp_dma, gsp_mb0, gsp_mb1;
static u32  gsp_wpr_lo, gsp_wpr_hi;

const char *nv_gsp_last_stage(void) { return gsp_stage; }
int  nv_gsp_attempts(void) { return gsp_attempts; }
bool nv_gsp_succeeded(void) { return gsp_succeeded; }

/* The registers read straight after the boot attempt - the card's own record
 * of what happened, for the on-stick log.  Zero until nv_gsp_start has run. */
void nv_gsp_registers(u32 *ctl, u32 *boot, u32 *dma, u32 *mb0, u32 *mb1,
                      u32 *wpr_lo, u32 *wpr_hi) {
    if (ctl)    *ctl    = gsp_ctl;
    if (boot)   *boot   = gsp_boot;
    if (dma)    *dma    = gsp_dma;
    if (mb0)    *mb0    = gsp_mb0;
    if (mb1)    *mb1    = gsp_mb1;
    if (wpr_lo) *wpr_lo = gsp_wpr_lo;
    if (wpr_hi) *wpr_hi = gsp_wpr_hi;
}

void nv_gsp_report(void) {
    if (!gsp_attempts) {
        kinfo("verdict", "GPU: the co-processor was never asked to start "
                         "(run `gpu start`)");
        return;
    }
    kinfo("verdict", "GPU: %d attempt(s) to start the co-processor; %s",
          gsp_attempts, gsp_succeeded ? "IT STARTED" : "it did not start");
    kinfo("verdict", "GPU: got as far as: %s", gsp_stage);
    kinfo("verdict", "GPU: control %#x, boot control %#x, dma cfg %#x",
          gsp_ctl, gsp_boot, gsp_dma);
    kinfo("verdict", "GPU: mailboxes %#x %#x", gsp_mb0, gsp_mb1);
    kinfo("verdict", "GPU: protected window %#x..%#x%s", gsp_wpr_lo, gsp_wpr_hi,
          (gsp_wpr_lo && gsp_wpr_hi && gsp_wpr_hi >= gsp_wpr_lo)
              ? " - an image was accepted into it"
              : " - nothing was placed in it");
}

bool nv_gsp_start(nv_card_t *c) {
    if (!c) return false;

    gsp_attempts++;
    gsp_stage = "card found, looking for its firmware files";

    name_files(c->chipset);
    for (int i = 0; i < file_count; i++)
        files[i].present = firmware_present(files[i].name, &files[i].size);

    for (int i = 0; i < file_count; i++) {
        if (files[i].present) continue;
        gsp_stage = "a firmware file it needs is not on the volume";
        kerr("nv-gsp", "%s is not here, and it is %s - nothing can be started "
                       "without it", files[i].name, files[i].what);
        return false;
    }
    if (file_count < 2) {
        gsp_stage = "this card is not one this driver knows how to start";
        kerr("nv-gsp", "this card is not one this driver knows how to start");
        return false;
    }

    /* Which route this card takes is decided further down, once the firmware
     * and its proof have been read - because the security processor's route
     * needs the proof and the host's route does not. */
    if (nv_gsp_core(c->chipset) != GSP_RISCV &&
        nv_gsp_core(c->chipset) != GSP_RISCV_FSP) {
        gsp_stage = "the card does not boot the way this sequence assumes";
        kerr("nv-gsp", "%s does not boot the way this sequence assumes",
             c->codename);
        return false;
    }

    /* The management controller image, and the three things the boot ROM
     * checks it with.  They are sections of a small ELF rather than the file
     * itself, so each is found by name. */
    firmware_t fmc;
    if (!firmware_load(files[0].name, &fmc)) {
        gsp_stage = "the firmware file would not load";
        kerr("nv-gsp", "%s would not load", files[0].name);
        return false;
    }

    const u8 *code = NULL, *sig = NULL, *key = NULL, *hash = NULL;
    u32 code_len = 0, sig_len = 0, key_len = 0, hash_len = 0;

    bool found = nv_fw_elf_section(fmc.data, fmc.size, "image", &code, &code_len) &&
                 nv_fw_elf_section(fmc.data, fmc.size, "signature", &sig, &sig_len) &&
                 nv_fw_elf_section(fmc.data, fmc.size, "publickey", &key, &key_len) &&
                 nv_fw_elf_section(fmc.data, fmc.size, "hash", &hash, &hash_len);
    if (!found) {
        gsp_stage = "the firmware lacks the four sections the boot ROM needs";
        kerr("nv-gsp", "%s does not carry the four sections the boot ROM is "
                       "told where to find", files[0].name);
        firmware_free(&fmc);
        return false;
    }

    kinfo("nv-gsp", "image %u bytes, signature %u, key %u, hash %u",
          code_len, sig_len, key_len, hash_len);

    /* Memory the card reads through, which is not the same as memory this
     * kernel can address: the co-processor fetches these itself over the bus,
     * so they have to be somewhere it can reach and physically contiguous. */
    size_t code_pages = ((size_t)code_len + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t data_pages = 1;                      /* the manifest and arguments */

    u64 code_phys = 0, data_phys = 0, args_phys = 0, manifest_phys = 0;
    void *code_va = dma_alloc_pages(code_pages, &code_phys);
    void *data_va = dma_alloc_pages(data_pages, &data_phys);
    void *args_va = dma_alloc_pages(1, &args_phys);

    if (!code_va || !data_va || !args_va) {
        gsp_stage = "not enough memory the card can reach";
        kerr("nv-gsp", "not enough memory the card can reach");
        firmware_free(&fmc);
        return false;
    }

    memset(code_va, 0, code_pages * PAGE_SIZE);
    memcpy(code_va, code, code_len);

    /* The manifest the boot ROM checks the image against: the signature, the
     * key and the hash, laid out one after another where it expects them. */
    u8 *m = data_va;
    memset(m, 0, data_pages * PAGE_SIZE);
    size_t at = 0;
    if (at + sig_len <= data_pages * PAGE_SIZE) { memcpy(m + at, sig, sig_len); at += sig_len; }
    if (at + key_len <= data_pages * PAGE_SIZE) { memcpy(m + at, key, key_len); at += key_len; }
    if (at + hash_len <= data_pages * PAGE_SIZE) { memcpy(m + at, hash, hash_len); }
    manifest_phys = data_phys;

    memset(args_va, 0, PAGE_SIZE);

    kwarn("nvidia", "starting the co-processor on %s.  If the screen does not "
                    "come back, the card was left halted and the machine has "
                    "to be power-cycled - nothing is damaged.", c->codename);

    /* Two routes, and the card decides which.
     *
     * On Hopper and Blackwell the host may not start the co-processor at all -
     * the boot registers are locked against it - so the firmware and its three
     * proofs go to the card's security processor and that does the starting.
     * On Ampere and Ada the host writes the registers itself.
     *
     * Attempting the host's route on a card that does not allow it is not a
     * harmless mistake: the co-processor stays halted and the card that was
     * driving the only screen may not come back without a power cycle. */
    gsp_stage = "firmware handed over; waiting for the card to start it";
    bool ok;
    bool fsp_route = nv_gsp_core(c->chipset) == GSP_RISCV_FSP;
    /* The command and status rings, set up ONCE - before the card is started,
     * not after.  GSP-RM reads the command-queue header while it brings up its
     * own queues during boot, so it must already be in shared memory when the
     * FSP releases the processor.  Kept across the boot so the same rings carry
     * the first RPCs afterwards. */
    static nv_gsp_queue_t boot_cmdq, boot_statq;
    if (fsp_route) {
        /* Refuse to build a second protected window over one that is already up.
         *
         * On a cold boot the machine's own UEFI driver lights the screen through
         * a minimal co-processor of its own, and on Hopper/Blackwell it leaves
         * that window - WPR2 - standing when the operating system takes over.
         * The security processor validates the descriptor we hand it against
         * that existing window and, finding one already locked, refuses with
         * mailbox0 = 0xb.  That is the exact stall seen on this silicon, and it
         * is the same situation NVIDIA's own driver guards against by failing
         * early ("unexpected WPR2 already up, cannot proceed with booting GSP";
         * kernel_gsp.c _kgspBootGspRm).  Pressing on cannot succeed - the window
         * has to be brought down first, which on a live card means a reset, and
         * a reset here is what blanked the screen before, so it is not done
         * unasked.  `gspforce` overrides this for a machine where the window is
         * genuinely stale and the risk is understood. */
        u32 wlo = nv_rd32(c, NV_PFB_PRI_MMU_WPR2_ADDR_LO);
        u32 whi = nv_rd32(c, NV_PFB_PRI_MMU_WPR2_ADDR_HI);
        bool wpr2_up = wlo && whi && wlo != 0xFFFFFFFFu && whi >= wlo;

        /* If WPR2 is already up - the machine's own firmware left a co-processor
         * running - a fresh GSP-RM boot is rejected with mailbox0 = 0xb.  The
         * only thing that brings the window down without a running GSP-RM to
         * unload is a secondary-bus reset that re-runs the card's boot sequence
         * (see nv_gpu_reset_wpr2); it blanks the screen until GSP-RM re-lights
         * it.  Armed with `gspreset` so this first, destructive attempt is a
         * deliberate act, not something an otherwise-working boot stumbles into. */
        if (wpr2_up && cmdline_has("gspreset")) {
            kwarn("nvidia", "WPR2 already up at %#x..%#x - clearing it with a "
                            "secondary-bus reset before booting GSP-RM (screen "
                            "will blank)", wlo, whi);
            if (nv_gpu_reset_wpr2(c)) {
                wlo = nv_rd32(c, NV_PFB_PRI_MMU_WPR2_ADDR_LO);
                whi = nv_rd32(c, NV_PFB_PRI_MMU_WPR2_ADDR_HI);
                wpr2_up = wlo && whi && wlo != 0xFFFFFFFFu && whi >= wlo;
                kinfo("nvidia", "after the reset WPR2 reads %#x..%#x - %s",
                      wlo, whi, wpr2_up ? "STILL UP (reset did not clear it)"
                                        : "DOWN (clear - GSP-RM can now boot)");
                /* Make this durable now: the screen is dark and the GSP-RM boot
                 * that follows is the step most likely to hang, so the record
                 * that the reset itself worked must survive it. */
                klog_persist_flush();
            } else {
                kerr("nvidia", "the secondary-bus reset did not bring the card "
                               "back; cannot boot GSP-RM");
                gsp_stage = "the card did not return after the reset";
                gsp_succeeded = false;
                firmware_free(&fmc);
                return false;
            }
        }

        if (wpr2_up && !cmdline_has("gspforce")) {
            gsp_stage = "the firmware's protected window (WPR2) is already up - "
                        "a fresh GSP-RM boot is rejected 0xb; boot with gspreset "
                        "to clear it with a secondary-bus reset first";
            gsp_wpr_lo = wlo; gsp_wpr_hi = whi; gsp_succeeded = false;
            kwarn("nvidia", "WPR2 already up at %#x..%#x - NOT building a second "
                            "window over it. This is the cause of the mailbox0 "
                            "0xb stall: the UEFI driver left a co-processor "
                            "running and its window locked. Boot with gspreset "
                            "to clear it first (a secondary-bus reset; the screen "
                            "blanks until GSP-RM re-lights it), or gspforce to "
                            "attempt the boot anyway (0xb is likely).", wlo, whi);
            firmware_free(&fmc);
            return false;
        }
        if (wpr2_up)
            kwarn("nvidia", "WPR2 still up at %#x..%#x but gspforce is set - "
                            "attempting the boot regardless (0xb is likely)",
                  wlo, whi);

        /* Assemble the boot arguments the FSP route needs: the GSP_FMC_BOOT_
         * PARAMS and everything it points at - the WPR meta, the radix3 over
         * the 60 MB GSP-RM image, the libos regions and the shared rings.
         * `args_phys` above is the FMC's own tiny argument page; the FSP
         * instead wants the boot-params structure, which is what the security
         * processor follows to find the resource manager firmware. */
        gsp_stage = "staging the GSP-RM boot arguments";
        u64 boot_params_phys = 0, rsvd = 0;
        if (!nv_gsp_boot_stage(c, &boot_params_phys, &rsvd)) {
            gsp_stage = "the GSP-RM image or boot staging is not ready";
            kerr("nvidia", "the co-processor cannot be started without the "
                           "GSP-RM boot arguments - see the lines above");
            firmware_free(&fmc);
            return false;
        }
        /* Write the command-queue header into the shared region BEFORE the
         * card is started.  GSP-RM reads this header while it initialises its
         * own queues during boot (nouveau does exactly this in
         * r535_gsp_shared_init, which runs before gsp->func->reset), so a
         * header written only afterwards - as this code used to do - leaves the
         * co-processor with a zeroed, wrong view of the ring, and the first RPC
         * after boot is deposited into a ring it is not reading correctly, so
         * the reply never comes.  The status ring's header the firmware writes
         * itself; we only remember where it is.  Verified against nouveau r535
         * and NVIDIA open-gpu-kernel-modules. */
        {
            u8 *cr = nv_gsp_boot_cmdq(), *mr = nv_gsp_boot_msgq();
            if (cr && mr) {
                nv_gsp_cmdq_init(&boot_cmdq, cr, mr);
                nv_gsp_msgq_adopt(&boot_statq, mr, cr);
                kinfo("nv-boot", "command ring header written before start "
                                 "(flags=1, %d entries) so the co-processor "
                                 "sees it during its own bring-up",
                      (int)boot_cmdq.count);
                /* Pre-queue GSP_SET_SYSTEM_INFO (72) then SET_REGISTRY (73) into
                 * the command ring BEFORE the co-processor starts, NOSEQ and
                 * without a doorbell (the RM is not up to ring for).  GSP-RM
                 * consumes these two during its own bring-up and needs them to
                 * finish init and send INIT_DONE (0x1001).
                 *
                 * An earlier version left the ring EMPTY, on the belief that
                 * nouveau "sends system_info/registry after the processor is up"
                 * and that pre-queuing had faulted the RM into a NOCAT loop.
                 * Both premises were wrong, checked against source this session:
                 *   - nouveau r535/r570 write BOTH RPCs in oneinit (r535_gsp.c
                 *     ~2193/2197), which runs BEFORE func->reset boots the RM -
                 *     i.e. it pre-queues, exactly like NVIDIA OGKM's
                 *     kgspQueueAsyncInitRpcs (before _kgspBootGspRm).
                 *   - the GSP_POST_NOCAT_RECORD (0x1020) events are NORMAL boot
                 *     telemetry - nouveau installs drop_post_nocat_record to
                 *     drain them - not a crash.  Draining them while waiting for
                 *     0x1001 is correct; the missing INIT_DONE came from the
                 *     empty ring, not from the NOCAT posts.
                 * The two payloads are byte-verified vs nouveau r570 this
                 * session (GspSystemInfo 928 B, every offset; packed registry
                 * 16-B entries).  `gspnoqueue` restores the old empty-ring boot
                 * for an A/B comparison. */
                if (cmdline_has("gspnoqueue")) {
                    gsp_stage = "command ring left empty (gspnoqueue)";
                    kwarn("nv-gsp", "gspnoqueue: NOT pre-queuing the init RPCs "
                                    "- INIT_DONE is not expected to arrive");
                } else if (nv_gsp_queue_async_init_rpcs(c, &boot_cmdq)) {
                    gsp_stage = "init RPCs pre-queued; waiting for the "
                                "co-processor to consume them";
                } else {
                    gsp_stage = "the init RPCs could not be pre-queued";
                    kerr("nvidia", "could not stage the GSP-RM early-init RPCs "
                                   "into the boot command ring");
                    firmware_free(&fmc);
                    return false;
                }
            } else {
                gsp_stage = "the GSP shared command rings were not staged";
                kerr("nvidia", "the boot arguments contain no command/status "
                               "rings for GSP-RM");
                firmware_free(&fmc);
                return false;
            }
        }

        gsp_stage = "firmware and boot arguments handed over; waiting for the "
                    "card to start it";
        /* All boot structures, radix leaves, image bytes, signature bytes and
         * preboot RPCs live in WB system memory.  Order those stores before
         * the MMIO doorbell that lets FSP/FMC fetch them.  x86 cache coherence
         * supplies visibility; mfence supplies the explicit store ordering at
         * this CPU/device ownership boundary. */
        __asm__ volatile("mfence" ::: "memory");
        ok = nv_fsp_boot_gsp(c, code_phys, boot_params_phys, rsvd,
                             hash, hash_len, key, key_len, sig, sig_len);
    } else {
        ok = start_from_fmc(c, args_phys, code_phys, data_phys, manifest_phys);
    }

    /* Everything the card will say about what just happened, whichever way it
     * went.
     *
     * This runs once, on somebody's own machine, and it is the only place this
     * sequence has ever met real silicon.  An attempt that fails and leaves
     * behind one sentence is an attempt that has to be made again to learn
     * anything; the registers below cost nothing to read and are the
     * difference between "it did not start" and knowing which step it stopped
     * at.  A card that will not take the image reports it differently from one
     * that could not reach an address, and both differ from one that never
     * left reset.
     */
    {
        u32 gsp = NV_FALCON_GSP;
        u32 ctl  = nv_rd32(c, gsp + NV_PRISCV_CPUCTL);
        u32 mb0  = nv_rd32(c, gsp + NV_PFALCON_MAILBOX0);
        u32 mb1  = nv_rd32(c, gsp + NV_PFALCON_MAILBOX1);
        u32 dma  = nv_rd32(c, gsp + NV_PRISCV_BCR_DMACFG);
        u32 boot = nv_rd32(c, NV_PGSP_FALCON_CPUCTL);

        gsp_ctl = ctl; gsp_boot = boot; gsp_dma = dma;
        gsp_mb0 = mb0; gsp_mb1 = mb1;

        kinfo("nvidia", "after the attempt: control %#x, boot control %#x, "
                        "dma configuration %#x", ctl, boot, dma);
        kinfo("nvidia", "  mailboxes %#x %#x", mb0, mb1);
        kinfo("nvidia", "  it was given code at %p, data at %p, manifest at "
                        "%p, arguments at %p",
              (void *)code_phys, (void *)data_phys, (void *)manifest_phys,
              (void *)args_phys);

        /* And whether the protected window moved, which is the card's own
         * record of having accepted an image. */
        u32 wpr_lo = nv_rd32(c, NV_PFB_PRI_MMU_WPR2_ADDR_LO);
        u32 wpr_hi = nv_rd32(c, NV_PFB_PRI_MMU_WPR2_ADDR_HI);
        gsp_wpr_lo = wpr_lo; gsp_wpr_hi = wpr_hi;
        kinfo("nvidia", "  the protected window reads %#x..%#x%s",
              wpr_lo, wpr_hi,
              (wpr_lo && wpr_hi && wpr_hi >= wpr_lo)
                  ? " - something was accepted into it"
                  : " - nothing was placed in it");
    }

    gsp_succeeded = ok;
    gsp_stage = ok ? "the co-processor started"
                   : (fsp_route ? nv_fsp_last_stage()
                                : "the card would not accept the image or could not "
                                  "reach an address it was given");

    if (ok) {
        u32 mb0 = nv_rd32(c, NV_FALCON_GSP + NV_PFALCON_MAILBOX0);
        kinfo("nvidia", "the security processor took the image; it reports %#x", mb0);

        /* The FSP has taken the message; now the co-processor has to come out
         * of lockdown and the resource manager inside it has to say it has
         * finished starting.  This is the r535_gsp_init step, and it is the
         * frontier: everything up to here is checked against a model, and from
         * here on it is the real firmware or nothing.  Each wait records where
         * it stopped so a log collected afterwards says how far the card got.
         *
         * Only attempted on real silicon - the model's FSP does not run a
         * resource manager to answer. */
        if (!c->modelled && nv_gsp_boot_cmdq()) {
            /* The co-processor's LOCKDOWN / target-mask bit lives in the GSP
             * register block, which the ACR keeps PLM-masked (every read is the
             * 0xBADF41xx PRI-error pattern) until deep into bring-up - so gating
             * on it is unreliable, and the previous code waited on it forever and
             * never even looked at the ring.  The RELIABLE signal that GSP-RM is
             * up is the RPC it writes to the status ring IN SYSTEM MEMORY, which
             * we can always read regardless of the register mask.  So: log the
             * register state for diagnosis, but WAIT ON THE RING, not the bit.
             * (Exactly the lesson the FSP taught: watch the queue the firmware
             * writes, not a walled register.) */
            gsp_stage = "waiting for the resource manager to start (status ring)";
            u32 hw0 = nv_rd32(c, NV_FALCON_GSP + NV_PFALCON_FALCON_HWCFG2);
            kinfo("nvidia", "after FSP success: GSP HWCFG2 %#x, mailbox0 %#x, "
                            "mailbox1 %#x (HWCFG2 0xBADF41xx = block still masked)",
                  hw0, nv_rd32(c, NV_FALCON_GSP + NV_PFALCON_MAILBOX0),
                  nv_rd32(c, NV_FALCON_GSP + NV_PFALCON_MAILBOX1));

            /* Poll the sysmem status ring for the firmware's own
             * initialisation-done event (0x1001).  A correctly-set-up GSP-RM
             * answers in a second or two (Windows boots this fast); 8s is a
             * generous margin, and a failure at 8s means the setup is wrong, not
             * that we needed to wait longer.  Rings set up before boot; no re-init. */
            if (nv_gsp_rpc_poll(c, &boot_statq, 0x1001, 8000)) {
                gsp_stage = "the resource manager is running";
                kinfo("nvidia", "the resource manager reported it has started "
                                "(INIT_DONE) - the card can now be given work");

                /* GSP-RM is up and answering on the shared rings.  Build the
                 * resource-manager object tree over those SAME live rings (the
                 * ones INIT_DONE arrived on): client -> device -> subdevice ->
                 * display.  This is the gate for everything the card can be
                 * asked to do - a display to set a mode on, channels to submit
                 * work through.  The object-tree code was proven only against a
                 * model until now; this is its first run on the real firmware. */
                static nv_rm_t rm;
                memset(&rm, 0, sizeof rm);
                rm.command = boot_cmdq;   /* the live rings, post-INIT_DONE */
                rm.status  = boot_statq;
                rm.ready   = true;
                /* Baseline: where the four ring pointers sit the instant after
                 * INIT_DONE was consumed, before the first alloc rings the
                 * doorbell.  Compared against the "no answer" dump 2s later,
                 * this says whether the processor consumed the alloc at all. */
                nv_gsp_ring_state("after INIT_DONE,", &rm.command, &rm.status);
                if (nv_rm_bring_up(c, &rm)) {
                    /* Make the live GSP transport available to NVIDIA's full
                     * NVKMS core.  NVKMS activation remains gated inside the
                     * port until every RM serializer is implemented. */
                    nvkms_host_attach(c, &rm);
                    gsp_stage = "the resource-manager object tree is up";
                    kinfo("nvidia", "RM object tree built over the live rings: "
                                    "%d object(s) allocated (client, device, "
                                    "subdevice, display) - the card is now "
                                    "controllable, not just booted",
                          rm.allocations);
                    /* The first real control RPC: ask the display what it can
                     * do.  This round-trips a GSP_RM_CONTROL and is the step a
                     * modeset is built on. */
                    static nv_rm_display_t disp;
                    if (nv_rm_query_display(c, &rm, &disp)) {
                        gsp_stage = "the display answered over RM";
                        kinfo("nvidia", "display over RM: mask %#x, %u head(s), "
                                        "%d connector(s), DP up to %u lane(s) at "
                                        "rate %#x",
                              disp.display_mask, disp.heads, disp.connectors,
                              disp.dp_max_lanes, disp.dp_max_rate);
                    } else {
                        kwarn("nvidia", "object tree is up but the display "
                                        "query was refused - the RM control "
                                        "path needs a look");
                    }

                    /* With the object tree up, open a real GPFIFO channel +
                     * copy engine - the gate for hardware-accelerated drawing.
                     * Each allocation logs, so the boot record says exactly how
                     * far the channel bring-up gets on the real card. */
                    gsp_stage = "opening a GPFIFO channel";
                    if (nv_chan_open(c, &rm) == 0) {
                        gsp_stage = "a GPFIFO channel is open";
                        kinfo("nvidia", "a real channel + copy engine is open on "
                                        "the card - hardware-accelerated work can "
                                        "now be submitted");
                        /* Prove it: run a real copy on the card. */
                        if (nv_chan_selftest() == 0)
                            gsp_stage = "the copy engine ran on the card";

                        /* The copy-engine result is the KEYSTONE of this boot -
                         * everything downstream (3D/compute) depends on the
                         * submission model it proves.  Flush it to the stick NOW,
                         * before the 3D/compute bring-up below: that path lacks a
                         * golden GR context and can hang the GR engine, and this
                         * board wipes RAM on reset, so an unflushed verdict would
                         * be lost on a power-cycle.  One flush guarantees the one
                         * answer this boot exists to get survives no matter what
                         * the GR bring-up does. */
                        void klog_persist_flush(void);
                        klog_persist_flush();

                        /* Now bring up the rest of the render/codec pipeline:
                         * 3D, compute, video decode and encode.  Each is its own
                         * channel and logs whether it opened or which alloc the
                         * card refused - so this one boot maps the whole GPU
                         * pipeline (2D/3D/compute/encode/decode) end to end. */
                        gsp_stage = "bringing up 3D / compute / video engines";
                        int engines = nv_chan_open_engines(c, &rm);
                        if (engines > 0)
                            gsp_stage = "the render/codec engines are up";

                        /* With the GR golden context up and the compute channel
                         * open, dispatch a real sm_120 shader on the card.  The
                         * NOP kernel's completion semaphore firing proves the
                         * compute engine executes - the first GR-engine work to
                         * run on this hardware.  Self-guards on the compute
                         * channel being open; flushed so a failure cannot lose
                         * the log and cannot regress the proven copy engine. */
                        g_hw_compute = nv_compute_selftest_hw();
                        if (g_hw_compute == 0)
                            gsp_stage = "a compute shader ran on the card";
                        klog_persist_flush();

                        /* 3D graphics via the COMPUTE RASTERIZER (now that compute
                         * runs): draw+verify a filled triangle in a VRAM surface
                         * with a real sm_120 shader - no 0xce97 graphics pipeline,
                         * no captured VS/FS microcode needed.  The old method-stream
                         * check (nv_3d_selftest_hw) still runs for completeness. */
                        int nv_3d_raster_selftest_hw(void);
                        (void)nv_3d_selftest_hw();
                        g_hw_3d = nv_3d_raster_selftest_hw();
                        /* Do not synchronously append KERNEL.LOG here.  The
                         * 06:22 hardware boot returned from the triangle and
                         * then stopped inside this FAT append, before display
                         * re-light could even log its entry marker.  The ring
                         * already retains these records; the main idle/log
                         * worker persists them after the time-critical GPU
                         * transaction has returned. */

                        /* DisplayPort re-light.  ORDER (hardware-proven): it runs
                         * AFTER compute+3D but BEFORE NVDEC.  Running it FIRST
                         * regressed compute/3D to FAIL (the disp bring-up disturbs
                         * GR/channel state before those self-tests); running it
                         * LAST put it on a GSP already wedged by NVDEC's exceptType
                         * -109 RC fault (cmdq stops being consumed).  Between the
                         * two: compute+3D already have their verdicts on a clean
                         * GSP, and the re-light still sees a live GSP because NVDEC
                         * (the wedger) has not run yet.  nouveau brings display up
                         * as a standalone subdev independent of these engines
                         * (r535/disp.c:1456,1503), so ordering is free to choose. */
                        if (!cmdline_has("nodisplay")) {
                            nv_disp_lightup(c, &rm);
                            gsp_stage = "display re-light attempted";
                        }

                        /* Prove the production GUI path, not merely a private
                         * engine buffer: CE fill/copy/image presentation and an
                         * SM raster triangle must read back from live scanout. */
                        g_hw_runtime = nv_chan_runtime_selftest_hw();
                        if (g_hw_runtime == 0)
                            gsp_stage = "the GUI render path wrote live scanout on the card";

                        /* Both Falcon streams now use NV906F SET_OBJECT with
                         * ENGINE_SW, matching Mesa's video-queue binding.  Run
                         * the independently documented NVDEC stream first; an
                         * encoder ABI failure must not prevent the decoder from
                         * receiving its own hardware verdict. */
                        g_hw_nvdec = nv_nvdec_selftest_hw();
                        if (g_hw_nvdec == 0)
                            gsp_stage = "an H.264 frame was decoded on the card";

                        /* Encode a real H.264 IDR with the NVIDIA-published CFB7
                         * picture ABI, then require clean status and Annex-B
                         * output.  It is last because this is the less publicly
                         * exercised of the two Falcon method streams. */
                        g_hw_nvenc = nv_nvenc_selftest_hw();
                        if (g_hw_nvenc == 0)
                            gsp_stage = "bounded NVENC IDR output was captured on the card";
                        if (g_hw_nvdec == 0 && g_hw_nvenc == 0) {
                            int roundtrip = nv_nvenc_roundtrip_hw();
                            kinfo("nvidia", "NVENC-to-NVDEC pixel round trip: %s",
                                  roundtrip == 0 ? "PASS" : "unproven");
                            if (roundtrip == 0)
                                gsp_stage = "NVENC output decoded into matching pixels on the card";
                        }

                        /* Codec experiments can RC/fault their own channels and
                         * wedge shared firmware state.  A pre-codec GUI PASS is
                         * therefore insufficient evidence that the desktop can
                         * still render.  Re-run the exact live fill/copy/present
                         * plus SM-triangle test after both codecs; only this final
                         * result is exposed to the boot-time desktop gate. */
                        if (g_hw_runtime == 0) {
                            int post_codec_runtime = nv_chan_runtime_selftest_hw();
                            if (post_codec_runtime != 0) {
                                g_hw_runtime = post_codec_runtime;
                                kwarn("nvidia", "post-codec GUI revalidation FAILED - desktop must not start");
                            } else {
                                kinfo("nvidia", "post-codec GUI revalidation PASS - live GPU desktop path remains responsive");
                                gsp_stage = "the post-codec GUI render path remains live";
                            }
                        }
                    } else {
                        kwarn("nvidia", "the object tree is up but a GPFIFO "
                                        "channel could not be opened - see the "
                                        "nv-chan lines for the failing step");
                    }
                } else {
                    kwarn("nvidia", "GSP-RM is running but the object tree "
                                    "could not be built over the live rings "
                                    "(%d object(s) allocated before the "
                                    "refusal) - the RM alloc path needs a look",
                          rm.allocations);
                }
            } else {
                gsp_stage = "the resource manager did not report in";
                u32 hw1 = nv_rd32(c, NV_FALCON_GSP + NV_PFALCON_FALCON_HWCFG2);
                kwarn("nvidia", "GSP-RM did not send its initialisation-done "
                                "message in time; HWCFG2 %#x -> %#x, mailbox0 %#x, "
                                "mailbox1 %#x", hw0, hw1,
                      nv_rd32(c, NV_FALCON_GSP + NV_PFALCON_MAILBOX0),
                      nv_rd32(c, NV_FALCON_GSP + NV_PFALCON_MAILBOX1));
                /* Dump the status ring's header words (they hold the write/read
                 * pointers) so the log says whether GSP-RM wrote ANYTHING - an
                 * all-zero header means the resource manager never came up, a
                 * non-zero write pointer means it did and the reply parse is
                 * what to chase next. */
                if (boot_statq.memory) {
                    volatile u32 *h = (volatile u32 *)(void *)boot_statq.memory;
                    kwarn("nvidia", "  status ring header: %#x %#x %#x %#x %#x %#x",
                          h[0], h[1], h[2], h[3], h[4], h[5]);
                }
                if (boot_statq.peer) {
                    volatile u32 *h = (volatile u32 *)(void *)boot_statq.peer;
                    kwarn("nvidia", "  status ring peer:   %#x %#x %#x %#x %#x %#x",
                          h[0], h[1], h[2], h[3], h[4], h[5]);
                }
            }
        }

        kinfo("nvidia", "this is the step every later one needs.  What it does "
                        "not do on its own is draw: channels and the copy "
                        "engine come next, and they were waiting on this.");
    } else {
        u32 ctl = nv_rd32(c, NV_FALCON_GSP + NV_PRISCV_CPUCTL);
        kerr("nvidia", "the co-processor did not start (control register %#x). "
                       "It either would not accept the image or could not "
                       "reach an address it was given.", ctl);
        kerr("nvidia", "the lines above are what it looked like afterwards, and "
                       "are what this needs to get further - nothing is "
                       "damaged, and a power cycle puts the card back");
    }

    firmware_free(&fmc);
    return ok;
}
