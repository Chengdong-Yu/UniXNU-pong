// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pong NT37705 DCS brightness. Register/packet facts are documented in
// README.backlight.md; this work-loop transport does not reset the display,
// reprogram the PLL, or lend a physical DMA buffer to the DSI controller.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void PongBacklightStaticInit(void);
class PongBacklight;

namespace {
PongBacklight *gPublishedBacklight;
const OSMetaClass *gAppleBacklightClass;
enum : uint32_t {
    Version = 0x000, Control = 0x004, Status = 0x008,
    DmaControl = 0x03c, DmaLength = 0x04c, AckError = 0x068,
    ReadData0 = 0x06c, ReadData1 = 0x070, TriggerControl = 0x084,
    DmaTrigger = 0x090, InterruptControl = 0x110,
    PatternControl = 0x15c, PatternData = 0x17c,
    ReadControl = 0x1d4, FifoReset = 0x1ec,
    DmaBusy = 1u << 1, DmaDone = 1u, BtaDone = 1u << 20,
    ErrorPending = 1u << 24,
    EnableMask = (1u << 1) | (1u << 9) | (1u << 11) | (1u << 13) |
                 (1u << 15) | (1u << 17) | (1u << 21) | (1u << 25) |
                 (1u << 29) | (1u << 31)
};
struct Board {
    uint64_t base;
    uint32_t span, maximum, minimum, swappedDbv, channel, version;
};
static_assert(sizeof(Board) == 32, "Pong backlight ADT wire size");
uint64_t nowNs() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}
}

class PongBacklight : public IOService {
    OSDeclareDefaultStructors(PongBacklight)
    Board board = {};
    IOMemoryMap *mapping = nullptr;
    volatile uint32_t *regs = nullptr;
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *timer = nullptr;
    const OSMetaClass *appleClass = nullptr;
    bool published = false, stopped = false, inFlight = false;
    bool needVerification = false;
    enum Operation { ReadInitial, WriteLevel, ReadVerify } operation = ReadInitial;
    uint32_t savedDma = 0, savedLength = 0, savedTrigger = 0, savedPattern = 0, savedControl = 0;
    uint32_t level = 0, sending = 0, from = 0, goal = 0;
    uint64_t deadline = 0, classDeadline = 0, fadeStart = 0, fadeDuration = 0;
    uint64_t request = 0;
    uint32_t requestedFade = 0, requests = 0, writes = 0, reads = 0;

