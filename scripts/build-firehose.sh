#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
if [ ! -d .cache/libdispatch-work/.git ]; then
  git clone --shared refer/libdispatch .cache/libdispatch-work
fi
python3 - <<'PY'
from pathlib import Path
p = Path('.cache/libdispatch-work/xcodeconfig/libfirehose_kernel.xcconfig')
s = p.read_text()
s = s.replace('$(SDKROOT)/System/Library/Frameworks/Kernel.framework',
              '$(FAKEROOT_DIR)/System/Library/Frameworks/Kernel.framework')
s = s.replace('$(SDKROOT)/usr/local/include', '$(FAKEROOT_DIR)/usr/local/include')
lines = s.splitlines()
for index, line in enumerate(lines):
    if line.startswith('HEADER_SEARCH_PATHS =') and '$(SDKROOT)/usr/include' not in line:
        lines[index] += (' $(SDKROOT)/usr/include $(FAKEROOT_DIR)/usr/local/include'
                         ' $(PROJECT_DIR)/../xnu-work/EXTERNAL_HEADERS')
s = '\n'.join(lines) + '\n'
if p.read_text() != s:
    p.write_text(s)
PY
xcodebuild install -project .cache/libdispatch-work/libdispatch.xcodeproj \
  -target libfirehose_kernel -configuration Release -sdk iphoneos -jobs "${JOBS:-4}" \
  ARCHS=arm64 VALID_ARCHS=arm64 ONLY_ACTIVE_ARCH=NO IPHONEOS_DEPLOYMENT_TARGET="${UNIXNU_IOS_TARGET:-16.0}" \
  CODE_SIGNING_ALLOWED=NO GCC_TREAT_WARNINGS_AS_ERRORS=NO \
  OBJROOT="$ROOT/build/firehose/obj" SYMROOT="$ROOT/build/firehose/sym" \
  DSTROOT="$ROOT/.cache/fakeroot" FAKEROOT_DIR="$ROOT/.cache/fakeroot"
if [ -f .cache/fakeroot/usr/local/lib/kernel/liblibfirehose_kernel.a ]; then
  cp .cache/fakeroot/usr/local/lib/kernel/liblibfirehose_kernel.a \
    .cache/fakeroot/usr/local/lib/kernel/libfirehose_kernel.a
fi
xcrun lipo -info .cache/fakeroot/usr/local/lib/kernel/libfirehose_kernel.a
