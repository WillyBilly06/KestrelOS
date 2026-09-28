#!/usr/bin/env python3
"""nvshader.py - compile a shader to real Blackwell (sm_120) machine code and
emit it as an OS-embeddable, verified asset.

This is the shader-supply half of native NVIDIA 3D/compute on the user's RTX
5070 Ti (GB203, sm_120).  It was long assumed the Blackwell instruction set was
unpublished and so shaders "could not be produced"; that is false.  NVIDIA's
own ptxas compiles to sm_120 and cuobjdump/nvdisasm disassemble it, and both
ship in the CUDA toolkit already installed on this machine.  So a shader the
OS will later submit through the GSP-booted card can be produced and CHECKED
here, exactly the way Mesa's NAK compiler validates its own sm_120 encodings
against nvdisasm.

What it does:
  1. nvcc -arch=sm_120 -cubin : source -> a real Blackwell cubin (ELF64).
  2. Parse the ELF, read the `.text.<kernel>` section : the raw SASS bytes.
  3. cuobjdump -res-usage      : registers / shared / constant-bank sizes,
                                 the launch descriptor a compute dispatch needs.
  4. Emit a C header embedding the bytes + metadata for the kernel to submit.
  5. VERIFY: the extracted bytes, disassembled, must match cuobjdump -sass of
     the cubin instruction-for-instruction.  A mismatch fails the build rather
     than shipping a shader that was mis-extracted.

Nothing here talks to the card; it is a build-time asset step.  Submitting the
asset is the OS's job once the GSP is up (see kestrelos-gpu-two-walls).
"""
import os, re, struct, subprocess, sys, tempfile

CUDA = os.environ.get(
    "CUDA_BIN",
    r"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3\bin")


def tool(name):
    p = os.path.join(CUDA, name + ".exe")
    return p if os.path.exists(p) else name


def find_cl():
    import glob
    base = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC"
    hits = glob.glob(os.path.join(base, "*", "bin", "Hostx64", "x64", "cl.exe"))
    return os.path.dirname(hits[0]) if hits else None


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.stderr.write("command failed: %s\n%s\n%s\n" %
                         (" ".join(cmd), r.stdout, r.stderr))
        raise SystemExit(1)
    return r.stdout


def compile_cubin(src, arch, out):
    env = dict(os.environ)
    cl = find_cl()
    if cl:
        env["PATH"] = cl + os.pathsep + env.get("PATH", "")
    subprocess.run([tool("nvcc"), "-arch=" + arch, "-cubin", "-o", out, src],
                   check=True, env=env)


# --- a very small ELF64 reader, just enough to pull one section's bytes ------

def elf_section(data, want_prefix):
    assert data[:4] == b"\x7fELF", "not an ELF (cubin)"
    is64 = data[4] == 2
    assert is64, "expected ELF64 cubin"
    (e_shoff,) = struct.unpack_from("<Q", data, 0x28)
    (e_shentsize,) = struct.unpack_from("<H", data, 0x3a)
    (e_shnum,) = struct.unpack_from("<H", data, 0x3c)
    (e_shstrndx,) = struct.unpack_from("<H", data, 0x3e)

    def sh(i):
        off = e_shoff + i * e_shentsize
        name, _typ, _flags, _addr, s_off, s_size = struct.unpack_from(
            "<IIQQQQ", data, off)
        return name, s_off, s_size

    str_name, str_off, str_size = sh(e_shstrndx)
    strtab = data[str_off:str_off + str_size]

    def cstr(o):
        end = strtab.find(b"\0", o)
        return strtab[o:end].decode("ascii", "replace")

    for i in range(e_shnum):
        name, s_off, s_size = sh(i)
        nm = cstr(name)
        if nm.startswith(want_prefix):
            return nm, data[s_off:s_off + s_size]
    raise SystemExit("section %s* not found in cubin" % want_prefix)


def sass_encodings(cubin, kernel):
    """The 64-bit words cuobjdump -sass prints per instruction, in order."""
    out = run([tool("cuobjdump"), "-sass", cubin])
    words = []
    for m in re.finditer(r"/\*\s*(0x[0-9a-fA-F]{16})\s*\*/", out):
        words.append(int(m.group(1), 16))
    return words


