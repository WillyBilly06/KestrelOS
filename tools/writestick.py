"""writestick.py - put KestrelOS on a USB stick that boots with Secure Boot on.

    python tools/writestick.py G: enroll    # the first boot: enrol the key
    python tools/writestick.py G: boot      # every boot after that

Secure Boot will not start an image the firmware cannot trace back to a key it
holds, and the only keys a consumer motherboard ships with are Microsoft's.
There are three ways round that and only one of them is available to somebody
building their own system:

  - Have Microsoft sign the loader.  That is a submission process with a real
    review behind it, not something that happens in an afternoon.
  - Turn Secure Boot off.  One firmware setting, and it works immediately - but
    it is a firmware setting, and the point here was to change none.
  - Use shim.  Shim is a small loader Microsoft HAS signed, so the firmware
    starts it without complaint; shim then checks the next image against a list
    of keys the machine's owner has enrolled.  Enrolling one is done at boot,
    in shim's own menu, and touches no firmware setting at all.

So the stick is laid out for shim:

    \\EFI\\BOOT\\BOOTX64.EFI     shim, signed by Microsoft's UEFI CA
    \\EFI\\BOOT\\grubx64.efi     the KestrelOS loader, signed with our own key
    \\EFI\\BOOT\\mmx64.efi       the enrollment tool shim runs the first time
    \\KESTREL.CER               our certificate, for enrolling
    \\KESTREL\\...               the kernel, the initrd and the configuration

grubx64.efi is not GRUB.  It is the name shim was built to look for, and
renaming shim's expectation would mean rebuilding shim, which would mean it was
no longer the binary Microsoft signed.

Which is also why this has two modes.  Shim does not offer to enrol a key when
verification fails - it says "Verification failed: (0x1A) Security Violation"
and stops, because a loader that offered to trust whatever just failed would
not be a security measure.  Shim opens the enrollment tool only when something
has asked it to, and on Linux the thing that asks is mokutil, writing a request
into a firmware variable from a running system.  There is no running system
here to ask from.

So the stick asks in the one way that needs nothing but the stick: in "enroll"
mode, the file shim goes looking for IS the enrollment tool.  Shim verifies it
against the certificate built into shim itself - Canonical signed both - starts
it, and the tool offers to enrol a key from disk.  After that the stick is
rewritten in "boot" mode, where that name holds the KestrelOS loader again, and
the key enrolled in the first step is what lets it through.

Two boots, no firmware setting touched, and nothing trusted that the machine's
owner did not sit down and approve.
"""

import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SB = os.path.join(ROOT, "secureboot")


def _sha256(path):
    import hashlib
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()
PAYLOAD = os.path.join(ROOT, "out", "windows", "payload")

ENROLL_STEPS = """FIRST BOOT - telling the machine to trust KestrelOS
====================================================

This stick is in enrollment mode.  Booting it goes straight into a plain
text tool called MokManager, on a blue screen.  That is meant to happen.

Choose  Enroll hash from disk  - NOT "Enroll key from disk".

  1. Choose  Enroll hash from disk
  2. Pick the disk it offers - it is this stick
  3. Go into  EFI  then  BOOT
  4. Choose  kestrel.efi
  5. Choose  Continue
  6. Choose  Yes  to confirm
  7. Choose  Reboot

Why the hash and not the key.  Enrolling a key says "trust anything signed
by this"; enrolling a hash says "trust exactly this one file".  The key way
is tidier and is what a distribution does, but it depends on the enrolled
certificate and the signature on the loader agreeing about a dozen details,
and when they do not the only thing the machine says is "Security
Violation" - which is the same thing it says when nothing was enrolled at
all.  A hash has nothing to disagree about.  It either matches the file or
it does not.

The cost is that the hash covers that exact file, so it has to be enrolled
again if the loader itself is ever rebuilt.  The loader is the small part
that rarely changes; the kernel and everything above it are loaded BY it
and are not covered, so ordinary work on the system does not need this
doing again.

If you saw "Verification failed: (0x1A) Security Violation" before, that
was shim refusing to start KestrelOS because this step had not taken.  It
is the system working, not failing.


"""

