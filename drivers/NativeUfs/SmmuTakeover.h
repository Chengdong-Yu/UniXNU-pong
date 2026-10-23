// SPDX-License-Identifier: BSD-2-Clause-Patent
// Plan before any SMMU write. Interface register facts: ARM SMMUv2.
struct UfsSmmuPlan {
  unsigned slot = 0xffffffffU, bank = 0, pages = 0;
  uint32_t smr = 0, s2cr = 0;
  unsigned previousBank = 0xffffffffU;
  bool reuse = false, migrate = false;
  const char *error = nullptr;
};
// ARM IHI 0062D.c sections 1.6 and 3.7: VA64=0 still rejects inputs above
// 32 bits with SCTLR.M=0. With VA64=1 the bypass range is bounded by all
// three hardware widths, not TCR2.PASize (which only limits translation).
static unsigned ufsSmmuAddressBits(unsigned code)
{
  switch (code) {
    case 0: return 32; case 1: return 36; case 2: return 40;
    case 3: return 42; case 4: return 44; case 5: return 48;
    default: return 0;
  }
}
template <class Read>
static UfsSmmuPlan planUfsSmmuBank(uint16_t stream, uint64_t dma, uint64_t bytes, Read read,
                                  unsigned ownedBanks, unsigned ownedGroups, unsigned bankFromTop)
{
  UfsSmmuPlan p;
  uint32_t idr0 = read(0x20), idr2 = read(0x28);
  unsigned count = idr0 & 255;
  if (!(idr0 & (1U << 30)) || !(idr2 & (1U << 12))) {
    p.error = "SMMU AArch64 stage 1 unsupported"; return p;
  }
  for (unsigned shift = 0; shift <= 8; shift += 4) {
    unsigned bits = ufsSmmuAddressBits((idr2 >> shift) & 15);
    if (!bits || !bytes || dma >= (1ULL << bits) || bytes > (1ULL << bits) - dma) {
      p.error = "DMA exceeds SMMU address width"; return p;
    }
  }
  uint32_t idr1 = read(0x24);
  unsigned banks = idr1 & 255;
  // DT and ID registers are independent upper bounds. Firmware can expose
  // fewer resources than the Android DT describes (Pong ID1.NUMCB=75,
  // DT limit=78). Intersect the bounds; never expand the hardware range.
  if (!ownedBanks || !ownedGroups || !banks || !count) {
    p.error = "empty SMMU ownership/capability range"; return p;
  }
  if (banks > ownedBanks) banks = ownedBanks;
  if (count > ownedGroups) count = ownedGroups;
  p.pages = 1U << (((idr1 >> 28) & 7) + 1);
  if ((idr1 >> 31) || !count || p.pages > 128 || banks < 2 || banks > 128 ||
      (p.pages + banks) * 4096 > 0x100000) {
    p.error = "unsupported SMMU geometry"; return p;
  }
  if (count > 128) { p.error = "unsupported stream-match group count"; return p; }
  if (bankFromTop >= banks - 1) { p.error = "no free context bank"; return p; }
  p.bank = banks - 1 - bankFromTop;
  unsigned matches = 0;
  for (unsigned i = 0; i < count; ++i) {
    uint32_t smr = read(0x800 + 4 * i);
    if (!(smr & 0x80000000U)) {
      if (!p.reuse && !p.migrate) p.slot = i;
      continue;
    }
    uint32_t s2cr = read(0xc00 + 4 * i);
    uint32_t mask = (smr >> 16) & 0x7fff;
    bool match = ((smr ^ stream) & 0xffff & ~mask) == 0;
    bool translate = ((s2cr >> 16) & 3) == 0;
    unsigned mappedBank = s2cr & 255;
    bool ourBank = translate && mappedBank == p.bank;
    if (match) {
      ++matches;
      // A single exact firmware mapping for the UFS SID is the handoff to
      // migrate after the queue is stopped. Never accept a masked mapping,
      // a non-translation type, or a context bank outside the owned range.
      if (matches > 1 || mask || !translate || mappedBank >= banks) {
        p.slot = i; p.smr = smr; p.s2cr = s2cr;
        p.error = "conflicting UFS stream mapping"; return p;
      }
      p.reuse = ourBank;
      p.migrate = !ourBank;
      p.previousBank = mappedBank;
      p.slot = i; p.smr = smr; p.s2cr = s2cr;
    } else if (ourBank) {
      p.slot = i; p.smr = smr; p.s2cr = s2cr;
      p.error = "reserved context bank already used"; return p;
    }
  }
  uint32_t sctlr = read((p.pages + p.bank) * 4096);
  if ((sctlr & 1) || (p.reuse && (sctlr || read(0x1000 + 4 * p.bank) != 0x10000 ||
                                (read(0x1800 + 4 * p.bank) & ~1U)))) {
    p.error = "reserved context bank not bypass"; return p;
  }
  if (p.slot == 0xffffffffU) p.error = "no free stream-match group";
  return p;
}

