#!/usr/bin/env python3
"""Package the verified GPU-test binaries for a different x86-64 UEFI PC.

No compilation, codec execution, VM, physical disk access or firmware changes.
The original dedicated image/references are verified and left untouched.
"""
from pathlib import Path
import hashlib
import json
import runpy
import shutil
import struct
import tempfile
import zipfile
import zlib

import mkimage
import mkkar

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / 'out/cross-system-trial'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def kar_entries(blob):
    if len(blob) < 24:
        raise ValueError('short KAR header')
    magic, count, total, table, data_start = struct.unpack_from('<IIQII', blob)
    if magic != mkkar.MAGIC or total != len(blob) or table < 24 or \
            table + count * 128 > data_start or data_start > total:
        raise ValueError('invalid KAR bounds')
    files, names = {}, set()
    for i in range(count):
        entry = blob[table + i * 128:table + (i + 1) * 128]
        name = entry[:100].split(b'\0', 1)[0].decode('utf-8')
        parts = name.split('/')[1:]
        if not name.startswith('/') or not parts or any(p in ('', '.', '..') for p in parts) or '\\' in name or ':' in name or name in names:
            raise ValueError('unsafe/duplicate KAR name: ' + name)
        names.add(name)
        kind, mode, size, offset = struct.unpack_from('<II4xQQ', entry, 100)
        if kind == mkkar.KAR_FILE:
            if offset < data_start or offset + size > total:
                raise ValueError('KAR file outside data area')
            files[name] = (mode, blob[offset:offset + size])
        elif kind != mkkar.KAR_DIR or size or offset:
            raise ValueError('invalid KAR entry')
    return files


