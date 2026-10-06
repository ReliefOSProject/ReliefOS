#!/usr/bin/env python3
"""Run production audio/platform functions against sanitized host fixtures."""
from pathlib import Path
import subprocess
import tempfile
import sys
ROOT = Path(__file__).resolve().parents[1]
SUITES = {'resources': 'pci_audio_resources_test.c', 'lifetime': 'audio_lifetime_test.c',
          'phase': 'driver_execution_phase_test.c', 'controller': 'hda_controller_test.c',
          'codec': 'hda_codec_test.c', 'stream': 'hda_stream_test.c',
          'controls': 'hda_controls_test.c', 'manager': 'driver_cleanup_test.c',
          'module': 'hda_module_test.c', 'legacy': 'ac97_queue_test.c', 'es1371': 'es1371_queue_test.c',
          'wavplay': 'wavplay_retry_test.c'}
failed = False
qa = ROOT / 'out/audio-hda/qa'
qa.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='hda-', dir=qa) as work:
    for suite in sys.argv[1:] or SUITES:
        print(f'SUITE {suite}', flush=True)
        binary = Path(work) / suite
        try:
            sources = [f'tools/tests/{SUITES[suite]}']
            if suite == 'codec':
                sources.append('kernel/reliefnt/drivers/hda/codec.c')
                sources.append('kernel/reliefnt/drivers/hda/controls.c')
            arguments = []
            extra_flags = ['-Iuserland/runtime/include', '-Iinclude',
                           '-Wall', '-Wextra', '-Werror'] if suite == 'wavplay' else []
            if suite == 'manager':
                module = Path(work) / 'cleanup.drv'
                subprocess.run(['cc', '-std=c11', '-fno-pic', '-fno-pie',
                    '-fno-stack-protector', '-mcmodel=large', '-ffreestanding', '-O1',
                    '-Ikernel/reliefnt/include', '-Ikernel/reliefnt/include/uapi', '-c',
                    'tools/tests/driver_cleanup_module.c', '-o', str(module)],
                    cwd=ROOT, check=True, timeout=10)
                arguments = [str(module)]
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-O1', '-g', '-no-pie', '-pthread',
                '-fsanitize=address,undefined', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                '-Ikernel/reliefnt/include', '-Ikernel/reliefnt/include/uapi', '-Ikernel/reliefnt/kernel/reliefnt/include',
                *extra_flags, *sources, '-o', str(binary)], cwd=ROOT, check=True, timeout=10)
            subprocess.run([str(binary), *arguments], check=True, timeout=10)
            print(f'PASS {suite}', flush=True)
        except (subprocess.SubprocessError, KeyError):
            failed = True
            print(f'FAIL {suite}', flush=True)
sys.exit(failed)
