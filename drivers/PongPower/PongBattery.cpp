// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pong's single-pack battery over PMIC GLINK: an IOPMPowerSource whose
// values come from the battery manager in the audio DSP over the PMIC GLINK channel (ShengBattmgr.c). powerd finds every IOPMPowerSource,
// reads CurrentCapacity/MaxCapacity, IsCharging, ExternalConnected, voltage, amperage and AdapterDetails and listens for the status change
// message updateStatus sends; the status bar, the lock screen's charging state and Settings > Battery follow from that.
//
// Design: everything runs on the service's work loop. The DSP thread's callbacks only copy a message into a small ring and signal an
// IOInterruptEventSource (no provider: a software event), so the DSP thread never waits for this driver. One request is in flight at a
// time; a 2 s timer gives up on an unanswered one. Battery notifications start a refresh at once, and a 5 s timer refreshes in any case
// (the fields that hardly change, every ten minutes). Charging is firmware-owned;
// this board only reads measurements and subscribes to changes.
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "ShengAdsp.h"
#include "ShengBattmgr.h"
}

extern "C" ShengAdspChannel *sheng_adsp_channel(const char *, uint32_t, uint32_t, int, const ShengAdspClient *, void *) __attribute__((weak_import));
extern "C" int sheng_adsp_send(ShengAdspChannel *, const void *, uint32_t) __attribute__((weak_import));
// [0] stage (1 class up, 2 channel registered, 3 channel up, 4 published; negative: the step that failed), [1] percent, [2] mV,
// [3] mA, [4] temperature (raw), [5] flags (bit 0 present, 1 external, 2 charging, 3 full, 4 notifications enabled), [6] refresh
// cycles, [7] answers, [8] timeouts | errors << 16, [9] notifications, [10] USB type (raw), [11] full mAh, [12] design mAh,
// [13] cycles, [14] messages dropped (ring full) | ignored << 16, [15] send refusals
extern "C" {extern volatile int32_t native_battery_stats[16];}
// The raw readings behind the values: [0] capacity property (hundredths of a percent), [1] gauge 1 remaining, [2] gauge 1 full, [3] gauge 2
// remaining, [4] gauge 2 full, [5] temperature property, [6] current property (uA, discharge positive), [7] valid field bits, [8] the field
// the last timeout skipped (-1 none)
extern "C" {volatile int32_t native_battery_raw[10];}
// The charger as the firmware reports it (-1 no reading): [0] USB online, [1] USB type, [2] adapter type, [3] the board's adapter type,
// [4] input current limit mA, [5] the most the port offers mA, [6] USB mV, [7] USB mA, [8] battery status, [9] charge type, [10] thermal
// level, [11] charge current mA, [12] adapter watts, [13] bits: 0 cell 1 authenticated, 1 cell 2, 2 PD adapter authenticated, 3 input
// suspended (each with bit +8 set when read), [14] limit sets | tries << 8 | port type << 16, [15] last limit result
extern "C" {volatile int32_t native_charger_stats[16];}
static volatile int32_t pongUsbCable=-1;
extern "C" int pong_battery_usb_online(void) {
    return __atomic_load_n(&pongUsbCable,__ATOMIC_ACQUIRE);
}

namespace {
constexpr uint32_t kRequestMs = 2000, kRefreshMs = 5000, kSlowEvery = 120, kRing = 16, kMessage = 64;
}

class PongBattery : public IOPMPowerSource {
    OSDeclareDefaultStructors(PongBattery)
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *request = nullptr, *refresh = nullptr;
    IOInterruptEventSource *inbox = nullptr;
    IOSimpleLock *ringLock = nullptr;
    struct {uint32_t length; uint8_t data[kMessage];} ring[kRing];
    uint32_t ringHead = 0, ringCount = 0;
    volatile uint32_t channelUp = 0, channelChanged = 0;
    bool wasUp = false, published = false;
    bool external = false, sourceCandidate = false, charging = false, full = false;
    unsigned sourceConfirmations = 0;
    uint32_t lastPowerFlags = ~0u;
    uint32_t refreshes = 0;
    ShengBattmgr bm;
    ShengAdspChannel *channel = nullptr;