def res_usage(cubin, kernel):
    out = run([tool("cuobjdump"), "-res-usage", cubin])
    reg = shared = const = 0
    for line in out.splitlines():
        if "REG:" in line and ("Function %s" % kernel in out):
            m = re.search(r"REG:(\d+).*SHARED:(\d+).*CONSTANT\[0\]:(\d+)", line)
            if m:
                reg, shared, const = (int(m.group(1)), int(m.group(2)),
                                      int(m.group(3)))
    return reg, shared, const


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("kernel", help="entry name, e.g. addvec")
    ap.add_argument("-a", "--arch", default="sm_120")
    ap.add_argument("-o", "--out", default=None, help="C header to write")
    ap.add_argument("--native", action="store_true",
                    help="require zero local/shared/stack/global storage and only constant banks 0/2")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as d:
        cubin = os.path.join(d, "k.cubin")
        compile_cubin(args.src, args.arch, cubin)
        data = open(cubin, "rb").read()
        sec, text = elf_section(data, ".text." + args.kernel)
        reg, shared, const = res_usage(cubin, args.kernel)
        usage = run([tool("cuobjdump"), "-res-usage", cubin])
        bank2_match = re.search(r"CONSTANT\[2\]:(\d+)", usage)
        bank2_size = int(bank2_match[1]) if bank2_match else 0
        bank2 = b""
        if bank2_size:
            bank2_name, bank2 = elf_section(data, ".nv.constant2." + args.kernel)
            if bank2_name != ".nv.constant2." + args.kernel or len(bank2) != bank2_size:
                raise SystemExit("constant bank 2 section does not match resource metadata")
            dump = run([tool("cuobjdump"), "-elf", cubin])
            block = re.search(r"(?m)^\.nv.constant2\." + re.escape(args.kernel) +
                              r"\s*\n((?:0x[^\n]*\n)+)", dump)
            dumped_words = [] if not block else [
                int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]{8}", block[1])]
            if len(bank2) % 4 or list(struct.unpack("<%dI" % (len(bank2)//4), bank2)) != dumped_words:
                raise SystemExit("constant bank 2 bytes do not match cuobjdump -elf")
        if args.native:
            for field in ("GLOBAL", "STACK", "SHARED", "LOCAL"):
                values = re.findall(r"\b" + field + r":(\d+)", usage)
                if not values or any(int(v) for v in values):
                    raise SystemExit("native launcher cannot supply " + field + " storage")
            banks = re.findall(r"CONSTANT\[(\d+)\]:(\d+)", usage)
            if any(int(n) not in (0, 2) and int(size) for n, size in banks):
                raise SystemExit("native launcher supports only constant banks 0 and 2")
            if const > 0x400 or bank2_size > 0x200:
                raise SystemExit("native constant bank exceeds its reserved slot")
            # Executable code / branch-table relocations would require a loader.
            # Ignore debug and Mercury intermediate-code metadata relocations.
            for prefix in (".rel.text." + args.kernel, ".rela.text." + args.kernel,
                           ".rel.nv.constant2." + args.kernel, ".rela.nv.constant2." + args.kernel):
                try:
                    _, relocation = elf_section(data, prefix)
                except SystemExit:
                    continue
                if relocation:
                    raise SystemExit("unresolved native shader relocations: " + prefix)

        # VERIFY: the section bytes, read as little-endian 64-bit words, must
        # equal the words cuobjdump disassembled.  Same bytes, same order.
        words = struct.unpack_from("<%dQ" % (len(text) // 8), text, 0)
        ref = sass_encodings(cubin, args.kernel)
        if list(words) != ref:
            sys.stderr.write(
                "VERIFY FAILED: extracted %d words, cuobjdump saw %d; "
                "first diff at %s\n" % (
                    len(words), len(ref),
                    next((i for i, (a, b) in enumerate(zip(words, ref))
                          if a != b), "none")))
            raise SystemExit(1)

        print("compiled %s:%s for %s" % (args.src, args.kernel, args.arch))
        print("  section %s: %d bytes, %d instructions" %
              (sec, len(text), len(text) // 16))
        print("  launch: REG=%d SHARED=%d CONST0=%d" % (reg, shared, const))
        print("  constant bank 2: %d bytes" % bank2_size)
        print("  VERIFIED: extracted SASS == cuobjdump -sass, %d words" %
              len(words))

        if args.out:
            name = args.kernel
            with open(args.out, "w") as f:
                f.write("/* Generated by tools/nvshader.py from %s.\n"
                        " * Real %s (Blackwell) machine code for %s, verified\n"
                        " * against cuobjdump -sass.  Do not edit by hand. */\n"
                        % (os.path.basename(args.src), args.arch, name))
                f.write("#ifndef KESTREL_SHADER_%s_H\n" % name.upper())
                f.write("#define KESTREL_SHADER_%s_H\n\n" % name.upper())
                f.write("static const unsigned int %s_sass_reg_count = %d;\n"
                        % (name, reg))
                f.write("static const unsigned int %s_sass_shared = %d;\n"
                        % (name, shared))
                f.write("static const unsigned int %s_sass_const0 = %d;\n\n"
                        % (name, const))
                f.write("static const unsigned char %s_sass[%d] = {\n"
                        % (name, len(text)))
                for i in range(0, len(text), 12):
                    f.write("    " + " ".join("0x%02x," % b
                                              for b in text[i:i + 12]) + "\n")
                f.write("};\n\n")
                # Bank 2 contains compiler-owned data, e.g. switch branch
                # tables; instruction-byte verification alone does not supply it.
                # Keep the arrays inside the same include guard.
                if bank2:
                    f.write("static const unsigned char %s_sass_const2[%d] = {\n" % (name, len(bank2)))
                    for i in range(0, len(bank2), 12):
                        f.write("    " + " ".join("0x%02x," % b for b in bank2[i:i+12]) + "\n")
                    f.write("};\n\n")
                f.write("#endif\n")
            print("  wrote %s (%d bytes of SASS)" % (args.out, len(text)))


if __name__ == "__main__":
    main()
