#!/usr/bin/env python3
"""build.py - one command to build every KestrelOS artifact.

    python build.py            # build everything into out/
    python build.py kernel     # just the kernel
    python build.py clean

Outputs, all under out/:
    kestrelos.img    raw GPT disk image (write to a USB stick, or attach as a disk)
    kestrelos.vmdk   the same image with a VMware descriptor beside it
    kestrelos.iso    bootable UEFI ISO (El Torito, no emulation)
    windows/         the installer to run from inside Windows
"""
import os
import contextlib
import hashlib
import json
import re
import shutil
import subprocess
import sys
import time
import uuid

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(ROOT, "build")
OUT = os.path.join(ROOT, "out")
TOOLS = os.path.join(ROOT, "tools")

LLVM = os.environ.get("LLVM_DIR", r"C:\Program Files\LLVM\bin")


def tool(name):
    exe = os.path.join(LLVM, name + ".exe")
    return exe if os.path.exists(exe) else name


CLANG = tool("clang")
LLD_LINK = tool("lld-link")
LD_LLD = tool("ld.lld")
LLVM_AR = tool("llvm-ar")
OBJCOPY = tool("llvm-objcopy")
LLVM_NM = tool("llvm-nm")
OBJCOPY = tool("llvm-objcopy")
DLLTOOL = tool("llvm-dlltool")

# ---------------------------------------------------------------- flags

COMMON_WARN = ["-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-address-of-packed-member"]

EFI_CFLAGS = [
    "--target=x86_64-unknown-windows",
    "-ffreestanding", "-fshort-wchar", "-fno-stack-protector",
    "-mno-red-zone", "-mno-sse", "-mno-mmx",
    "-std=c11", "-O2",
] + COMMON_WARN

KERNEL_CFLAGS = [
    # A system call runs on a stack shared with everything beneath it, and a
    # frame that grows quietly is the kind of bug that shows up somewhere else
    # entirely.  Anything large enough to matter says so at build time.
    "-Wframe-larger-than=6144",
    "--target=x86_64-unknown-none-elf",
    "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fno-pie",
    "-mno-red-zone", "-mcmodel=kernel", "-mgeneral-regs-only",
    "-fno-omit-frame-pointer",
    "-std=c11", "-O2", "-g",
] + COMMON_WARN

# NVIDIA's NVKMS core is intentionally OS-neutral.  Its public portability
# boundary and RM operation layouts live in the extracted 595.99.02 source
# tree supplied alongside KestrelOS.  Keep the include list explicit: this is
# not a Linux-header dependency and it must never silently pick host headers.
NV_OPEN = os.path.join(ROOT, "out", "nvidia-open-595.99.02")
NV_COMMON = os.path.join(NV_OPEN, "src", "common")
NV_MODESET = os.path.join(NV_OPEN, "src", "nvidia-modeset")
NVKMS_CFLAGS = [
    "-I", os.path.join(NV_COMMON, "sdk", "nvidia", "inc"),
    "-I", os.path.join(NV_OPEN, "src", "nvidia", "arch", "nvalloc", "unix", "include"),
    "-I", os.path.join(NV_MODESET, "os-interface", "include"),
    "-I", os.path.join(NV_MODESET, "interface"),
    "-I", os.path.join(NV_MODESET, "kapi", "interface"),
    "-I", os.path.join(NV_OPEN, "kernel-open", "common", "inc"),
]

USER_CFLAGS = [
    "--target=x86_64-unknown-none-elf",
    "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fno-pie",
    "-mno-red-zone", "-mcmodel=small",
    "-std=c11", "-O2", "-g",
    # One section per function lets the linker drop the parts of the GUI
    # toolkit that a text-mode program never calls.
    "-ffunction-sections", "-fdata-sections",
    "-I", os.path.join(ROOT, "user", "libc"),
    "-I", os.path.join(ROOT, "user", "libgui"),
    "-I", os.path.join(ROOT, "user", "libgl"),
    "-I", os.path.join(ROOT, "user", "libweb"),
    "-I", os.path.join(ROOT, "user", "libarm"),
] + COMMON_WARN

# ---------------------------------------------------------------- helpers

_start = time.time()
_failed = False


def say(msg, indent=0):
    print("%7.2fs %s%s" % (time.time() - _start, "  " * indent, msg), flush=True)


def run(cmd, quiet=False):
    global _failed
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        _failed = True
        say("FAILED: " + " ".join(os.path.basename(c) if i == 0 else c
                                  for i, c in enumerate(cmd)))
        out = (r.stdout or "") + (r.stderr or "")
        for line in out.splitlines()[:60]:
            print("        " + line, flush=True)
        raise SystemExit(1)
    if not quiet and (r.stdout or r.stderr):
        out = (r.stdout or "") + (r.stderr or "")
        for line in out.splitlines():
            if line.strip():
                print("        " + line, flush=True)
    return r


def mkdirs(*paths):
    for p in paths:
        os.makedirs(p, exist_ok=True)


def newer(src, dst):
    return not os.path.exists(dst) or os.path.getmtime(src) > os.path.getmtime(dst)


_compiler_identities = {}


