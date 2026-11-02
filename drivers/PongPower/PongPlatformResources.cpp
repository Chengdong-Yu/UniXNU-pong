/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Shared Pong APPSS resources. Audio must not depend on the Wi-Fi driver
 * being selected. GPU retains its separate active TCS3 ownership. */
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>
#include "PongPlatformResources.h"

namespace {
IOLock *powerLock, *smmuLeaseLock, *scmCallLock;
IOMemoryMap *rscMap;
volatile uint8_t *rsc;

void initLock(IOLock **slot) {
    if (__atomic_load_n(slot, __ATOMIC_ACQUIRE)) return;
    IOLock *lock = IOLockAlloc(), *empty = nullptr;
    if (lock && !__atomic_compare_exchange_n(slot, &empty, lock, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) IOLockFree(lock);
}
uint64_t now() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000;
}
uint32_t rd(unsigned off) {
    uint32_t value = *reinterpret_cast<volatile uint32_t *>(rsc + off);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}
void wr(unsigned off, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(rsc + off) = value;
    __asm__ volatile("dsb oshst" ::: "memory");
}
bool mapped() {
    if (rsc) return true;
    auto memory = IODeviceMemory::withRange(0x17a20000, 0x10000);
    if (!memory) return false;
    rscMap = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    if (rscMap) rsc = reinterpret_cast<volatile uint8_t *>(rscMap->getVirtualAddress());
    return rsc != nullptr;
}
bool fail(const char *message) {
    IOLog("Pong resources: %s\n", message);
    return false;
}
bool rawVote(uint32_t addr, uint32_t value) {
    constexpr unsigned b = 0xd00 + 4 * 0x2a0, other = 0xd00 + 3 * 0x2a0;
    unsigned caps = rd(0xc), count = (caps >> 12) & 63, ncpt = (caps >> 27) & 31;
    if (((rd(0) >> 16) & 255) >= 3 || count < 8 || ncpt < 1 || !rd(b + 0x18))
        return fail("active TCS4 unavailable");
    if (!rd(other + 0x18)) {
        unsigned mask = rd(other + 0x1c);
        for (unsigned i = 0; i < ncpt && i < 16; i++)
            if ((mask & (1u << i)) && rd(other + 0x34 + 20 * i) == addr)
                return fail("conflicting RPMh transaction");
    }
    wr(0xd00, rd(0xd00) & ~(1u << 4));
    wr(b + 0x14, rd(b + 0x14) & ~((1u << 24) | (1u << 16)));
    wr(b + 0x1c, 0); wr(0xd08, 1u << 4);
    uint64_t end = now() + 10;
    while (rd(0xd04) & (1u << 4)) {
        if (now() >= end) return fail("TCS4 completion did not clear");
        IOSleep(1);
    }
    wr(b + 0x30, 0x10108); wr(b + 0x34, addr); wr(b + 0x38, value);
    wr(b + 0x10, 1); wr(b + 0x1c, 1); wr(b + 0x14, 1u << 16);
    wr(b + 0x14, (1u << 16) | (1u << 24));
    bool progressed = false, done = false;
    end = now() + 100;
    do {
        unsigned st = rd(b + 0x3c), idle = rd(b + 0x18);
        if ((st & 0x10100) != 0x10100 || !idle) progressed = true;
        if ((st & 0x10100) == 0x10100 && idle && (progressed || (rd(0xd04) & (1u << 4)))) {
            done = true; break;
        }
        IODelay(10);
    } while (now() < end);
    if (!done) return fail("RPMh response timeout; transaction retained");
    wr(b + 0x1c, 0); wr(0xd08, 1u << 4); wr(b + 0x14, rd(b + 0x14) & ~(1u << 24));
    return true;
}
}

extern "C" void pong_rpmh_init(void) {
    initLock(&powerLock); initLock(&smmuLeaseLock);
}
extern "C" bool pong_rpmh_vote(uint32_t address, uint32_t value) {
    pong_rpmh_init();
    if (!powerLock) return false;
    IOLockLock(powerLock);
    bool ok = mapped() && rawVote(address, value);
    IOLockUnlock(powerLock);
    return ok;
}
extern "C" bool pong_smmu_lease_lock(void) {
    pong_rpmh_init();
    if (!smmuLeaseLock) return false;
    IOLockLock(smmuLeaseLock);
    return true;
}
extern "C" void pong_smmu_lease_unlock(void) {
    if (smmuLeaseLock) IOLockUnlock(smmuLeaseLock);
}

/* A suspended/interrupted secure transaction owns its session across
 * resumes. Serialize the complete GPU/ADSP call, never individual SMCs. */
extern "C" bool pong_scm_call_lock(void) {
    initLock(&scmCallLock);if(!scmCallLock)return false;
    IOLockLock(scmCallLock);return true;
}
extern "C" void pong_scm_call_unlock(void) {
    if(scmCallLock)IOLockUnlock(scmCallLock);
}
