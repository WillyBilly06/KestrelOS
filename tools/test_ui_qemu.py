#!/usr/bin/env python3
"""Isolated visual smoke run; never flashes or modifies a build image.

This emulates a VGA display, not RTX/NVENC. Screenshots are visual evidence only.
Optional actions JSON: [{"key":"ctrl-pgdn"}, {"click":[x,y]}, {"wait":2}].
Each action produces a screenshot using the actual captured display dimensions.
"""
import argparse
import json
import shutil
import socket
import subprocess
import tempfile
import time
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--seconds', type=int, default=45)
    p.add_argument('--actions', default='[]')
    p.add_argument('--actions-file', type=Path)
    p.add_argument('--audio', action='store_true', help='Emulate an HD Audio duplex codec (silent backend)')
    p.add_argument('--audio-debug', action='store_true', help='Log emulated codec commands')
    args = p.parse_args()
    if not 1 <= args.seconds <= 180:
        p.error('seconds must be 1..180')
    actions = json.loads(args.actions_file.read_text() if args.actions_file else args.actions)
    root = Path(__file__).resolve().parents[1]
    qemu = Path('C:/Program Files/qemu')
    run = Path(tempfile.mkdtemp(prefix='ui-visual-', dir=root/'out'))
    print('Visual artifacts:', run, flush=True)
    for source, name in [(qemu/'share/edk2-x86_64-code.fd','code.fd'),
                         (qemu/'share/edk2-i386-vars.fd','vars.fd'),
                         (root/'out/kestrelos.img','disk.img')]:
        shutil.copyfile(source, run/name)
    # Obtain an ephemeral loopback port. QEMU binds only loopback, not the LAN.
    with socket.socket() as reserve:
        reserve.bind(('127.0.0.1', 0))
        port = reserve.getsockname()[1]
    command = [str(qemu/'qemu-system-x86_64.exe'), '-machine','q35', '-m','4096',
               '-drive',f'if=pflash,format=raw,readonly=on,file={run / "code.fd"}',
               '-drive',f'if=pflash,format=raw,file={run / "vars.fd"}',
               '-device','VGA,xres=1920,yres=1080','-display','none',
               '-serial',f'file:{run / "serial.log"}', '-no-reboot',
               '-qmp',f'tcp:127.0.0.1:{port},server=on,wait=off',
               '-drive',f'if=none,id=nvm,format=raw,file={run / "disk.img"}',
               '-device','nvme,drive=nvm,serial=uiqa',
               '-device','qemu-xhci,id=xhci','-device','usb-kbd,bus=xhci.0',
               '-device','usb-tablet,bus=xhci.0']
    if args.audio:
        command += ['-audiodev','none,id=a0','-device','intel-hda' + (',debug=3' if args.audio_debug else ''),
                    '-device','hda-duplex,audiodev=a0' + (',debug=3' if args.audio_debug else '')]
    with (run/'qemu.log').open('w') as log:
        child = subprocess.Popen(command, stdout=log, stderr=log,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            for elapsed in range(args.seconds):
                if child.poll() is not None:
                    raise RuntimeError(f'QEMU exited early ({child.returncode}); see {run / "qemu.log"}')
                if elapsed and elapsed % 15 == 0:
                    print(f'VM boot: {elapsed}s', flush=True)
                time.sleep(1)
            with socket.create_connection(('127.0.0.1',port),timeout=10) as sock:
                stream = sock.makefile('rwb')
                json.loads(stream.readline())
                sequence = 0

                def call(execute, arguments=None):
                    nonlocal sequence
                    sequence += 1
                    payload = {'execute':execute, 'id':sequence}
                    if arguments is not None:
                        payload['arguments'] = arguments
                    stream.write(json.dumps(payload).encode()+b'\n')
                    stream.flush()
                    while True:
                        line = stream.readline()
                        if not line:
                            raise RuntimeError('QMP disconnected')
                        reply = json.loads(line)
                        if reply.get('id') != sequence:
                            continue
                        if 'error' in reply:
                            raise RuntimeError(reply['error'])
                        return reply.get('return')

                call('qmp_capabilities')
                from PIL import Image

                def capture(index):
                    ppm = run/f'{index:02}.ppm'
                    call('screendump',{'filename':ppm.as_posix()})
                    with Image.open(ppm) as im:
                        im.save(run/f'{index:02}.png')
                        print(f'Screenshot {index}: {im.size}', flush=True)
                        return im.size

                width,height = capture(0)
                for index, action in enumerate(actions,1):
                    if 'key' in action:
                        call('send-key',{'keys':[{'type':'qcode','data':k}
                                                for k in action['key'].split('-')]})
                    elif 'click' in action:
                        x,y = action['click']
                        if not (0 <= x < width and 0 <= y < height):
                            raise ValueError('click outside captured display')
                        call('input-send-event',{'events':[
                            {'type':'abs','data':{'axis':'x','value':int(x*32767/(width-1))}},
                            {'type':'abs','data':{'axis':'y','value':int(y*32767/(height-1))}}]})
                        time.sleep(.2)
                        for down in [True,False]:
                            call('input-send-event',{'events':[
                                {'type':'btn','data':{'down':down,'button':'left'}}]})
                            time.sleep(.15)
                    elif 'wait' in action:
                        time.sleep(min(10,max(0,float(action['wait']))))
                    else:
                        raise ValueError('unknown action')
                    time.sleep(1)
                    width,height = capture(index)
                call('quit')
        finally:
            if child.poll() is None:
                child.terminate()
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=10)
    print('UI_QEMU_CAPTURE_COMPLETE (not native GPU/codec validation)', flush=True)
    if args.audio:
        serial = (run/'serial.log').read_text(errors='replace')
        if '/dev/audio - the highest this codec reported' not in serial or 'recording: node' not in serial:
            raise RuntimeError('Emulated HD Audio output/input initialization failed; inspect serial.log')
        print('UI_QEMU_HDA_DUPLEX_STARTUP_PASS (silent emulated device)', flush=True)


if __name__ == '__main__':
    main()