    static void dspState(void *context, int up) {
        PongBattery *self = static_cast<PongBattery *>(context);
        __atomic_store_n(&self->channelUp, up ? 1u : 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&self->channelChanged, 1u, __ATOMIC_RELEASE);
        self->inbox->interruptOccurred(nullptr, nullptr, 0);
    }
    static void dspReceive(void *context, const uint8_t *data, uint32_t length) {
        PongBattery *self = static_cast<PongBattery *>(context);
        IOSimpleLockLock(self->ringLock);
        if (self->ringCount < kRing && length <= kMessage) {
            auto &slot = self->ring[(self->ringHead + self->ringCount) % kRing];
            memcpy(slot.data, data, length);
            slot.length = length;
            self->ringCount++;
        } else {
            native_battery_stats[14] = native_battery_stats[14] + 1;
        }
        IOSimpleLockUnlock(self->ringLock);
        self->inbox->interruptOccurred(nullptr, nullptr, 0);
    }

    void pump() {
        uint8_t out[SHENG_BATTMGR_REQUEST_BYTES];
        const uint32_t n = sheng_battmgr_next(&bm, out);
        if (!n) return;
        if (sheng_adsp_send(channel, out, n)) native_battery_stats[15] = native_battery_stats[15] + 1;  // the timer skips the field
        request->setTimeoutMS(kRequestMs);
    }
    void number(const char *key, int64_t value) {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        OSNumber *n = OSNumber::withNumber((unsigned long long)value, 32);
        if (symbol && n) setPSProperty(symbol, n);
        OSSafeReleaseNULL(n); OSSafeReleaseNULL(symbol);
    }
    void flag(const char *key, bool value) {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        if (symbol) setPSProperty(symbol, value ? kOSBooleanTrue : kOSBooleanFalse);
        OSSafeReleaseNULL(symbol);
    }
    void recordRaw() {
        static const int fields[8] = {BmCapacity, BmGauge1Remaining, BmGauge1Full, BmGauge2Remaining, BmGauge2Full, BmTemperature, BmCurrent, -1};
        for (unsigned i = 0; i < 7; ++i) native_battery_raw[i] = (bm.valid >> fields[i]) & 1 ? bm.value[fields[i]] : -999999;
        native_battery_raw[7] = (int32_t)bm.valid;
        native_battery_raw[8] = bm.last_timeout;
        static const int charger[13] = {BmUsbOnline, BmUsbType, BmUsbAdapter, BmRealType, BmUsbLimit, BmUsbMax, BmUsbVoltage, BmUsbCurrent,
                                        BmStatus, BmChargeType, BmThermalLevel, BmChargeCurrent, BmPowerMax};
        static const int milli[13] = {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0};
        for (unsigned i = 0; i < 13; ++i)
            native_charger_stats[i] = (bm.valid >> charger[i]) & 1 ? (milli[i] ? bm.value[charger[i]] / 1000 : bm.value[charger[i]]) : -1;
        __atomic_store_n(&pongUsbCable,((bm.valid>>BmUsbOnline)&1)?
            (bm.value[BmUsbOnline]!=0):-1,__ATOMIC_RELEASE);
        static const int bits[4] = {BmAuthentic, BmSlaveAuthentic, BmPdVerified, BmInputSuspend};
        int32_t flags = 0;
        for (unsigned i = 0; i < 4; ++i)
            if ((bm.valid >> bits[i]) & 1) flags |= (bm.value[bits[i]] != 0) << i | 1 << (i + 8);
        native_charger_stats[13] = flags;
        native_charger_stats[14] = (int32_t)(bm.limit_sets | bm.limit_tries << 8 | (uint32_t)(bm.limit_port & 0xff) << 16);
        native_charger_stats[15] = bm.limit_result;
    }
    void publish() {
        // Capacity publication must not depend on optional charger/VBUS
        // replies. A single unsupported USB field hid the entire battery.
        ShengBatterySnapshot s;
        sheng_battmgr_snapshot(&bm, &s);
        if (!s.charge_known) {
            setProperty("PongBatteryStale", true);
            if (published) updateStatus();
            return;
        }
        // PMIC notifications can arrive in the middle of charger policy
        // transitions. Confirm a source change with two complete cycles;
        // a failed read retains the last coherent snapshot as stale above.
        // A capacity reply does not make a failed charger read valid.
        // Preserve the last measured source/status until their next fresh
        // response. In particular, a transient timeout is not unplugging.
        bool pending=false;
        if(published && !s.source_known) {s.external=external;s.charging=charging;s.fully_charged=full;}
        if(published && !s.status_known) {s.charging=charging;s.fully_charged=full;}
        if(published && s.source_known && bool(s.external)!=external) {
            if(!sourceConfirmations || sourceCandidate!=bool(s.external)) {
                sourceCandidate=s.external;sourceConfirmations=1;
                pending=true;
                // Debounce only the source flag, never capacity/voltage.
                s.external=external;s.charging=charging;s.fully_charged=full;
            }
        }
        if(!s.external){s.charging=false;s.fully_charged=false;}
        if(!pending && s.source_known){external=s.external;sourceConfirmations=0;}
        charging=s.charging;full=s.fully_charged;
        setProperty("PongBatterySourcePending",pending);
        setProperty("PongBatteryStale", false);
        setProperty("PongBatteryMissingFields", uint64_t((s.source_known?0u:1u)|(s.status_known?0u:2u)), 32);
        setBatteryInstalled(s.present);
        setExternalConnected(s.external);
        setExternalChargeCapable(s.external);
        setIsCharging(s.charging);
        setAtCriticalLevel(s.critical);
        setCurrentCapacity((unsigned)s.percent);
        setMaxCapacity(100);
        setVoltage((unsigned)(s.voltage_mv > 0 ? s.voltage_mv : 0));
        if (bm.valid & (1u << BmCurrent)) setAmperage(s.current_ma);
        if (s.cycles > 0) setCycleCount((unsigned)s.cycles);
        // Full on external power has zero time-to-full, not the unknown
        // discharge lifetime. Keep the source flag independent of drawing.
        setTimeRemaining(s.external ? (s.fully_charged ? 0 :
            (s.time_to_full_min >= 0 ? s.time_to_full_min : 0xffff)) :
            (s.time_to_empty_min >= 0 ? s.time_to_empty_min : 0xffff));
        flag("FullyCharged", s.fully_charged);
        if (bm.valid & (1u << BmCurrent)) number("InstantAmperage", s.current_ma);
        if (s.temperature_valid) number("Temperature", s.temperature);
        if (s.raw_mah > 0) number("AppleRawCurrentCapacity", s.raw_mah);
        if (s.full_mah > 0) {number("AppleRawMaxCapacity", s.full_mah); number("NominalChargeCapacity", s.full_mah);}
        if (s.design_mah > 0) number("DesignCapacity", s.design_mah);
        OSDictionary *adapter = OSDictionary::withCapacity(5);
        if (adapter) {
            OSNumber *family = OSNumber::withNumber(s.adapter_family, 32);
            if (family) {adapter->setObject(kIOPMPSAdapterDetailsFamilyKey, family); family->release();}
            if (s.external) {
                OSNumber *w = OSNumber::withNumber((unsigned long long)s.adapter_watts, 32), *mv = OSNumber::withNumber((unsigned long long)s.adapter_mv, 32),
                         *ma = OSNumber::withNumber((unsigned long long)s.adapter_ma, 32);
                if (w) {adapter->setObject(kIOPMPSAdapterDetailsWattsKey, w); w->release();}
                if (mv) {adapter->setObject(kIOPMPSAdapterDetailsVoltage, mv); mv->release();}
                if (ma) {adapter->setObject(kIOPMPSAdapterDetailsAmperageKey, ma); ma->release();}
            }
            const OSSymbol *key = OSSymbol::withCString(kIOPMPSAdapterDetailsKey);
            if (key) setPSProperty(key, adapter);
            OSSafeReleaseNULL(key);
            adapter->release();
        }
        native_battery_stats[1] = s.percent; native_battery_stats[2] = s.voltage_mv; native_battery_stats[3] = s.current_ma;
        native_battery_stats[4] = s.temperature;
        native_battery_stats[5] = s.present | s.external << 1 | s.charging << 2 | s.fully_charged << 3 | bm.enabled << 4;
        native_battery_stats[10] = (bm.valid >> BmUsbType) & 1 ? bm.value[BmUsbType] : -1;
        native_battery_stats[11] = s.full_mah; native_battery_stats[12] = s.design_mah; native_battery_stats[13] = s.cycles;
        recordRaw();
        const uint32_t powerFlags=s.external | s.charging<<1 | s.fully_charged<<2;
        if (powerFlags!=lastPowerFlags) {
            lastPowerFlags=powerFlags;
            IOLog("PongBattery: source update external=%d charging=%d full=%d USB online=%d VBUS=%dmV status=%d subscribed=%u\n",
                s.external,s.charging,s.fully_charged,native_charger_stats[0],native_charger_stats[6],native_charger_stats[8],unsigned(bm.enabled));
        }
        if (!published && !s.charge_known) return;  // no charge reading yet: 0 % would read as an empty battery
        if (!published) {
            // The properties are in place before anyone finds the service.
            published = true;
            registerService();
            native_battery_stats[0] = 4;IOLog("PongBattery: physical battery power source published %d%% %dmV external=%d charging=%d full=%d adapter=%x %dmV/%dmA\n",s.percent,s.voltage_mv,s.external,s.charging,s.fully_charged,s.adapter_family,s.adapter_mv,s.adapter_ma);
        }
        updateStatus();
    }
    void counters() {
        native_battery_stats[6] = (int32_t)bm.cycles_done;
        native_battery_stats[7] = (int32_t)bm.responses;
        native_battery_stats[8] = (int32_t)(bm.timeouts | bm.errors << 16);
        native_battery_stats[9] = (int32_t)bm.notifications;
        native_battery_stats[14] = (native_battery_stats[14] & 0xffff) | (int32_t)(bm.ignored << 16);
    }
    void handle(int result) {
        if (result == BmCycleDone) publish();
        pump();
        counters();
    }