    uint32_t rd(uint32_t offset) const { return regs[offset / 4]; }
    void wr(uint32_t offset, uint32_t value) {
        regs[offset / 4] = value;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    uint32_t encodedLevel(uint32_t value) const {
        // Linux's MIPI helper begins with low-byte/high-byte. Pong's DT
        // bl-inverted-dbv swaps those bytes: the actual wire is high/low.
        return board.swappedDbv ? ((value & 0xff) << 8) | (value >> 8) : value;
    }
    uint32_t clamped(uint32_t value) const {
        if (value > board.maximum) value = board.maximum;
        if (value && value < board.minimum) value = board.minimum;
        return value;
    }
    void acknowledge() {
        // Preserve interrupt masks, W1C only our command/BTA completions.
        wr(InterruptControl, (rd(InterruptControl) & EnableMask) | DmaDone | BtaDone);
    }
    bool canBegin() const {
        return !inFlight && !__atomic_load_n(&stopped, __ATOMIC_ACQUIRE) &&
            !(rd(Status) & DmaBusy) && !(rd(PatternControl) & 1u) &&
            (rd(Control) & 1u) && !(rd(InterruptControl) & ErrorPending);
    }
    bool beginTransaction(const uint32_t *words, unsigned count, bool lowPower) {
        if (!canBegin()) return false;
        savedDma = rd(DmaControl); savedLength = rd(DmaLength);
        savedTrigger = rd(TriggerControl); savedPattern = rd(PatternControl);
        savedControl = rd(Control);
        // Firmware may leave video mode on with CMD_MODE_EN clear. Enable
        // only the command engine; keep DSI_EN and the pixel engine intact.
        wr(Control, savedControl | (1u << 2));
        acknowledge();
        wr(TriggerControl, (savedTrigger & ~0xfu) | 4); // software DMA trigger only
        wr(PatternControl, (3u << 16) | 6); // custom command FIFO, not video TPG
        for (unsigned i = 0; i < count; i++) wr(PatternData, words[i]);
        if (count & 1) wr(PatternData, 0); // hardware requires an even DWORD count
        uint32_t control = savedDma & ~((3u << 30) | (1u << 29) | (1u << 26) | (1u << 24));
        control |= 1u << 28; // embedded command packet
        if (lowPower) control |= 1u << 26;
        wr(DmaControl, control); wr(DmaLength, count * 4);
        deadline = nowNs() + 50000000ULL;
        inFlight = true;
        wr(DmaTrigger, 1);
        return true;
    }
    void restoreTransaction() {
        // A timeout with DMA still busy retains the FIFO and its register
        // state. Reusing/resetting it could corrupt an unfinished transfer.
        if (rd(Status) & DmaBusy) return;
        acknowledge();
        wr(PatternControl, 0); wr(FifoReset, 1); IODelay(1); wr(FifoReset, 0);
        wr(DmaControl, savedDma); wr(DmaLength, savedLength);
        wr(TriggerControl, savedTrigger); wr(PatternControl, savedPattern);
        wr(Control, savedControl);
        inFlight = false;
    }
    bool startRead(Operation next) {
        if (!canBegin()) return false;
        // SET_MAXIMUM_RETURN_PACKET_SIZE(2), then GET_DISPLAY_BRIGHTNESS.
        // Header byte 3 marks BTA (bit 5) and last packet (bit 7).
        const uint32_t words[] = {0x00370002u, 0xa0060052u};
        wr(ReadControl, 1); wr(ReadControl, 0);
        if (!beginTransaction(words, 2, true)) return false;
        operation = next;
        return true;
    }
    bool readLevel(uint32_t &answer) const {
        if ((rd(ReadControl) >> 16) < 4) return false;
        uint32_t data = rd(ReadData0);
        uint32_t first = 0, second = 0;
        // The readback register is network byte order; short DCS response
        // 0x22 carries the two bytes immediately after the data type.
        if (((data >> 24) & 0x3f) == 0x22) {
            first = (data >> 16) & 0xff; second = (data >> 8) & 0xff;
        } else {
            const uint32_t header = rd(ReadData1);
            if ((rd(ReadControl) >> 16) < 8 || ((header >> 24) & 0x3f) != 0x1c ||
                ((header >> 16) & 0xff) != 2 || ((header >> 8) & 0xff)) return false;
            first = data >> 24; second = (data >> 16) & 0xff;
        }
        answer = board.swappedDbv ? first * 256 + second : second * 256 + first;
        return answer <= board.maximum;
    }
    void publishLevel() {
        OSDictionary *item = OSDictionary::withCapacity(3);
        OSDictionary *parameters = OSDictionary::withCapacity(1);
        OSNumber *minimum = OSNumber::withNumber(0ULL, 32);
        OSNumber *maximum = OSNumber::withNumber(board.maximum, 32);
        OSNumber *value = OSNumber::withNumber(level, 32);
        if (item && parameters && minimum && maximum && value) {
            item->setObject("min", minimum); item->setObject("max", maximum); item->setObject("value", value);
            parameters->setObject("brightness", item); setProperty("IODisplayParameters", parameters);
        }
        OSSafeReleaseNULL(item); OSSafeReleaseNULL(parameters);
        OSSafeReleaseNULL(minimum); OSSafeReleaseNULL(maximum); OSSafeReleaseNULL(value);
    }
    void fail(const char *reason) {
        __atomic_store_n(&stopped, true, __ATOMIC_RELEASE);
        timer->cancelTimeout();
        if (inFlight) restoreTransaction();
        setProperty("PongBacklightError", reason);
        IOLog("PongBacklight: %s; requests=%u writes=%u reads=%u status=%x irq=%x ack=%x\n",
              reason, __atomic_load_n(&requests, __ATOMIC_RELAXED), writes, reads, rd(Status), rd(InterruptControl), rd(AckError));
    }
    void publishFirst() {
        // Use the actual AppleARMPlatform metaclass for CoreBrightness's
        // conformance query. The getter below is compiled against Darwin24;
        // no vtable-size assumption, fixed slot, or duplicate class name.
        gAppleBacklightClass = appleClass;
        __atomic_store_n(&gPublishedBacklight, this, __ATOMIC_RELEASE);
        published = true;
        setName("AppleARMBacklight");
        setProperty("NativeSandboxGrantProcess", "backboardd");
        setProperty("backlight-control", true); setProperty("built-in", true);
        publishLevel(); registerService();
        IOLog("PongBacklight: NT37705 DCS service ready, level=%u max=%u, readback PASS\n", level, board.maximum);
    }
    void service() {
        if (stopped) return;
        if (!appleClass) {
            // AppleARMPlatform initializes asynchronously after framebuffer
            // startup. Never register its private class name in the kernel:
            // its later real initializer would panic on the duplicate.
            const OSSymbol *name = OSSymbol::withCString("AppleARMBacklight");
            appleClass = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
            OSSafeReleaseNULL(name);
            if (!appleClass) {
                if (nowNs() >= classDeadline) { fail("native AppleARMBacklight class unavailable"); return; }
                timer->setTimeoutMS(100); return;
            }
            IOLog("PongBacklight: native AppleARMBacklight class ready\n");
        }
        const uint64_t latest = __atomic_exchange_n(&request, 0ULL, __ATOMIC_ACQUIRE);
        if (latest >> 63) {
            from = level; goal = clamped((latest >> 16) & 0xffff);
            fadeStart = nowNs(); fadeDuration = (latest & 0xffff) * 1000000ULL;
        }
        if (inFlight) {
            const uint32_t irq = rd(InterruptControl);
            if (irq & ErrorPending) { fail("DSI command error"); return; }
            bool done = (irq & DmaDone) && !(rd(Status) & DmaBusy);
            if (done && operation != WriteLevel) done = (rd(ReadControl) >> 16) >= 4;
            if (!done) {
                if (nowNs() >= deadline) { fail("DSI command/readback timeout"); return; }
                timer->setTimeoutMS(1); return;
            }
            uint32_t returned = 0;
            const bool readOk = operation == WriteLevel || readLevel(returned);
            restoreTransaction();
            if (!readOk) { fail("invalid DCS brightness response"); return; }
            if (operation == WriteLevel) {
                writes++; needVerification = true;
            } else {
                reads++;
                if (operation == ReadVerify && returned != sending) { fail("DCS brightness readback differs"); return; }
                level = returned;
                if (!published) { from = goal = level; publishFirst(); }
                else publishLevel();
                needVerification = false;
                setProperty("PongBrightnessReadback", level, 32);
                setProperty("PongBrightnessWrites", writes, 32);
            }
        }
        if (needVerification) {
            if (!startRead(ReadVerify)) { fail("DSI readback engine unavailable"); return; }
            timer->setTimeoutMS(1); return;
        }
        if (!published) {
            if (!startRead(ReadInitial)) { fail("DSI engine not ready at handoff"); return; }
            timer->setTimeoutMS(1); return;
        }
        const uint64_t elapsed = nowNs() - fadeStart;
        uint32_t next = goal;
        if (fadeDuration && elapsed < fadeDuration)
            next = (uint32_t)((int64_t)from + ((int64_t)goal - from) * (int64_t)elapsed / (int64_t)fadeDuration);
        next = clamped(next);
        if (next != level) {
            sending = next;
            const uint32_t wire = encodedLevel(next);
            const uint32_t words[] = {0xc0390003u, 0xff000051u | (wire << 8)};
            if (!beginTransaction(words, 2, false)) { fail("DSI write engine unavailable"); return; }
            operation = WriteLevel;
            timer->setTimeoutMS(1);
        } else if (next != goal) timer->setTimeoutMS(16);
    }
    static void tick(OSObject *owner, IOTimerEventSource *) {
        static_cast<PongBacklight *>(owner)->service();
    }
public:
    bool prepare() {
        IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
        OSData *data = chosen ? OSDynamicCast(OSData, chosen->getProperty("pong-dsi-backlight")) : nullptr;
        if (data && data->getLength() == sizeof(board)) memcpy(&board, data->getBytesNoCopy(), sizeof(board));
        OSSafeReleaseNULL(chosen);
        if (board.version != 1 || board.base != 0xae94000 || board.span != 0x400 ||
            board.maximum != 4095 || board.minimum != 10 || board.swappedDbv != 1 || board.channel) return false;
        IODeviceMemory *memory = IODeviceMemory::withRange(board.base, board.span);
        mapping = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(memory);
        regs = mapping ? reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress()) : nullptr;
        if (!regs) return false;
        if (!rd(Version) || rd(Version) == ~0u || !(rd(Control) & 1u) || (rd(PatternControl) & 1u)) {
            IOLog("PongBacklight: DSI0 unavailable version=%x control=%x pattern=%x\n",rd(Version),rd(Control),rd(PatternControl));
            return false;
        }
        loop = IOWorkLoop::workLoop();
        timer = IOTimerEventSource::timerEventSource(this, tick);
        if (!loop || !timer || loop->addEventSource(timer)) return false;
        setProperty("PongPanel", "NT37705 Visionox / SM8475 DSI0");
        setProperty("PongDSIVersion", rd(Version), 32);
        classDeadline = nowNs() + 30000000000ULL;
        timer->setTimeoutMS(1);
        return true;
    }
    IOReturn setProperties(OSObject *object) override {
        OSDictionary *properties = OSDynamicCast(OSDictionary, object);
        if (!properties) return kIOReturnBadArgument;
        if (__atomic_load_n(&stopped, __ATOMIC_ACQUIRE)) return kIOReturnNotReady;
        if (properties->getCount() > 2 ||
            properties->getCount() != (properties->getObject("brightness") ? 1u : 0u) +
                                      (properties->getObject("FadePeriod") ? 1u : 0u)) return kIOReturnUnsupported;
        OSNumber *brightness = OSDynamicCast(OSNumber, properties->getObject("brightness"));
        OSNumber *fade = OSDynamicCast(OSNumber, properties->getObject("FadePeriod"));
        if ((properties->getObject("brightness") && !brightness) ||
            (properties->getObject("FadePeriod") && !fade)) return kIOReturnBadArgument;
        const uint64_t fixed = fade ? fade->unsigned64BitValue() : 0;
        const uint64_t ms = fade ? (fixed > 5u * 65536u ? 5000u : (fixed * 1000) >> 16) :
                                  __atomic_load_n(&requestedFade, __ATOMIC_RELAXED);
        __atomic_store_n(&requestedFade, (uint32_t)ms, __ATOMIC_RELAXED);
        if (!brightness) return kIOReturnSuccess;
        const int64_t value = (int64_t)brightness->unsigned64BitValue();
        const uint32_t target = value < 0 ? 0 : clamped(value > board.maximum ? board.maximum : (uint32_t)value);
        __atomic_fetch_add(&requests, 1u, __ATOMIC_RELAXED);
        __atomic_store_n(&request, (1ULL << 63) | ((uint64_t)target << 16) | ms, __ATOMIC_RELEASE);
        timer->setTimeoutUS(1); // caller never waits on DSI or the work-loop gate
        return kIOReturnSuccess;
    }
    void free() override {
        if (__atomic_load_n(&gPublishedBacklight, __ATOMIC_ACQUIRE) == this)
            __atomic_store_n(&gPublishedBacklight, (PongBacklight *)nullptr, __ATOMIC_RELEASE);
        if (timer) { timer->cancelTimeout(); if (loop) loop->removeEventSource(timer); }
        OSSafeReleaseNULL(timer); OSSafeReleaseNULL(loop); OSSafeReleaseNULL(mapping);
        IOService::free();
    }
};

