#!/bin/sh
# Shared user-facing stage logging for ReliefOS shell scripts.
# Keep this format in sync with scripts/reliefos/Makefile.logging and tools/build/format-log.awk.

reliefos_log() {
    [ "$#" -eq 2 ] || return 2
    printf '  %-8s %s\n' "$1" "$2"
}
