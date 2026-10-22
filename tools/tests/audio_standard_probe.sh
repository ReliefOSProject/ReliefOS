#!/bin/sh
set -eu
mode=
next=
frames=4096
output=/tmp/u6-standard-capture.raw
doom_seconds=5
load_seconds=0
for arg in "$@"; do
    if [ "$next" = mode ]; then mode=$arg; next=; continue; fi
    if [ "$next" = frames ]; then frames=$arg; next=; continue; fi
    if [ "$next" = output ]; then output=$arg; next=; continue; fi
    if [ "$next" = doom_seconds ]; then doom_seconds=$arg; next=; continue; fi
    if [ "$next" = load_seconds ]; then load_seconds=$arg; next=; continue; fi
    if [ "$arg" = --mode ]; then next=mode; fi
    if [ "$arg" = --frames ]; then next=frames; fi
    if [ "$arg" = --output ]; then next=output; fi
    if [ "$arg" = --doom-seconds ]; then next=doom_seconds; fi
    if [ "$arg" = --load-seconds ]; then next=load_seconds; fi
done
printf '[u6-standard] mode=%s doom_seconds=%s load_seconds=%s args=%s\n' "$mode" "$doom_seconds" "$load_seconds" "$*"
case "$mode" in
abi)
    aplay -l >/tmp/u6-playback-list.txt
    cat /tmp/u6-playback-list.txt
    grep -q '^card 0:.*device 0:' /tmp/u6-playback-list.txt
    arecord -l >/tmp/u6-capture-list.txt
    cat /tmp/u6-capture-list.txt
    grep -q '^card 0:.*device 0:' /tmp/u6-capture-list.txt
    amixer -c 0 contents
    soundctl cards
    soundctl controls
    test -x /usr/lib/reliefos/apps/settings/settings.elf
    test -x /usr/lib/reliefos/apps/doom/doom.elf
    test -s /usr/lib/reliefos/apps/doom/freedoom1.wad
    printf '%s\n' 'PASS standard-enumeration settings-doom-payload'
    ;;
play)
    # Exercise the exclusive hardware path before the persistent dmix server
    # acquires the endpoint lease for the two default-client checks.
    aplay -D hw:0,0 --period-size=512 --buffer-size=2048 -t raw -f S16_LE -c 2 -r 48000 -d 1 /dev/zero
    aplay -D default --period-size=768 --buffer-size=12288 -t raw -f S16_LE -c 2 -r 48000 -d 1 /dev/zero
    aplay -D default --period-size=768 --buffer-size=12288 -t raw -f S16_LE -c 2 -r 48000 -d 1 /dev/zero
    # The default dmix server now owns the hardware lease. Exercise the
    # shared endpoint with the upstream utility and retain its full output.
    speaker-test -D default -c 2 -r 48000 -F S16_LE -t sine -l 1
    printf '%s\n' 'PASS standard-speaker-test default'
    soundctl --pcm default test --channels 2 --seconds 2
    printf '%s\n' 'PASS standard-playback hw-default-speaker-soundctl'
    ;;
capture)
    arecord -D default --period-size=768 --buffer-size=12288 -t raw -f S16_LE -c 2 -r 48000 -s "$frames" "$output"
    test "$(wc -c < "$output")" -eq "$((frames * 4))"
    printf '%s\n' 'PASS standard-capture default-arecord'
    ;;
lifetime)
    arecord -D default --period-size=768 --buffer-size=12288 --start-delay=1 -f S16_LE -c 2 -r 48000 -d 1 /tmp/u6-record.raw
    test -s /tmp/u6-record.raw
    printf '%s\n' 'PASS standard-capture default'
    ;;
concurrent)
    aplay -D default --period-size=768 --buffer-size=12288 -t raw -f S16_LE -c 2 -r 48000 -d 1 /dev/zero >/tmp/u6-play-1.log 2>&1 & p1=$!
    aplay -D default --period-size=768 --buffer-size=12288 -t raw -f S16_LE -c 2 -r 48000 -d 1 /dev/zero >/tmp/u6-play-2.log 2>&1 & p2=$!
    wait "$p1" || { cat /tmp/u6-play-1.log; exit 1; }
    wait "$p2" || { cat /tmp/u6-play-2.log; exit 1; }
    arecord -D default --period-size=768 --buffer-size=12288 -t raw -f S16_LE -c 2 -r 48000 -d 1 /tmp/u6-record-1.raw >/tmp/u6-record-1.log 2>&1 & r1=$!
    arecord -D default --period-size=768 --buffer-size=12288 -t raw -f S16_LE -c 2 -r 48000 -d 1 /tmp/u6-record-2.raw >/tmp/u6-record-2.log 2>&1 & r2=$!
    wait "$r1" || { cat /tmp/u6-record-1.log; exit 1; }
    wait "$r2" || { cat /tmp/u6-record-2.log; exit 1; }
    test -s /tmp/u6-record-1.raw
    test -s /tmp/u6-record-2.raw
    printf '%s\n' 'PASS standard-concurrent dmix-dsnoop'
    ;;
