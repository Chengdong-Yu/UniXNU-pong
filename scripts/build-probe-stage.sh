#!/usr/bin/env bash
# Host-only build and tests. This script never invokes adb or fastboot.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ROOT=$(cd "$HERE/.." && pwd)
if [[ -d /usr/lib/llvm-18/bin ]]; then
  export PATH=/usr/lib/llvm-18/bin:$PATH
elif command -v brew >/dev/null 2>&1; then
  export PATH="$(brew --prefix llvm)/bin:$(brew --prefix lld)/bin:$PATH"
fi
bash "$HERE/scripts/build-firmware.sh"
python3 "$HERE/scripts/build-payload-probe.py"
python3 "$HERE/scripts/test-initrd-qemu.py"
python3 "$HERE/scripts/build-bootimg.py" \
  --ramdisk "$ROOT/UniXNU/out/pong/payload-probe/payload.ramdisk" \
  --output-dir "$ROOT/UniXNU/out/pong/boot-v3-mapped-probe"
python3 "$HERE/scripts/verify-firmware.py" \
  --output-dir "$ROOT/UniXNU/out/pong/boot-v3-mapped-probe"
