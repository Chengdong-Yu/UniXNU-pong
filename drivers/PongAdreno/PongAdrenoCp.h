/* SPDX-License-Identifier: MIT */
#pragma once
#include "PongAdrenoGmu.h"
#include "submit/NativeAdrenoUserClient.h"
struct PongCpOps {
    NativeAdrenoRingIo registers;
    ArmSmmu4KIo smmu;
    void (*record)(const char *,unsigned,uint64_t,uint64_t);
};
struct PongCpBackend {
    NativeAdrenoVm *vm;
    NativeAdrenoSubmit *submit;
    NativeAdrenoRing *ring;
    NativeAdrenoTimer *timer;
    NativeAdrenoStatus *status;
    NativeAdrenoRingClock clock;
    NativeAdrenoAbiDeviceInfo info;
    unsigned output_bits;
    bool (*healthy)();
};
/* prepare is called while GX and the CM3 are still stopped; nothing published.
 * start requires the actual GMU power ACK, GX OOB and START ACK. */
IOReturn PongAdrenoCpPrepare(const PongCpOps *);
IOReturn PongAdrenoCpStart(PongGpuGmuDevice *);
bool PongAdrenoPublishClient(PongCpBackend *);
