#!/usr/bin/env python3
"""Check actual SPS/PPS -> NVDEC picture mapping; no simulated GPU success."""
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_h264_decode import stream

ROOT = Path(__file__).resolve().parents[1]


def main():
    code = r'''
#include <stddef.h>
extern int printf(const char *, ...);
extern int memcmp(const void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern void abort(void);
void __main(void) {}
#define assert(x) ((x)?(void)0:(printf("line %d: %s\n",__LINE__,#x),abort()))
'''
    code += '#include "' + (ROOT / 'kernel/nvdec_h264_context.h').as_posix() + '"\n'
    cases = [dict(w=64, h=64), dict(w=256, h=256),
             dict(w=320, h=240, crop=(1, 2, 3, 4), qp=-12, chroma=-8,
                  second_chroma=7, direct=1, fbits=9, pbits=12,
                  bottom=1, poc_lsb=351, delta_bottom=-6, redundant=1),
             dict(w=4096, h=4096, poc=2, qp=25, delta_qp=-25),
             dict(w=48, h=64)]
    for i, params in enumerate(cases):
        data, p, _, _ = stream(**params)
        code += f'static const unsigned char input{i}[]={{' + ','.join(map(str, data)) + '};\n'
    code += r'''
int main(void) {
    nvdec_h264_pic_s pic, zero={0}; kh264_decode_desc desc, dzero={0};
'''
    for i, params in enumerate(cases):
        _, p, _, _ = stream(**params)
        pitch = (p['w'] + 63) & ~63
        code += f'''
    assert(nvdec_h264_parse_pic(&pic,&desc,input{i},sizeof input{i},{pitch},{pitch},0xc00,0x200)==KH264_OK);
    assert(pic.PicWidthInMbs=={p['w']//16} && pic.FrameHeightInMbs=={p['h']//16});
    assert(pic.pitch_luma=={pitch} && pic.pitch_chroma=={pitch});
    assert(pic.luma_bot_offset=={p['w']} && pic.chroma_bot_offset=={pitch//2});
    assert(pic.stream_len==sizeof input{i}+16 && pic.slice_count==1);
    assert(pic.pic_order_cnt_type=={p['poc']} && pic.log2_max_frame_num_minus4=={p['fbits']-4});
    assert(pic.log2_max_pic_order_cnt_lsb_minus4=={p['pbits']-4 if p['poc']==0 else 0});
    assert(pic.pic_init_qp_minus26=={p['qp']} && pic.chroma_qp_index_offset=={p['chroma']});
    assert(pic.second_chroma_qp_index_offset=={p['chroma'] if p['second_chroma'] is None else p['second_chroma']});
    assert(pic.pic_order_present_flag=={p['bottom']} && pic.redundant_pic_cnt_present_flag=={p['redundant']});
    assert(pic.deblocking_filter_control_present_flag=={p['deblock']} && pic.direct_8x8_inference_flag=={p['direct']});
    assert(pic.CurrFieldOrderCnt[0]=={p['poc_lsb']} && pic.CurrFieldOrderCnt[1]=={p['poc_lsb']+p['delta_bottom']});
    assert(desc.display_width=={p['w']-2*(p['crop'][0]+p['crop'][1])});
    assert(desc.display_height=={p['h']-2*(p['crop'][2]+p['crop'][3])});
    assert(pic.tileFormat==1 && pic.gob_height==0 && pic.HistBufferSize==12);
    assert(pic.ref_pic_flag==1 && pic.frame_mbs_only_flag==1 && pic.chroma_format_idc==1);
    assert(!memcmp(pic.dpb,zero.dpb,sizeof pic.dpb));
    for(unsigned a=0;a<6;a++)for(unsigned b=0;b<4;b++)for(unsigned c=0;c<4;c++)assert(pic.WeightScale[a][b][c]==16);
    for(unsigned a=0;a<2;a++)for(unsigned b=0;b<8;b++)for(unsigned c=0;c<8;c++)assert(pic.WeightScale8x8[a][b][c]==16);
'''
    code += r'''
    const unsigned bad[][4]={{63,64,0xc00,0x200},{64,63,0xc00,0x200},
        {0,64,0xc00,0x200},{64,0,0xc00,0x200},{64,64,0,0x200},
        {64,64,0xc01,0x200},{64,64,0xc00,0},{4160,64,0xc00,0x200},
        {64,4160,0xc00,0x200}};
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++){
        memset(&pic,0xa5,sizeof pic);memset(&desc,0xa5,sizeof desc);
        assert(nvdec_h264_parse_pic(&pic,&desc,input0,sizeof input0,
            bad[i][0],bad[i][1],bad[i][2],bad[i][3])==KH264_INVALID);
        assert(!memcmp(&pic,&zero,sizeof pic) && !memcmp(&desc,&dzero,sizeof desc));
    }
    /* A cropped display width does NOT permit an undersized coded pitch. */
    assert(nvdec_h264_parse_pic(&pic,&desc,input2,sizeof input2,256,256,0xc00,0x200)==KH264_INVALID);
    assert(nvdec_h264_parse_pic(&pic,&desc,input0,5,64,64,0xc00,0x200)!=KH264_OK);
    assert(!memcmp(&pic,&zero,sizeof pic) && !memcmp(&desc,&dzero,sizeof desc));
    assert(nvdec_h264_parse_pic(NULL,&desc,input0,sizeof input0,64,64,0xc00,0x200)==KH264_INVALID);
    assert(nvdec_h264_parse_pic(&pic,NULL,input0,sizeof input0,64,64,0xc00,0x200)==KH264_INVALID);
    printf("NVDEC real-header context mapping PASS: 5 parameter sets, 13 rejection cases; no hardware claim\n");
    return 0;
}
'''
    clang = shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe'
    with tempfile.TemporaryDirectory(prefix='kestrel-nvdec-context-') as tmp:
        c, obj, exe = (Path(tmp) / n for n in ('test.c', 'test.obj', 'test.exe'))
        c.write_text(code)
        subprocess.run([clang, '--target=x86_64-w64-windows-gnu', '-mno-ms-bitfields',
                        '-ffreestanding', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-c', str(c), '-o', str(obj)], check=True)
        subprocess.run([clang, str(obj), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
