#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
: "${RELIEFOS_KERNEL_TOOLS:?set RELIEFOS_KERNEL_TOOLS to the native Kbuild host tool}"
exec "$RELIEFOS_KERNEL_TOOLS" modules-stage "$@"