def main():
    if OUTPUT.exists():
        raise SystemExit('Refusing to overwrite existing trial package: ' + str(OUTPUT))
    # This verifier intentionally executes first: source image, references,
    # boot config, GSP and owned EDID hashes must all agree before repackaging.
    source = runpy.run_path(str(ROOT / 'tools/verify_gputest_image.py'))
    fat = source['fat']
    original_files = kar_entries(source['initrd'])
    excluded = [p for p in original_files if p.startswith('/lib/firmware/edid/') or p == '/etc/startapp']
    files = {p: value for p, value in original_files.items() if p not in excluded}
    original_image_hash = sha(source['raw'])
    OUTPUT.mkdir(parents=True)
    with tempfile.TemporaryDirectory(prefix='kestrel-cross-system-') as temp:
        staging = Path(temp).resolve()

        def stage(name, data):
            target = (staging / name.lstrip('/')).resolve()
            if not target.is_relative_to(staging) or target == staging:
                raise ValueError('staging path escapes temporary directory')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            return str(target)

        tree = {p: stage('/kar' + p, data) for p, (_, data) in files.items()}
        trial_initrd = staging / 'trial.kar'
        mkkar.build(tree, trial_initrd)
        packed_files = kar_entries(trial_initrd.read_bytes())
        if packed_files != files:
            raise ValueError('trial KAR changed a retained file or its permissions')

        esp_files, visited = {}, set()

        def walk(cluster, parent=''):
            if cluster in visited:
                raise ValueError('duplicate FAT directory cluster')
            visited.add(cluster)
            for name, (attr, child, _) in fat.entries(cluster).items():
                if name in ('.', '..'):
                    continue
                path = parent + '/' + name
                if attr & 0x10:
                    walk(child, path)
                else:
                    esp_files[path] = fat.read(path)

        walk(fat.root_cluster)
        # Never redistribute persisted trial logs or captured rendered pixels.
        if any(p.endswith(('.LOG', '.RAW')) for p in esp_files):
            raise ValueError('source image contains runtime logs/captures')
        esp_files['/KESTREL/INITRD.KAR'] = trial_initrd.read_bytes()
        esp_files['/KESTREL/BOOT.CFG'] = (
            '# Cross-system trial: optional GPU tests remain in the menu\n'
            'timeout=8\nkernel=\\KESTREL\\KERNEL.ELF\n'
            'initrd=\\KESTREL\\INITRD.KAR\ncmdline=\n'
            'default=main\ndisplaymode=extend\n').encode()
        staged_esp = {p: stage('/esp' + p, data) for p, data in esp_files.items()}
        image = OUTPUT / 'kestrelos-trial.img'
        layout = mkimage.build_disk(str(image), staged_esp, esp_mb=len(fat.v) // (1024 * 1024), data_mb=64)
        raw = image.read_bytes()
        # Verify both GPT copies, not only the builder's input dictionary.
        for lba, other in ((1, len(raw) // 512 - 1), (len(raw) // 512 - 1, 1)):
            sector = raw[lba * 512:(lba + 1) * 512]
            if sector[:8] != b'EFI PART':
                raise ValueError('missing GPT')
            size = struct.unpack_from('<I', sector, 12)[0]
            header = bytearray(sector[:size])
            expected_crc = struct.unpack_from('<I', header, 16)[0]
            struct.pack_into('<I', header, 16, 0)
            if not 92 <= size <= 512 or zlib.crc32(header) & 0xffffffff != expected_crc:
                raise ValueError('GPT header CRC')
            current, alternate = struct.unpack_from('<QQ', header, 24)
            entries_lba, count, stride, crc = struct.unpack_from('<QIII', header, 72)
            table = raw[entries_lba * 512:entries_lba * 512 + count * stride]
            if (current, alternate) != (lba, other) or zlib.crc32(table) & 0xffffffff != crc:
                raise ValueError('GPT location/table CRC')
        start = layout['esp_start'] * 512
        reader = source['FatReader'](raw[start:start + layout['esp_sectors'] * 512])
        cfg = reader.read('/KESTREL/BOOT.CFG')
        cfg_lines = cfg.splitlines()
        for required in (b'default=main', b'cmdline=', b'displaymode=extend', b'timeout=8'):
            if required not in cfg_lines:
                raise ValueError('trial boot configuration mismatch')
        for p, data in esp_files.items():
            if p != '/KESTREL/BOOT.CFG' and reader.read(p) != data:
                raise ValueError('embedded ESP mismatch: ' + p)
        if kar_entries(reader.read('/KESTREL/INITRD.KAR')) != files:
            raise ValueError('embedded trial archive mismatch')
        if sha((ROOT / 'out/kestrelos-gputest.img').read_bytes()) != original_image_hash:
            raise ValueError('original GPU-test image changed during packaging')

    manifest = {
        'image': image.name, 'image_sha256': sha(raw), 'image_bytes': len(raw),
        'source_gpu_test_sha256': original_image_hash,
        'kernel_sha256': sha(source['kernel']), 'loader_sha256': sha(source['second_stage']),
        'excluded_initrd_files': excluded, 'default': 'main',
        'hardware_validated_on_second_pc': False, 'rufus_end_to_end_validated': False,
        'checks': ['primary/backup GPT CRC', 'embedded ESP byte comparison',
                   'retained KAR file content/mode equality', 'no EDID overrides/autostart',
                   'source GPU-test image unchanged'],
    }
    (OUTPUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (OUTPUT / 'SHA256SUMS.txt').write_text(manifest['image_sha256'] + '  ' + image.name + '\n')
    shutil.copyfile(ROOT / 'docs/cross-system-trial.md', OUTPUT / 'README.md')
    fallback = OUTPUT / 'writer'
    fallback.mkdir()
    for name in ('writeusb.ps1', 'expandgpt.py'):
        shutil.copyfile(ROOT / 'tools' / name, fallback / name)
    bundle = OUTPUT / 'kestrelos-trial.zip'
    with zipfile.ZipFile(bundle, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for item in sorted(OUTPUT.rglob('*')):
            if item.is_file() and item != bundle:
                archive.write(item, item.relative_to(OUTPUT).as_posix())
    print('TRIAL_IMAGE_INTEGRITY_PASS (not hardware or Rufus execution validation)')
    print(json.dumps(manifest, indent=2))
    print('Share package: ' + str(bundle))


if __name__ == '__main__':
    main()