INSTRUCTIONS = """KestrelOS on this stick
=======================

Boot the machine from this stick.  On most motherboards that is F12, F11 or
F8 at the vendor logo, then picking the USB device from the list.

Nothing in the firmware setup is changed by any of this.  Secure Boot stays
on.  What has changed is one list, kept by shim, of who the machine's owner
sat down and approved - and it can be cleared again from the same tool.


If the screen stays black
-------------------------

Two things on this stick record how far it got, and both survive a screen
that shows nothing:

  KERNEL.LOG in the KESTREL folder.  The kernel keeps its log in memory and
  the loader writes it to this file the NEXT time the stick is booted, so
  the log always describes the boot BEFORE the current one.  To collect a
  log: boot the stick, let it do whatever it does, then RESTART the machine
  (restart, not power off - memory keeps its contents across a restart and
  loses them when the power goes) and boot the stick a second time.  The
  first boot's log is then in KESTREL/KERNEL.LOG, and can be read on any
  computer.  The file is appended to, so it keeps every boot collected this
  way and one can be compared against another.

  BOOT.LOG in the root of the stick.  Every step the loader reached is
  appended to it before the step runs, so the last line names the step that
  did not come back.  Read it on any computer afterwards.

  The two-digit display on the motherboard, if it has one - usually beside
  the memory slots.  E0 to EF is the loader, F0 to FE is the kernel, FF is a
  kernel panic.  In order:

    E0  the loader is running          F0  the kernel is running
    E1  it found the screen            F1  it has drawn to the screen
    E2  it opened this stick           F2  descriptor tables installed
    E3  it read the configuration      F3  memory managers up
    E4  it is showing the menu         F4  on its own stack
    E5  the kernel is in memory        F5  interrupts and timer running
    E6  the initrd is in memory        F6  root filesystem mounted
    E7  the video mode is chosen       F7  the bus has been walked
    E8  the page tables are built      F8  graphics drivers starting
    E9  shutting the firmware down     F9  starting the first program
    EA  entering the kernel            FF  it panicked
    EF  the loader gave up

  A number outside those ranges means the firmware never started the loader
  at all - which with Secure Boot on usually means the key above was not
  enrolled.

If it stops at F8, the graphics driver is what upset the machine.  Boot again
and choose the last entry on the menu, "KestrelOS, leaving the graphics card
alone" - the display then stays exactly as the firmware set it up.
"""


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    print("  %-34s %8d bytes" % (dst, os.path.getsize(dst)))


