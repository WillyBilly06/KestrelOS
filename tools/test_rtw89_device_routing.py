#!/usr/bin/env python3
"""Compile real chip routing; ensure RTL8922 sequencing cannot hit AX chips."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT/'kernel/rtw.c').read_text()


def function(name):
    match = re.search(r'^static (?:bool|void) '+name+r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    return source[match.start():source.index('\n}', match.end())+2]


def main():
    code = '#include <stdbool.h>\n#include <stdint.h>\n#include <assert.h>\n#include <stdio.h>\ntypedef uint16_t u16;\n'
    code += function('generation_is_be')+'\n'+function('generation_is_supported')
    code += r'''
int main(void) {
    for(unsigned id=0;id<=0xffff;++id) assert(generation_is_be(id)==(id==0x8922));
    unsigned ax[]={0x8852,0xa85a,0xb852,0xb85b,0xc852,0x8851};
    for(unsigned i=0;i<sizeof ax/sizeof ax[0];++i) {
        assert(!generation_is_be(ax[i]));
        assert(!generation_is_supported(ax[i]));
    }
    assert(!generation_is_supported(0x8922));
    puts("PASS: all PCI IDs; 8922-only BE sequence, AX chips excluded; runtime legacy guards present");
}
'''
    # Start/stop/key UI callbacks must reject the unsupported generation before
    # reaching old firmware, CPU-reset or key-table register operations.
    start = function('rtw_start')
    assert start.index('if (c->is_be)') < start.index('rtw_parse_firmware(')
    assert 'dev->unsupported_generation && !c->is_be' in start
    stop = function('rtw_stop')
    assert stop.index('c->is_be || dev->unsupported_generation') < stop.index('hold_cpu(c)')
    key = function('rtw_set_key')
    assert key.index('if (c->is_be) return false;') < key.index('send_h2c(')
    with tempfile.TemporaryDirectory(prefix='kestrel-rtw-routing-') as tmp:
        c = Path(tmp)/'routing.c'
        exe = Path(tmp)/'routing.exe'
        c.write_text(code)
        cc = shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe'
        subprocess.run([cc,'-std=c11','-O2',str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)


if __name__ == '__main__':
    main()
