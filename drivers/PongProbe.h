// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PONG_PROBE_H
#define PONG_PROBE_H

// Optional probes report a result and return. A FAIL must never publish a
// successful hardware resource or touch later MMIO stages that need it.
enum PongProbeState : unsigned {
  PongProbePass = 1, PongProbeFail = 2, PongProbeSkip = 3, PongProbeRunning = 4
};

static inline void PongProbeRecord(unsigned slot, const char *name,
                                   PongProbeState state, const char *reason,
                                   uint64_t detail = 0)
{
  (void)slot;
  (void)name;
  (void)state;
  (void)reason;
  (void)detail;
}
#endif
