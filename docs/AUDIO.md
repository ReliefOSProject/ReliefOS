# ReliefOS audio

Applications use the Linux ALSA ABI through libasound, or the OSS `/dev/dsp`
compatibility interface. The base `libreliefos.so.2` does not depend on ALSA.
Applications can opt into `libreliefos-audio.so.1` for control and test-tone
helpers. The SDK exports both public headers and the link-name symlink.

`soundctl cards` lists card numbers and names. Query the selected device with
`soundctl --card 0 controls`, then use the exact returned name or numeric ID:

```sh
soundctl --card 0 get 'Playback Volume'
soundctl --card 0 set 'Playback Volume' 50%
soundctl --card 0 set 'Playback Volume' 10,20
soundctl --card 0 watch
soundctl --pcm default test --channels 2 --seconds 2
```

Device names vary. A card exposing `Master Playback Switch` accepts `on`/`off`;
a card exposing `Playback Mute` uses `on` to mute. Numeric values must follow
the reported range and step. One value broadcasts to all channels; multiple
values must match the channel count. Percentages map to the actual device
range and round to a valid step. Enum controls accept their reported labels
or numeric item IDs.

`route headphones` and `capture-source microphone` select labels in `Output
Source` and `Input Source`, respectively. If the card lacks these controls,
the command reports ENOTSUP and lists available controls. Use `set` with an
actual device-specific enum, such as `Capture Source`, when appropriate.
The CLI queries labels rather than assuming a fixed headphone item number.

Control access follows the device node permissions. Current devfs defaults are
root-owned mode 0666; administrators can restrict nodes with chmod/chown.
Playback and capture nodes have independent DAC metadata. Owner, primary group,
supplementary group, denied other-user opens (EACCES), and root opens have been
verified in an actual HDA guest, with the original metadata restored afterward.
Open/list/get/set return
0 on success or -1 with POSIX errno. A list capacity query returns ERANGE and
the required count for a nonempty card. Event wait returns 1 for an element
event, 0 for timeout, or -1 with errno; EINTR remains visible. Close is harmless
for NULL or an already closed handle. Discard a closed pointer before opening
new handles, since allocator address reuse is possible. In-flight operations
retain ownership; worker shutdown should use finite waits.

The test tone uses a nonblocking S16 PCM, queries the negotiated sample rate,
and produces 440 Hz on the left and 660 Hz on the right. Recovery and shutdown
run in the calling worker or CLI process; a GUI must not call it from rendering.

The default PCM uses ALSA `dmix` for playback and `dsnoop` for capture on
`hw:0,0`, with an 8,192-frame period and a 65,536-frame hardware buffer
(170.7 ms periods, about 1.365 s of buffering at 48 kHz). This is the current
candidate configuration, with a substantial latency cost. Both
plugins share the actual hardware lease through the direct-plugin server.
Use `hw:0,0` for exclusive access; it can report EBUSY while the shared server
owns that direction. An absent capture endpoint has no capture device node.

The OpenRC `reliefos-audio` service restores `/var/lib/alsa/asound.state` when
present. On stop it runs `alsactl store` into a temporary file and atomically
publishes the result. Failed stores preserve the previous state. The settings
sound model and worker are implemented; visible GUI acceptance is deferred to
the future Xorg work at the user's request.

Acceptance records live under `.superpowers/sdd/2026-09-30-intel-hda-audio`
and `docs/superpowers/progress`. U6 remains open. The R5 matrix and 600-second
Doom run validated a larger native probe with an explicitly submitted silent
tail. They do not close the original 512/2048, unflushed finite-stream gate.
New R6/R7 evidence preserves intermittent native playback EPIPE and a dual-codec
60-second capture EPIPE under concurrent host build pressure. A subsequent full
dual-codec run passed, but does not erase the counterexample. A new PCM DRAIN
repair silences all unsubmitted ring frames without advancing appl_ptr. Complete
finite-stream output and repeated close/reopen still require final acceptance.

