/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "PongWifiPci.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>

/* Resource facts from Pong's live DT and PCI sysfs; clock offsets from
 * Nothing SM8475 / Linux gcc-sm8450.c. No SM8550 clock addresses or WCN7850
 * endpoint IDs occur in this adapter. The DT profile retains the separate
 * power, PHY, GPIO, stream and active ITS inputs for the board sequencer. */
struct Region {
    IOMemoryMap *map = nullptr;
    volatile uint8_t *bytes = nullptr;
    bool open(uint64_t base, size_t length) {
        IODeviceMemory *memory = IODeviceMemory::withRange(base, length);
        if (!memory) return false;
        map = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
        memory->release();
        bytes = map ? reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress()) : nullptr;
        return bytes != nullptr;
    }
    uint32_t read32(size_t offset) const {
        const uint32_t v = *reinterpret_cast<volatile uint32_t *>(bytes + offset);
        __asm__ volatile("dmb oshld" ::: "memory");
        return v;
    }
    void close() { if (map) map->release(); map = nullptr; bytes = nullptr; }
};

struct PongWifiPci {
    Region gcc, dbi, config, bar;
    uint16_t pcie = 0, msi = 0;
    IOLock *configLock = nullptr;
};

void PongWifiPciClose(PongWifiPci *p)
{
    if (!p) return;
    p->bar.close(); p->config.close(); p->dbi.close(); p->gcc.close();
    if (p->configLock) IOLockFree(p->configLock);
    IODelete(p, PongWifiPci, 1);
}

static bool config_offset(PongWifiPci *p, uint32_t *offset, unsigned width)
{
    if (!p || !p->config.bytes || (width != 1 && width != 2 && width != 4)) return false;
    uint32_t off = *offset;
    if (off & 0x80000000u) {
        off &= 0x7fffffffu;
        if (!p->pcie || off >= 0x1000u - p->pcie) return false;
        off += p->pcie;
    }
    if (off > 0x1000u - width || (off & (width - 1))) return false;
    *offset = off;
    return true;
}

int PongWifiPciRead(PongWifiPci *p, uint32_t offset, unsigned width, uint32_t *value)
{
    if (!value || !config_offset(p, &offset, width)) return -1;
    IOLockLock(p->configLock);
    volatile uint8_t *at = p->config.bytes + offset;
    *value = width == 1 ? *at : width == 2 ?
        *reinterpret_cast<volatile uint16_t *>(at) : *reinterpret_cast<volatile uint32_t *>(at);
    __asm__ volatile("dmb oshld" ::: "memory");
    IOLockUnlock(p->configLock);
    return 0;
}

int PongWifiPciWrite(PongWifiPci *p, uint32_t offset, unsigned width, uint32_t value)
{
    if (!config_offset(p, &offset, width)) return -1;
    /* DMA activation and BAR reassignment belong to the owner which has
     * established the endpoint's SMMU stream/MSI mapping. This config helper
     * must not expose those writes before that implementation is connected. */
    if (offset < 0x40) return -1;
    IOLockLock(p->configLock);
    volatile uint8_t *at = p->config.bytes + offset;
    /* Native byte enables avoid clearing adjacent write-one-to-clear PCI
     * status bits, which a dword read/modify/write would accidentally do. */
    if (width == 1) *at = (uint8_t)value;
    else if (width == 2) *reinterpret_cast<volatile uint16_t *>(at) = (uint16_t)value;
    else *reinterpret_cast<volatile uint32_t *>(at) = value;
    __asm__ volatile("dsb oshst" ::: "memory");
    IOLockUnlock(p->configLock);
    return 0;
}

void *PongWifiPciBar(PongWifiPci *p, size_t *bytes)
{
    if (!p || !bytes || !p->bar.bytes) return nullptr;
    *bytes = 0x200000;
    return const_cast<uint8_t *>(p->bar.bytes);
}
uint16_t PongWifiPciMsiCapability(PongWifiPci *p) { return p ? p->msi : 0; }

PongWifiPciResult PongWifiPciOpen(PongWifiPci **out)
{
    if (!out || *out) return PongWifiPciArgument;
    PongWifiPci *p = IONewZero(PongWifiPci, 1);
    if (!p) return PongWifiPciMemory;
    auto fail = [&](PongWifiPciResult result) { PongWifiPciClose(p); return result; };
    p->configLock = IOLockAlloc();
    if (!p->configLock || !p->gcc.open(0x100000, 0xa0000)) return fail(PongWifiPciMemory);
    if (!(p->gcc.read32(0x7b004) & (1u << 31))) return fail(PongWifiPciPower);
    /* Never touch the controller's DBI/config view with its bus clocks off.
     * These are SM8475 PCIe0 gates, not Sheng's 0x6bxxx/0x520xx branches. */
    const unsigned branches[] = {0x7b020, 0x7b028, 0x7b030, 0x7b034, 0x7b03c};
    for (unsigned offset : branches)
        if (p->gcc.read32(offset) & (1u << 31)) return fail(PongWifiPciClock);
    if (!p->dbi.open(0x60000000, 0x1000)) return fail(PongWifiPciMemory);
    if (!(p->dbi.read32(0x72c) & (1u << 4)) || (p->dbi.read32(0x728) & 0x3f) != 0x11)
        return fail(PongWifiPciLink);
    /* This opens an existing root-port handoff; it does not silently change
     * the iATU while another controller owner could have outstanding access. */
    if (!p->config.open(0x60100000, 0x1000)) return fail(PongWifiPciMemory);
    if (p->config.read32(0) != 0x110317cb || p->config.read32(0x2c) != 0x010817cb)
        return fail(PongWifiPciIdentity);
    uint32_t bar = p->config.read32(0x10);
    if ((bar & 0xfffffff0) != 0x60200000 || (bar & 1) || (bar & 6) != 4 ||
        p->config.read32(0x14) != 0 || !(p->config.read32(4) & 2)) return fail(PongWifiPciBarInvalid);
    unsigned cap = p->config.read32(0x34) & 0xff;
    uint64_t visited = 0;
    for (unsigned guard = 0; cap && guard < 48; ++guard) {
        if (cap < 0x40 || cap > 0xfc || (cap & 3) || (visited & (1ull << (cap / 4))))
            return fail(PongWifiPciCapability);
        visited |= 1ull << (cap / 4);
        uint32_t value = p->config.read32(cap);
        if ((value & 0xff) == 0x10) p->pcie = (uint16_t)cap;
        if ((value & 0xff) == 0x05) p->msi = (uint16_t)cap;
        cap = (value >> 8) & 0xff;
    }
    if (cap || !p->pcie || !p->msi) return fail(PongWifiPciCapability);
    if (!p->bar.open(0x60200000, 0x200000)) return fail(PongWifiPciMemory);
    *out = p;
    return PongWifiPciOK;
}