def _file_fingerprint(path, cache):
    """Hash each input once per compile_all call, unless its stat changes."""
    path = os.path.abspath(path)
    st = os.stat(path)
    key = (path, st.st_size, st.st_mtime_ns, st.st_ctime_ns)
    if key not in cache:
        digest = hashlib.sha256()
        with open(path, "rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        cache[key] = digest.hexdigest()
    return cache[key]


def _compiler_identity():
    executable = os.path.abspath(shutil.which(CLANG) or CLANG)
    st = os.stat(executable)
    key = (executable, st.st_size, st.st_mtime_ns, st.st_ctime_ns)
    if key not in _compiler_identities:
        _compiler_identities[key] = _file_fingerprint(executable, {})
    return [executable, _compiler_identities[key]]


def _compiler_dependencies(text):
    """Decode Clang/GNU make dependencies, including escaped Windows paths.

    A fixed -MT target means a drive letter's colon can never be mistaken for
    the rule separator. Backslashes before ordinary letters remain literal;
    make's escaped whitespace/hash/backslash and doubled dollars are decoded.
    No -MP phony rules are requested, and malformed output is never cached.
    """
    text = re.sub(r"\\\r?\n", " ", text)
    prefix = "kestrel_object:"
    if not text.startswith(prefix):
        raise ValueError("unexpected compiler dependency target")
    paths, word = [], []
    i = len(prefix)
    while i < len(text):
        c = text[i]
        if c == "\\" and i + 1 < len(text) and text[i + 1] in " \t\r\n#\\:":
            i += 1
            word.append(text[i])
        elif c == "$" and i + 1 < len(text) and text[i + 1] == "$":
            word.append("$")
            i += 1
        elif c.isspace():
            if word:
                paths.append("".join(word))
                word = []
        elif c == "#":
            raise ValueError("unescaped comment in compiler dependencies")
        else:
            word.append(c)
        i += 1
    if word:
        paths.append("".join(word))
    if not paths:
        raise ValueError("empty compiler dependencies")
    return paths


def _object_stamp(path):
    st = os.stat(path)
    return [st.st_size, st.st_mtime_ns, st.st_ctime_ns]


def _compile_current(obj, manifest, command, cache):
    try:
        with open(manifest, encoding="utf-8") as stream:
            saved = json.load(stream)
        if (saved["version"] != 2 or saved["command"] != command or
                saved["object"] != _object_stamp(obj) or not saved["inputs"]):
            return False
        for path, digest in saved["inputs"]:
            if _file_fingerprint(path, cache) != digest:
                return False
        return True
    except (OSError, ValueError, KeyError, TypeError):
        return False


@contextlib.contextmanager
def _compile_staging(outdir, base):
    """Unique sibling files inherit the output directory's Windows DACL.

    Python's private TemporaryDirectory uses a restrictive Windows DACL which
    follows files through os.replace. Do not stage shared build artifacts in
    such a directory. O_EXCL creates only our own files; cleanup names them
    individually and never traverses/removes the output directory.
    """
    paths = []
    try:
        token = uuid.uuid4().hex
        for suffix in (".o", ".d", ".json"):
            path = os.path.join(outdir, "." + base + "-" + token + suffix)
            fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o666)
            paths.append(path)
            os.close(fd)
        yield paths
    finally:
        for path in paths:
            try:
                os.remove(path)
            except FileNotFoundError:
                pass


def compile_all(sources, outdir, flags, deps=()):
    """Compile with transitive include and command tracking; publish atomically.

    Explicit deps remain additional prerequisites, but no longer define the
    complete header closure. Objects from an old build without a manifest are
    rebuilt once. A failed compiler run cannot make a previous object current.
    """
    mkdirs(outdir)
    objs, cache = [], {}
    dependencies = sorted(set(os.path.abspath(d) for d in deps))
    compiler = None
    # These variables can change Clang's include search without changing flags.
    environment = {name: os.environ.get(name) for name in (
        "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
        "INCLUDE", "SDKROOT", "CLANG_CONFIG_FILE_SYSTEM_DIR", "CLANG_CONFIG_FILE_USER_DIR")}
    for src in sources:
        src = os.fspath(src)
        if compiler is None:
            compiler = _compiler_identity()
        base = os.path.splitext(os.path.basename(src))[0]
        obj = os.path.join(outdir, base + ".o")
        depfile, manifest = obj + ".d", obj + ".deps.json"
        command = {"compiler": compiler, "flags": list(flags), "source": os.path.abspath(src),
                   "source_argument": src, "output": os.path.abspath(obj),
                   "cwd": os.getcwd(), "environment": environment, "deps": dependencies}
        if not _compile_current(obj, manifest, command, cache):
            # Invalidate before invoking Clang. Even if an old object survives a
            # failure, no subsequent invocation may treat it as a valid cache hit.
            try:
                os.remove(manifest)
            except FileNotFoundError:
                pass
            with _compile_staging(outdir, base) as staged:
                staged_obj, staged_dep, staged_manifest = staged
                started = time.time_ns()
                run([CLANG] + list(flags) + ["-MD", "-MF", staged_dep, "-MT", "kestrel_object",
                                           "-c", src, "-o", staged_obj])
                with open(staged_dep, encoding="utf-8") as stream:
                    inputs = _compiler_dependencies(stream.read())
                inputs = sorted(set(os.path.abspath(p) for p in inputs) |
                                set(dependencies) | {os.path.abspath(src)})
                fingerprints = [[p, _file_fingerprint(p, cache)] for p in inputs]
                # Do not link an object whose inputs are known to have changed
                # during its compile. A missing manifest only protects a later
                # build; this build must fail before publishing either output.
                changed = [p for p in inputs if os.stat(p).st_mtime_ns > started]
                if changed:
                    raise RuntimeError("compiler inputs changed during compilation; retry build: " +
                                       ", ".join(changed))
                os.replace(staged_dep, depfile)
                os.replace(staged_obj, obj)
                state = {"version": 2, "command": command, "object": _object_stamp(obj),
                         "inputs": fingerprints}
                with open(staged_manifest, "w", encoding="utf-8") as stream:
                    json.dump(state, stream, sort_keys=True)
                    stream.flush()
                    os.fsync(stream.fileno())
                os.replace(staged_manifest, manifest)  # commit marker, always last
        objs.append(obj)
    return objs


def archive(objs, path):
    """Gather objects into a library.

    The difference between handing the linker a pile of objects and handing it
    a library is which of them end up in the program.  A pile all goes in; a
    library gives up only the members that something actually needs.  For a
    console program that never draws anything, that is the difference between
    carrying four anti-aliased typefaces it will never use and carrying none of
    them - about a hundred and twenty kilobytes, in every program on the disk.
    """
    if os.path.exists(path):
        os.remove(path)
    run([LLVM_AR, "rcs", path] + objs)
    return path