The new controls waveform test found correct amp readback with ineffective
mute/gain after converter format binding. The HDA prepare order has been fixed
and host regression passed. R8 guest output now correctly mutes, halves gain and
changes stereo balance, with restoration. Its full duration check still failed
by 125 muted frames; functional gain evidence does not close finite EOF.
A 50-second unwarped Freedoom run proved title-to-demo music changes, repeated
weapon sounds and overlapping SFX. The R9 600-second scene/load run completed the music/SFX/attack/overlap counters,
but recovered from two XRUNs and lost 386 timer-tail frames. R10 long-run validation
remains pending; its first three standard matrix cases passed capture lifecycle
and the complete known input, while complete native finite output still failed. Visible GUI is deferred to Xorg. Physical codecs are not_run;
Linux host inventory does not establish ReliefOS physical-codec support.

For a fresh SDK-built native probe and guest run:

```sh
python3 tools/test_linux_audio.py --build-guest \
  --sdk out/audio-hda-u6/sdk/reliefos-musl-sdk \
  --output out/audio-hda-u6/qa/audio-guest-new
python3 tools/test_audio_e2e_qemu.py \
  --image out/audio-hda-u6/images/reliefos-lifecycle-r10.vmdk \
  --probe out/audio-hda-u6/qa/audio-guest-new \
  --standard-probe out/audio-hda-u6/qa/u6-closeout-20261004/standard-attract-8192x65536.sh \
  --output out/u6-new-run --smp 4 --duration 600 --load-seconds 60
```

These commands describe the current reproducible candidate, not an accepted
U6 result. Use a fresh output directory. The runner retains image/probe/runner
SHA, guest commands, serial, private disk, capture and waveform reports. Its
initial 96,000-frame check is only one segment; full finite-stream and long
capture checkers are separate gates. The native probe keeps period/buffer
512/2048 and submits no artificial silent tail. The e2e wrapper requires all
software gates and whole-serial XRUN cleanliness; GUI and physical status are
recorded separately.

EAGAIN indicates temporary lack of space/data for a nonblocking PCM; retry
through poll while preserving positive short progress and EINTR. EPIPE indicates
XRUN and requires PREPARE before resuming. EBUSY can indicate an exclusive
hardware lease owned by dmix/dsnoop; ENODEV indicates an absent/disconnected
endpoint; unsupported operations can return ENOTSUP. For nonblocking ALSA DRAIN,
observe STATUS reaching SETUP: Linux can keep returning EAGAIN even after normal
completion. A POLLERR completion wakeup alone must not be converted to EIO;
STATUS XRUN remains a real failure. STATUS availability can exceed one ring
after XRUN, and running playback delay is signed when hardware overtakes the
application; non-running delay is zero, matching Linux. DELAY still returns
EPIPE in XRUN.

Inspect `aplay -l`, `arecord -l`, `amixer -c 0 contents`, device permissions,
`/etc/asound.conf` and HDA/PCM serial diagnostics before changing geometry.
A second raw-file success is insufficient acceptance evidence. AC97/ES1371
ABI-v1 devices do not expose ALSA cards; their narrow OSS playback path and
continuous waveform quality have separate, unfinished validation. With no
audio controller, the fixed `/dev/dsp` node can exist but open returns ENODEV;
absent PCM/control nodes return ENOENT. R8 actually ran Doom silently for
20 seconds and exited normally in this configuration.

Core-backed OSS byte streams retain partial frames up to 32 bytes, including
fixed 8- and 16-channel S16 HDA endpoints. Applications must honor the channel
count returned by `SNDCTL_DSP_CHANNELS`. `wavplay` places its stereo source on
the first two negotiated channels and fills the remaining channels with zero
PCM. Its fragment size scales with the hardware frame size to retain at least
the default 512 frames per period; this is separate from the unaccepted U6
ALSA geometry. Native VMware HDA audibility still requires a VMware retest.