def main():
    if len(sys.argv) not in (2, 3):
        print(__doc__)
        return 2
    drive = sys.argv[1].rstrip("\\/")
    if not drive.endswith(":"):
        drive += ":"
    mode = sys.argv[2] if len(sys.argv) == 3 else "enroll"
    if mode not in ("enroll", "boot"):
        print("mode is 'enroll' or 'boot', not %r" % mode)
        return 2

    if not os.path.isdir(drive + "\\"):
        print("%s is not there." % drive)
        return 2

    # Refusing to write to anything that is not removable is the one check
    # worth having here: the cost of getting it wrong is somebody's disk.
    import ctypes
    kind = ctypes.windll.kernel32.GetDriveTypeW(ctypes.c_wchar_p(drive + "\\"))
    if kind != 2:                       # DRIVE_REMOVABLE
        print("%s is not a removable drive (type %d).  Refusing." % (drive, kind))
        return 2

    # Stage the loader that was just built.
    #
    # The copy under secureboot/ is what actually reaches the stick, and until
    # now it was put there by hand.  That is a quiet way to ship the wrong
    # thing: the build succeeds, the stick is written, everything reports
    # success, and the machine boots the loader from whenever somebody last
    # remembered to copy it.  Every loader change since then is simply absent,
    # and nothing anywhere says so.
    #
    # Taking it straight from the build removes the step that can be forgotten.
    built = os.path.join(ROOT, "build", "boot", "BOOTX64.EFI")
    if os.path.exists(built):
        staged = os.path.join(SB, "grubx64.efi")
        before = _sha256(staged) if os.path.exists(staged) else None
        shutil.copy2(built, staged)
        after = _sha256(staged)
        if before != after:
            print("staged the loader that was just built (%s)" % after[:16])
            print("NOTE: the loader changed, so its hash is no longer the one")
            print("      enrolled in the firmware.  Shim will refuse it and")
            print("      show MOK Manager again - enrol the new hash once.")

    need = [
        os.path.join(SB, "shimx64.signed.efi"),
        os.path.join(SB, "grubx64.efi"),
        os.path.join(SB, "mmx64.efi"),
        os.path.join(SB, "KESTREL.CER"),
        os.path.join(PAYLOAD, "KESTREL", "KERNEL.ELF"),
        os.path.join(PAYLOAD, "KESTREL", "INITRD.KAR"),
    ]
    missing = [n for n in need if not os.path.exists(n)]
    if missing:
        print("missing:")
        for n in missing:
            print("   " + n)
        return 2

    print("writing to %s, in %s mode" % (drive, mode))

    # Shim is what the firmware starts, so it takes the name the firmware
    # looks for.
    copy(os.path.join(SB, "shimx64.signed.efi"), drive + "\\EFI\\BOOT\\BOOTX64.EFI")

    # And whatever shim should start takes the name shim looks for.  On the
    # first boot that is the enrollment tool, because there is no other way to
    # reach it from a stick alone; afterwards it is the KestrelOS loader.
    second = "mmx64.efi" if mode == "enroll" else "grubx64.efi"
    copy(os.path.join(SB, second), drive + "\\EFI\\BOOT\\grubx64.efi")

    # Both are kept under their own names as well, so switching modes later
    # needs nothing rebuilt and nothing downloaded.
    copy(os.path.join(SB, "mmx64.efi"), drive + "\\EFI\\BOOT\\mmx64.efi")
    copy(os.path.join(SB, "grubx64.efi"), drive + "\\EFI\\BOOT\\kestrel.efi")
    copy(os.path.join(SB, "KESTREL.CER"), drive + "\\KESTREL.CER")

    copy(os.path.join(PAYLOAD, "KESTREL", "KERNEL.ELF"), drive + "\\KESTREL\\KERNEL.ELF")
    copy(os.path.join(PAYLOAD, "KESTREL", "INITRD.KAR"), drive + "\\KESTREL\\INITRD.KAR")

    # A longer pause than the disk image uses.  This is the boot where
    # somebody is watching to see whether anything appears at all, and three
    # seconds is not long enough to read a screen and decide.
    cfg = drive + "\\KESTREL\\BOOT.CFG"
    os.makedirs(os.path.dirname(cfg), exist_ok=True)
    with open(cfg, "w", newline="\r\n") as f:
        f.write("# KestrelOS loader configuration\n")
        f.write("timeout=10\n")
        # Whatever mode the firmware is already in, unchanged.  That mode is
        # provably being displayed - the vendor logo appeared in it - and any
        # other is one that has never been tried on this machine.  With more
        # than one monitor on the card it matters more still: the output whose
        # mode gets changed is not necessarily the one being looked at.
        #
        # Replace with a size (video=2560x1440) or delete the line to let the
        # loader choose the best mode it can find.
        f.write("video=keep\n")
        f.write("kernel=\\KESTREL\\KERNEL.ELF\n")
        f.write("initrd=\\KESTREL\\INITRD.KAR\n")
        f.write("cmdline=\n")
    print("  %-34s %8d bytes" % (cfg, os.path.getsize(cfg)))

    readme = drive + "\\READ-ME-FIRST.txt"
    with open(readme, "w", newline="\r\n") as f:
        f.write(ENROLL_STEPS if mode == "enroll" else "")
        f.write(INSTRUCTIONS)
    print("  %-34s %8d bytes" % (readme, os.path.getsize(readme)))

    if mode == "enroll":
        print("\ndone.  The next boot from this stick goes straight into the "
              "enrollment tool.\nAfter enrolling and rebooting into Windows, "
              "run:\n    python tools/writestick.py %s boot" % drive)
    else:
        print("\ndone.  This stick now boots KestrelOS.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
