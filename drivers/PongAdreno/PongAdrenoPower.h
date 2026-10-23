/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PONG_ADRENO_POWER_H
#define PONG_ADRENO_POWER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {PongPowerSuccess,PongPowerArgument,PongPowerCorrupt,
    PongPowerMissing,PongPowerDuplicate,PongPowerRailRange} PongPowerStatus;
typedef struct {uint32_t address;const uint8_t *aux;size_t bytes;} PongPowerResource;
typedef struct {
    uint32_t chip_id,db_version;
    PongPowerResource gfx,mx,cx,sh0,mc0,acv,cn0;
} PongPowerInputs;
typedef struct {uint32_t hz,level,cx_level,bus,bus_min,bus_max,acd;} PongPowerLevel;
/* Captured live Pong DT profile, descending Android order. Runtime speed-bin
 * policy must select only actual enabled levels; never substitute A740 OPPs.
 */
extern const PongPowerLevel PongAdrenoPowerLevels[13];
extern const uint32_t PongAdrenoDdrBandwidth[12];
typedef struct {
    /* Exact modern vendor wire payloads, excluding the HFI message header.
     * PERF has 16 GX entries of (ARC vote,CX index,kHz) and four CX entries.
     */
    uint32_t performance[58],bandwidth[159];
    uint32_t default_gpu_index,default_bus_index;
} PongPowerTables;

/* Parse the actual retained command-db reservation at0x80860000/0x20000.
 * This does not map it, hash it, write it or invent missing resource data.
 * Output spans refer into the caller's immutable retained mapping. No MMIO.
 */
PongPowerStatus PongAdrenoReadPowerInputs(const uint8_t *,size_t,uint32_t chip_id,
    PongPowerInputs *);
/* CPU serialization only: actual gfx/mx/cx aux corners and SH0/MC0/ACV/CN0
 * addresses/aux are required. Rounded primary voltage determines the secondary
 * dependency, and every Pong GPU level gets its actual CX vote. Average DDR
 * bandwidth stays zero; only peak is voted, as in Nothing's gen7_rpmh writer.
 * Success is not a regulator vote, clock change, HFI ACK or ready GPU.
 */
PongPowerStatus PongAdrenoBuildPowerTables(const PongPowerInputs *,PongPowerTables *);
#ifdef __cplusplus
}
#endif
#endif
