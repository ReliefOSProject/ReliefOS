#!/bin/sh
# Installed only in a private QA image. Variables are substituted by the host runner.
exec > /dev/ttyS0 2>&1
NONCE='@@NONCE@@'
RELEASE='@@RELEASE@@'
MODES='@@MODES@@'
REPEAT=@@REPEAT@@
JOBS=@@JOBS@@
TEST=/usr/lib/reliefos/tests/smp-latency
RUN=/var/tmp/smp-latency
CONTROL=/proc/reliefnt/lock_stats
export DISPLAY=:0
xpid= wpid= gpid= active_key= active_mode=

snapshot() {
    snapshot_key=$active_key
    snapshot_mode=$active_mode
    # Clear first: even a failed disable/read is not silently retried.
    active_key=
    if [ "$snapshot_mode" = absent ]; then
        echo "SMP_ABSENT:$snapshot_key"
    else
        printf '0\n' >"$CONTROL" || return 1
        echo "SMP_STATS_BEGIN:$snapshot_key"
        cat "$CONTROL" || return 1
        echo "SMP_STATS_END:$snapshot_key"
    fi
}

cleanup() {
    if [ -n "$gpid" ]; then kill "$gpid" 2>/dev/null || :; wait "$gpid" 2>/dev/null || :; fi
    if [ -n "$wpid" ]; then kill "$wpid" 2>/dev/null || :; wait "$wpid" 2>/dev/null || :; fi
    if [ -n "$xpid" ]; then kill "$xpid" 2>/dev/null || :; wait "$xpid" 2>/dev/null || :; fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
abort() {
    if [ -n "$active_key" ]; then snapshot || echo 'profiler snapshot failed'; fi
    for file in "$RUN/icewm.log" "$RUN/x11-wait.log" "$RUN/x11.err" "$RUN/x11.out" "$RUN/work.log" /var/log/Xorg.0.log; do
        if [ -f "$file" ]; then
            echo "DIAGNOSTIC_LOG_BEGIN $file"
            cat "$file"
            echo "DIAGNOSTIC_LOG_END $file"
        fi
    done
    echo "SMP_DONE:$NONCE:failures=1"
    exit 1
}

echo "SMP_START:$NONCE:$RELEASE"
[ "$(uname -r)" = "$RELEASE" ] && [ "$(cat /proc/sys/kernel/osrelease)" = "$RELEASE" ] || abort
for command in gcc make as ld ffmpeg ffprobe icewm; do
    command -v "$command" >/dev/null 2>&1 || { echo "missing $command"; abort; }
done
[ -f /usr/include/stdio.h ] && [ -x /usr/lib/reliefos/xorg-tty-wrapper ] || abort
ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libx264 || { echo 'missing libx264'; abort; }
mkdir -p "$RUN" || abort
for command in gcc make as ld ffmpeg ffprobe; do
    version_file="$RUN/version-$command"
    version_status=0
    case "$command" in
        ffmpeg|ffprobe) "$command" -version >"$version_file" 2>&1 || version_status=$? ;;
        *) "$command" --version >"$version_file" 2>&1 || version_status=$? ;;
    esac
    version_count=0
    while IFS= read -r version_line || [ -n "$version_line" ]; do
        version_count=$((version_count + 1))
        printf 'SMP_VERSION_LINE:%s:%s:%s:%s\n' "$NONCE" "$command" "$version_count" "$version_line"
    done <"$version_file"
    printf 'SMP_VERSION_END:%s:%s:%s:%s\n' "$NONCE" "$command" "$version_status" "$version_count"
    [ "$version_status" -eq 0 ] && [ "$version_count" -gt 0 ] || abort
done
[ ! -S /tmp/.X11-unix/X0 ] || { echo 'existing X server'; abort; }

# QA-only: single isolated VT, root-local X access (-ac); TCP listener disabled.
/usr/lib/reliefos/xorg-tty-wrapper :0 -config /etc/X11/xorg.conf -nolisten tcp -ac vt1 -keeptty &
xpid=$!
i=0
while [ ! -S /tmp/.X11-unix/X0 ] && [ "$i" -lt 30 ]; do
    kill -0 "$xpid" 2>/dev/null || abort
    sleep 1; i=$((i + 1))
done
[ -S /tmp/.X11-unix/X0 ] || abort
icewm >"$RUN/icewm.log" 2>&1 &
wpid=$!
i=0
while [ "$i" -lt 30 ]; do
    if "$TEST/smp-x11" --samples 1 --interval-ms 1 >"$RUN/x11-wait.log" 2>&1; then break; fi
    kill -0 "$xpid" 2>/dev/null && kill -0 "$wpid" 2>/dev/null || abort
    sleep 1; i=$((i + 1))
done
[ "$i" -lt 30 ] || { cat "$RUN/x11-wait.log"; abort; }

compile_work() {
    work_status=0; check_status=0
    (cd "$TEST/fixture" && make clean && if [ "$JOBS" -eq 0 ]; then make -j; else make "-j$JOBS"; fi) || work_status=$?
    if [ "$work_status" -eq 0 ]; then
        output=$("$TEST/fixture/fixture") || check_status=$?
        [ "$output" = 660e5ee6e1d4b501 ] || check_status=1
    fi
}

