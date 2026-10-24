/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Fixed records adapted from NativeMobileDisplay/NativeGOPWire.h. */
#pragma once
#include <stdint.h>
enum {PongScanoutVersion=1,PongScanoutBgra=1,PongScanoutMode=0x7000,
      PongScanoutWait=0x7002,PongScanoutComplete=0x7003,PongScanoutMemory=0x5343};
// A presenter rejected the frame before committing any GPU command. This is
// distinct from a failed/unknown fence: CPU fallback cannot race DMA here.
enum : uint32_t {PongScanoutNotSubmitted=0x4e4f444d};
struct PongScanoutModeInfo {uint32_t version,format;uint64_t width,height,stride,bytes;};
struct PongScanoutWaitInfo {uint64_t timeout_ms;};
struct PongScanoutFrame {uint64_t sequence,address,bytes,stride;uint32_t width,height,format,reserved;};
struct PongScanoutDone {uint64_t sequence;uint32_t status,reserved;};
static_assert(sizeof(PongScanoutModeInfo)==40 && sizeof(PongScanoutWaitInfo)==8 &&
    sizeof(PongScanoutFrame)==48 && sizeof(PongScanoutDone)==16,"Pong scanout wire");
