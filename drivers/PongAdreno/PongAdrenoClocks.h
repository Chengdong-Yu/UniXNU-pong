/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PONG_ADRENO_CLOCKS_H
#define PONG_ADRENO_CLOCKS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {PongClockGpucc,PongClockGcc} PongClockWindow;
typedef struct {
    void *cookie;
    /* All callbacks return0 on success, nonzero on failure. */
    int (*read32)(void *,PongClockWindow,size_t,uint32_t *);
    int (*write32)(void *,PongClockWindow,size_t,uint32_t);
    int (*barrier)(void *);
    int (*now_us)(void *,uint64_t *);
    int (*wait_us)(void *,unsigned);
    /* Real platform checks before access and mutation: exclusive Cape/GCC
     * leases, stopped GPU/GMU, live MMIO and retained parent CX/MX votes.
     * Check the existing GPLL0 at600MHz, its divided parent at300MHz and
     * TCXO at19.2MHz. A configured register is not a measured clock.
     */
    int (*prerequisites)(void *);
} PongClockOps;
typedef struct {unsigned attempted,ready,poisoned;} PongClockState;
typedef struct {
    unsigned phase,writes,polls,resources_must_retain;
    PongClockWindow window;
    uint32_t offset,value;
} PongClockResult;
typedef enum {PongClockSuccess,PongClockArgument,PongClockPrerequisite,
    PongClockBusy,PongClockIo,PongClockTimeout,PongClockPoisoned} PongClockStatus;
/* Pong Cape GPUCC0x03d90000/0xa000, GCC0x00100000/0x1f4200 only.
 * Actual bounded CX GDSC and early GMU200MHz/HUB150MHz/XO configuration.
 * Configure stopped Cape LUCID_OLE GPU PLL0/1 using SM8475 probe values;
 * retain running PLLs. Initial GMU/HUB clocks use already running GPLL0
 * parents; the GMU owns subsequent GX rate changes and PLL enabling.
 * A successful enable retains all clocks/parent votes. It does not
 * establish native SMMU ownership, secure firmware, CP execution or scanout.
 * Failure poisons state and retains resources; do not blindly undo votes
 * while a partially started master may still issue traffic.
 */
PongClockStatus PongAdrenoEnableCxClocks(const PongClockOps *,PongClockState *,PongClockResult *);
#ifdef __cplusplus
}
#endif
#endif