controls)
    amixer -c 0 contents
    soundctl controls
    volume=$(soundctl get 1 | sed -n 's/.* values=//p')
    mute=$(soundctl get 2 | sed -n 's/.* values=//p')
    test -n "$volume"
    test -n "$mute"
    # This probe targets the QEMU codec's actual two-channel 0..74 gain and
    # writable mute. Save through the production OpenRC/alsactl service first.
    /etc/init.d/reliefos-audio stop
    test -s /var/lib/alsa/asound.state
    soundctl set 1 10,20
    amixer -c 0 cget numid=1 >/tmp/u6-amixer-volume.txt
    cat /tmp/u6-amixer-volume.txt
    grep -q 'values=10,20$' /tmp/u6-amixer-volume.txt
    amixer -c 0 cset numid=1 30,40
    soundctl get 1 >/tmp/u6-soundctl-volume.txt
    cat /tmp/u6-soundctl-volume.txt
    grep -q 'values=30,40$' /tmp/u6-soundctl-volume.txt
    soundctl set 2 on
    amixer -c 0 cget numid=2 >/tmp/u6-amixer-mute.txt
    cat /tmp/u6-amixer-mute.txt
    grep -q 'values=on$' /tmp/u6-amixer-mute.txt
    printf '%s\n' 'PASS standard-controls bidirectional-write-readback'
    /etc/init.d/reliefos-audio start
    test "$(soundctl get 1 | sed -n 's/.* values=//p')" = "$volume"
    test "$(soundctl get 2 | sed -n 's/.* values=//p')" = "$mute"
    printf '%s\n' 'PASS standard-controls persisted-state-restoration'
    printf '%s\n' 'PASS standard-controls amixer-soundctl'
    ;;
doom)
    # The headless mode still runs Doom's real WAD, SFX and MIDI setup and
    # submits PCM through /dev/dsp; it only omits the framebuffer/Xorg window.
    cpu_pid=
    io_pid=
    cleanup_pid=
    load_status=0
    doom_status=0
    doom_log=/tmp/u6-doom.log
    load_log=/tmp/u6-doom-io-load.log
    load_done=/tmp/u6-doom-load.finished
    rm -f "$load_done" /tmp/u6-doom-load.bin
    /usr/lib/reliefos/apps/doom/doom.elf \
        -iwad /usr/lib/reliefos/apps/doom/freedoom1.wad \
        -headless -headless-seconds "$doom_seconds" -warp 1 -skill 1 -nodraw \
        >"$doom_log" 2>&1 & doom_pid=$!
    if [ "$load_seconds" -gt 0 ]; then
        # Start pressure only after actual mixed PCM begins. WAD/level setup
        # must not consume the entire pressure window before playback starts.
        tries=0
        until grep -q 'first nonzero mixed PCM block' "$doom_log"; do
            if ! kill -0 "$doom_pid" 2>/dev/null || [ "$tries" -ge 30 ]; then
                load_status=1
                break
            fi
            /bin/busybox sleep 1
            tries=$((tries + 1))
        done
        /bin/busybox yes >/dev/null 2>&1 & cpu_pid=$!
        # One persistent process continuously overwrites a bounded 256 KiB
        # file. This measures CPU/I/O pressure without an exec/page-in storm.
        /usr/lib/reliefos/tests/audio-guest --mode io-load \
            --output /tmp/u6-doom-load.bin >"$load_log" 2>&1 & io_pid=$!
        (
            /bin/busybox sleep "$load_seconds"
            if kill -0 "$doom_pid" 2>/dev/null &&
               kill -0 "$cpu_pid" 2>/dev/null && kill -0 "$io_pid" 2>/dev/null &&
               grep -q '\[audio-load\] I/O active bytes=262144' "$load_log"; then
                printf 'PASS standard-doom-load-overlap seconds=%s\n' "$load_seconds" >"$load_done"
            fi
            kill "$cpu_pid" "$io_pid" 2>/dev/null || true
        ) & cleanup_pid=$!
        /bin/busybox sleep 1
        kill -0 "$cpu_pid" 2>/dev/null || load_status=1
        kill -0 "$io_pid" 2>/dev/null || load_status=1
    fi
    wait "$doom_pid" || doom_status=$?
    cat "$doom_log"
    if [ -n "$cleanup_pid" ]; then
        if [ -s "$load_done" ]; then cat "$load_done"; else load_status=1; fi
        kill "$cleanup_pid" 2>/dev/null || true
        kill "$cpu_pid" "$io_pid" 2>/dev/null || true
        wait "$cleanup_pid" 2>/dev/null || true
        wait "$io_pid" || load_status=1
        cat "$load_log"
        grep -q '^PASS audio-io-load bytes=' "$load_log" || load_status=1
        rm -f /tmp/u6-doom-load.bin
    fi
    if [ "$doom_status" -ne 0 ]; then
        exit "$doom_status"
    fi
    if [ "$load_status" -ne 0 ]; then
        printf '%s\n' 'FAIL standard-doom-load cpu-or-io-worker-exited' >&2
        exit 1
    fi
    if [ "$load_seconds" -gt 0 ]; then
        printf '%s\n' 'PASS standard-doom-load cpu-io-workers-active'
    fi
    printf '%s\n' 'PASS standard-doom headless-freedoom-level-audio'
    ;;
*)
    printf '%s\n' 'FAIL unknown-mode' >&2
    exit 2
    ;;
esac
