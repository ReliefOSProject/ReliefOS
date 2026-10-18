#!/usr/bin/env python3
"""Verify real X11 window icon pixels in a hash-checked private disk copy."""
from pathlib import Path
import argparse
import json
import os
import subprocess
import time
import test_fltk_apps_qemu as fltk
import test_xorg_vm_qemu as xorg
from test_reliefexcel_qemu import Session
from test_installer_window_qemu import wait_log

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out/xorg-vmware-debug'
APPS = (*(app for app in fltk.APP_NAMES if app != 'doom'), 'reliefexcel', 'reliefword', 'reliefppt')


def prepare(image, kernel, output):
    output.mkdir(parents=True, exist_ok=False)
    probe = fltk.target_compile_probe(output, OUT/'sysroot/musl', 'clang++', 'ld.lld')
    commands = json.loads((output/'probe-build.command.json').read_text())
    commands['compile'][-3] = str(ROOT/'tools/tests/x11_app_icon_probe.cpp')
    commands['compile'][-1] = str(output/'icon_probe.o')
    commands['link'][commands['link'].index(str(output/'fltk_ui_probe.o'))] = str(output/'icon_probe.o')
    commands['link'][commands['link'].index(str(probe))] = str(output/'icon_probe.elf')
    with (output/'icon-probe-build.log').open('w') as log:
        for command in commands.values():
            subprocess.run(command, cwd=ROOT, env={**os.environ,'TMPDIR':str(ROOT/'out/tmp')},
                           stdout=log, stderr=log, check=True)
    runner = output/'icon-test'
    runner.write_text('''#!/bin/sh
export LC_ALL=zh_CN.UTF-8 LANG=zh_CN.UTF-8
app=$1
shift
/usr/lib/reliefos/apps/$app/$app.elf "$@" >/dev/ttyS0 2>&1 &
pid=$!
echo "ICON_APP_START app=$app pid=$pid" >/dev/ttyS0
/usr/lib/reliefos/tests/icon-probe "$pid" "$app" >/dev/ttyS0 2>&1
status=$?
echo "ICON_PROBE_STATUS app=$app status=$status" >/dev/ttyS0
sleep 6
if ! kill -0 "$pid"; then
    echo "ICON_APP_DIED app=$app" >/dev/ttyS0
    exit 1
fi
kill "$pid"
wait "$pid"
echo "ICON_APP_EXIT app=$app" >/dev/ttyS0
''')
    payload = [(runner,'/usr/bin/it',0o100755),
               (output/'icon_probe.elf','/usr/lib/reliefos/tests/icon-probe',0o100755)]
    for app in APPS:
        payload.extend([
            (OUT/f'userland/{app}.elf',f'/usr/lib/reliefos/apps/{app}/{app}.elf',0o100755),
            (ROOT/f'resources/build-art/app-icons/{app}.bmp',f'/usr/lib/reliefos/apps/{app}/{app}.bmp',0o100644),
            (ROOT/f'userland/apps/{app}/{app}.desktop',f'/usr/share/applications/reliefos-{app}.desktop',0o100644),
        ])
        image_file = ROOT/f'userland/apps/{app}/{app}.png'
        if image_file.exists(): payload.append((image_file,f'/usr/lib/reliefos/apps/{app}/{app}.png',0o100644))
    for path in sorted((ROOT/'resources/build-art/app-icons/hicolor').rglob('*.png')):
        payload.append((path,'/usr/share/icons/hicolor/'+str(path.relative_to(ROOT/'resources/build-art/app-icons/hicolor')),0o100644))
    for name in ('menu','toolbar'):
        payload.append((ROOT/'system/xorg/icewm'/name,'/etc/reliefos/icewm/'+name,0o100644))
    payload.append((ROOT/'third_party/doomgeneric/freedoom1.wad','/usr/lib/reliefos/apps/doom/freedoom1.wad',0o100644))
    metadata = fltk.prepare_disk(image,kernel,output,payload)
    (output/'prepare.json').write_text(json.dumps(metadata,indent=2)+'\n')
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--apps', nargs='+', choices=APPS, default=APPS,
                        help='limit runtime checks; all icon resources are still injected')
    args = parser.parse_args()
    output = args.output.resolve()
    metadata = prepare(args.image.resolve(),args.kernel.resolve(),output)
    session = None
    passed = []
    try:
        session = Session(output)
        wait_log(session.serial,'boot complete:',session.process,120)
        # Xorg/Fontconfig may still be starting long after the console boot
        # marker, especially while another validation VM is using the host.
        xorg.wait_greeter_stage(session.probe,session.process,'login','icon-greeter',timeout=120)
        session.login()
        for app in args.apps:
            offset = len(session.serial.read_text(errors='replace'))
            session.probe.click(300,200)
            session.probe.text('it '+app+(' --graphical' if app=='installer' else ''))
            session.probe.key('ret')
            xorg.wait_serial_after(session.serial,'APP_ICON_PASS app='+app+' ',session.process,offset,timeout=40)
            # The WM publishes the client before its first Expose is painted.
            # Icon properties are already testable; screenshots need a frame.
            time.sleep(2)
            session.probe.frame(app+'-icon')
            xorg.wait_serial_after(session.serial,'ICON_APP_EXIT app='+app,session.process,offset,timeout=15)
            passed.append(app)
            print('verified',app,flush=True)
        session.probe.click(20,786); time.sleep(.7); session.probe.frame('start-menu-icons')
    finally:
        if session: session.close()
    unchanged = fltk.sha256(args.image)==metadata['source_image_sha256_before']
    if not unchanged: raise AssertionError('Source disk image changed')
    result = {'passed':True,'applications':passed,'source_image_unchanged':True,
              'window_sizes':[16,32,48,64],'pixel_comparison':'exact RGBA to _NET_WM_ICON ARGB',
              'vmware_verified':False}
    (output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))


if __name__ == '__main__': main()
