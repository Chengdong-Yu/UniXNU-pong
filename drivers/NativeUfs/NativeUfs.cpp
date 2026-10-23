// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM8475/Pong frontend. Optional register/registry probes default off;
// the functional transport only issues read-only storage commands.
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSIterator.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include "../PongProbe.h"
extern "C" bool pong_ufs_transport_start(IOService *);

extern "C" {
volatile uint32_t native_ufs_probe_calls;
volatile uint32_t native_ufs_matched;
volatile uint32_t native_ufs_probe_requested;
volatile uint32_t native_ufs_mapped;
volatile uint32_t native_ufs_status;
volatile uint32_t pong_ufs_report[16];
extern volatile uint64_t armcpu_timer_interrupts;
}

namespace {
constexpr uint64_t kGccUfsPage = 0x00187000ULL;
constexpr uint64_t kUfsHost = 0x01d84000ULL;
constexpr uint64_t kUfsHostBytes = 0x3000ULL;
constexpr uint32_t kUfsStreamId = 0xe0;

enum ReportIndex : unsigned {
  ReportGdsc, ReportAxi, ReportAhb, ReportTx, ReportRx0, ReportRx1,
  ReportUnipro, ReportIce, ReportAux, ReportAggregate,
  ReportCapability, ReportVersion, ReportInterruptStatus, ReportHostStatus,
  ReportHostEnable, ReportStream
};

static uint32_t read32 (uintptr_t base, unsigned offset)
{
  return *reinterpret_cast<volatile uint32_t *> (base + offset);
}

static bool contains (const char *data, size_t length, const char *needle)
{
  size_t wanted = strlen (needle);
  for (size_t at = 0; at + wanted < length; ++at) {
    if ((at == 0 || data[at - 1] == '\0') && data[at + wanted] == '\0' &&
        !memcmp (data + at, needle, wanted)) return true;
  }
  return false;
}

static bool compatible (IOService *provider)
{
  OSObject *property = provider->getProperty ("compatible");
  if (OSData *data = OSDynamicCast (OSData, property)) {
    const char *bytes = static_cast<const char *> (data->getBytesNoCopy ());
    return bytes && (contains (bytes, data->getLength (), "qcom,sm8475-ufshc") ||
                     contains (bytes, data->getLength (), "qcom,sm8450-ufshc"));
  }
  if (OSString *string = OSDynamicCast (OSString, property)) {
    const char *bytes = string->getCStringNoCopy ();
    return bytes && (!strcmp (bytes, "qcom,sm8475-ufshc") ||
                     !strcmp (bytes, "qcom,sm8450-ufshc"));
  }
  return false;
}

static uintptr_t mapPhysical (uint64_t physical, uint64_t bytes,
                              OSSharedPtr<IOMemoryMap> &mapping)
{
  OSSharedPtr<IOMemoryDescriptor> memory (
      IOMemoryDescriptor::withPhysicalAddress (physical, bytes, kIODirectionInOut), OSNoRetain);
  if (!memory) return 0;
  mapping = OSSharedPtr<IOMemoryMap> (
      memory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  return mapping ? static_cast<uintptr_t> (mapping->getVirtualAddress ()) : 0;
}

static unsigned serviceCount (const char *name)
{
  OSSharedPtr<OSDictionary> matching (IOService::serviceMatching (name), OSNoRetain);
  OSSharedPtr<OSIterator> iterator (matching ? IOService::getMatchingServices (matching.get()) : nullptr, OSNoRetain);
  unsigned count = 0;
  while (iterator && iterator->getNextObject ()) ++count;
  return count;
}

// Run outside the matching thread. Waking from this sleep also demonstrates
// that the scheduler and the architectural timer can make forward progress.
static void registrationReport (void *, wait_result_t)
{
  IOLog ("PONG REGISTRY: observer started, waiting for scheduler tick\n");
  PongProbeRecord (1, "timer-scheduler", PongProbeRunning, "waiting for one-second sleep");
  uint64_t before = armcpu_timer_interrupts;
  IOSleep (1000);
  uint64_t after = armcpu_timer_interrupts;
  IOLog ("PONG REGISTRY: timer IRQ before=%llu after=%llu\n", before, after);
  PongProbeRecord (1, "timer-scheduler", after > before ? PongProbePass : PongProbeFail,
                   after > before ? "sleep returned and timer IRQ advanced" : "sleep returned without timer IRQ growth", after - before);
  unsigned platform = serviceCount ("ArmDTPlatform"), cpu = serviceCount ("ArmCpu");
  unsigned controller = serviceCount ("IOCPUInterruptController"), nvram = serviceCount ("IODTNVRAM");
  unsigned ufs = serviceCount ("NativeUfs"), media = serviceCount ("IOMedia");
  IOLog ("PONG REGISTRY: platform=%u cpu=%u cpu-intc=%u nvram=%u ufs-probe=%u\n",
         platform, cpu, controller, nvram, ufs);
  PongProbeRecord (2, "core-registry", platform && cpu && controller && nvram ? PongProbePass : PongProbeFail,
                   "platform/cpu/intc/nvram counts packed as bytes",
                   ((uint64_t)platform << 24) | ((uint64_t)cpu << 16) | ((uint64_t)controller << 8) | nvram);
  IOLog ("PONG REGISTRY: block-device=%u media=%u; diagnostic-only mode skips block transport\n",
         serviceCount ("IOBlockStorageDevice"), media);
  PongProbeRecord (3, "root-media", media ? PongProbePass : PongProbeSkip,
                   media ? "published IOMedia observed" : "no block transport; waiting for real root media", media);
}

static bool probeFailure (const char *reason, uint64_t detail = 0)
{
  native_ufs_status |= 0x80000000U;
  PongProbeRecord (0, "ufs-registers", PongProbeFail, reason, detail);
  return false;
}
}

class NativeUfs : public IOService {
  OSDeclareDefaultStructors (NativeUfs)
public:
  IOService *probe (IOService *, SInt32 *) override;
  bool start (IOService *) override;
};

OSDefineMetaClassAndStructors (NativeUfs, IOService)

IOService *NativeUfs::probe (IOService *provider, SInt32 *score)
{
  ++native_ufs_probe_calls;
  if (!compatible (provider)) return nullptr;
  native_ufs_matched = 1;
  if (score) *score = 1000;
  return IOService::probe (provider, score);
}

bool NativeUfs::start (IOService *provider)
{
  unsigned mode = 0;
  PE_parse_boot_argn ("native-ufs-probe", &mode, sizeof (mode));
  native_ufs_probe_requested = mode;
  unsigned enabled = 1;
  PE_parse_boot_argn ("native-ufs", &enabled, sizeof (enabled));
  if (!enabled || !compatible (provider)) return false;
  IOLog("PONG_UFS BUILD smmu-cap-v2: optional-probe=%u; transport=%s\n", mode,
        mode == 2 ? "disabled by diagnostic mode" : "read-only");
  if (mode == 2) PongProbeRecord (0, "ufs-registers", PongProbeRunning, "validate SM8475 DT and read registers");
  if (!IOService::start (provider)) return probeFailure ("IOService start failed");
  // Refuse a board description from a different Qualcomm generation.  These
  // windows are deliberately validated before the first MMIO read.
  struct Window { const char *name; uint64_t expected; };
  const Window windows[] = {
    {"pong,ufs-hc-base", kUfsHost}, {"pong,ufs-hc-size", kUfsHostBytes},
    {"pong,gcc-ufs-page", kGccUfsPage},
    {"pong,smmu-base", 0x15000000}, {"pong,smmu-size", 0x100000}
  };
  for (const auto &window : windows) {
    OSData *value = OSDynamicCast (OSData, provider->getProperty (window.name));
    uint64_t physical = 0;
    if (!value || value->getLength () != sizeof (physical))
      return probeFailure (window.name);
    memcpy (&physical, value->getBytesNoCopy (), sizeof (physical));
    if (physical != window.expected) return probeFailure (window.name, physical);
  }
  struct Cell { const char *name; uint32_t expected; };
  const Cell cells[] = {{"pong,smmu-stream-id", kUfsStreamId}, {"pong,ufs-spi", 265},
                       {"pong,smmu-context-banks", 78}, {"pong,smmu-match-groups", 120}};
  for (const auto &cell : cells) {
    OSData *value = OSDynamicCast (OSData, provider->getProperty (cell.name));
    uint32_t observed = 0;
    if (!value || value->getLength () != sizeof (observed))
      return probeFailure (cell.name);
    memcpy (&observed, value->getBytesNoCopy (), sizeof (observed));
    if (observed != cell.expected) return probeFailure (cell.name, observed);
  }
  IOLog ("PONG_UFS: matched SM8475, validate firmware clocks\n");
  native_ufs_status = 1;

  OSSharedPtr<IOMemoryMap> gccMap, hostMap;
  uintptr_t gcc = mapPhysical (kGccUfsPage, 0x1000, gccMap);
  if (!gcc) return probeFailure ("GCC map failed");
  native_ufs_mapped = 1;
  native_ufs_status = 2;

  // Offsets are from upstream gcc-sm8450.c, which also matches qcom,sm8475-gcc.
  pong_ufs_report[ReportGdsc] = read32 (gcc, 0x004);
  pong_ufs_report[ReportAxi] = read32 (gcc, 0x018);
  pong_ufs_report[ReportAhb] = read32 (gcc, 0x020);
  pong_ufs_report[ReportTx] = read32 (gcc, 0x024);
  pong_ufs_report[ReportRx0] = read32 (gcc, 0x028);
  pong_ufs_report[ReportRx1] = read32 (gcc, 0x0c0);
  pong_ufs_report[ReportUnipro] = read32 (gcc, 0x064);
  pong_ufs_report[ReportIce] = read32 (gcc, 0x06c);
  pong_ufs_report[ReportAux] = read32 (gcc, 0x0a4);
  pong_ufs_report[ReportAggregate] = read32 (gcc, 0x0d4);
  pong_ufs_report[ReportStream] = kUfsStreamId;

  bool powered = (pong_ufs_report[ReportGdsc] & 0x80000000U) != 0;
  bool buses = (pong_ufs_report[ReportAxi] & 1U) &&
               (pong_ufs_report[ReportAhb] & 1U) &&
               (pong_ufs_report[ReportAggregate] & 1U);
  if (powered && buses) {
    IOLog ("PONG_UFS: clocks active, read host state\n");
    uintptr_t host = mapPhysical (kUfsHost, kUfsHostBytes, hostMap);
    if (!host) return probeFailure ("host map failed");
    native_ufs_status = 3;
    pong_ufs_report[ReportCapability] = read32 (host, 0x00);
    pong_ufs_report[ReportVersion] = read32 (host, 0x08);
    pong_ufs_report[ReportInterruptStatus] = read32 (host, 0x20);
    pong_ufs_report[ReportHostStatus] = read32 (host, 0x30);
    pong_ufs_report[ReportHostEnable] = read32 (host, 0x34);
    native_ufs_status = 4;
  }

  IOLog ("PONG_UFS READ ONLY\n"
         "gdsc=%08x axi=%08x ahb=%08x aggr=%08x\n"
         "tx=%08x rx0=%08x rx1=%08x unipro=%08x\n"
         "ice=%08x aux=%08x powered=%u buses=%u sid=%x\n"
         "cap=%08x ver=%08x is=%08x hcs=%08x hce=%08x\n",
         pong_ufs_report[ReportGdsc], pong_ufs_report[ReportAxi],
         pong_ufs_report[ReportAhb], pong_ufs_report[ReportAggregate],
         pong_ufs_report[ReportTx], pong_ufs_report[ReportRx0],
         pong_ufs_report[ReportRx1], pong_ufs_report[ReportUnipro],
         pong_ufs_report[ReportIce], pong_ufs_report[ReportAux], powered, buses,
         pong_ufs_report[ReportStream], pong_ufs_report[ReportCapability],
         pong_ufs_report[ReportVersion], pong_ufs_report[ReportInterruptStatus],
         pong_ufs_report[ReportHostStatus], pong_ufs_report[ReportHostEnable]);
  if (!powered || !buses) return probeFailure ("power/clocks unavailable; host access skipped");
  if (!pong_ufs_report[ReportVersion] || pong_ufs_report[ReportVersion] == 0xffffffffU ||
      !(pong_ufs_report[ReportHostEnable] & 1U))
    return probeFailure ("host disabled or invalid version", pong_ufs_report[ReportHostEnable]);
  setProperty ("PongProbeOnly", mode == 2);
  setProperty ("PongUfsStatus", static_cast<unsigned long long> (native_ufs_status), 32);
  uint32_t snapshot[16];
  for (unsigned index = 0; index < 16; ++index) snapshot[index] = pong_ufs_report[index];
  setProperty ("PongUfsRegisterSnapshot", snapshot, sizeof (snapshot));
  if (mode != 2) {
    if (!pong_ufs_transport_start(this)) return false;
    registerService();
    return true;
  }
  registerService ();
  IOLog ("PONG_UFS: diagnostic service registered; continue IOKit, no block media published\n");
  PongProbeRecord (0, "ufs-registers", PongProbePass, "registers read; diagnostic service published", pong_ufs_report[ReportVersion]);
  thread_t thread = nullptr;
  kern_return_t result = kernel_thread_start (registrationReport, nullptr, &thread);
  if (result == KERN_SUCCESS) thread_deallocate (thread);
  else PongProbeRecord (1, "timer-scheduler", PongProbeFail, "observer thread creation failed", result);
  return true;
}
