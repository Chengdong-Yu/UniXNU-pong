// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pong/SM8475 UFS transport. Writes are boot-arg gated and limited to the
// declared SHNGROOT windows, including the independent persistent APFS Data.
// Shared queue/protocol code derives
// from Sheng; board addresses and stream ID come from Pong's live DT.
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOBSD.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/IOLib.h>
#include <vm/vm_kern.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSString.h>
#include <libkern/OSKextLib.h>
#include <libkern/crypto/sha2.h>
#include <pexpert/pexpert.h>
#include <sys/param.h>
#include <sys/types.h>
#include "../PongProbe.h"

namespace {
#include "RootTransportLayout.h"
#include "RootDiagnostics.h"
#include "TransferPoll.h"
#include "SmmuTakeover.h"
static UfsRootDiagnostics gRootDiagnostic;
struct UfsHost {
  OSSharedPtr<IOMemoryMap> hcMap, smmuMap;
  uintptr_t hc = 0, smmu = 0, cb = 0;
  IOBufferMemoryDescriptor *dma = nullptr;
  uint8_t *virt = nullptr;
  uint64_t phys = 0, blocks = 0;
  IOLock *lock = nullptr;
  bool ready = false, poisoned = false;
  bool writable = false;
};
static UfsHost gHost;
static bool gTransportAttempted;
enum : uint64_t {
  // Pong LUN0 GPT entry "userdata", captured from this phone in recovery.
  kPongUserdataFirst4k = 0x413248ULL,
  kPongUserdataLast4k = 0x75a40eaULL,
  kRootLogicalBlock = 512,
};
struct RootDiskWindow {
  const char *name;
  const char *product;
  const char *signature;
  unsigned signatureOffset;
  uint64_t baseByte;
  uint64_t size;
};
static RootDiskWindow gRootWindows[3];
static unsigned gRootWindowCount;
static uint32_t read32(uintptr_t base, unsigned offset) {
  return *reinterpret_cast<volatile uint32_t *>(base + offset);
}
static void write32(uintptr_t base, unsigned offset, uint32_t value) {
  __asm__ volatile("dsb sy" ::: "memory");
  *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
  __asm__ volatile("dsb sy" ::: "memory");
}
static uintptr_t mapPhys(uint64_t base, uint64_t size, OSSharedPtr<IOMemoryMap> &keep) {
  OSSharedPtr<IOMemoryDescriptor> memory(IOMemoryDescriptor::withPhysicalAddress(base, size, kIODirectionInOut), OSNoRetain);
  if (memory) keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  return keep ? keep->getVirtualAddress() : 0;
}
static IOReturn recordTransport(IOReturn result, uint32_t ocs) {
  gRootDiagnostic.result = static_cast<uint32_t>(result); gRootDiagnostic.ocs = ocs;
  gRootDiagnostic.hcs = read32(gHost.hc, 0x30);
  gRootDiagnostic.hce = read32(gHost.hc, 0x34);
  gRootDiagnostic.doorbell = read32(gHost.hc, 0x58);
  gRootDiagnostic.interrupt = read32(gHost.hc, 0x20);
  gRootDiagnostic.list = uint64_t(read32(gHost.hc, 0x50)) | (uint64_t(read32(gHost.hc, 0x54)) << 32);
  gRootDiagnostic.runStop = read32(gHost.hc, 0x60);
  gRootDiagnostic.ahit = read32(gHost.hc, 0x18);
  for (unsigned i = 0; i < 5; ++i) gRootDiagnostic.uic[i] = read32(gHost.hc, 0x38 + i * 4);
  if (gHost.smmu && gHost.cb) {
    gRootDiagnostic.gfsr = read32(gHost.smmu, 0x48);
    gRootDiagnostic.gsyn = read32(gHost.smmu, 0x54);
    gRootDiagnostic.fsr = read32(gHost.cb, 0x58);
    gRootDiagnostic.fsyn = read32(gHost.cb, 0x68);
    gRootDiagnostic.far = uint64_t(read32(gHost.cb, 0x60)) | (uint64_t(read32(gHost.cb, 0x64)) << 32);
  }
  return result;
}
static void cacheCivac(const void *start, size_t size) {
  uintptr_t end = reinterpret_cast<uintptr_t>(start) + size;
  for (uintptr_t at = reinterpret_cast<uintptr_t>(start) & ~uintptr_t(63); at < end; at += 64)
    __asm__ volatile("dc civac, %0" :: "r"(at) : "memory");
  __asm__ volatile("dsb sy" ::: "memory");
}
static void cacheInvalidate(const void *start, size_t size) {
  __asm__ volatile("dsb sy" ::: "memory");
  uintptr_t end = reinterpret_cast<uintptr_t>(start) + size;
  for (uintptr_t at = reinterpret_cast<uintptr_t>(start); at < end; at += 64)
    __asm__ volatile("dc ivac, %0" :: "r"(at) : "memory");
  __asm__ volatile("dsb sy" ::: "memory");
}
static void hostWrite(unsigned offset, uint32_t value) { write32(gHost.hc, offset, value); }
#include "RootQueue.h"
#include "RootCommands.h"
#include "RootLayout.h"

static bool transportFailure(const char *why) {
  gHost.ready = false;
  // No panic, reset, retry or DMA release after a timeout. The controller
  // may still own the allocation. Preserve evidence and let BSD continue.
  char text[2400];
  formatUfsRootDiagnostics(text, sizeof(text), gRootDiagnostic, why, "PONG_UFS TRANSPORT");
  IOLog("%s\nPONG_UFS: no media published; continue boot\n", text);
  PongProbeRecord(4, "ufs-transport", PongProbeFail, why, gRootDiagnostic.result);
  snprintf(text, sizeof(text), "stage=%s op=%x ocs=%x scsi=%x protocol=%u", gRootDiagnostic.stage,
           gRootDiagnostic.opcode, gRootDiagnostic.ocs, gRootDiagnostic.scsi, gRootDiagnostic.protocol);
  PongProbeRecord(5, "ufs-command", PongProbeFail, text, gRootDiagnostic.dma);
  snprintf(text, sizeof(text), "hcs=%x db=%x is=%x fsr=%x syn=%x", gRootDiagnostic.hcs,
           gRootDiagnostic.doorbell, gRootDiagnostic.interrupt, gRootDiagnostic.fsr, gRootDiagnostic.fsyn);
  PongProbeRecord(6, "ufs-hardware", PongProbeFail, text, gRootDiagnostic.far);
  return false;
}

static bool hostBringUp() {
  uint32_t requestWrites = 0;
  gHost.writable = PE_parse_boot_argn("pong_ufs_readwrite", &requestWrites, sizeof(requestWrites)) &&
      requestWrites == 1;
  IOLog("PONG_UFS: storage write path %s by pong_ufs_readwrite boot-arg\n",
        gHost.writable ? "enabled" : "disabled");
  gRootDiagnostic.stage = "controller map";
  gHost.hc = mapPhys(0x01d84000, 0x3000, gHost.hcMap);
  if (!gHost.hc) return transportFailure("controller map");
  gRootDiagnostic.cap = read32(gHost.hc, 0);
  gRootDiagnostic.version = read32(gHost.hc, 8);
  recordTransport(kIOReturnNotReady, 15);
  gRootDiagnostic.stage = "DMA allocation";
  gHost.dma = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
    IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous) | IOOptionBits(kIOMemoryMapperNone), kDmaBytes, 4096);
  gHost.lock = IOLockAlloc();
  if (!gHost.dma || !gHost.lock) return transportFailure("DMA allocation");
  gHost.virt = static_cast<uint8_t *>(gHost.dma->getBytesNoCopy());
  IOByteCount contiguous = 0;
  gHost.phys = gHost.dma->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
  gRootDiagnostic.dma = gHost.phys; gRootDiagnostic.contiguous = contiguous;
  if (!gHost.virt || !gHost.phys || (gHost.phys & 4095) || contiguous < kDmaBytes ||
      (!(gRootDiagnostic.cap & 0x01000000) && gHost.phys + kDmaBytes > 0x100000000ULL))
    return transportFailure("DMA address/contiguity");
  bzero(gHost.virt, kDmaBytes); cacheCivac(gHost.virt, kDmaBytes);
  gRootDiagnostic.stage = "quiesce firmware queue";
  const char *why = nullptr;
  if (!hostStopQueue(&why)) return transportFailure(why);
  gRootDiagnostic.stage = "SMMU plan";
  gHost.smmu = mapPhys(0x15000000, 0x100000, gHost.smmuMap);
  if (!gHost.smmu) return transportFailure("SMMU map");
  auto read = [](unsigned offset) { return read32(gHost.smmu, offset); };
  UfsSmmuPlan plan = planUfsSmmu(0xe0, gHost.phys, kDmaBytes, read, 78, 120);
  gRootDiagnostic.smmuId1 = read(0x24); gRootDiagnostic.smmuId2 = read(0x28);
  gRootDiagnostic.smmuSlot = plan.slot; gRootDiagnostic.smr = plan.smr; gRootDiagnostic.s2cr = plan.s2cr;
  unsigned rawBanks = gRootDiagnostic.smmuId1 & 255, rawGroups = read(0x20) & 255;
  IOLog("PONG_UFS: SMMU id0=%08x id1=%08x id2=%08x HW banks=%u groups=%u effective banks=%u groups=%u\n",
        read(0x20), gRootDiagnostic.smmuId1, gRootDiagnostic.smmuId2, rawBanks, rawGroups,
        rawBanks < 78 ? rawBanks : 78, rawGroups < 120 ? rawGroups : 120);
  if (plan.error) return transportFailure(plan.error);
  IOLog("PONG_UFS: SID e0 slot=%u firmware-bank=%u target-bank=%u action=%s\n",
        plan.slot, plan.previousBank, plan.bank,
        plan.migrate ? "migrate exact firmware mapping" : plan.reuse ? "reuse bypass" : "install exact mapping");
  gHost.cb = gHost.smmu + (plan.pages + plan.bank) * 4096;
  gRootDiagnostic.stage = "SMMU stream configuration";
  why = configureUfsSmmu(plan, 0xe0, read, [](unsigned offset, uint32_t value) { write32(gHost.smmu, offset, value); });
  gRootDiagnostic.smr = read(0x800 + 4 * plan.slot); gRootDiagnostic.s2cr = read(0xc00 + 4 * plan.slot);
  gRootDiagnostic.cbar = read(0x1000 + 4 * plan.bank); gRootDiagnostic.cba2r = read(0x1800 + 4 * plan.bank);
  gRootDiagnostic.sctlr = read32(gHost.cb, 0);
  if (why) return transportFailure(why);
  gRootDiagnostic.stage = "request list start";
  if (!hostStartQueue(&why)) return transportFailure(why);
  gRootDiagnostic.stage = "NOP OUT";
  if (hostNop() != kIOReturnSuccess) return transportFailure("NOP OUT");
  uint32_t power = 0, scsi = 0;
  gRootDiagnostic.stage = "power mode query";
  if (hostQueryRead(3, 2, &power) != kIOReturnSuccess) return transportFailure("power mode query");
  gRootDiagnostic.power = power;
  if ((power & 0xff) != 0x11) {
    gRootDiagnostic.stage = "wake device";
    uint8_t ssu[6] = {0x1b, 0, 0, 0, 0x10, 0};
    if (hostScsi(0xd0, ssu, 6, false, 0, 500000, &scsi) != kIOReturnSuccess) return transportFailure("START STOP UNIT");
    IOSleep(50);
  }
  gRootDiagnostic.stage = "LUN 0 ready";
  for (unsigned attempt = 0; attempt < 20 && !gHost.poisoned; ++attempt) {
    uint8_t tur[6] = {};
    if (hostScsi(0, tur, 6, false, 0, 200000, &scsi) == kIOReturnSuccess) { gHost.ready = true; break; }
    IOSleep(10);
  }
  if (!gHost.ready) return transportFailure("LUN 0 not ready");
  gRootDiagnostic.stage = "READ CAPACITY";
  uint8_t capacity[10] = {0x25};
  if (hostScsi(0, capacity, 10, true, 8, 200000, &scsi) != kIOReturnSuccess) return transportFailure("READ CAPACITY");
  uint32_t last = loadBe32(gHost.virt + kDataOffset), blockSize = loadBe32(gHost.virt + kDataOffset + 4);
  if (!last || last == 0xffffffff || blockSize != 4096) return transportFailure("unsupported LUN geometry");
  gHost.blocks = uint64_t(last) + 1;
  IOLog("PONG_UFS: transport ready LUN=0 block=4096 blocks=%llu; WRITE(10) FUA=%s; writes stay inside declared SHNGROOT windows\n",
        gHost.blocks, gHost.writable ? "enabled" : "disabled");
  return true;
}
}

class PongUfsBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(PongUfsBlock)
  uint8_t *bounce = nullptr;
  IOLock *mutex = nullptr;
  bool online = false;
  RootDiskWindow window = {};
public:
  bool init(OSDictionary *properties, const RootDiskWindow &selected);
  bool start(IOService *provider) override;
  void free() override;
  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnNotWritable; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  char *getVendorString() override { return const_cast<char *>("Pong"); }
  char *getProductString() override { return const_cast<char *>(window.product); }
  char *getRevisionString() override { return const_cast<char *>("1"); }
  char *getAdditionalDeviceInfoString() override { return const_cast<char *>(window.name); }
  IOReturn reportBlockSize(UInt64 *v) override { *v = kRootLogicalBlock; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock(UInt64 *v) override { *v = window.size / kRootLogicalBlock - 1; return kIOReturnSuccess; }
  IOReturn reportEjectability(bool *v) override { *v = false; return kIOReturnSuccess; }
  IOReturn reportRemovability(bool *v) override { *v = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection(bool *v) override {
    *v = !gHost.writable || (window.baseByte & 4095) || (window.size & 4095);
    return kIOReturnSuccess;
  }
  IOReturn reportMediaState(bool *v, bool *changed) override { *v = online && gHost.ready; if (changed) *changed = false; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache() override { return hostSyncCache(); }
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache(); }
  IOReturn getWriteCacheState(bool *v) override { *v = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState(bool) override { return kIOReturnNotWritable; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *, IOStorageCompletion *) override;
};
OSDefineMetaClassAndStructors(PongUfsBlock, IOBlockStorageDevice)

// If an expected APFS volume is absent, list only IOMedia objects beneath the
// selected UFS window. This keeps the failure report specific to the
// SystemOS image and reveals whether the UUID, the volume, or its ancestry is
// the mismatch.
static unsigned native_pong_log_ufs_window_media(const char *windowKey) {
  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  OSIterator *iterator = matching ? IOService::getMatchingServices(matching) : nullptr;
  OSSafeReleaseNULL(matching);
  if (!iterator) {
    IOLog("PONG APFS: window media enumeration unavailable window=%s\n", windowKey);
    return 0;
  }

  unsigned candidates = 0;
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    if (!media) continue;

    bool targetWindow = false;
    bool readOnlyWindow = false;
    bool writableWindow = false;
    unsigned depth = 0;
    for (IOService *provider = media; provider && depth < 64;
         provider = provider->getProvider(), ++depth) {
      PongUfsBlock *block = OSDynamicCast(PongUfsBlock, provider);
      if (!block) continue;
      targetWindow = block->getProperty(windowKey) == kOSBooleanTrue;
      readOnlyWindow = block->getProperty("PongReadOnly") == kOSBooleanTrue;
      writableWindow = block->getProperty("PongWritable") == kOSBooleanTrue;
      break;
    }
    if (!targetWindow) continue;

    ++candidates;
    OSString *uuid = OSDynamicCast(OSString, media->getProperty("UUID"));
    OSString *name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
    IOLog("PONG APFS: window media uuid=%s bsd=%s readonly=%d writable=%d depth=%u\n",
          uuid ? uuid->getCStringNoCopy() : "?",
          name ? name->getCStringNoCopy() : "?", readOnlyWindow, writableWindow, depth);
  }
  iterator->release();
  IOLog("PONG APFS: window media count=%u window=%s\n", candidates, windowKey);
  return candidates;
}

// Development APFS handoff: resolve a unique APFS IOMedia by its observed
// volume UUID and prove that it descends from the matching SHNGROOT UFS window.
// This is device ancestry, not cryptographic image authentication.
extern "C" int native_pong_ufs_volume_device(const char *uuid, int kind,
                                               dev_t *device, char *path, size_t pathSize) {
  if (!uuid || strlen(uuid) != 36 || !device || !path || pathSize < 8 || kind<0 || kind>2) return 0;
  const char *windowName=kind==2?"Data":kind==1?"SystemOS":"OS";
  const char *windowKey=kind==2?"PongDataWindow":kind==1?"PongSystemOSWindow":"PongRootWindow";
  *device = NODEV;
  path[0] = 0;

  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity(1);
  OSString *wanted = OSString::withCString(uuid);
  if (!matching || !properties || !wanted) {
    OSSafeReleaseNULL(wanted);
    OSSafeReleaseNULL(properties);
    OSSafeReleaseNULL(matching);
    return 0;
  }
  bool ready = properties->setObject("UUID", wanted) &&
      matching->setObject("IOPropertyMatch", properties);
  wanted->release();
  properties->release();
  OSIterator *iterator = nullptr;
  if (ready && kind) {
    IOLog("PONG APFS: wait for %s UUID %.36s, timeout=5s\n", windowName,uuid);
    auto published = IOService::waitForMatchingService(matching, 5ULL * 1000000000ULL);
    if (published) {
      IOLog("PONG APFS: %s UUID appeared; rechecking IOMedia\n",windowName);
      iterator = IOService::getMatchingServices(matching);
    } else {
      IOLog("PONG APFS: %s UUID wait timed out\n",windowName);
    }
  } else if (ready) {
    iterator = IOService::getMatchingServices(matching);
  }
  matching->release();
  if (!iterator) {
    IOLog("PONG APFS: UUID lookup unavailable uuid=%.36s window=%s\n", uuid,
          windowName);
    if (kind) native_pong_log_ufs_window_media(windowKey);
    return 0;
  }

  unsigned matches = 0;
  bool accepted = false;
  dev_t selected = NODEV;
  char selectedPath[64] = {};
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    if (!media) continue;
    ++matches;
    if (matches != 1) { accepted = false; break; }
    OSString *identity = OSDynamicCast(OSString, media->getProperty("UUID"));
    OSString *name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
    OSNumber *major = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMajorKey));
    OSNumber *minor = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMinorKey));
    if (!identity || strcmp(identity->getCStringNoCopy(), uuid) || !name || !major || !minor ||
        !name->getLength() || name->getLength() >= sizeof(selectedPath) - 6 ||
        name->getLength() >= pathSize - 6 || major->unsigned64BitValue() > 255 ||
        minor->unsigned64BitValue() > 0xffffff) {
      IOLog("PONG APFS: UUID candidate malformed uuid=%.36s identity=%d bsd=%d major=%d minor=%d\n",
            uuid, identity != nullptr, name != nullptr, major != nullptr, minor != nullptr);
      continue;
    }

    bool validWindow = false;
    bool foundUfsWindow = false;
    unsigned depth = 0;
    for (IOService *provider = media; provider && depth < 64;
         provider = provider->getProvider(), ++depth) {
      PongUfsBlock *block = OSDynamicCast(PongUfsBlock, provider);
      if (!block) continue;
      foundUfsWindow = true;
      validWindow = block->getProperty(windowKey) == kOSBooleanTrue &&
          block->getProperty("PongUfsWindow") == kOSBooleanTrue;
      if(kind==2) {
        validWindow=validWindow && block->getProperty("PongWritable")==kOSBooleanTrue &&
            media->getProperty(kIOMediaWritableKey)==kOSBooleanTrue;
      }
      break;
    }
    if (!validWindow) {
      IOLog("PONG APFS: UUID candidate rejected uuid=%.36s bsd=%s ufs-window=%d depth=%u window=%s\n",
            uuid, name->getCStringNoCopy(), foundUfsWindow, depth,
            windowName);
      continue;
    }

    selected = static_cast<dev_t>(makedev(major->unsigned32BitValue(), minor->unsigned32BitValue()));
    snprintf(selectedPath, sizeof(selectedPath), "/dev/%s", name->getCStringNoCopy());
    accepted = true;
  }
  iterator->release();
  if (!accepted || matches != 1) {
    IOLog("PONG APFS: UUID lookup miss wanted=%.36s matches=%u accepted=%d window=%s\n",
          uuid, matches, accepted, windowName);
    if (kind) native_pong_log_ufs_window_media(windowKey);
    return 0;
  }
  *device = selected;
  strlcpy(path, selectedPath, pathSize);
  IOLog("PONG APFS: UUID %.36s bound to %s UFS window %s\n", uuid,
        windowName, selectedPath);
  return 1;
}

