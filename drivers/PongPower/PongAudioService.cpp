// SPDX-License-Identifier: BSD-2-Clause-Patent
// PongAudioPlugIn opens this service after the physical speaker graph is ready.
// PongAudio.cpp owns the control/output mappings and consumes start/stop
// requests. No capture memory or input endpoint is implemented on Pong.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include "ShengAudioAbi.h"

extern "C" IOBufferMemoryDescriptor *sheng_audio_memory(uint32_t type);   // retained; null when absent
extern "C" int sheng_audio_request(uint32_t selector);                     // 0 accepted
// [0] service state (1 published; negative: the step that failed), [1] opens, [2] closes, [3] requests, [4] refused requests,
// [5] memory maps
extern "C" {volatile int32_t native_audio_service[8];}
extern "C" vm_offset_t phystokv(uint64_t pa);
extern "C" uint64_t pmap_find_pa(void *pmap, uint64_t va);
// Darwin24 keeps this XNU function in Unsupported.exports; the generated
// kext header omits its declaration. Its task_t -> pmap_t ABI is unchanged.
extern "C" struct pmap *get_task_pmap(task_t task);

// CoreAudio18 loads the Apple 24 MHz host frequency from the dyld shared
// cache. Pong's architectural counter is 19.2 MHz. Adapt just that double,
// following Sheng's clock handoff, after resolving it in the caller's pmap.
static IOReturn setHostClock(task_t task,uint64_t address) {
    constexpr uint64_t apple=0x4176e36000000000ULL;
    if(task!=current_task() || (address&7) || address<0x180000000ULL || address>=0x400000000ULL)
        return kIOReturnBadArgument;
    auto memory=IOMemoryDescriptor::withAddressRange(address,8,kIODirectionOut,task);
    if(!memory)return kIOReturnNoMemory;
    IOReturn r=memory->prepare();
    if(r!=kIOReturnSuccess){memory->release();return r;}
    uint64_t seen=0;
    if(copyin(address,&seen,8))r=kIOReturnNotReadable;
    else {
        uint64_t pa=pmap_find_pa(get_task_pmap(task),address);
        r=kIOReturnNoResources;
        if(pa) {
            mach_timebase_info_data_t tb;clock_timebase_info(&tb);
            uint64_t hz=1000000000ULL*tb.denom/tb.numer;
            unsigned top=63-(unsigned)__builtin_clzll(hz);
            if(top<=52) {
                uint64_t bits=(uint64_t)(1023+top)<<52|((hz<<(52-top))&((1ULL<<52)-1));
                auto value=reinterpret_cast<volatile uint64_t *>(phystokv(pa));
                r=kIOReturnUnsupported;
                if(*value==seen && (seen==apple || seen==bits)) {
                    *value=bits;__asm__ volatile("dsb sy":::"memory");r=kIOReturnSuccess;
                    IOLog("PongAudio: CoreAudio host clock=%llu Hz\n",hz);
                }
            }
        }
    }
    memory->complete();memory->release();return r;
}

class PongAudio : public IOService {
    OSDeclareDefaultStructors(PongAudio)
public:
    bool startAudio() {
        // iOS's audio server is sandboxed against unknown user clients; the kernel lets these processes open this service.
        setProperty("NativeSandboxGrantProcess", "audiomxd,mediaserverd");
        setProperty("IOUserClientClass", "PongAudioUserClient");
        registerService();
        return true;
    }
};
OSDefineMetaClassAndStructors(PongAudio, IOService)

class PongAudioUserClient : public IOUserClient {
    OSDeclareDefaultStructors(PongAudioUserClient)
    task_t owner = nullptr;
public:
    bool initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties) override {
        if (!IOUserClient::initWithTask(task, security, type, properties)) return false;
        owner = task;
        return true;
    }
    bool start(IOService *provider) override {
        if (!OSDynamicCast(PongAudio, provider) || !IOUserClient::start(provider)) return false;
        native_audio_service[1] = native_audio_service[1] + 1;
        return true;
    }
    IOReturn clientClose() override {
        // The plug-in's process went away or closed: nothing plays from its rings any more.
        sheng_audio_request(ShengAudioStopOutput);
        native_audio_service[2] = native_audio_service[2] + 1;
        terminate();
        return kIOReturnSuccess;
    }
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override {
        IOBufferMemoryDescriptor *m = sheng_audio_memory(type);
        if (!m) return kIOReturnBadArgument;
        *options = 0;
        *memory = m;   // the caller consumes the reference
        native_audio_service[5] = native_audio_service[5] + 1;
        return kIOReturnSuccess;
    }
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch, OSObject *target,
                            void *reference) override {
        if(selector==ShengAudioSetHostClock) {
            if(!args || args->scalarInputCount!=1 || args->structureInputSize ||
               args->scalarOutputCount || args->structureOutputSize)return kIOReturnBadArgument;
            return setHostClock(owner,args->scalarInput[0]);
        }
        if (!args || args->scalarInputCount || args->structureInputSize ||
            args->scalarOutputCount || args->structureOutputSize) return kIOReturnBadArgument;
        if (selector > ShengAudioStopOutput) return kIOReturnUnsupported;
        native_audio_service[3] = native_audio_service[3] + 1;
        if (sheng_audio_request(selector)) {native_audio_service[4] = native_audio_service[4] + 1; return kIOReturnNotReady;}
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(PongAudioUserClient, IOUserClient)

// Called by AudioProbe.cpp once the speaker path is ready and this file's classes are registered.
extern "C" void sheng_audio_service_start(void) {
    PongAudio *audio = OSTypeAlloc(PongAudio);
    if (!audio || !audio->init() || !audio->attach(IOService::getPlatform())) {
        if (audio) audio->release();
        native_audio_service[0] = -1;
        return;
    }
    if (!audio->start(IOService::getPlatform()) || !audio->startAudio()) {
        native_audio_service[0] = -2;
        audio->detach(IOService::getPlatform());
        audio->release();
        return;
    }
    native_audio_service[0] = 1;
    audio->release();   // the platform's attachment keeps it
}
