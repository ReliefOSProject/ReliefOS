#!/bin/sh
# Verify interactive defaults without changing scripts or redirected output.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
colors=$src/system/rootfs/etc/bash/colors.sh
mkdir -p "$src/out/tmp"
work=$(TMPDIR="$src/out/tmp" mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

bash --noprofile --norc -c '
    unset LS_COLORS LESS
    . "$1"
    ! alias ls 2>/dev/null
    [[ ! ${LS_COLORS+x} && ! ${LESS+x} ]]
' check "$colors"

# Each command runs after sourcing, outside the parser's current input line.
# Bash must actually expand the aliases, rather than just print their values.
cat > "$work/interactive" <<'BASH'
set -e
unset NO_COLOR LS_COLORS LESS
unalias ls dir vdir 2>/dev/null || :
. "$1"
[[ $LESS == -R && ${LS_COLORS+x} ]]
for name in ls dir vdir; do
    [[ $(alias "$name") == "alias $name='$name --color=auto'" ]]
done
ls -1 -- "$2" > "$2/redirected"
ls -1 -- "$2" | cat > "$2/piped"
! LC_ALL=C grep -q "$(printf '\033')" "$2/redirected" "$2/piped"
alias ls='ls --color=never'
unalias dir
dir() { printf 'user function\n'; }
LS_COLORS=custom
LESS=
. "$1"
[[ $(alias ls) == "alias ls='ls --color=never'" ]]
[[ $(type -t dir) == function && $LS_COLORS == custom && $LESS == '' ]]
unset LS_COLORS LESS
unalias ls vdir
NO_COLOR=1
. "$1"
! alias ls 2>/dev/null
[[ ! ${LS_COLORS+x} && ! ${LESS+x} ]]
unset NO_COLOR
TERM=dumb
. "$1"
! alias ls 2>/dev/null
[[ ! ${LS_COLORS+x} && ! ${LESS+x} ]]
unset TERM
. "$1"
! alias ls 2>/dev/null
[[ ! ${LS_COLORS+x} && ! ${LESS+x} ]]
exit
BASH
mkdir "$work/directory"
TERM=xterm-256color bash --noprofile --norc -i -s -- "$colors" "$work/directory" \
    < "$work/interactive" > "$work/report" 2>&1 || {
        cat "$work/report" >&2
        echo 'FAIL - interactive shell color policy' >&2
        exit 1
    }
printf '%s\n' 'Interactive colors, user overrides and plain pipelines: PASS'