ffmpeg_work() {
    work_status=0; check_status=0
    ffmpeg -v error -nostdin -y -f lavfi -i testsrc2=size=640x360:rate=24 -t 5 \
        -an -c:v libx264 -threads 4 -preset medium -pix_fmt yuv420p "$RUN/video.mp4" || work_status=$?
    if [ "$work_status" -eq 0 ]; then
        ffprobe -v error -select_streams v:0 -count_frames \
            -show_entries stream=width,height,nb_read_frames -of default=noprint_wrappers=1 \
            "$RUN/video.mp4" >"$RUN/ffprobe.txt" || check_status=$?
        grep -qx 'width=640' "$RUN/ffprobe.txt" && grep -qx 'height=360' "$RUN/ffprobe.txt" && \
            grep -qx 'nb_read_frames=120' "$RUN/ffprobe.txt" || check_status=1
        ffmpeg -v error -nostdin -i "$RUN/video.mp4" -f null - || check_status=$?
    fi
}

work() {
    case "$1" in
        idle) work_status=0; check_status=0; sleep 5 || work_status=$? ;;
        compile) compile_work ;;
        ffmpeg) ffmpeg_work ;;
        native) work_status=0; check_status=0
            "$TEST/smp-native" --workers 4 --pages 128 --rounds 4 --directory "$RUN" >"$RUN/native.out" || work_status=$? ;;
        *) work_status=1; check_status=1 ;;
    esac
}

# Fixed warm policy: compile and encode/decode once without sampling, then
# clean/recompile and encode/decode every recorded repetition; no cache dropping.
compile_work >"$RUN/warm-compile.log" 2>&1
[ "$work_status" -eq 0 ] && [ "$check_status" -eq 0 ] || { cat "$RUN/warm-compile.log"; abort; }
ffmpeg_work >"$RUN/warm-ffmpeg.log" 2>&1
[ "$work_status" -eq 0 ] && [ "$check_status" -eq 0 ] || { cat "$RUN/warm-ffmpeg.log"; abort; }

for mode in $MODES; do
    for case_name in idle compile ffmpeg native; do
        rep=1
        while [ "$rep" -le "$REPEAT" ]; do
            key="$NONCE:$mode:$case_name:$rep"
            if [ "$mode" = absent ]; then
                [ ! -e "$CONTROL" ] || { echo 'profiler unexpectedly present'; abort; }
            else
                [ -f "$CONTROL" ] || { echo 'profiler missing'; abort; }
                printf '0\n' >"$CONTROL" && printf 'reset\n' >"$CONTROL" || abort
            fi
            active_key=$key; active_mode=$mode
            if [ "$mode" != absent ]; then
                if [ "$mode" = on ]; then printf '1\n' >"$CONTROL" || abort; fi
            fi
            ready="$RUN/ready"; stop="$RUN/stop"
            rm -f "$ready" "$stop" "$RUN/native.out"
            "$TEST/smp-x11" --ready-file "$ready" --stop-file "$stop" >"$RUN/x11.out" 2>"$RUN/x11.err" &
            gpid=$!
            i=0
            while [ ! -f "$ready" ] && [ "$i" -lt 100 ]; do
                kill -0 "$gpid" 2>/dev/null && kill -0 "$xpid" 2>/dev/null && kill -0 "$wpid" 2>/dev/null || break
                sleep 0.1; i=$((i + 1))
            done
            [ -f "$ready" ] || { cat "$RUN/x11.err"; abort; }
            start=$("$TEST/smp-native" --now) || abort
            work "$case_name" >"$RUN/work.log" 2>&1
            finish=$("$TEST/smp-native" --now) || abort
            elapsed=$((finish - start))
            kill -0 "$xpid" 2>/dev/null && kill -0 "$wpid" 2>/dev/null || check_status=1
            : >"$stop" || abort
            wait "$gpid"; gui_status=$?; gpid=
            snapshot || abort
            echo "SMP_X11_BEGIN:$key"
            cat "$RUN/x11.out"
            echo "SMP_X11_END:$key"
            if [ "$case_name" = native ]; then
                echo "SMP_NATIVE_BEGIN:$key"
                cat "$RUN/native.out"
                echo "SMP_NATIVE_END:$key"
            fi
            gui_samples=$(sed -n 's/^X11 count=\([0-9]*\) .*/\1/p' "$RUN/x11.out")
            [ -n "$gui_samples" ] || gui_samples=0
            cat "$RUN/work.log" "$RUN/x11.err"
            echo "SMP_ROW:$key:elapsed_ns=$elapsed:work=$work_status:check=$check_status:gui=$gui_status:gui_samples=$gui_samples"
            [ "$work_status" -eq 0 ] && [ "$check_status" -eq 0 ] && [ "$gui_status" -eq 0 ] && \
                [ "$gui_samples" -ge 10 ] && [ "$elapsed" -gt 0 ] || abort
            rep=$((rep + 1))
        done
    done
done
echo "SMP_DONE:$NONCE:failures=0"
exit 0
