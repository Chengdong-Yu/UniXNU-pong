#!/usr/bin/env bash
set -euo pipefail

DEVICE_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
UNIXNU_ROOT=${UNIXNU_ROOT:-$(cd "$DEVICE_ROOT/.." && pwd)/UniXNU}
if [[ ! -x "$UNIXNU_ROOT/unixnu" ]]; then
  echo "UniXNU core checkout not found: $UNIXNU_ROOT" >&2
  echo "Set UNIXNU_ROOT to the UniXNU repository directory." >&2
  exit 2
fi

cd "$UNIXNU_ROOT"
./unixnu defconfig pong_defconfig -D "$DEVICE_ROOT"
./unixnu fetch
python3 "$DEVICE_ROOT/scripts/fetch-ios16-assets.py"
./unixnu build kernel
./unixnu build package
