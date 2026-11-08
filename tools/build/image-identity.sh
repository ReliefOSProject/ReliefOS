#!/bin/sh
# Deterministic GPT identities for an explicitly named image instance.
set -eu
[ "$#" = 1 ] || exit 2
instance=${RELIEFOS_IMAGE_INSTANCE:-reliefos}
case $instance in ''|*[!A-Za-z0-9._-]*) echo 'invalid IMAGE_INSTANCE_ID' >&2; exit 2;; esac
[ "${#instance}" -le 128 ] || exit 2
case $1 in disk|bios|esp|root) ;; *) exit 2;; esac
digest=$(printf 'ReliefOS image identity v1\n%s\n%s\n' "$instance" "$1" | sha256sum)
digest=${digest%% *}
printf '%s-%s-%s-%s-%s\n' "$(printf %s "$digest" | cut -c1-8)" \
    "$(printf %s "$digest" | cut -c9-12)" "$(printf %s "$digest" | cut -c13-16)" \
    "$(printf %s "$digest" | cut -c17-20)" "$(printf %s "$digest" | cut -c21-32)"
