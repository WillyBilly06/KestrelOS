#!/usr/bin/env python3
"""Boot the actual kernel's user-thread test on emulated CPUs only.

Builds a private minimal initrd/GPT image in a unique out/smp-qemu-* directory.
No release image, USB, GPU passthrough, NVIDIA firmware, or codecs are used.
A pass requires completed user workloads on multiple APIC IDs, not core-online
messages. The supplied kernel is copied; build it first with build.py kernel.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tools'))
import build
import mkimage
import mkkar
import mkroots


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cpus', type=int, default=4)
    p.add_argument('--seconds', type=int, default=180)
    p.add_argument('--kernel', type=Path, default=ROOT/'build/kernel/kernel.elf')
    p.add_argument('--prepare-only', action='store_true')
    args = p.parse_args()
    if not 4 <= args.cpus <= 32 or not 10 <= args.seconds <= 600:
        p.error('cpus must be 4..32 and seconds 10..600')
    qemu = Path('C:/Program Files/qemu')
    run = Path(tempfile.mkdtemp(prefix='smp-qemu-', dir=ROOT/'out'))
    print('CPU-only VM artifacts:', run, flush=True)
    for source, name in [(qemu/'share/edk2-x86_64-code.fd', 'code.fd'),
                         (qemu/'share/edk2-i386-vars.fd', 'vars.fd'),
                         (args.kernel.resolve(), 'kernel.elf'),
                         (ROOT/'build/boot/BOOTX64.EFI', 'BOOTX64.EFI')]:
        shutil.copyfile(source, run/name)
    libc = ROOT/'user/libc'
    objects = build.compile_all(build.sources_in(str(libc)), str(run/'libc'),
                                build.USER_CFLAGS,
                                build.headers_in(str(libc), str(ROOT/'include/kestrel')))
    guest = run/'init.elf'
    subprocess.run([build.CLANG, *build.USER_CFLAGS, '-c',
                    str(ROOT/'tests/smp/user_parallel.c'), '-o', str(run/'test.o')], check=True)
    subprocess.run([build.LD_LLD, '-T', str(libc/'user.ld'), '-nostdlib',
                    '--gc-sections', '-o', str(guest), str(run/'test.o'), *objects], check=True)
    build.stamp_kestrel(str(guest))
    linux = run/'threads.linux'
    subprocess.run([build.CLANG, '--target=x86_64-unknown-linux-gnu',
                    '-ffreestanding', '-fno-stack-protector', '-fno-pie', '-O2',
                    '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,-e,_start',
                    '-Wl,--image-base=0x400000', '-o', str(linux),
                    str(ROOT/'tests/linux/threads.c')], check=True)
    # Carry the normal trust-list fixture so CPU boot self-tests don't report
    # an unrelated missing-file error. No network device is attached.
    mkroots.build(str(ROOT/'data/cacert.pem'), str(run/'roots.bin'))
    mkkar.build({'/bin/init':str(guest), '/bin/threads.linux':str(linux),
                '/etc/ssl/roots.bin':str(run/'roots.bin')},
                str(run/'initrd.kar'))
    (run/'BOOT.CFG').write_text('timeout=0\nkernel=\\KESTREL\\KERNEL.ELF\n'
                               'initrd=\\KESTREL\\INITRD.KAR\ncmdline=nogpu\n')
    mkimage.build_disk(str(run/'disk.img'), {
        '/EFI/BOOT/BOOTX64.EFI':str(run/'BOOTX64.EFI'),
        '/KESTREL/KERNEL.ELF':str(run/'kernel.elf'),
        '/KESTREL/INITRD.KAR':str(run/'initrd.kar'),
        '/KESTREL/BOOT.CFG':str(run/'BOOT.CFG')}, esp_mb=128, data_mb=16)
    command = [str(qemu/'qemu-system-x86_64.exe'), '-machine', 'q35',
               '-accel', 'tcg,thread=multi', '-cpu', 'max', '-smp', str(args.cpus),
               '-m', '2048', '-display', 'none', '-nic', 'none', '-no-reboot',
               '-device', 'VGA,xres=1024,yres=768',
               '-drive', f'if=pflash,format=raw,readonly=on,file={run / "code.fd"}',
               '-drive', f'if=pflash,format=raw,file={run / "vars.fd"}',
               '-serial', f'file:{run / "serial.log"}',
               '-drive', f'if=none,id=nvm,format=raw,file={run / "disk.img"}',
               '-device', 'nvme,drive=nvm,serial=smptest']
    manifest = {'scope':'CPU-only QEMU; no GPU or codecs', 'command':command,
                'kernel_sha256':hashlib.sha256((run/'kernel.elf').read_bytes()).hexdigest(),
                'guest_sha256':hashlib.sha256(guest.read_bytes()).hexdigest()}
    (run/'manifest.json').write_text(json.dumps(manifest, indent=2))
    if args.prepare_only:
        print('CPU-only image prepared; VM not started', flush=True)
        return
    result = 'TIMEOUT'
    with (run/'qemu.log').open('w') as output:
        child = subprocess.Popen(command, stdout=output, stderr=output,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            for elapsed in range(args.seconds):
                serial = (run/'serial.log').read_text(errors='replace') if (run/'serial.log').exists() else ''
                if 'SMP_QEMU_FAIL' in serial or 'PANIC' in serial:
                    result = 'FAIL'
                    break
                if 'SMP_QEMU_PASS:' in serial:
                    result = 'PASS'
                    break
                if child.poll() is not None:
                    result = f'QEMU_EXIT_{child.returncode}'
                    break
                if elapsed % 15 == 0:
                    print(f'CPU-only VM running: {elapsed}s', flush=True)
                time.sleep(1)
        finally:
            if child.poll() is None:
                child.terminate()
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=10)
    (run/'result.json').write_text(json.dumps({'result':result}, indent=2))
    print(f'CPU-only scheduler integration: {result}; {run}', flush=True)
    if result != 'PASS':
        raise SystemExit(1)


if __name__ == '__main__':
    main()
