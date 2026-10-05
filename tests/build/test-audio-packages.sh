#!/bin/sh
# Inspect real target products; execute only the SDK compiler, never target ELFs.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
audio_out=${AUDIO_OUT:-"$repo/out/audio-hda"}
require() { test -s "$1" || { echo "FAIL missing audio product: $1" >&2; exit 1; }; }
require "$audio_out/system/lib/libreliefos-audio.so.1"
require "$audio_out/userland/soundctl.elf"
require "$audio_out/userland/settings.elf"
readelf -d "$audio_out/system/lib/libreliefos-audio.so.1" | grep -F '[libreliefos-audio.so.1]'
readelf -d "$audio_out/system/lib/libreliefos-audio.so.1" | grep -F '[libasound.so.2]'
readelf -d "$audio_out/userland/soundctl.elf" | grep -F '[libreliefos-audio.so.1]'
if readelf -d "$audio_out/system/lib/libreliefos.so.2" | grep -F 'libasound'; then
 echo 'FAIL base runtime unexpectedly depends on ALSA' >&2; exit 1
fi
for spec in 'alsa-lib usr/lib/libasound.so.2' 'alsa-lib usr/include/alsa/asoundlib.h' \
 'alsa-lib usr/share/alsa/alsa.conf' 'alsa-lib usr/share/alsa/pcm/dmix.conf' \
 'alsa-lib usr/share/alsa/pcm/dsnoop.conf' 'alsa-lib usr/share/licenses/alsa-lib/COPYING' \
 'alsa-utils usr/bin/aplay' 'alsa-utils usr/bin/arecord' 'alsa-utils usr/bin/amixer' \
 'alsa-utils usr/sbin/alsactl' 'alsa-utils usr/bin/speaker-test' \
 'alsa-utils usr/share/licenses/alsa-utils/COPYING' 'nuked-opl3 usr/lib/libopl3.so.1' \
 'nuked-opl3 usr/include/opl3.h' 'nuked-opl3 usr/share/licenses/nuked-opl3/LICENSE'; do
 set -- $spec; require "$audio_out/upstream/$1/root/$2"
done
readelf -d "$audio_out/upstream/alsa-lib/root/usr/lib/libasound.so.2" | grep -F '[libasound.so.2]'
readelf -d "$audio_out/upstream/nuked-opl3/root/usr/lib/libopl3.so.1" | grep -F '[libopl3.so.1]'
# dmix/dsnoop are built into libasound in this pinned configuration.
for plugin in dmix dsnoop plug hw; do
 nm -D "$audio_out/upstream/alsa-lib/root/usr/lib/libasound.so.2" | grep -F "_snd_pcm_${plugin}_open"
done
for elf in "$audio_out/upstream/alsa-lib/root/usr/lib/libasound.so.2" \
 "$audio_out/upstream/nuked-opl3/root/usr/lib/libopl3.so.1" \
 "$audio_out/upstream/alsa-utils/root/usr/bin/aplay" \
 "$audio_out/upstream/alsa-utils/root/usr/bin/amixer" \
 "$audio_out/upstream/alsa-utils/root/usr/sbin/alsactl" \
 "$audio_out/upstream/alsa-utils/root/usr/bin/speaker-test"; do
 readelf -d "$elf" | grep -F '[libc.so]'
 if readelf --version-info "$elf" | grep -F 'GLIBC_'; then
  echo "FAIL host glibc dependency: $elf" >&2; exit 1
 fi
done
test ! -e "$audio_out/upstream/alsa-lib/root/usr/lib/libasound.la"
for f in usr/lib/libasound.so.2 usr/lib/libopl3.so.1 usr/lib/libreliefos-audio.so.1 usr/bin/soundctl etc/asound.conf \
 usr/share/alsa/pcm/dmix.conf usr/share/alsa/pcm/dsnoop.conf usr/bin/aplay usr/bin/arecord \
 usr/bin/amixer usr/sbin/alsactl usr/bin/speaker-test usr/lib/reliefos/apps/settings/settings.elf \
 etc/init.d/reliefos-audio etc/runlevels/default/reliefos-audio; do require "$audio_out/rootfs/raw/$f"; done
test -x "$audio_out/rootfs/raw/etc/init.d/reliefos-audio"
test -L "$audio_out/rootfs/raw/etc/runlevels/default/reliefos-audio"
readlink "$audio_out/rootfs/raw/etc/runlevels/default/reliefos-audio" | grep -F "../../init.d/reliefos-audio"
sdk="$audio_out/sdk/reliefos-musl-sdk"
for f in bin/reliefos-musl-cc include/alsa/asoundlib.h include/opl3.h include/reliefos/audio_control.h lib/libreliefos-audio.so.1 lib/libasound.so.2 \
 lib/libasound.a lib/libopl3.so.1 share/alsa/pcm/dmix.conf share/licenses/alsa-lib/COPYING \
 share/licenses/nuked-opl3/LICENSE; do require "$sdk/$f"; done
work=$(mktemp -d "$audio_out/qa/audio-sdk.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work/sdk path"
tar -xf "$audio_out/packages/reliefos-musl-sdk.tar.gz" -C "$work/sdk path"
cat > "$work/client.c" <<'EOF'
#include <alsa/asoundlib.h>
#include <opl3.h>
int main(void) {
    snd_pcm_t *pcm;
    opl3_chip chip;
    OPL3_Reset(&chip,48000);
    int r=snd_pcm_open(&pcm,"default",SND_PCM_STREAM_PLAYBACK,0);
    if(r<0)return 1;
    r=snd_pcm_set_params(pcm,SND_PCM_FORMAT_S16_LE,SND_PCM_ACCESS_RW_INTERLEAVED,
                         2,48000,1,50000);
    snd_pcm_close(pcm);
    return r<0;
}
EOF
"$work/sdk path/reliefos-musl-sdk/bin/reliefos-musl-cc" "$work/client.c" -lasound -lopl3 -o "$work/client"
readelf -d "$work/client" | grep -F '[libasound.so.2]'
readelf -d "$work/client" | grep -F '[libopl3.so.1]'
cat > "$work/control-client.c" <<'EOF'
#include <reliefos/audio_control.h>
int main(void) {
    struct reliefos_audio_control *control;
    if (reliefos_audio_control_open(0, &control)) return 1;
    reliefos_audio_control_close(control);
    return 0;
}
EOF
"$work/sdk path/reliefos-musl-sdk/bin/reliefos-musl-cc" "$work/control-client.c" -lreliefos-audio -o "$work/control-client"
readelf -d "$work/control-client" | grep -F '[libreliefos-audio.so.1]'
cat > "$work/static-client.c" <<'EOF'
#include <alsa/asoundlib.h>
int main(void) {
    snd_pcm_t *pcm;
    int r=snd_pcm_open(&pcm,"default",SND_PCM_STREAM_PLAYBACK,0);
    if(r<0)return 1;
    snd_pcm_close(pcm);
    return 0;
}
EOF
"$work/sdk path/reliefos-musl-sdk/bin/reliefos-musl-cc" -static \
 "$work/static-client.c" -lasound -lm -ldl -o "$work/static-client"
if readelf -l -d "$work/static-client" | grep -E 'INTERP|\(NEEDED\)'; then
 echo 'FAIL static ALSA client has dynamic dependencies' >&2; exit 1
fi
echo 'audio upstream/rootfs/SDK products, plugins, licenses, target dependencies and relocated dynamic/static links PASS'
