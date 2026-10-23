/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PONG_ADRENO_HFI_H
#define PONG_ADRENO_HFI_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    PongAdrenoHfiSuccess,PongAdrenoHfiInvalid,PongAdrenoHfiBusy,PongAdrenoHfiNoSpace,
    PongAdrenoHfiEmpty,PongAdrenoHfiCorrupt,PongAdrenoHfiUnexpected,PongAdrenoHfiFirmwareError,
    PongAdrenoHfiNotifyError,PongAdrenoHfiWaitError,PongAdrenoHfiTimeout,PongAdrenoHfiPoisoned
} PongAdrenoHfiStatus;
typedef struct {
    volatile uint32_t *shared;
    uint32_t iova,sequence,pending_header,poisoned;
    void *cookie;
    int (*notify)(void *); /* real HOST2GMU interrupt write, 0=success */
    /* Diagnostics of skipped response packets (a6xx_hfi_wait_for_ack skips firmware error messages and replies to
     * another sequence number): their count and the first one's header, return header and code. */
    uint32_t skipped,skipped_header,skipped_ret,skipped_code;
} PongAdrenoHfi;
/* Modern (nonlegacy) GMU HFI only. Caller owns an exclusive, 16KiB extent
 * mapped with four native4K pages, coherent for CPU and GMU, and a valid 32bit
 * GMU IOVA. Coherence, ownership and active mappings are platform obligations,
 * not established by this API. The GMU MUST be stopped and unable to access
 * this extent during initialization. No reset of a running queue is allowed.
 * AArch64 little-endian; real outer-shareable barriers, no fake ACKs.
 * All calls on an instance are serialized by the caller. Shared memory,
 * object and message/output storage must be disjoint and remain owned.
 * Never free/reuse shared storage until an external real stop+TLB drain is
 * proved. Poison/timeout/notify failure do not establish safe release. */
PongAdrenoHfiStatus PongAdrenoHfiInitializeStopped(PongAdrenoHfi *,volatile uint32_t *,
    size_t shared_bytes,uint64_t iova,void *cookie,int (*notify)(void *));
/* payload excludes the wire header. One command may be outstanding. A failed
 * notify after publishing is indeterminate: poisons the session, no retry.
 * NoSpace/Busy refuse before publishing. No firmware readiness is inferred. */
PongAdrenoHfiStatus PongAdrenoHfiSend(PongAdrenoHfi *,unsigned id,const uint32_t *payload,
    size_t payload_words,uint32_t *wire_header);
/* Poll the response queue. As a6xx_hfi_wait_for_ack: the reply whose return
 * header carries the pending command's sequence number completes it (error=0
 * succeeds, otherwise FirmwareError); F2H error messages and replies to other
 * sequence numbers are consumed, counted and skipped. payload_words reports
 * actual returned words; output remains untouched on failure. Capacity must be
 * >=16, the protocol's maximum ACK payload. Empty keeps the pending command.
 * Malformed queues or packets poison the session; no success or queue reset is
 * synthesized. Debug queue not drained. */
PongAdrenoHfiStatus PongAdrenoHfiPoll(PongAdrenoHfi *,uint32_t *payload,size_t capacity,
    size_t *payload_words,uint32_t *firmware_error);
/* Serialized command + bounded response polling. Poll budget limits software
 * iterations, not hardware elapsed microseconds. Optional wait callback must
 * return promptly; an error poisons the session. Timeout also poisons it.
 * This transport alone performs no power/firmware/performance-table startup. */
PongAdrenoHfiStatus PongAdrenoHfiExchange(PongAdrenoHfi *,unsigned id,const uint32_t *,size_t,
    unsigned poll_budget,int (*wait_once)(void *),uint32_t *,size_t,
    size_t *,uint32_t *firmware_error);
#ifdef __cplusplus
}
#endif
#endif

