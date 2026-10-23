/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PONG_ADRENO_SMMU_H
#define PONG_ADRENO_SMMU_H
#include "ArmSmmu4K/ArmSmmu4KAudit.h"
#include "ArmSmmu4K/ArmSmmu4KStreams.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {PongAdrenoSmmuGpu,PongAdrenoSmmuGmu} PongAdrenoSmmuRole;
typedef struct {
    unsigned attempted,ready,poisoned;
    PongAdrenoSmmuRole role;
    ArmSmmu4KContextConfig context;
    ArmSmmu4KAuditConfig tables;
    ArmSmmu4KStreamState streams;
} PongAdrenoSmmuSession;
typedef struct {
    unsigned phase,must_retain_resources;
    ArmSmmu4KProgramResult context;
    ArmSmmu4KStreamResult streams;
    ArmSmmu4KAuditResult tables;
    ArmSmmu4KStatus cleanup;
} PongAdrenoSmmuResult;

/* Actual Pong GPU SMMU only: physical 0x03da0000/0x40000. It is separate
 * from the applications SMMU used by the working UFS/display drivers.
 * DT mask 0x400 expands to TWO exact SIDs for each role: GPU 0/0x400,
 * GMU 5/0x405. The caller supplies two actually leased slots (not SID=slot)
 * and a leased bank/ASID. Secure and LPAC streams are never taken here.
 *
 * Before calling, platform must establish real power/clocks, stopped masters,
 * exclusive global configuration ownership, context/global fault handlers,
 * Pong ACTLR/SCM setup, and published physical native4KiB table/backing
 * ownership. This module does not establish any of those prerequisites.
 * GPU bank0 uses upper TTBR1; GMU uses a leased nonzero bank and lower TTBR0.
 * No bypass or masked route is installed. Unrelated live routes are preserved.
 * All config/table/result/session storage is disjoint and retained. Normal
 * completion and failure BOTH retain resources: a TLB sync is not a DMA drain.
 */
ArmSmmu4KStatus PongAdrenoSmmuStart(const ArmSmmu4KIo *,uint64_t physical,
    PongAdrenoSmmuRole,unsigned bank,const unsigned slots[2],
    const ArmSmmu4KContextConfig *,const ArmSmmu4KAuditConfig *,unsigned budget,
    PongAdrenoSmmuSession *,PongAdrenoSmmuResult *);
ArmSmmu4KStatus PongAdrenoSmmuObserve(const ArmSmmu4KIo *,uint64_t physical,
    const PongAdrenoSmmuSession *,PongAdrenoSmmuResult *);
/* Caller must first prove actual masters stopped. Fault both exact streams
 * before disabling their owned bank. Never releases DMA memory or a lease.
 */
ArmSmmu4KStatus PongAdrenoSmmuStop(const ArmSmmu4KIo *,uint64_t physical,
    unsigned budget,PongAdrenoSmmuSession *,PongAdrenoSmmuResult *);
#ifdef __cplusplus
}
#endif
#endif
