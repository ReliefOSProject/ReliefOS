#!/usr/bin/env python3
"""Check guest serial evidence without launching a VM or opening a sound card."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("audio_e2e", ROOT / "tools/test_audio_e2e_qemu.py")
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


class SerialEvidenceTest(unittest.TestCase):
    def test_software_exit_preserves_unrun_external_gates(self):
        evidence = {
            "gates": {"alsa_guest_preflight": True,
                      "soundctl_amixer_settings": True,
                      "doom_freedoom_long_run": True,
                      "visible_gui_xorg": False,
                      "physical_codec_matrix": False},
            "long_run": {"all_serial_xrun_clean": True},
        }
        self.assertTrue(report.software_gate_pass(evidence))
        for gate in ("alsa_guest_preflight", "soundctl_amixer_settings",
                     "doom_freedoom_long_run"):
            evidence["gates"][gate] = False
            self.assertFalse(report.software_gate_pass(evidence))
            evidence["gates"][gate] = True
        evidence["long_run"]["all_serial_xrun_clean"] = False
        self.assertFalse(report.software_gate_pass(evidence))
        self.assertFalse(report.software_gate_pass({}))

    def test_pressure_requires_full_overlap_with_audio(self):
        active = ('PASS standard-doom-load cpu-io-workers-active\n'
                  'PASS audio-io-load bytes=524288 cycles=2\n')
        self.assertFalse(report.doom_load_complete(active, 60))
        self.assertTrue(report.doom_load_complete(
            active + 'PASS standard-doom-load-overlap seconds=60\n', 60))
        self.assertFalse(report.doom_load_complete(
            active + 'PASS standard-doom-load-overlap seconds=6\n', 60))
        self.assertTrue(report.doom_load_complete('', 0))

    def test_pressure_requires_completed_io_progress(self):
        text = ('PASS standard-doom-load cpu-io-workers-active\n'
                'PASS standard-doom-load-overlap seconds=60\n')
        self.assertFalse(report.doom_load_complete(text, 60))
        self.assertFalse(report.doom_load_complete(
            text + 'PASS audio-io-load bytes=0 cycles=0\n', 60))
        self.assertTrue(report.doom_load_complete(
            text + 'PASS audio-io-load bytes=524288 cycles=2\n', 60))

    def test_stopped_ring_cannot_pass_long_run(self):
        text = ("[doom-audio] stats written_bytes=65536 startup_recoveries=0 "
                "xrun_recoveries=0 suspend_recoveries=0\n")
        self.assertFalse(report.doom_audio_throughput_clean(text, 60))
        full = text.replace('written_bytes=65536', 'written_bytes=11520000')
        self.assertTrue(report.doom_audio_throughput_clean(full, 60))
        tolerated = text.replace('written_bytes=65536', 'written_bytes=11289600')
        self.assertTrue(report.doom_audio_throughput_clean(tolerated, 60))
        self.assertFalse(report.doom_audio_throughput_clean(
            tolerated.replace('11289600', '11289599'), 60))

    def test_doom_interval_excludes_later_capture_xrun(self):
        text = ("[u6-standard] mode=doom doom_seconds=600\n"
                "PASS standard-doom headless-freedoom-level-audio\n"
                "[u6-standard] mode=capture doom_seconds=5\noverrun!!!\n")
        self.assertFalse(report.has_xrun(report.standard_mode_output(text, "doom")))
        self.assertTrue(report.has_xrun(text))

    def test_whole_serial_includes_recovered_xrun_counters(self):
        clean = "[doom-audio] stats written_bytes=163238104 startup_recoveries=0 xrun_recoveries=0 suspend_recoveries=0\n"
        self.assertFalse(report.has_xrun(clean))
        recovered = clean.replace("xrun_recoveries=0", "xrun_recoveries=2")
        self.assertTrue(report.has_xrun(recovered))
        self.assertTrue(report.has_xrun(recovered + clean))

    def test_recovery_statistics_are_required(self):
        self.assertFalse(report.doom_audio_xrun_clean("PASS standard-doom\n"))

    def test_startup_and_steady_recovery_are_distinct(self):
        clean = ("[doom-audio] stats written_bytes=192000 startup_recoveries=1 "
                 "xrun_recoveries=0 suspend_recoveries=0\n")
        self.assertTrue(report.doom_audio_xrun_clean(clean))
        self.assertFalse(report.doom_audio_xrun_clean(clean.replace("xrun_recoveries=0", "xrun_recoveries=1")))
        self.assertFalse(report.doom_audio_xrun_clean(clean.replace("suspend_recoveries=0", "suspend_recoveries=1")))
        self.assertFalse(report.doom_audio_xrun_clean(clean.replace("written_bytes=192000", "written_bytes=0")))
        self.assertFalse(report.doom_audio_xrun_clean(clean + "underrun!!!\n"))

    def test_music_only_cannot_prove_gameplay_audio(self):
        self.assertFalse(report.doom_scenes_complete("[doom] first nonzero mixed PCM block\n"))
        valid = "[doom-audio] scenes music_starts=2 sfx_starts=14 attack_starts=3 max_sfx_voices=2\n"
        self.assertTrue(report.doom_scenes_complete(valid))
        for old, new in (("music_starts=2", "music_starts=1"),
                         ("sfx_starts=14", "sfx_starts=0"),
                         ("attack_starts=3", "attack_starts=1"),
                         ("max_sfx_voices=2", "max_sfx_voices=1")):
            self.assertFalse(report.doom_scenes_complete(valid.replace(old, new)))
        separate = ("[doom-audio] scenes music_starts=1 sfx_starts=14 attack_starts=3 max_sfx_voices=2\n" * 2)
        self.assertFalse(report.doom_scenes_complete(separate))

    def test_multiple_output_lifetimes_cannot_hide_a_recovery(self):
        text = ("[doom-audio] stats written_bytes=100 startup_recoveries=0 xrun_recoveries=1 suspend_recoveries=0\n"
                "[doom-audio] stats written_bytes=200 startup_recoveries=0 xrun_recoveries=0 suspend_recoveries=0\n")
        self.assertEqual(report.doom_audio_statistics(text), {
            "reports": 2, "written_bytes": 300, "startup_recoveries": 0,
            "xrun_recoveries": 1, "suspend_recoveries": 0,
        })
        self.assertFalse(report.doom_audio_xrun_clean(text))


if __name__ == "__main__":
    unittest.main()
