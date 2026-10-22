#!/bin/sh
# A missing/corrupt locked input must fail before unpacking or publication.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
deps=${RELIEFOS_DEPS:-}
[ -n "$deps" ] && [ -x "$deps" ] || { echo 'test-grub-cache: set RELIEFOS_DEPS to the built manifest tool' >&2; exit 1; }
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/grub-cache-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/stage" "$work/cache" "$work/bin"
printf 'prior complete stage\n' > "$work/stage/sentinel"
cp "$work/stage/sentinel" "$work/sentinel"
for command in curl wget; do
    printf '#!/bin/sh\nprintf "unexpected fetch\\n" >> "%s"\nexit 99\n' "$work/network" > "$work/bin/$command"
    chmod +x "$work/bin/$command"
done
for state in missing corrupt; do
    if [ "$state" = corrupt ]; then printf 'corrupt archive\n' > "$work/cache/grub-2.12.tar.xz"; fi
    if PATH="$work/bin:$PATH" sh "$src/tools/build/grub.sh" host "$deps" \
        "$src/configs/dependencies.lock.json" "$work/cache" "$work/work" "$work/stage" - cc unused \
        > "$work/$state.log" 2>&1; then
        echo "$state GRUB archive unexpectedly accepted" >&2; exit 1
    fi
    cmp "$work/sentinel" "$work/stage/sentinel"
    test ! -e "$work/work"
    test ! -e "$work/network"
done
printf 'PASS GRUB cache: absent/corrupt inputs fail offline, existing stage unchanged\n'