def headers_in(*dirs):
    out = []
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for f in os.listdir(d):
            if f.endswith(".h"):
                out.append(os.path.join(d, f))
    return out


def sources_in(d, exclude=()):
    if not os.path.isdir(d):
        return []
    return sorted(os.path.join(d, f) for f in os.listdir(d)
                  if (f.endswith(".c") or f.endswith(".S")) and f not in exclude)


# ---------------------------------------------------------------- targets

def build_icon_font():
    """Write the icon set out as a TrueType file for the system volume.

    Kept separate from the fonts the interface draws with, which are compiled
    in: those have to exist before any filesystem is mounted.  This one is a
    file on a volume, which is the point of it."""
    out = os.path.join(OUT, "KestrelIcons.ttf")
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "iconfont.py"), out],
                       capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        say("could not build the icon font: " + (r.stderr or r.stdout).strip()[:120], 1)
        return

    # Staged where the provisioning script looks for things bound for the
    # system volume, rather than the boot one - the firmware does not read it
    # and nothing needs it before the disks are up.
    staged = os.path.join(OUT, "windows", "system", "fonts")
    os.makedirs(staged, exist_ok=True)
    shutil.copyfile(out, os.path.join(staged, "KestrelIcons.ttf"))
    say("-> KestrelIcons.ttf (%d bytes, for the system volume)"
        % os.path.getsize(out), 1)


def build_app_icons():
    """Render the app-icon PNGs (rounded gradient tiles + white glyphs) into
    out/icons.  Returns the directory, or None if the generator failed - the
    desktop then falls back to drawn glyphs.  See tools/genicons.py."""
    out = os.path.join(OUT, "icons")
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "genicons.py"), out],
                       capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        say("could not build the app icons: " + (r.stderr or r.stdout).strip()[:160], 1)
        return None
    # The OS logo (detailed kestrel): a tintable mask + a full-colour tile.
    rl = subprocess.run([sys.executable, os.path.join(TOOLS, "genlogo.py"), out],
                        capture_output=True, text=True, cwd=ROOT)
    if rl.returncode != 0:
        say("could not build the logo: " + (rl.stderr or rl.stdout).strip()[:160], 1)
    say("-> app icons + logo (%s)" % (r.stdout.strip() or "ok"), 1)
    return out


def build_font():
    hdr = os.path.join(ROOT, "kernel", "font8x16.h")
    gen = os.path.join(TOOLS, "genfont.ps1")
    if os.path.exists(hdr) and not newer(gen, hdr):
        return
    say("generating the console font")
    run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", gen], quiet=True)


def build_boot():
    say("bootloader")
    d = os.path.join(BUILD, "boot")
    deps = headers_in(os.path.join(ROOT, "boot"), os.path.join(ROOT, "include", "kestrel"))
    build_font()   # the loader draws its own text with the same glyphs
    objs = compile_all([os.path.join(ROOT, "boot", "boot.c")], d, EFI_CFLAGS, deps)
    efi = os.path.join(d, "BOOTX64.EFI")
    # /Brepro makes the link reproducible: the timestamp field in the PE header
    # becomes a function of the content instead of the clock, so building the
    # same source twice gives the same bytes.
    #
    # That matters more here than it usually does.  Booting this with Secure
    # Boot on means a hash of this exact file has been enrolled in the
    # machine's firmware by hand, and a timestamp that moves every build would
    # invalidate that enrolment on every build - including builds that changed
    # nothing in the loader at all.  With this, the enrolment survives until
    # the loader itself actually changes.
    run([LLD_LINK, "-subsystem:efi_application", "-entry:efi_main",
         "-nodefaultlib", "-dll", "-Brepro", "-out:" + efi] + objs)
    say("-> %s (%d bytes)" % (os.path.basename(efi), os.path.getsize(efi)), 1)
    return efi


