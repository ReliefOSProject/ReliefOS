#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
PYTHONDONTWRITEBYTECODE=1 exec python3 "$src/tools/test_blockdev_ext3.py"
