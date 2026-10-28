/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PONG_WIFI_PCI_H
#define PONG_WIFI_PCI_H
#include <stddef.h>
#include <stdint.h>

/* PCI access for the QCA6490 ath11k/MHI backend. This is a handoff adapter,
 * not a substitute for powering the board or for the ath11k protocol stack.
 * Open refuses an unpowered/unclocked controller, a link that is down, an
 * unexpected endpoint, or a BAR which has not actually been assigned.
 * It never enables bus mastering or borrows UFS/GPU DMA/context ownership. */
struct PongWifiPci;
enum PongWifiPciResult {
    PongWifiPciOK, PongWifiPciArgument, PongWifiPciMemory,
    PongWifiPciPower, PongWifiPciClock, PongWifiPciLink,
    PongWifiPciIdentity, PongWifiPciBarInvalid, PongWifiPciCapability
};
PongWifiPciResult PongWifiPciOpen(PongWifiPci **out);
void PongWifiPciClose(PongWifiPci *pci);
int PongWifiPciRead(PongWifiPci *, uint32_t offset, unsigned width, uint32_t *value);
int PongWifiPciWrite(PongWifiPci *, uint32_t offset, unsigned width, uint32_t value);
/* Bit 31 of offset selects the endpoint's actual PCI Express capability,
 * as in UniXNU's nw_board.cfg_read/cfg_write contract. */
void *PongWifiPciBar(PongWifiPci *, size_t *bytes);
uint16_t PongWifiPciMsiCapability(PongWifiPci *);
/* A production backend must quiesce submissions, disable MSI/bus mastering,
 * and stop endpoint DMA before Close; Close does not release DMA memory. */
#endif
