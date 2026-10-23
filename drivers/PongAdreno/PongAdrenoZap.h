/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PONG_ADRENO_ZAP_H
#define PONG_ADRENO_ZAP_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {PongZapPasId=13,PongZapReservedPhysical=0x8b91a000,
      PongZapReservedBytes=0x2000};
typedef enum {PongZapSuccess,PongZapArgument,PongZapFirmware,
    PongZapPrerequisite,PongZapIo,PongZapSecureError,PongZapPoisoned} PongZapStatus;
typedef struct {
    unsigned load_file_index,signature_file_index;
    uint32_t segment_bytes,segment_memory_bytes,relocation_base,image_span;
    size_t header_bytes,signature_bytes,metadata_bytes;
} PongZapPlan;
typedef struct {volatile uint8_t *cpu;uint64_t physical;size_t bytes;} PongZapMemory;
typedef struct {
    const uint8_t *mdt;size_t mdt_bytes;
    const uint8_t *signature;size_t signature_bytes;unsigned signature_file_index;
    const uint8_t *segment;size_t segment_bytes;unsigned segment_file_index;
    PongZapMemory metadata,reserved;
} PongZapConfig;
typedef struct {
    void *cookie;
    unsigned smccc_width; /* Actual platform convention, 32 or64; not guessed. */
    /* All callbacks return0 on success and nonzero on adapter failure. */
    /* Requires exclusive PAS13/DT reserved-memory ownership, stopped GPU,
     * real SCM clocks/votes and a retained physically contiguous, native4K
     * aligned NON-CACHEABLE metadata allocation (vendor XPU requirement).
     */
    int (*platform_ready)(void *);
    int (*publish)(void *,const PongZapMemory *,size_t);
    /* One bounded QCOM SCM call. The adapter handles the actual SMCCC
     * interrupted/session protocol with finite retry/time limits.
     * transport is signed x0 and secure_result x1;
     * neither may be replaced by assumed success from a previous kernel.
     */
    int (*scm)(void *,uint32_t function,uint32_t arginfo,
               const uint64_t args[3],int32_t *transport,int64_t *secure_result);
} PongZapOps;
typedef struct {unsigned attempted,ready,poisoned;} PongZapState;
typedef struct {
    unsigned phase,resources_must_retain;
    uint32_t function;
    int32_t transport;
    int64_t secure_result;
    PongZapPlan plan;
} PongZapResult;

/* CPU-only planner for Pong's ELF32 split MDT firmware, with exactly one
 * relocatable PT_LOAD code segment. The code filename comes from its program
 * header index: actual a730 is b01, not Redwood's b02. The signature file
 * selected by its own header (actual b02) is metadata, never executable
 * bytes. Start repacks the ELF header and that actual separate file, as the
 * vendor split-firmware loader does; it does not assume MDT tail contents.
 * This does not compute image hashes or
 * authenticate anything; actual authentication is delegated to PAS below.
 */
PongZapStatus PongAdrenoPlanZap(const uint8_t *,size_t,PongZapPlan *);
/* Real one-shot PAS init-image -> relocation setup -> code copy
 * -> auth/reset. The actual DT reservation is 0x8b91a000/0x2000; the PAS image
 * span comes from the ELF, rounded to4KiB, not the full reservation size.
 * No automatic entry or service publication. All buffers are disjoint and
 * stay retained on success AND failure until real secure/GPU quiescence.
 */
PongZapStatus PongAdrenoStartZap(const PongZapOps *,const PongZapConfig *,
                                PongZapState *,PongZapResult *);
#ifdef __cplusplus
}
#endif
#endif
