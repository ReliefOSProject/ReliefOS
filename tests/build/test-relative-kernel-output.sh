#!/bin/sh
# Relative output paths must keep the parent and ReliefNT sub-build in one tree.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
cd "$repo_root"
reliefnt=${RELIEFNT_DIR:-${NTCLKS_DIR:-$repo_root/kernel/reliefnt}}

mkdir -p out
work=$(mktemp -d "$repo_root/out/test-relative-kernel-output.XXXXXX")
relative_o=${work#"$repo_root"/}
trap 'rm -rf -- "$work" "$reliefnt/$relative_o"' EXIT HUP INT TERM

for kernel_o in default override; do
    if [ "$kernel_o" = override ]; then
        reliefnt_o=$relative_o/kernel-custom
        if ! make -s O="$relative_o" NTCLKS_O="$reliefnt_o" headers_install; then
            printf 'FAIL - relative NTCLKS_O compatibility export failed\n' >&2
            exit 1
        fi
    else
        reliefnt_o=$relative_o/reliefnt
        if ! make -s O="$relative_o" headers_install; then
            printf 'FAIL - relative O export failed\n' >&2
            exit 1
        fi
    fi

    test -f "$repo_root/$reliefnt_o/kernel-export/include/reliefos/audio_abi.h"
    test -f "$work/kernel-export/include/reliefos/audio_abi.h"
done

printf 'ok   - relative output paths publish ReliefOS UAPI headers\n'
