#!/usr/bin/env python3
"""Decode an owned Windows NVENC artifact on the host CPU, never native proof."""
from pathlib import Path
import argparse
import hashlib
import json
import struct
import av
from verify_nvenc_live_capture import verify


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    directory=parser.parse_args().directory
    verify(directory)
    manifest=json.loads((directory/'manifest.json').read_text())
    assert manifest['config']['input_pattern']=='neutral-128'
    assert manifest['config']['qp']==26 and manifest['hardware']['avg_qp']==26
    assert (directory/'input.nv12').read_bytes()==bytes([128])*98304
    checked=0
    for name,digest in manifest['picture_capture']['sha256'].items():
        blob=(directory/name).read_bytes()
        assert hashlib.sha256(blob).hexdigest()==digest
        if name.endswith('-md.bin'):
            assert len(blob)==128 and not struct.unpack_from('<I',blob,52)[0] & (1<<30)
            checked+=1
    assert checked, 'No captured mode-decision record proves normal encoding'
    with av.open(str(directory/'frame.h264'),format='h264') as container:
        frames=list(container.decode(video=0))
    assert len(frames)==1
    frame=frames[0]
    assert (frame.width,frame.height,frame.format.name)==(256,256,'yuv420p')
    planes=[]
    for plane in frame.planes:
        raw=bytes(plane)
        planes.append(b''.join(raw[y*plane.line_size:y*plane.line_size+plane.width]
                               for y in range(plane.height)))
    assert [len(p) for p in planes]==[65536,16384,16384]
    assert all(p==bytes([128])*len(p) for p in planes)
    print(json.dumps({'kind':'Windows NVENC output decoded on host CPU; NOT native Kestrel/NVDEC proof',
                      'av_version':av.__version__, 'plane_bytes':list(map(len,planes)),
                      'all_samples_128':True,
                      'decoded_sha256':hashlib.sha256(b''.join(planes)).hexdigest()}))


if __name__=='__main__':
    main()