    static void inboxAction(OSObject *owner, IOInterruptEventSource *, int) {
        PongBattery *self = OSDynamicCast(PongBattery, owner);
        if (!self) return;
        if (__atomic_exchange_n(&self->channelChanged, 0u, __ATOMIC_ACQ_REL)) {
            const bool up = __atomic_load_n(&self->channelUp, __ATOMIC_ACQUIRE) != 0;
            if (up && !self->wasUp) {
                self->setProperty("PongBatteryStale", true);
                native_battery_stats[0] = self->published ? 4 : 3;
                uint8_t enable[SHENG_BATTMGR_ENABLE_BYTES];
                sheng_adsp_send(self->channel, enable, sheng_battmgr_enable_request(enable));
                sheng_battmgr_refresh(&self->bm, 1);
                self->pump();
                self->refresh->setTimeoutMS(kRefreshMs);
            }
            if (!up && self->wasUp) {
                __atomic_store_n(&pongUsbCable,-1,__ATOMIC_RELEASE);
                // Loss of DSP transport is not battery removal/unplugging.
                // Keep the last real snapshot, mark it stale, and discard
                // queued responses from the old channel incarnation.
                self->setProperty("PongBatteryStale", true);
                self->request->cancelTimeout(); self->refresh->cancelTimeout();
                IOSimpleLockLock(self->ringLock);
                self->ringHead = self->ringCount = 0;
                IOSimpleLockUnlock(self->ringLock);
                sheng_battmgr_init(&self->bm);
                if (self->published) self->updateStatus();
            }
            self->wasUp = up;
        }
        for (;;) {
            uint8_t data[kMessage];
            uint32_t length = 0;
            IOSimpleLockLock(self->ringLock);
            if (self->ringCount) {
                auto &slot = self->ring[self->ringHead];
                length = slot.length;
                memcpy(data, slot.data, length);
                self->ringHead = (self->ringHead + 1) % kRing;
                self->ringCount--;
            }
            IOSimpleLockUnlock(self->ringLock);
            if (!length) break;
            const int result = sheng_battmgr_receive(&self->bm, data, length);
            if (result == BmAnswer || result == BmCycleDone) self->request->cancelTimeout();
            self->handle(result);
        }
    }
    static void requestAction(OSObject *owner, IOTimerEventSource *) {
        PongBattery *self = OSDynamicCast(PongBattery, owner);
        if (!self || !self->wasUp) return;
        const uint32_t before = self->bm.cycles_done;
        sheng_battmgr_timeout(&self->bm);
        self->handle(self->bm.cycles_done != before ? BmCycleDone : BmAnswer);
    }
    static void refreshAction(OSObject *owner, IOTimerEventSource *sender) {
        PongBattery *self = OSDynamicCast(PongBattery, owner);
        if (!self || !self->wasUp) return;
        sheng_battmgr_refresh(&self->bm, ++self->refreshes % kSlowEvery == 0);
        self->handle(BmAnswer);
        sender->setTimeoutMS(kRefreshMs);
    }

public:
    bool startBattery() {
        sheng_battmgr_init(&bm);
        loop = IOWorkLoop::workLoop();
        ringLock = IOSimpleLockAlloc();
        inbox = IOInterruptEventSource::interruptEventSource(this, &PongBattery::inboxAction);
        request = IOTimerEventSource::timerEventSource(this, &PongBattery::requestAction);
        refresh = IOTimerEventSource::timerEventSource(this, &PongBattery::refreshAction);
        if (!loop || !ringLock || !inbox || !request || !refresh || loop->addEventSource(inbox) != kIOReturnSuccess ||
            loop->addEventSource(request) != kIOReturnSuccess || loop->addEventSource(refresh) != kIOReturnSuccess)
            return false;
        native_battery_stats[0] = 1;
        const ShengAdspClient client = {&PongBattery::dspState, &PongBattery::dspReceive};
        channel = sheng_adsp_channel ? sheng_adsp_channel("PMIC_RTR_ADSP_APPS", 1024, 4, 0, &client, this) : nullptr;
        if (!channel) return false;
        native_battery_stats[0] = 2;
        return true;
    }
    void free() override {
        IOTimerEventSource *sources[] = {request, refresh};
        for (IOTimerEventSource *source : sources) if (source) {
            source->cancelTimeout();
            if (source->getWorkLoop() == loop) loop->removeEventSource(source);
        }
        if (inbox && inbox->getWorkLoop() == loop) loop->removeEventSource(inbox);
        OSSafeReleaseNULL(request); OSSafeReleaseNULL(refresh); OSSafeReleaseNULL(inbox);
        OSSafeReleaseNULL(loop);
        if (ringLock) IOSimpleLockFree(ringLock);
        IOPMPowerSource::free();
    }
};
OSDefineMetaClassAndStructors(PongBattery, IOPMPowerSource)

// Called by BatteryProbe.cpp once this file's classes are registered.
extern "C" void sheng_battery_start(void) {
    PongBattery *battery = OSTypeAlloc(PongBattery);
    if (!battery || !battery->init() || !battery->attach(IOService::getPlatform())) {
        if (battery) battery->release();
        native_battery_stats[0] = -1;
        return;
    }
    if (!battery->startBattery()) {
        native_battery_stats[0] = native_battery_stats[0] > 0 ? -native_battery_stats[0] - 1 : -1;
        battery->detach(IOService::getPlatform());
        battery->release();
        return;
    }
    battery->release();  // the platform's attachment keeps it
}
