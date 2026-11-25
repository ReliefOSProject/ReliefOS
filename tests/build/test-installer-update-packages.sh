#!/bin/sh
# Real signed APK transactions, including bounded read-only Installer preflight.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
RELIEFOS_TEST_OUTPUT=${RELIEFOS_TEST_OUTPUT:-${RELIEFOS_TEST_BUILD_ROOT:-$src/out/x86_64/release}}
export RELIEFOS_TEST_OUTPUT
exec "${RELIEFOS_TEST_PYTHON:-python3}" "$src/tools/test_installer_update_packages.py" -v
