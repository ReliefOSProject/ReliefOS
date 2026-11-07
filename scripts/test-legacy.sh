#!/bin/sh
# Explicit, non-production Python regression checks. Guest-specific historical
# harnesses remain individual tools and are not silently treated as passes.
set -eu
. "$(CDPATH= cd -- "$(dirname "$0")" && pwd)/logging.sh"
src=$1
cd "$src"
reliefos_log LEGACY 'standalone kernel regression group'
make -C "${RELIEFNT_DIR:-$src/kernel/reliefnt}" O="${RELIEFNT_O:-$src/out/legacy-kernel-tests}" test-legacy
for suite in test_uapi test_unix_ipc; do
    reliefos_log LEGACY "$suite"
    python3 "tools/$suite.py"
done