def build_kernel():
    say("kernel")
    build_font()
    d = os.path.join(BUILD, "kernel")
    kdir = os.path.join(ROOT, "kernel")
    deps = headers_in(kdir, os.path.join(ROOT, "include", "kestrel"))
    # The NTFS reader is shared with the loader, so it lives outside kernel/
    # and is compiled into both rather than existing twice.
    shared = [os.path.join(ROOT, "common", "ntfs_core.c"),
              os.path.join(ROOT, "common", "ntfs_write.c"),
              os.path.join(ROOT, "common", "exfat.c"),
              os.path.join(ROOT, "common", "ext4.c"),
              os.path.join(ROOT, "common", "xfs.c"),
              os.path.join(ROOT, "common", "btrfs.c")]
    # nv_dsc_pps.c (our hand-written DSC PPS generator) is redundant with the
    # open NVKMS core, which bundles NVIDIA's real DSC library and defines the
    # same DSC_GeneratePPS* symbols.  Drop ours so NVKMS uses its own.
    objs = compile_all(sources_in(kdir, exclude=("nv_dsc_pps.c",)) + shared, d,
                       KERNEL_CFLAGS + NVKMS_CFLAGS, deps)

    # Use NVIDIA's complete NVKMS state machine rather than duplicating its
    # modeset sequencing a register at a time.  This relocatable object is the
    # OS-neutral core shipped by the user's extracted 595.99.02 driver; all of
    # its host imports are implemented by kernel/nvkms_port.c and
    # kernel/nvkms_thunks.S.  The build fails on any missing import.
    nvkms_core = os.path.join(ROOT, "Linux NVIDIA Driver", "kernel-open",
                              "nvidia-modeset", "nv-modeset-kernel.o_binary")
    if not os.path.exists(nvkms_core):
        raise SystemExit("missing supplied NVIDIA 595.99.02 NVKMS core: " + nvkms_core)
    # Preserve NVIDIA's supplied object and derive a fail-closed Kestrel copy.
    # The compatibility patch keeps client-provided and live DP-SST EDIDs in
    # NVKMS's own validation path instead of losing them in an RM SET/GET cache
    # round trip.  The script checks both the full vendor SHA-256 and the exact
    # instruction bytes before writing anything. Its second stage corrects
    # DP DSC's flatness threshold to use PPS bits-per-component, not bpp*16.
    nvkms_core_patched = os.path.join(d, "nv-modeset-kernel-kestrel.o_binary")
    run([sys.executable,
         os.path.join(TOOLS, "patch_nvkms_edid_override.py"),
         nvkms_core, nvkms_core_patched])
    objs.append(nvkms_core_patched)

    # NVKMS' MapMemory operations are completed by the host Resource Manager,
    # not by GSP-RM: it retains the memory descriptors needed to map USERD,
    # display-channel MMIO and VRAM through BAR0/BAR1.  Link the matching RM
    # core from the same 595.99.02 package.  Activation remains separately
    # gated by tools/audit_nvrm_port.py until every reachable host primitive
    # has a typed Kestrel implementation (weak placeholders only close the
    # link so the integration can be developed and audited incrementally).
    # RTX 50-series (Blackwell GB20x) HAL exists ONLY in the open-kernel-module
    # RM core; the proprietary kernel/nvidia blob has no GB202/GB203 support and
    # refuses ("requires use of the NVIDIA open kernel modules").
    nvrm_core = os.path.join(ROOT, "Linux NVIDIA Driver", "kernel-open",
                             "nvidia", "nv-kernel.o_binary")
    if not os.path.exists(nvrm_core):
        raise SystemExit("missing supplied NVIDIA 595.99.02 RM core: " + nvrm_core)
    objs.append(nvrm_core)
    elf = os.path.join(d, "kernel.elf")
    # -z muldefs: the open nvidia + nvidia-modeset cores each bundle NVIDIA's
    # shared nvstatusToString (g_nvid_string.c); normally they live in two
    # separate .ko modules, but we link both into one image, so allow the
    # first (nvidia-modeset) definition to win.  This is NVIDIA's own identical
    # utility, so either copy is correct.
    run([LD_LLD, "-T", os.path.join(kdir, "kernel.ld"), "-nostdlib",
         "-z", "max-page-size=4096", "-z", "muldefs",
         "--wrap=nvDPGetEDID",
         "-o", elf] + objs)
    say("-> kernel.elf (%d bytes)" % os.path.getsize(elf), 1)
    return elf


# A KestrelOS program and a Linux program are both static x86-64 ELF files and
# are, byte for byte in their headers, the same thing.  They are not the same
# thing at run time: the two disagree about what number means "exit", so a
# Linux program run as a native one calls read() where it meant to stop.
#
# ELF has a byte for exactly this - e_ident[EI_OSABI], which says whose system
# calls the file expects - and every toolchain leaves it zero, meaning "no
# claim".  So a claim is stamped here: KestrelOS programs say so, and anything
# that does not is treated as a Linux program, which is the safe way round -
# an unmarked file gets the translated path rather than the raw one.
EI_OSABI = 7
OSABI_KESTREL = 0x4B


def stamp_kestrel(path):
    with open(path, "r+b") as f:
        f.seek(EI_OSABI)
        f.write(bytes([OSABI_KESTREL]))


def build_user():
    say("userland")
    udir = os.path.join(ROOT, "user")
    libc_dir = os.path.join(udir, "libc")
    deps = headers_in(libc_dir, os.path.join(ROOT, "include", "kestrel"))

    libc_objs = compile_all(sources_in(libc_dir), os.path.join(BUILD, "user", "libc"),
                            USER_CFLAGS, deps)

    gui_dir = os.path.join(udir, "libgui")
    gui_deps = deps + headers_in(gui_dir)
    gui_objs = compile_all(sources_in(gui_dir), os.path.join(BUILD, "user", "libgui"),
                           USER_CFLAGS, gui_deps)

    gl_dir = os.path.join(udir, "libgl")
    gl_deps = gui_deps + headers_in(gl_dir)
    gl_objs = compile_all(sources_in(gl_dir), os.path.join(BUILD, "user", "libgl"),
                          USER_CFLAGS, gl_deps)

    # Real ARM code for the interpreter to run, compiled by the ordinary
    # toolchain for a processor this machine does not have.
    arm_dir = os.path.join(udir, "libarm")
    arm_probe = os.path.join(ROOT, "tests", "arm", "probe.c")
    if os.path.exists(arm_probe):
        sys.path.insert(0, TOOLS)
        import mkarmtest
        header = os.path.join(arm_dir, "arm_probe.h")
        if newer(arm_probe, header):
            size, count = mkarmtest.build(CLANG, OBJCOPY, LLVM_NM,
                                          arm_probe, header)
            say("-> %d bytes of ARM code, %d functions" % (size, count), 1)

    web_dir = os.path.join(udir, "libweb")
    web_deps = gl_deps + headers_in(web_dir)
    web_objs = compile_all(sources_in(web_dir), os.path.join(BUILD, "user", "libweb"),
                           USER_CFLAGS, web_deps)

    # Everything above libc becomes a library, so a program takes only the
    # parts of it that it reaches.
    arm_deps = deps + headers_in(arm_dir)
    arm_objs = compile_all(sources_in(arm_dir), os.path.join(BUILD, "user", "libarm"),
                           USER_CFLAGS, arm_deps)
    arm_lib = archive(arm_objs, os.path.join(BUILD, "user", "libarm.a"))

    gui_lib = archive(gui_objs, os.path.join(BUILD, "user", "libgui.a"))
    gl_lib  = archive(gl_objs,  os.path.join(BUILD, "user", "libgl.a"))
    web_lib = archive(web_objs, os.path.join(BUILD, "user", "libweb.a"))

    programs = {}
    for name in sorted(os.listdir(udir)):
        pdir = os.path.join(udir, name)
        if name in ("libc", "libgui", "libgl", "libweb", "libarm") or not os.path.isdir(pdir):
            continue
        srcs = sources_in(pdir)
        if not srcs:
            continue
        objs = compile_all(srcs, os.path.join(BUILD, "user", name), USER_CFLAGS, web_deps)
        elf = os.path.join(BUILD, "user", name + ".elf")
        run([LD_LLD, "-T", os.path.join(libc_dir, "user.ld"), "-nostdlib",
             "--gc-sections",
             "-o", elf] + objs + libc_objs + [web_lib, arm_lib, gl_lib, gui_lib])
        stamp_kestrel(elf)
        programs[name] = elf
        say("-> %s (%d bytes)" % (name, os.path.getsize(elf)), 1)
    return programs


