#!/bin/sh
# Real production Kbuild behavior, using disposable sources and output trees.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
exec python3 "$root/tools/test_build_incremental.py"
