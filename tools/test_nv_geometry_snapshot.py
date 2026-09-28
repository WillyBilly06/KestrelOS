#!/usr/bin/env python3
"""Exercise production snapshot allocation/layout on CPU; no GPU emulation.

Also guards the source ordering of retirement and semantic validation. This
does not establish GPU completion, image correctness, or a frame-time gain.
"""
from pathlib import Path
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]
SRC = (ROOT / 'kernel/nv_chan.c').read_text()
BODY = function(SRC, 'nv_surface_shader_geometry')


def main():
    allocation = BODY[BODY.index('    u32 metadata_bytes='):BODY.index('    nv_setup_window_t *plan=')]
    readback = '!nv_surface_transfer(owner,submission->workspace,storage.rects,metadata,metadata_bytes,true)'
    assert BODY.count(readback) == 1
    assert BODY.index('!nv_compute_geometry_pair(ch,setup_args,compact_args,triangles)') < BODY.index(readback)
    pair = function(SRC, 'nv_compute_geometry_pair')
    assert pair.index('shader_setup_sass,sizeof shader_setup_sass') < pair.index('"surface/geometry-compact"')
    assert 'submit_and_wait(ch,pair.start,pair.signal)' in pair
    assert BODY.index(readback) < BODY.index('total=compact[0].total;')
    assert BODY.index('if(prefix!=total)goto done;') < BODY.index('nv_setup_rect_valid(&rects[i],c)')
    assert BODY.index('nv_setup_rect_valid(&rects[i],c)') < BODY.index('nv_setup_plan(rects,total')
    assert 'storage.compact,compact,compact_bytes,true' not in BODY
    assert 'kfree(metadata);' in BODY and 'kfree(rects)' not in BODY and 'kfree(compact)' not in BODY
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef uint32_t u32; typedef uint8_t u8;
static bool allocation_fails;
static void *kmalloc(size_t bytes) { return allocation_fails?NULL:malloc(bytes); }
'''
    source += '#include "' + (ROOT/'include/kestrel/shader_setup.h').as_posix() + '"\n'
    source += r'''
int main(void) {
    unsigned maximum=0;
    for(unsigned triangles=1;triangles<=KSHS_MAX_TRIANGLES;triangles++) {
        kshs_storage_layout_t storage;
        assert(kshs_storage_layout(triangles,&storage));
        u32 compact_bytes=(u32)KSHS_COMPACT_STATUS_BYTES(triangles);
        for(unsigned fail=0;fail<2;fail++) {
            allocation_fails=fail;
'''
    source += allocation
    source += r'''
            assert(storage.compact>=storage.rects);
            assert(storage.compact+compact_bytes<=storage.bytes);
            assert(metadata_bytes<193u*1024u && metadata_bytes<=4u*1024u*1024u);
            assert(!(metadata_bytes&3u));
            assert(storage.compact-storage.rects>=KSHS_COMPACT_RECT_BYTES(triangles*KSHS_OUTPUT_TRIANGLES));
            if(fail) { assert(!metadata && !compact && !rects);continue; }
            memset(metadata,0xa5,metadata_bytes);
            for(unsigned i=0;i<triangles*KSHS_OUTPUT_TRIANGLES;i++) rects[i]=(kshs_rect_t){1,2,3,4};
            for(unsigned i=0;i<compact_bytes;i++) assert(((u8 *)compact)[i]==0xa5);
            for(unsigned i=0;i<triangles;i++) compact[i]=(kshs_compact_status_t){KSH_COMPLETE,1,i,triangles};
            for(unsigned i=0;i<triangles*KSHS_OUTPUT_TRIANGLES;i++) assert(rects[i].width==3 && rects[i].height==4);
            for(size_t i=KSHS_COMPACT_RECT_BYTES(triangles*KSHS_OUTPUT_TRIANGLES);
                i<storage.compact-storage.rects;i++) assert(((u8 *)metadata)[i]==0xa5);
            if(metadata_bytes>maximum)maximum=metadata_bytes;
            free(metadata);
        }
    }
    printf("PASS 1365 production snapshot layouts, allocation failures and disjoint views; maximum %u bytes\n",maximum);
}
'''
    run_test(source,'nv_geometry_snapshot')


if __name__ == '__main__':
    main()