# Which export table in the subsystem's source belongs to which library.  The
# import libraries the test programs link against are generated from these, so
# a function that is not implemented cannot be linked against by accident - it
# fails at the link step, where the mistake is obvious, rather than at run time
# in the middle of something else.
WIN_LIBRARIES = {
    "kernel32.dll": ["kernel32_core", "k32_file_exports", "k32_thread_exports", "seh_exports"],
    "user32.dll":   ["user32"],
    "gdi32.dll":    ["gdi32"],
    "msvcrt.dll":   ["msvcrt"],
    "advapi32.dll": ["advapi32"],
    "ws2_32.dll":   ["ws2_32"],
    "shell32.dll":  ["shell32"],
    "shlwapi.dll":  ["shlwapi"],
    "ole32.dll":    ["ole32"],
    "d3d.dll":      ["d3d_exports"],
    "d3d12.dll":    ["d3d12_exports"],
}


def scan_export_tables():
    """Pull every {"Name", ...} entry out of the subsystem's export tables."""
    import re
    wdir = os.path.join(ROOT, "user", "winrun")
    tables = {}
    for f in sorted(os.listdir(wdir)):
        if not f.endswith(".c"):
            continue
        text = open(os.path.join(wdir, f), encoding="utf-8").read()
        for m in re.finditer(r"win_export_t\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\n\};",
                             text, re.S):
            names = re.findall(r'\{\s*"([^"]+)"\s*,', m.group(2))
            tables.setdefault(m.group(1), []).extend(names)
    return tables


def build_import_libs(outdir):
    """One import library per Windows DLL the tests may link against."""
    tables = scan_export_tables()
    mkdirs(outdir)
    libs = []
    for dll, table_names in sorted(WIN_LIBRARIES.items()):
        names = []
        for t in table_names:
            names.extend(tables.get(t, []))
        # A name exported twice would make two entries in the library.
        seen, unique = set(), []
        for n in names:
            if n not in seen:
                seen.add(n)
                unique.append(n)
        if not unique:
            continue

        stem = os.path.splitext(dll)[0]
        deffile = os.path.join(outdir, stem + ".def")
        body = "LIBRARY %s\nEXPORTS\n" % dll
        body += "".join("    %s\n" % n for n in unique)
        old = open(deffile).read() if os.path.exists(deffile) else None
        if old != body:
            with open(deffile, "w", newline="\n") as f:
                f.write(body)
        lib = os.path.join(outdir, stem + ".lib")
        if newer(deffile, lib):
            run([DLLTOOL, "-m", "i386:x86-64", "-d", deffile, "-l", lib], quiet=True)
        libs.append(lib)
    return libs


WIN_CFLAGS = [
    "--target=x86_64-pc-windows-msvc", "-ffreestanding",
    "-fno-stack-protector", "-mno-stack-arg-probe", "-O1",
    # Without this, a hardware fault is not something the compiler believes
    # can happen, so it emits no unwind tables and optimises a __try away
    # entirely.  This is what Microsoft's own /EHa means.
    "-fasync-exceptions",
    "-nostdlib", "-Wall", "-Wno-unused-function",
]