// Expand the ordinary metaclass definition so the conformance getter is
// selected in source, rather than replacing a guessed Darwin17 vtable slot.
PongBacklight::MetaClass PongBacklight::gMetaClass;
const OSMetaClass *const PongBacklight::metaClass = &PongBacklight::gMetaClass;
const OSMetaClass *const PongBacklight::superClass = &IOService::gMetaClass;
PongBacklight::PongBacklight(const OSMetaClass *meta) : IOService(meta) {}
PongBacklight::~PongBacklight() {}
const OSMetaClass *PongBacklight::getMetaClass() const {
    return __atomic_load_n(&gPublishedBacklight, __ATOMIC_ACQUIRE) == this ? gAppleBacklightClass : &gMetaClass;
}
OSMetaClassConstructorInit(PongBacklight, IOService, )
OSDefineDefaultStructors(PongBacklight, IOService)

extern "C" void pong_backlight_start(IOService *display) {
    static PongBacklight *backlight;
    static bool registered;
    unsigned enabled = 1;
    PE_parse_boot_argn("pong_backlight", &enabled, sizeof(enabled));
    if (!enabled || backlight || !display) return;
    if (!registered) {
        void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
        if (!handle) { IOLog("PongBacklight: class registration unavailable\n"); return; }
        PongBacklightStaticInit();
        if (OSMetaClass::postModLoad(handle)) { IOLog("PongBacklight: class registration failed\n"); return; }
        registered = true;
    }
    PongBacklight *service = OSTypeAlloc(PongBacklight);
    if (!service) return;
    if (!service->init() || !service->attach(display) || !service->prepare()) {
        IOLog("PongBacklight: unavailable DT, AppleARMBacklight ABI or running DSI0; keep current panel brightness\n");
        if (service->getProvider()) service->detach(display);
        service->release(); return;
    }
    backlight = service; // the service and hardware handoff persist until reboot
}
