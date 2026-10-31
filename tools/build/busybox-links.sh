#!/bin/sh
# Route BusyBox's historical paths to GNU providers and keep other applets.
set -eu
[ "$#" = 2 ] || { echo 'usage: busybox-links.sh PLAN LINKS' >&2; exit 2; }
plan=$1 links=$2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

# Validate the entire input before emitting any records.
awk '
$0 !~ /^\/(bin|sbin|usr\/bin|usr\/sbin)\/[^/[:space:]]+$/ ||
$0 ~ /\/(\.|\.\.|busybox)$/ {print "unsafe BusyBox link: " $0 > "/dev/stderr"; bad=1}
seen[$0]++ {print "duplicate BusyBox link: " $0 > "/dev/stderr"; bad=1}
END {exit bad}' "$links"

# A tree rule claims only files actually present in that selected source tree.
while IFS="$(printf '\t')" read -r kind source guest mode owner policy gid; do
    case $kind in
        t)
            find "$source" \( -type f -o -type l \) -print > "$work/tree"
            while IFS= read -r path; do
                relative=${path#"$source"/}
                printf '%s/%s\t%s\n' "${guest%/}" "$relative" "$owner"
            done < "$work/tree"
            ;;
        f|l) printf '%s\t%s\n' "$guest" "$owner" ;;
    esac
done < "$plan" > "$work/claimed-owners"
cut -f1 "$work/claimed-owners" > "$work/claimed"

awk -F / '$0 ~ /^\/(bin|sbin|usr\/bin|usr\/sbin)\/[^/]+$/ {print $NF}' \
    "$work/claimed" > "$work/commands"
while IFS= read -r guest; do
    command=${guest##*/}
    [ "$command" != bash ] || continue
    if grep -Fqx "$command" "$work/commands"; then
        # GNU commands live in /usr/bin. Keep callers of the old non-usr-merge
        # /bin and /usr/sbin paths on that same provider, without duplicating
        # a destination already explicitly present in the selected plan.
        if ! grep -Fqx "$guest" "$work/claimed" &&
            awk -F '\t' -v path="/usr/bin/$command" \
                '$1==path && $2=="coreutils" {found=1} END {exit !found}' "$work/claimed-owners"; then
            case $guest in
                /bin/*|/sbin/*) target=../usr/bin/$command ;;
                /usr/sbin/*) target=../bin/$command ;;
                *) continue ;;
            esac
            printf 'l\t%s\t%s\t0777\tcoreutils\tunique\t0\n' "$target" "$guest"
        fi
        continue
    fi
    target="../../bin/busybox"
    case $guest in
        /bin/*) target=busybox ;;
        /sbin/*) target=../bin/busybox ;;
    esac
    printf 'l\t%s\t%s\t0777\tbusybox\tunique\t0\n' "$target" "$guest"
done < "$links"
