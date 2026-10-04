#!/usr/bin/env python3
"""Reject audible gaps and descriptor repetition in the actual VM analyzer."""
import importlib.util
import math
from pathlib import Path
import struct
import tempfile
import unittest
import wave

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('audio_vm', ROOT/'tools/test_hda_qemu.py')
vm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vm)


class WaveAcceptance(unittest.TestCase):
    def test_empty_or_truncated_container_has_reportable_error(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'out/audio-hda/qa', prefix='wave-') as d:
            p = Path(d)/'tone.wav'
            for raw in (b'', b'RIFF', b'RIFF\x24\x00\x00\x00WAVE'):
                p.write_bytes(raw)
                with self.assertRaisesRegex(ValueError, 'truncated WAV'):
                    vm.analyze_wave(p)

    def analyze(self, frames, **kwargs):
        with tempfile.TemporaryDirectory(dir=ROOT/'out/audio-hda/qa', prefix='wave-') as d:
            p = Path(d)/'tone.wav'
            with wave.open(str(p), 'wb') as w:
                w.setparams((2, 2, 48000, 0, 'NONE', 'not compressed'))
                w.writeframes(b''.join(struct.pack('<hh', *frame) for frame in frames))
            return vm.analyze_wave(p, **kwargs)

    @staticmethod
    def tone():
        return [(int(12000*math.sin(2*math.pi*n/48)),
                 int(12000*math.sin(4*math.pi*n/48))) for n in range(96000)]

    def test_continuous_stereo_with_idle_padding(self):
        self.analyze([(0, 0)]*1024+self.tone()+[(0, 0)]*512,
                     core_frames=None)

    def test_initial_tone_excludes_following_pcm_timer_playback(self):
        # The real native probe plays two seconds, then the timer probe plays
        # one more second. The initial 96,000-frame gate must stay independent.
        metrics = self.analyze(self.tone()+[(0, 0)]*512+self.tone()[:48000])
        self.assertEqual(metrics['continuity_frames_tested'], 95999)

    def test_initial_gap_is_rejected_even_with_later_timer_audio(self):
        frames = self.tone()
        frames[24000:24512] = [(0, 0)]*512
        with self.assertRaisesRegex(ValueError, 'continuity'):
            self.analyze(frames+[(0, 0)]*512+self.tone()[:48000])

    def test_silent_descriptor_inside_tone_is_rejected(self):
        frames = self.tone()
        frames[24000:24000] = [(0, 0)]*512
        with self.assertRaisesRegex(ValueError, 'continuity'):
            self.analyze(frames)

    def test_repeated_descriptor_outside_fft_window_is_rejected(self):
        frames = self.tone()
        frames[24000:24512] = frames[23488:24000]
        with self.assertRaisesRegex(ValueError, 'continuity'):
            self.analyze(frames)


if __name__ == '__main__':
    unittest.main()
