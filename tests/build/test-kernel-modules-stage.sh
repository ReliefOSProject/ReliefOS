#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
exec python3 "$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)/tools/test_kernel_modules_stage.py"