extern "C" int native_pong_ufs_data_volume_device(const char *uuid,dev_t *device,
    char *path,size_t pathSize,uint64_t *bytes) {
  if(!bytes)return 0;
  *bytes=0;
  if(!native_pong_ufs_volume_device(uuid,2,device,path,pathSize))return 0;
  for(unsigned i=0;i<gRootWindowCount;++i)if(!strcmp(gRootWindows[i].name,"Data")) {
    *bytes=gRootWindows[i].size;return 1;
  }
  return 0;
}

bool PongUfsBlock::init(OSDictionary *properties, const RootDiskWindow &selected) {
  if (!selected.name || !selected.product || !selected.signature || !selected.size ||
      (selected.baseByte & 4095) || (selected.size & 511) ||
      !IOBlockStorageDevice::init(properties)) return false;
  window = selected;
  return true;
}
bool PongUfsBlock::start(IOService *provider) {
  bounce = static_cast<uint8_t *>(IOMalloc(4096)); mutex = IOLockAlloc();
  if (!bounce || !mutex || !gHost.ready || !window.size || !IOBlockStorageDevice::start(provider)) return false;
  gRootDiagnostic.stage = "first root read";
  if (hostRead4k(0, window.baseByte / 4096, 1, bounce) != kIOReturnSuccess)
    return transportFailure("first root read");
  if (memcmp(bounce + window.signatureOffset, window.signature, strlen(window.signature)))
    return transportFailure("SHNGROOT window signature mismatch");
  gRootDiagnostic.first = kPongUserdataFirst4k;
  gRootDiagnostic.last = kPongUserdataLast4k;
  gRootDiagnostic.windowBytes = window.size;
  const bool writableWindow = gHost.writable && !(window.baseByte & 4095) && !(window.size & 4095);
  IOLog("PONG_UFS: SHNGROOT %s first read complete signature=1 base=%llx bytes=%llx; publish 512-byte %s disk\n",
        window.name, window.baseByte, window.size, writableWindow ? "read-write" : "read-only");
  setProperty("PongReadOnly", !writableWindow);
  setProperty("PongWritable", writableWindow);
  setProperty("PongUfsWindow", true);
  setProperty("PongRootWindow", !strcmp(window.name, "OS"));
  setProperty("PongSystemOSWindow", !strcmp(window.name, "SystemOS"));
  setProperty("PongDataWindow", !strcmp(window.name, "Data"));
  setProperty("Physical Interconnect", "UFS");
  online = true; registerService();
  char detail[96];
  snprintf(detail, sizeof(detail), "SHNGROOT %s disk published", window.name);
  PongProbeRecord(4, "ufs-transport", PongProbePass, detail, window.size);
  return true;
}
void PongUfsBlock::free() {
  if (bounce) IOFree(bounce, 4096);
  if (mutex) IOLockFree(mutex);
  IOBlockStorageDevice::free();
}
IOReturn PongUfsBlock::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                      IOStorageAttributes *, IOStorageCompletion *completion) {
  const uint64_t rootBlocks = window.size / kRootLogicalBlock;
  const bool write = buffer && buffer->getDirection() == kIODirectionOut;
  if (!buffer || !completion || !completion->action || !count || block >= rootBlocks ||
      count > rootBlocks - block || count > UINT64_MAX / kRootLogicalBlock ||
      buffer->getLength() < count * kRootLogicalBlock ||
      (buffer->getDirection() != kIODirectionIn && !write))
    return kIOReturnBadArgument;
  if (write && (!gHost.writable || (window.baseByte & 4095) || (window.size & 4095)))
    return kIOReturnNotWritable;
  if (!online || !gHost.ready) return kIOReturnNotReady;
  uint64_t done = 0, bytes = count * kRootLogicalBlock;
  IOReturn result = kIOReturnSuccess;
  IOLockLock(mutex);
  while (done < bytes) {
    const uint64_t absolute = window.baseByte + block * kRootLogicalBlock + done;
    const uint32_t inside = static_cast<uint32_t>(absolute & 4095);
    uint64_t wanted = bytes - done;
    if (wanted > kDataMax - inside) wanted = kDataMax - inside;
    uint32_t physicalBlocks = static_cast<uint32_t>((inside + wanted + 4095) / 4096);
    uint64_t available = uint64_t(physicalBlocks) * 4096 - inside;
    uint64_t take = wanted < available ? wanted : available;
    if (write) {
      uint64_t writeStart = absolute & ~UINT64_C(4095);
      uint64_t writeEnd = writeStart + uint64_t(physicalBlocks) * 4096;
      uint64_t windowEnd = window.baseByte + window.size;
      if (writeStart < window.baseByte || writeEnd < writeStart || writeEnd > windowEnd) {
        result = kIOReturnNotWritable;
        break;
      }
    }
    IOLockLock(gHost.lock);
    if (!gHost.ready || gHost.poisoned) {
      result = kIOReturnNotReady;
    } else {
      // Prepared storage descriptors can copy directly to/from the shared
      // payload. Keep ownership until the copy and any RMW write complete.
      // Full physical-block writes need no read of the old contents.
      if (!write || inside || take != uint64_t(physicalBlocks) * 4096)
        result = hostRead4kLocked(0, absolute / 4096, physicalBlocks);
      if (result == kIOReturnSuccess && write && !hostIdle()) result = kIOReturnNotReady;
      if (result == kIOReturnSuccess) {
        uint8_t *data = gHost.virt + kDataOffset + inside;
        uint64_t copied = write ? buffer->readBytes(done, data, take) : buffer->writeBytes(done, data, take);
        if (copied != take) result = kIOReturnUnderrun;
        else if (write) result = hostWrite4kLocked(0, absolute / 4096, physicalBlocks);
      }
    }
    IOLockUnlock(gHost.lock);
    if (result != kIOReturnSuccess) break;
    done += take;
  }
  IOLockUnlock(mutex);
  if (result != kIOReturnSuccess && result != kIOReturnNotWritable)
    transportFailure(write ? "block write failed" : "block read failed");
  completion->action(completion->target, completion->parameter, result, done);
  return kIOReturnSuccess;
}
extern "C" bool pong_ufs_transport_start(IOService *provider) {
  if (gTransportAttempted) {
    IOLog("PONG_UFS: transport attempt already completed; skip duplicate match\n");
    return false;
  }
  gTransportAttempted = true;
  // A built-in transport has no separate kext dependency record. Explicitly
  // start the genuine family and its personalities before using its classes.
  OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
  if (loaded != kOSReturnSuccess) {
    gRootDiagnostic.result = static_cast<uint32_t>(loaded);
    return transportFailure("IOStorageFamily load/start failed");
  }
  loaded = OSKextLoadKextWithIdentifier("com.apple.filesystems.apfs");
  if (loaded != kOSReturnSuccess) {
    gRootDiagnostic.result = static_cast<uint32_t>(loaded);
    return transportFailure("APFS load/start failed");
  }
  // Original prelinked storage metadata must be initialized before allocation.
  OSSharedPtr<const OSSymbol> name(OSSymbol::withCStringNoCopy("IOBlockStorageDevice"), OSNoRetain);
  const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name.get()) : nullptr;
  if (meta != &IOBlockStorageDevice::gMetaClass)
    return transportFailure("original storage metaclass not registered");
  if (meta->getClassSize() != sizeof(IOBlockStorageDevice) || meta->getSuperClass() != &IOService::gMetaClass)
    return transportFailure("original storage class/parent ABI mismatch");
  if (!hostBringUp()) return false;
  RootLayout layout;
  const char *why = nullptr;
  if (!findRootLayout(&layout, &why)) return transportFailure(why);
  if (layout.userdataFirst != kPongUserdataFirst4k || layout.userdataLast != kPongUserdataLast4k ||
      layout.userdataLast >= gHost.blocks)
    return transportFailure("Pong userdata geometry changed");
  if (layout.manifestStatus) return transportFailure("invalid SHNGROOT manifest/hash/bounds");
  static const struct { const char *name, *product, *signature; unsigned offset; } wanted[] = {
    {"OS", "iOS 16 OS root", "EFI PART", 512},
    {"SystemOS", "iOS 16 SystemOS", "NXSB", 32},
#if XNU_PONG_UFS_DATA
    {"Data", "iOS 16 persistent Data", "EFI PART", 512},
#endif
  };
  gRootWindowCount = 0;
  for (const auto &item : wanted) {
    bool found = false;
    for (uint32_t index = 0; index < layout.windows; ++index) {
      if (strcmp(layout.window[index].name, item.name)) continue;
      if (gRootWindowCount >= 3) return transportFailure("too many SHNGROOT disks");
      gRootWindows[gRootWindowCount++] = {item.name, item.product, item.signature, item.offset,
        layout.userdataFirst * 4096ULL + layout.window[index].offset, layout.window[index].size};
      found = true;
      break;
    }
    if (!found) return transportFailure("required SHNGROOT window missing");
  }
  for (unsigned index = 0; index < gRootWindowCount; ++index) {
    OSSharedPtr<PongUfsBlock> block(new PongUfsBlock, OSNoRetain);
    if (!block || !block->init(nullptr, gRootWindows[index]) || !block->attach(provider))
      return transportFailure("block allocation/attachment");
    if (!block->start(provider)) { block->detach(provider); return transportFailure("block start"); }
  }
  return true;
}
