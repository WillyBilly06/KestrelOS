#!/usr/bin/env python3
"""Validate retained Windows-only NVENC observations; never a native success gate."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def verify(directory):
    manifest = json.loads((directory / 'manifest.json').read_text())
    assert manifest['kind'].startswith('Windows NVIDIA NVENC reference; NOT native')
    assert manifest['hardware']['hw_status'] == 2
    assert manifest['hardware']['bytes'] == (directory / 'frame.h264').stat().st_size
    assert manifest['parser_status'] == 0
    hashes = manifest['live_method_capture']['sha256']
    for name, digest in {**manifest['sha256'], **hashes}.items():
        assert Path(name).name == name
        assert hashlib.sha256((directory / name).read_bytes()).hexdigest() == digest, name
    stream = json.loads((directory / 'live-encoder-tokens.json').read_text())
    assert stream['dropped'] == 0 and len(stream['batches']) == 1
    batch = stream['batches'][0]
    assert batch['execute_data'] == 0
    assert batch['original_count'] == len(batch['tokens'])
    controls = {t['method']: t['data'] for t in batch['tokens'] if t['type'] == 0}
    assert controls[0x200] == 1 and controls[0x700] & 15 == 3 and controls[0x704] == 0
    bindings = [t for t in batch['tokens'] if t['type'] == 4]
    methods = {t['method'] for t in bindings}
    assert {0x70c, 0x710, 0x718, 0x71c, 0x720, 0x724, 0x72c,
            0x730, 0x734, 0x740, 0x744, 0x74c} <= methods
    for token in bindings:
        assert token['address_metadata_valid'] and int(token['gpu_va']) != 0
        assert 'extent' not in token, 'Old mislabeled slot38 value is not an allocation bound'
    records = [directory / f'rc-initializer-{i}.bin' for i in range(2)]
    assert all(p.name in hashes for p in records)
    blobs = [p.read_bytes() for p in records]
    assert len(blobs[0]) == 256 and blobs[0] == blobs[1]
    words = struct.unpack('<64I', blobs[0])
    # Linux FUN_001c8de0 / Windows FUN_18011fa00. Dynamic fields 0,1,14,15,16
    # deliberately are not equated to defaults or transplanted into Kestrel.
    assert words[2:14] == (24, 12, 48, 24, 24, 24, 12, 12, 12, 48, 48, 48)
    assert words[17:20] == (0, 0, 0) and words[20:22] == (256, 26)
    assert not any(words[22:])
    for name,digest in manifest['picture_capture']['sha256'].items():
        if name.startswith('cfb7-picture-') and name.count('-')==2:
            picture=(directory/name).read_bytes()
            assert hashlib.sha256(picture).hexdigest()==digest
            assert len(picture)==768 and picture[0xba]==1
    print('WINDOWS_NVENC_CAPTURE_PASS: completed H264, retained bindings, two source-matching RC records; NOT native Kestrel proof')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    verify(parser.parse_args().directory)
