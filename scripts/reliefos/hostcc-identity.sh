#!/bin/sh
# Hash every HOSTCC word that resolves to a file without evaluating the command.
set -eu

if [ "$#" -eq 0 ]; then
    set -- cc
fi

identity=$(for tool do
    path=$(command -v "$tool" 2>/dev/null || true)
    [ -n "$path" ] && [ -f "$path" ] || continue
    digest=$(sha256sum "$path" | awk '{ print $1 }')
    printf '%s %s\n' "$tool" "$digest"
done)

if [ -z "$identity" ]; then
    printf 'unavailable\n'
else
    printf '%s\n' "$identity" | sha256sum | awk '{ print $1 }'
fi