// Upstream's bounded warm-reset fallback, intersected with Pong's owned
// ranges above. Keep exact SID matching, firmware migration and VA64 checks.
template <class Read>
static UfsSmmuPlan planUfsSmmu(uint16_t stream, uint64_t dma, uint64_t bytes, Read read,
                              unsigned ownedBanks, unsigned ownedGroups)
{
  UfsSmmuPlan first = planUfsSmmuBank(stream, dma, bytes, read, ownedBanks, ownedGroups, 0);
  if (!first.error) return first;
  auto retryable = [](const char *error) {
    return !__builtin_strcmp(error, "reserved context bank already used") ||
           !__builtin_strcmp(error, "reserved context bank not bypass");
  };
  // An inexact/masked SID is still an unconditional refusal on Pong; the
  // existing planner already migrates an exact mapping from another bank.
  if (!retryable(first.error)) return first;
  for (unsigned attempt = 1; attempt < 8; ++attempt) {
    UfsSmmuPlan next = planUfsSmmuBank(stream, dma, bytes, read, ownedBanks, ownedGroups, attempt);
    if (!next.error) return next;
    if (!retryable(next.error)) break;
  }
  return first;
}

// The caller must first quiesce the UFS queue. The planner refuses other
// streams sharing this bank and enabled translation; no live context is
// disabled here. Write callbacks must complete an MMIO barrier before return.
template <class Read, class Write>
static const char *configureUfsSmmu(const UfsSmmuPlan &p, uint16_t stream, Read read, Write write)
{
  if (p.error) return p.error;
  unsigned cb = (p.pages + p.bank) * 4096;
  if (!p.reuse) write(cb, 0);
  if (read(cb) != 0) return "SMMU bypass disable readback";
  // Set VA64 before exposing a newly allocated stream. Repair an exclusive
  // existing 32-bit bypass too; do not silently reuse the faulty state.
  if (read(0x1800 + 4 * p.bank) != 1) write(0x1800 + 4 * p.bank, 1);
  if (!p.reuse) write(0x1000 + 4 * p.bank, 0x10000);
  if (read(0x1800 + 4 * p.bank) != 1 || read(0x1000 + 4 * p.bank) != 0x10000 || read(cb) != 0)
    return "SMMU VA64 bypass readback";
  if (!p.reuse) {
    // For a firmware handoff the SMR is already the exact UFS SID. Configure
    // the new bank first, then atomically change only this slot's S2CR.
    write(0xc00 + 4 * p.slot, p.bank);
    if (read(0xc00 + 4 * p.slot) != p.bank) return "SMMU stream context readback";
    if (!p.migrate) write(0x800 + 4 * p.slot, 0x80000000U | stream);
  }
  if (read(0x800 + 4 * p.slot) != (0x80000000U | stream) ||
      read(0xc00 + 4 * p.slot) != p.bank)
    return "SMMU stream mapping readback";
  return nullptr;
}
