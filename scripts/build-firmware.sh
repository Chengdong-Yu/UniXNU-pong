#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ROOT=$(cd "$HERE/.." && pwd)
export PYTHON_COMMAND=python3
if [[ -z ${CLANG_BIN:-} && -d /usr/lib/llvm-18/bin ]]; then
  export CLANG_BIN=/usr/lib/llvm-18/bin/
elif [[ -z ${CLANG_BIN:-} ]] && command -v brew >/dev/null 2>&1; then
  # Let unixnu assemble a tool prefix, finding matching LLVM/lld first.
  export PATH="$(brew --prefix llvm)/bin:$(brew --prefix lld)/bin:$PATH"
fi
export PYTHONPATH="$ROOT/Mu-Silicium/Mu_Basecore/BaseTools/Source/Python${PYTHONPATH:+:$PYTHONPATH}"
export PATH="$ROOT/Mu-Silicium/Mu_Basecore/BaseTools/BinWrappers/PosixLike:$ROOT/Mu-Silicium/Mu_Basecore/BaseTools/Source/C/bin:$PATH"
cd "$ROOT/UniXNU"
python3 unixnu build firmware -D "$HERE" --uefi "$ROOT/Mu-Silicium" \
  --edk2 "$ROOT/Mu-Silicium/Mu_Basecore" \
  --local-module "refer:edk2=$ROOT/Mu-Silicium/Mu_Basecore" \
  --out "$ROOT/UniXNU/out" --target RELEASE -j "${JOBS:-8}"