def build_win_tests():
    """Windows programs used to exercise the subsystem: console, graphical,
    threaded, and one that ships a DLL of its own."""
    tdir = os.path.join(ROOT, "tests", "win")
    if not os.path.isdir(tdir):
        return {}
    say("windows test programs")
    outdir = os.path.join(BUILD, "win")
    mkdirs(outdir)

    libs = build_import_libs(outdir)

    # A program's own shaders, compiled by Microsoft's own compiler into the
    # blob a Windows program would hand to Direct3D.  Written out as a header
    # so the test program carries them rather than reading them from a file -
    # what matters is the bytes, not where they came from.
    hlsl = os.path.join(tdir, "shaders.hlsl")
    fxc = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                       "Windows Kits", "10", "bin", "10.0.26100.0", "x64", "fxc.exe")
    header = os.path.join(outdir, "shaders_blob.h")
    if os.path.exists(hlsl) and os.path.exists(fxc) and newer(hlsl, header):
        lines = ["/* Generated from tests/win/shaders.hlsl by Microsoft's",
                 " * shader compiler - these are the exact bytes a Windows",
                 " * program would hand to Direct3D. */",
                 "#pragma once"]
        for entry, profile, name in (("VSMain", "vs_4_0", "shader_vertex_blob"),
                                     ("PSMain", "ps_4_0", "shader_pixel_blob")):
            cso = os.path.join(outdir, name + ".cso")
            run([fxc, "/T", profile, "/E", entry, "/Fo", cso, hlsl], quiet=True)
            with open(cso, "rb") as f:
                data = f.read()
            lines.append("")
            lines.append("static const unsigned char %s[%d] = {" % (name, len(data)))
            for i in range(0, len(data), 12):
                lines.append("    " + ",".join("0x%02X" % b for b in data[i:i+12]) + ",")
            lines.append("};")
        with open(header, "w", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        say("-> shaders compiled by fxc (%d bytes of blob)" % len(data), 1)

    # A .def written by hand alongside the tests, for names this system does
    # NOT implement: the loader's stub path needs a program that imports one.
    for d in sorted(f for f in os.listdir(tdir) if f.endswith(".def")):
        stem = os.path.splitext(d)[0]
        lib = os.path.join(outdir, stem + ".lib")
        if newer(os.path.join(tdir, d), lib):
            run([DLLTOOL, "-m", "i386:x86-64",
                 "-d", os.path.join(tdir, d), "-l", lib], quiet=True)
        libs.append(lib)

    made = {}

    # DLLs first: a program that imports from one needs its import library to
    # exist before it can be linked.
    dll_libs = {}
    for f in sorted(os.listdir(tdir)):
        if not f.endswith("lib.c"):
            continue
        base = os.path.splitext(f)[0]
        dll = os.path.join(outdir, base + ".dll")
        implib = os.path.join(outdir, base + ".lib")
        run([CLANG] + WIN_CFLAGS +
            ["-shared", "-Wl,-dll", "-Wl,-nodefaultlib",
             "-Wl,-implib:" + implib, "-Wl,-entry:DllMain",
             os.path.join(tdir, f)] + libs + ["-o", dll])
        dll_libs[base] = implib
        made[base + ".dll"] = dll
        say("-> %s (%d bytes)" % (base + ".dll", os.path.getsize(dll)), 1)

    for f in sorted(os.listdir(tdir)):
        cplusplus = f.endswith(".cpp")
        if not (f.endswith(".c") or cplusplus) or f.endswith("lib.c"):
            continue
        base = os.path.splitext(f)[0]
        exe = os.path.join(outdir, base + ".exe")
        # A graphical program is marked as one in its header, which is what
        # tells the subsystem to bring the display up before it starts.
        subsystem = "windows" if base.startswith("gui") else "console"
        extra = list(dll_libs.values()) if base == "dlltest" else []
        language = ["-std=c++17", "-I", tdir] if cplusplus else []
        run([CLANG] + WIN_CFLAGS + language +
            ["-Wl,-subsystem:" + subsystem, "-Wl,-entry:mainCRTStartup",
             "-I", outdir,
             "-Wl,-nodefaultlib", os.path.join(tdir, f)] + libs + extra + ["-o", exe])
        made[base + ".exe"] = exe
        say("-> %s (%d bytes)" % (base + ".exe", os.path.getsize(exe)), 1)
    return made


def strip_copy(path, name):
    """A debug-info-free copy for packaging; the originals stay for debugging."""
    out_dir = os.path.join(BUILD, "stripped")
    mkdirs(out_dir)
    dst = os.path.join(out_dir, name)
    if newer(path, dst):
        run([OBJCOPY, "--strip-debug", "--strip-unneeded", path, dst], quiet=True)
    return dst


def build_images(efi, kernel, programs, win):
    say("assembling images")
    sys.path.insert(0, TOOLS)
    import mkkar, mkfat, mkimage, mkroots

    mkdirs(OUT, BUILD)

    # ---- initrd -------------------------------------------------------
    tree = {}

    # A whole program built for ARM, carried as an ordinary file so that
    # running it is running a program rather than a test: the interpreter
    # opens it, lays it out, and answers what it asks for.
    arm_hello = os.path.join(ROOT, "tests", "arm", "hello.c")
    if os.path.exists(arm_hello):
        out = os.path.join(BUILD, "hello.arm")
        if newer(arm_hello, out):
            run([CLANG, "--target=aarch64-unknown-none-elf", "-ffreestanding",
                 "-fno-stack-protector", "-fno-pic", "-fno-pie", "-O2",
                 "-nostdlib", "-static", "-fuse-ld=lld", "-Wl,-e,_start",
                 "-o", out, arm_hello])
        tree["/bin/hello.arm"] = out
        say("-> hello.arm (%d bytes of AArch64)" % os.path.getsize(out), 1)
    # And a whole program built for Linux.  This one is not interpreted: it is
    # the same processor, so the file is loaded and the instructions run at
    # full speed.  What stands between it and the kernel is the system call
    # numbering, which is translated - so the file is left exactly as the
    # compiler produced it, byte 7 and all, and is recognised as a Linux
    # program because it does not claim to be anything else.
    #
    # It is linked at 0x400000 because that is where Linux puts a static
    # program; lld picks a lower address by default, and KestrelOS puts a
    # program's image at the same 0x400000 Linux does.
    for stem in ("hello", "threads"):
        src = os.path.join(ROOT, "tests", "linux", stem + ".c")
        if not os.path.exists(src):
            continue
        out = os.path.join(BUILD, stem + ".linux")
        if newer(src, out):
            run([CLANG, "--target=x86_64-unknown-linux-gnu", "-ffreestanding",
                 "-fno-stack-protector", "-fno-pie", "-O2",
                 "-nostdlib", "-static", "-fuse-ld=lld", "-Wl,-e,_start",
                 "-Wl,--image-base=0x400000", "-o", out, src])
        tree["/bin/%s.linux" % stem] = out
        say("-> %s.linux (%d bytes of Linux x86-64)"
            % (stem, os.path.getsize(out)), 1)

    for name, elf in programs.items():
        tree["/bin/" + name] = strip_copy(elf, name)
    for name, exe in win.items():
        tree["/bin/" + name] = exe


    # The installer reproduces the running system on the target disk, so the
    # loader and the kernel travel inside the initrd; the initrd itself is read
    # back through /dev/initrd, which avoids the obvious circularity.
    #
    # Not under /boot: the kernel mounts the boot volume there once it finds
    # one, which mounts a filesystem directly over these two files and leaves
    # the installer reporting that its own sources do not exist.
    tree["/lib/boot/BOOTX64.EFI"] = efi
    tree["/lib/boot/KERNEL.ELF"] = strip_copy(kernel, "kernel.elf")

    etc = os.path.join(BUILD, "etc")
    mkdirs(etc)
    with open(os.path.join(etc, "motd"), "w", newline="\n") as f:
        f.write("KestrelOS\nType `help` for a list of commands.\n")
    tree["/etc/motd"] = os.path.join(etc, "motd")

    # The app-icon images, bundled read-only for the desktop to decode at start.
    icon_dir = build_app_icons()
    if icon_dir and os.path.isdir(icon_dir):
        for f in sorted(os.listdir(icon_dir)):
            if f.endswith(".png"):
                tree["/usr/share/icons/" + f] = os.path.join(icon_dir, f)

    # Optional autostart list: names of desktop apps to open at start instead of
    # the Terminal, one per line.  Only written when KESTREL_STARTAPP is set in
    # the environment (used to build an image that comes up on a chosen app so
    # its window can be captured for review off-hardware); a normal build omits
    # the file and the desktop opens the Terminal as before.
    startapp = os.environ.get("KESTREL_STARTAPP")
    if startapp:
        sa = os.path.join(etc, "startapp")
        with open(sa, "w", newline="\n") as f:
            f.write(startapp.replace(",", "\n") + "\n")
        tree["/etc/startapp"] = sa
        say("-> /etc/startapp = %r" % startapp, 1)

    # What the Applications window offers to fetch.  Plain text, so it can be
    # read and added to without a tool.
    apps_list = os.path.join(ROOT, "data", "apps.list")
    if os.path.exists(apps_list):
        tree["/etc/apps.list"] = apps_list

    # The certificate authorities a secure connection is checked against.  The
    # text bundle in data/ is converted here so the kernel does not have to
    # carry a base64 decoder to read its own trust list.
    roots_pem = os.path.join(ROOT, "data", "cacert.pem")
    if os.path.exists(roots_pem):
        roots_bin = os.path.join(etc, "roots.bin")
        count, _, size = mkroots.build(roots_pem, roots_bin)
        tree["/etc/ssl/roots.bin"] = roots_bin
        say("-> %d certificate authorities, %d bytes" % (count, size), 1)
    else:
        say("!! data/cacert.pem is missing; secure connections will have "
            "nothing to check certificates against", 1)

    # Vendor firmware the user has supplied.
    #
    # A modern graphics card is driven through a resource manager that runs on
    # the card's own processor, and that image is NVIDIA's to distribute rather
    # than ours to carry.  So it is not in this repository: anything dropped
    # into firmware/ is picked up here and travels in the initrd.
    #
    # The initrd is the right place for it because the loader reads that into
    # memory before the kernel starts.  Firmware on a partition would need a
    # working storage driver to reach, and on a machine that boots from a USB
    # stick the storage driver is one of the things the firmware is needed for.
    fw_root = os.path.join(ROOT, "firmware")
    fw_count, fw_bytes = 0, 0
    if os.path.isdir(fw_root):
        for dirpath, _dirs, files in os.walk(fw_root):
            for name in files:
                # The note explaining what goes here is not firmware.
                if name.lower() in ("readme.txt", "readme.md", ".gitignore"):
                    continue
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, fw_root).replace("\\", "/")
                # EDID captures are provenance for a generated/normalized
                # connector override, not firmware consumed at runtime.  Keep
                # the raw bytes in the workspace without leaking redundant
                # monitor serial data into every initrd.
                if rel.startswith("edid/captures/"):
                    continue
                tree["/lib/firmware/" + rel] = full
                fw_count += 1
                fw_bytes += os.path.getsize(full)
    if fw_count:
        say("-> %d firmware file(s) from firmware/, %.1f MiB"
            % (fw_count, fw_bytes / (1024.0 * 1024.0)), 1)

    initrd = os.path.join(BUILD, "initrd.kar")
    n = mkkar.build(tree, initrd)
    say("-> initrd.kar (%d entries, %d bytes)" % (n, os.path.getsize(initrd)), 1)

    # ---- ESP contents -------------------------------------------------
    bootcfg = os.path.join(BUILD, "BOOT.CFG")
    with open(bootcfg, "w", newline="\n") as f:
        f.write("# KestrelOS loader configuration\n")
        f.write("timeout=3\n")
        f.write("kernel=\\KESTREL\\KERNEL.ELF\n")
        f.write("initrd=\\KESTREL\\INITRD.KAR\n")
        # KESTREL_CMDLINE lets a test build set the kernel command line without
        # editing anything.  Used to reproduce a machine that has no PS/2 port
        # - which is every machine built in the last decade, and the case where
        # the USB input path has to work on its own rather than being masked by
        # a PS/2 device that answers.
        f.write("cmdline=%s\n" % os.environ.get("KESTREL_CMDLINE", ""))
        # The multi-display arrangement for two-or-more displays (#7):
        # extend (default) | mirror | onlyother.  KESTREL_DISPLAYMODE sets it
        # without editing anything, the same way KESTREL_CMDLINE does.
        _dm = os.environ.get("KESTREL_DISPLAYMODE", "")
        if _dm:
            f.write("displaymode=%s\n" % _dm)
        # Dedicated validation media can default to the real GPU-test menu
        # entry without contaminating the main desktop entry's command line.
        # Pressing S still boots main immediately.
        _default = os.environ.get("KESTREL_DEFAULT_ENTRY", "")
        if _default:
            f.write("default=%s\n" % _default)

    esp_files = {
        "/KESTREL/KERNEL.ELF": kernel,
        "/KESTREL/INITRD.KAR": initrd,
        "/KESTREL/BOOT.CFG": bootcfg,
        # The desktop's own settings, in a file somebody can open and edit -
        # on the boot volume so it can be changed from another machine with
        # the stick plugged in.
        "/desktop.conf": os.path.join(ROOT, "boot", "desktop.conf"),
    }

    # Secure Boot.  When Microsoft's signed shim is present, lay the chain the
    # firmware accepts: shim at BOOTX64.EFI, OUR loader at grubx64.efi (where the
    # shim looks), and MokManager beside it so the loader's hash can be enrolled
    # on the machine itself.  This is the SAME chain tools/installusb.ps1 lays;
    # putting it IN the image means writeusb.ps1 (a raw whole-disk write) also
    # produces a stick that boots with Secure Boot ON, instead of leaving our
    # unsigned loader at BOOTX64.EFI where the firmware refuses it outright
    # ("Invalid signature detected").  The loader is built -Brepro, so its hash
    # is stable across rebuilds and an existing enrolment keeps working.
    #
    # A shimmed BOOTX64.EFI still boots with Secure Boot OFF too (shim runs the
    # second stage without verification), so QEMU/OVMF is unaffected.  Without
    # the shim file (a source tree that has not fetched it) our loader sits at
    # BOOTX64.EFI as before, which is right for a machine with Secure Boot off.
    _sb = os.path.join(ROOT, "secureboot")
    _shim = os.path.join(_sb, "shimx64.signed.efi")
    _mok  = os.path.join(_sb, "mmx64.efi")
    _cer  = os.path.join(_sb, "KESTREL.CER")
    if os.path.exists(_shim):
        for _base in ("/EFI/BOOT", "/EFI/KESTREL"):
            esp_files[_base + "/BOOTX64.EFI"] = _shim   # the signed shim
            esp_files[_base + "/grubx64.efi"] = efi      # our loader, where shim looks
            esp_files[_base + "/kestrel.efi"] = efi      # and the named-entry path
            if os.path.exists(_mok):
                esp_files[_base + "/mmx64.efi"] = _mok   # MokManager, to (re)enrol
        if os.path.exists(_cer):
            esp_files["/KESTREL.CER"] = _cer             # the key to enrol
        say("-> Secure Boot chain: shim at BOOTX64.EFI, loader at grubx64.efi", 1)
    else:
        esp_files["/EFI/BOOT/BOOTX64.EFI"] = efi
        esp_files["/EFI/KESTREL/BOOTX64.EFI"] = efi

    # The matching NVIDIA RM core and GSP firmware are intentionally large.
    # Size the ESP from the actual payload (plus 25% and 32 MiB slack), rounded
    # to a 64 MiB boundary, so a driver update cannot silently overflow the
    # historical fixed 128 MiB partition.
    _esp_payload = sum(os.path.getsize(src) for src in esp_files.values())
    _esp_need = (_esp_payload * 5 // 4) + 32 * 1024 * 1024
    _esp_mb = max(128, (_esp_need + 64 * 1024 * 1024 - 1) // (64 * 1024 * 1024) * 64)
    say("-> ESP capacity %d MiB for %.1f MiB payload" %
        (_esp_mb, _esp_payload / (1024 * 1024)), 1)

    img = os.path.join(OUT, "kestrelos.img")
    mkimage.build_disk(img, esp_files, esp_mb=_esp_mb, data_mb=64)
    say("-> kestrelos.img (%d MiB)" % (os.path.getsize(img) // (1024 * 1024)), 1)

    vmdk = os.path.join(OUT, "kestrelos.vmdk")
    mkimage.write_vmdk_descriptor(vmdk, img)
    say("-> kestrelos.vmdk", 1)

    iso = os.path.join(OUT, "kestrelos.iso")
    mkimage.build_iso(iso, esp_files, esp_mb=_esp_mb)
    say("-> kestrelos.iso (%d MiB)" % (os.path.getsize(iso) // (1024 * 1024)), 1)

    # ---- windows installer -------------------------------------------
    windir = os.path.join(OUT, "windows")
    if os.path.isdir(windir):
        shutil.rmtree(windir)
    mkdirs(windir, os.path.join(windir, "payload"))
    src = os.path.join(ROOT, "windows")
    if os.path.isdir(src):
        for f in os.listdir(src):
            shutil.copy2(os.path.join(src, f), windir)
    for dst, s in esp_files.items():
        target = os.path.join(windir, "payload", dst.strip("/").replace("/", os.sep))
        mkdirs(os.path.dirname(target))
        shutil.copy2(s, target)
    say("-> windows/ installer", 1)

    # The icon set as a font, staged for the system volume.
    #
    # After the installer, not before: that step clears out/windows and rebuilds
    # it, so anything staged there earlier is gone by the time it finishes.
    #
    # It is not needed to run - the interface draws its icons from primitives
    # and always has, which is why they come out right at any size.  This is the
    # same set written out as something another machine can install, and it goes
    # on the system volume rather than into the binary because that is what
    # makes it replaceable.
    build_icon_font()


def clean():
    if os.path.isdir(BUILD):
        shutil.rmtree(BUILD)
    # nvidia-open-595.99.02 is a checked-out vendor source dependency, not a
    # generated artifact, despite predating this integration under out/.  A
    # normal clean must never delete the only RM ABI headers used by NVKMS.
    if os.path.isdir(OUT):
        for name in os.listdir(OUT):
            if name == "nvidia-open-595.99.02":
                continue
            path = os.path.join(OUT, name)
            if os.path.isdir(path):
                shutil.rmtree(path)
            else:
                os.remove(path)
    say("cleaned")


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "all"
    if target == "clean":
        clean()
        return

    mkdirs(BUILD, OUT)
    if target in ("all", "boot"):
        efi = build_boot()
    if target in ("all", "kernel"):
        kernel = build_kernel()
    if target in ("all", "user"):
        programs = build_user()
    if target == "all":
        win = build_win_tests()
        build_images(efi, kernel, programs, win)
        say("build complete -> %s" % OUT)


if __name__ == "__main__":
    main()
