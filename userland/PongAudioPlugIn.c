// SPDX-License-Identifier: BSD-2-Clause-Patent
// Derived from UniXNU-sheng; Pong has only a physical stereo output.
// The built-in audio device for the iOS audio server: one device with a stereo output stream (Pong speakers); no capture stream is exposed.
// Samples move through rings shared with the kernel service ShengAudio (package/ShengAudioAbi.h); the service
// feeds the audio DSP. Objects: the plug-in (1), the device (2), the output stream (3), the input stream (4), the output volume (5).
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include "ShengAudioAbi.h"
#define PONG_ROUTE_LOG(...) syslog(LOG_NOTICE,__VA_ARGS__)
#include "PongAudioRoute.h"
#undef PONG_ROUTE_LOG

enum {kPlugIn = kAudioObjectPlugInObject, kDevice = 2, kOutStream = 3, kInStream = 4, kVolume = 5, kMute = 6};
#define kPlugInBundleID "org.unixnu.pong.audio"
#define kDeviceUID "PongAudioDevice"
#define kModelUID "PongAudioModel"
// AudioHardware.h legacy selectors; its whole macOS header must not replace
// the selected iPhoneOS SDK. These ABI values match the public SDK header.
enum {kPongDeviceVolumeScalar='volm',kPongDeviceVolumeDecibels='vold',kPongDeviceMute='mute',
    kPongDeviceDbRange='vdb#',kPongDeviceScalarToDb='v2db',kPongDeviceDbToScalar='db2v'};
static const Float64 kRate = SHENG_AUDIO_RATE;

static AudioServerPlugInHostRef gHost;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint gRefs = 1;
static io_connect_t gConnect;
static _Atomic(ShengAudioControl *) gControl;
static int16_t *gOut;
static atomic_uint gIoUsers;
static atomic_uint gOutActive = 1;
static _Atomic(Float32) gVolume = 0.25f;
static atomic_uint gMuted;
static Float64 gHostTicksPerFrame;
static uint64_t gZeroOrigin, gZeroSeed;
static void setHostClock(void);
static Float32 currentVolume(void) {
    return gControl?(Float32)__atomic_load_n(&gControl->out_gain_q16,__ATOMIC_ACQUIRE)/65536.0f:gVolume;
}

// ---- Kernel service ------------------------------------------------------------------------------------------------------------
static mach_vm_address_t gMapped[2];
static int physicalReady(void) {
    return gControl && (__atomic_load_n(&gControl->state,__ATOMIC_ACQUIRE)&ShengAudioOutputReady);
}
static int nativeOutput(void) {
    return gControl && (__atomic_load_n(&gControl->state,__ATOMIC_ACQUIRE)&ShengAudioNativeOutput);
}
static int ready(void) {return physicalReady() && !nativeOutput();}
static void disconnectService(void) {
    gControl = NULL; gOut = NULL;
    for (unsigned i=0; i<2; ++i) if(gMapped[i]) {
        IOConnectUnmapMemory64(gConnect, i, mach_task_self(), gMapped[i]); gMapped[i]=0;
    }
    if(gConnect) IOServiceClose(gConnect);
    gConnect=0;
}
static int mapType(uint32_t type, void **out, mach_vm_size_t minimum) {
    mach_vm_address_t address=0; mach_vm_size_t size=0;
    if(IOConnectMapMemory64(gConnect,type,mach_task_self(),&address,&size,kIOMapAnywhere)!=kIOReturnSuccess)return -1;
    gMapped[type]=address;
    if(size<minimum)return -1;
    *out=(void *)(uintptr_t)address; return 0;
}
static int connectService(void) {
    if(gControl)return physicalReady()?0:-1;
    io_service_t service=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching(SHENG_AUDIO_CLASS));
    if(!service)return -1;
    kern_return_t kr=IOServiceOpen(service,mach_task_self(),0,&gConnect);
    IOObjectRelease(service);
    if(kr!=KERN_SUCCESS){gConnect=0;return -1;}
    void *control=NULL,*out=NULL;
    if(mapType(0,&control,sizeof(ShengAudioControl)) || mapType(1,&out,SHENG_AUDIO_RING_FRAMES*4)) {
        disconnectService();return -1;
    }
    ShengAudioControl *c=control;
    if(c->magic!=SHENG_AUDIO_MAGIC || c->version!=SHENG_AUDIO_VERSION || c->rate!=48000 ||
       c->out_channels!=2 || c->in_channels!=0 || c->ring_frames!=SHENG_AUDIO_RING_FRAMES ||
       !c->ticks_per_frame_q32 || !(c->state&ShengAudioOutputReady)) {
        disconnectService();return -1;
    }
    gOut=out;gControl=c;
    gHostTicksPerFrame=(Float64)c->ticks_per_frame_q32/4294967296.0;
    if(gHost)setHostClock();
    syslog(LOG_NOTICE,"PongAudio HAL: physical stereo service connected");
    return 0;
}
static void *awaitService(void *unused) {
    (void)unused;
    // The DSP and real amplifier graph can become ready after audiomxd loads.
    // Advertise no audio device until the physical endpoint actually exists.
    usleep(100000); // Initialize must return before notifying its host.
    for(unsigned i=0;i<900;i++) {
        pthread_mutex_lock(&gLock);int connected=connectService()==0;pthread_mutex_unlock(&gLock);
        if(connected && nativeOutput()) {
            // Keep only the validated host-clock adapter. Publishing a
            // second PCM endpoint routes samples into a ring the native
            // engine does not consume and lets one client stop the other.
            syslog(LOG_NOTICE,"PongAudio HAL: native Speaker owns PCM; compatibility clock only");
            return NULL;
        }
        if(connected) {
            AudioObjectPropertyAddress p[2]={
                {kAudioPlugInPropertyDeviceList,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain},
                {kAudioObjectPropertyOwnedObjects,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain}};
            OSStatus result=gHost?gHost->PropertiesChanged(gHost,kPlugIn,2,p):kAudioHardwareNotRunningError;
            if(!result || i<3 || !(i%100))
                syslog(result?LOG_ERR:LOG_NOTICE,"PongAudio HAL: real device discovery notification result=%d",result);
            if(result) {usleep(100000);continue;}
            // This is the discovery worker, outside Initialize, host
            // callbacks and gLock. Embedded CoreAudio's public HAL API
            // operates on the calling process's HALS_System. Select the
            // real output here in audiomxd, not in an unrelated helper's
            // separately started audio server. Never call it from an ASPL
            // property/IO callback, and never substitute a fabricated ID.
            int routed=pong_audio_select_native_route();
            syslog(routed?LOG_ERR:LOG_NOTICE,"PongAudio HAL: native host route selection result=%d",routed);
            // Native HAL controls own the shared gain. Publish any actual
            // gain/mute change to the host; physical keys never write it.
            UInt32 gain=__atomic_load_n(&gControl->out_gain_q16,__ATOMIC_ACQUIRE);
            UInt32 muted=__atomic_load_n(&gControl->out_muted,__ATOMIC_ACQUIRE);
            while(ready()) {
                usleep(200000);
                UInt32 next=__atomic_load_n(&gControl->out_gain_q16,__ATOMIC_ACQUIRE);
                UInt32 mute=__atomic_load_n(&gControl->out_muted,__ATOMIC_ACQUIRE);
                if(next!=gain || mute!=muted) {
                    gain=next;muted=mute;
                    AudioObjectPropertyAddress control[2]={{kAudioLevelControlPropertyScalarValue,kAudioObjectPropertyScopeGlobal,0},
                        {kAudioLevelControlPropertyDecibelValue,kAudioObjectPropertyScopeGlobal,0}};
                    AudioObjectPropertyAddress volume[2]={{'volm',kAudioObjectPropertyScopeOutput,0},{'mute',kAudioObjectPropertyScopeOutput,0}};
                    gHost->PropertiesChanged(gHost,kVolume,2,control);
                    gHost->PropertiesChanged(gHost,kDevice,2,volume);
                }
            }
            return NULL;
        }
        usleep(100000);
    }
    syslog(LOG_ERR,"PongAudio HAL: no physical output ready within 90 seconds");
    return NULL;
}
static kern_return_t call(uint32_t selector) {return gConnect ? IOConnectCallScalarMethod(gConnect, selector, NULL, 0, NULL, NULL) : KERN_FAILURE;}

// Match the actual 19.2 MHz Pong timer in CoreAudio's shared host clock.
// Resolve the selected runtime's constant from its instruction pair; the
// kernel accepts only an aligned shared-cache double with the expected value.
static void setHostClock(void) {
    uintptr_t symbol=(uintptr_t)dlsym(RTLD_DEFAULT,"AudioGetHostClockFrequency")&((((uintptr_t)1)<<47)-1);
    if(!symbol || !gConnect)return;
    const uint32_t *code=(const uint32_t *)symbol;
    uint32_t adrp=code[0],ldr=code[1];
    if((adrp&0x9f000000u)!=0x90000000u || (ldr&0xffc00000u)!=0xfd400000u ||
       ((ldr>>5)&31)!=(adrp&31))return;
    int64_t pages=(int64_t)(((adrp>>5)&0x7ffffu)<<2|((adrp>>29)&3));
    if(pages&(1<<20))pages-=1<<21;
    uint64_t address=(symbol&~(uintptr_t)0xfff)+(uint64_t)(pages*4096)+((ldr>>10)&0xfff)*8;
    kern_return_t r=IOConnectCallScalarMethod(gConnect,ShengAudioSetHostClock,&address,1,NULL,NULL);
    syslog(r?LOG_ERR:LOG_NOTICE,"PongAudio HAL: host clock adjustment result=%x",r);
}

// ---- Helpers ---------------------------------------------------------------------------------------------------------------------
static AudioStreamBasicDescription format(UInt32 channels) {
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = kRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 4 * channels; f.mFramesPerPacket = 1; f.mBytesPerFrame = 4 * channels;
    f.mChannelsPerFrame = channels; f.mBitsPerChannel = 32;
    return f;
}
#define REPLY(type, value) do { if (inDataSize < sizeof(type)) return kAudioHardwareBadPropertySizeError; *(type *)outData = (value); *outDataSize = sizeof(type); return 0; } while (0)
#define REPLY_STR(text) REPLY(CFStringRef, CFSTR(text))
static UInt32 latencyFrames(void) {return gControl ? gControl->lead_frames : 2 * SHENG_AUDIO_PERIOD_FRAMES;}

// ---- IUnknown --------------------------------------------------------------------------------------------------------------------
static HRESULT QueryInterface(void *self, REFIID uuid, LPVOID *out);
static ULONG AddRef(void *self) {(void)self; return atomic_fetch_add(&gRefs, 1) + 1;}
static ULONG Release(void *self) {(void)self; UInt32 r = atomic_load(&gRefs); if (r) r = atomic_fetch_sub(&gRefs, 1) - 1; return r;}

// The nonwaiting registry lookup makes an already-ready output visible during
// HAL's initial enumeration. Retry in a worker only when hardware is absent.
static OSStatus Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef host) {
    (void)d;
    gHost = host;
    pthread_mutex_lock(&gLock);
    const int alreadyConnected=gControl!=NULL;
    int connected=connectService()==0;
    if(connected && alreadyConnected)setHostClock();
    pthread_mutex_unlock(&gLock);
    syslog(LOG_NOTICE,"PongAudio HAL: AudioServerPlugIn initialized connected=%d",connected);
    // Explicitly publish the real endpoint after Initialize returns, even
    // when the DSP was already ready during initial bundle loading.
    pthread_t worker;
    int created=pthread_create(&worker,NULL,awaitService,NULL);
    if(!created)pthread_detach(worker);
    else syslog(LOG_ERR,"PongAudio HAL: discovery worker unavailable result=%d",created);
    // AudioServerPlugIn must never reenter the CoreAudio client HAL API.
    // The host owns discovery and route selection; callbacks above publish
    // the actual physical device after Initialize has returned.
    return 0;
}
static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *c, AudioObjectID *o) {(void)d; (void)desc; (void)c; (void)o; return kAudioHardwareUnsupportedOperationError;}
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID o) {(void)d; (void)o; return kAudioHardwareUnsupportedOperationError;}
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) {(void)d; (void)o; (void)c; return 0;}
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) {(void)d; (void)o; (void)c; return 0;}
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i) {(void)d; (void)o; (void)a; (void)i; return 0;}
static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i) {(void)d; (void)o; (void)a; (void)i; return 0;}

// ---- Properties ------------------------------------------------------------------------------------------------------------------
static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a);
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qs, const void *q, UInt32 *size);
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                                const void *q, UInt32 inDataSize, UInt32 *outDataSize, void *outData);

static bool deviceGainProperty(AudioObjectID o,const AudioObjectPropertyAddress *a) {
    return o==kDevice && a->mScope==kAudioObjectPropertyScopeOutput && a->mElement<=2 &&
        (a->mSelector==kPongDeviceVolumeScalar || a->mSelector==kPongDeviceVolumeDecibels ||
         a->mSelector==kPongDeviceMute);
}
static void gainChanged(void) {
    if(!gHost)return;
    AudioObjectPropertyAddress control[2]={{kAudioLevelControlPropertyScalarValue,kAudioObjectPropertyScopeGlobal,0},
        {kAudioLevelControlPropertyDecibelValue,kAudioObjectPropertyScopeGlobal,0}};
    AudioObjectPropertyAddress device[3]={{kPongDeviceVolumeScalar,kAudioObjectPropertyScopeOutput,0},
        {kPongDeviceVolumeDecibels,kAudioObjectPropertyScopeOutput,0},
        {kPongDeviceMute,kAudioObjectPropertyScopeOutput,0}};
    gHost->PropertiesChanged(gHost,kVolume,2,control);
    const AudioObjectPropertyAddress mute={kAudioBooleanControlPropertyValue,kAudioObjectPropertyScopeGlobal,0};
    gHost->PropertiesChanged(gHost,kMute,1,&mute);
    gHost->PropertiesChanged(gHost,kDevice,3,device);
}

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out) {
    (void)d; (void)pid;
    *out = (o == kVolume && (a->mSelector == kAudioLevelControlPropertyScalarValue || a->mSelector == kAudioLevelControlPropertyDecibelValue)) ||
           (o == kMute && a->mSelector == kAudioBooleanControlPropertyValue) ||
           (o == kOutStream && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat || a->mSelector == kAudioStreamPropertyIsActive)) ||
           (o == kDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate) || deviceGainProperty(o,a);
    return HasProperty(d, o, pid, a) ? 0 : kAudioHardwareUnknownPropertyError;
}

static Float32 toDb(Float32 scalar) {return scalar <= 0.0001f ? -96.0f : 20.0f * log10f(scalar);}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                                const void *q, UInt32 size, const void *data) {
    (void)d; (void)pid; (void)qs; (void)q;
    if((deviceGainProperty(o,a) && a->mSelector==kPongDeviceMute) ||
       (o==kMute && a->mSelector==kAudioBooleanControlPropertyValue)) {
        if(size!=sizeof(UInt32))return kAudioHardwareBadPropertySizeError;
        UInt32 muted=*(const UInt32 *)data;
        if(muted>1)return kAudioHardwareIllegalOperationError;
        atomic_store(&gMuted,muted);if(gControl)__atomic_store_n(&gControl->out_muted,muted,__ATOMIC_RELEASE);gainChanged();return 0;
    }
    if (o == kVolume || deviceGainProperty(o,a)) {
        if(size!=sizeof(Float32))return kAudioHardwareBadPropertySizeError;
        Float32 v = *(const Float32 *)data;
        if (a->mSelector == kAudioLevelControlPropertyDecibelValue || a->mSelector==kPongDeviceVolumeDecibels)
            v = v <= -96.0f ? 0.0f : powf(10.0f, v / 20.0f);
        else if (a->mSelector != kAudioLevelControlPropertyScalarValue && a->mSelector!=kPongDeviceVolumeScalar)
            return kAudioHardwareUnknownPropertyError;
        if(!isfinite(v))return kAudioHardwareIllegalOperationError;
        gVolume = v < 0 ? 0 : v > 1 ? 1 : v;
        if(gControl)__atomic_store_n(&gControl->out_gain_q16,(UInt32)(gVolume*65536.0f),__ATOMIC_RELEASE);
        gainChanged();
        return 0;
    }
    if(o==kOutStream && a->mSelector==kAudioStreamPropertyIsActive) {
        if(size!=sizeof(UInt32))return kAudioHardwareBadPropertySizeError;
        const UInt32 active=*(const UInt32 *)data;
        if(active>1)return kAudioHardwareIllegalOperationError;
        if(atomic_exchange(&gOutActive,active)!=active && gHost)
            gHost->PropertiesChanged(gHost,kOutStream,1,a);
        return 0;
    }
    if (o == kOutStream && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        if(size!=sizeof(AudioStreamBasicDescription))return kAudioHardwareBadPropertySizeError;
        const AudioStreamBasicDescription *f = data, expected = format(2);
        return f->mSampleRate==expected.mSampleRate && f->mFormatID==expected.mFormatID &&
            f->mFormatFlags==expected.mFormatFlags && f->mBytesPerPacket==expected.mBytesPerPacket &&
            f->mFramesPerPacket==1 && f->mBytesPerFrame==expected.mBytesPerFrame &&
            f->mChannelsPerFrame==2 && f->mBitsPerChannel==32 ? 0 : kAudioDeviceUnsupportedFormatError;
    }
    if (o == kDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate)
        return size >= sizeof(Float64) && *(const Float64 *)data == kRate ? 0 : kAudioDeviceUnsupportedFormatError;
    return kAudioHardwareUnknownPropertyError;
}

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a) {
    UInt32 size = 0;
    return GetPropertyDataSize(d, o, pid, a, 0, NULL, &size) == 0;
}

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qs, const void *q, UInt32 *size) {
    // Conversion selectors read their input value even for a size query.
    // Give that path defined input rather than uninitialized stack bytes.
    UInt8 scratch[1024] = {0};
    UInt32 written = 0;
    OSStatus r = GetPropertyData(d, o, pid, a, qs, q, sizeof scratch, &written, scratch);
    if (r == 0) *size = written;
    return r;
}

static UInt32 objects(AudioObjectID o, AudioObjectPropertyScope scope, AudioObjectID *list) {
    UInt32 n = 0;
    if (o == kPlugIn && ready()) list[n++] = kDevice;
    else if (o == kDevice) {
        if (scope != kAudioObjectPropertyScopeInput) list[n++] = kOutStream;
        if (scope != kAudioObjectPropertyScopeInput) list[n++] = kVolume;
        if (scope != kAudioObjectPropertyScopeInput) list[n++] = kMute;
    }
    return n;
}

static OSStatus propertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                             const void *q, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    (void)d; (void)pid;
    if(o==kInStream)return kAudioHardwareBadObjectError;
    const AudioObjectPropertySelector s = a->mSelector;
    // iOS18 enumerates these lists before accepting a newly discovered
    // device. An empty supported list differs from an unknown property.
    if(o>=kPlugIn && o<=kMute &&
       (s==kAudioObjectPropertyCustomPropertyInfoList ||
        (o==kPlugIn && (s==kAudioPlugInPropertyBoxList || s==kAudioPlugInPropertyClockDeviceList)) ||
        (o==kOutStream && s=='taps'))) {
        *outDataSize=0;return 0;
    }
    switch (o) {
    case kPlugIn:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: REPLY_STR("UniXNU");
        case kAudioObjectPropertyName: REPLY_STR("Pong Audio");
        case kAudioPlugInPropertyBundleID: REPLY_STR(kPlugInBundleID);
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            if (!ready() || inDataSize < sizeof(AudioObjectID)) {*outDataSize = 0; return 0;}
            ((AudioObjectID *)outData)[0] = kDevice; *outDataSize = sizeof(AudioObjectID); return 0;
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            CFStringRef uid = q && qs >= sizeof(CFStringRef) ? *(const CFStringRef *)q : NULL;
            REPLY(AudioObjectID, ready() && uid && CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo ? kDevice : kAudioObjectUnknown);
        }
        case kAudioPlugInPropertyResourceBundle: REPLY_STR("");
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kDevice:
        switch (s) {
        case kPongDeviceDbRange:
            if(a->mScope!=kAudioObjectPropertyScopeOutput || a->mElement>2)return kAudioHardwareUnknownPropertyError;
            REPLY(AudioValueRange,((AudioValueRange){-96.0,0.0}));
        case kPongDeviceScalarToDb: case kPongDeviceDbToScalar: {
            if(a->mScope!=kAudioObjectPropertyScopeOutput || a->mElement>2)return kAudioHardwareUnknownPropertyError;
            if(inDataSize<sizeof(Float32))return kAudioHardwareBadPropertySizeError;
            Float32 value=*(Float32 *)outData;
            if(!isfinite(value))return kAudioHardwareIllegalOperationError;
            if(s==kPongDeviceScalarToDb)value=toDb(value<0?0:value>1?1:value);
            else value=value<=-96?0:value>=0?1:powf(10.0f,value/20.0f);
            REPLY(Float32,value);
        }
        // Legacy device-volume queries and the native level-control object
        // must describe the same gain actually applied to PCM, including mute.
        case kPongDeviceVolumeScalar:
            if(!deviceGainProperty(o,a))return kAudioHardwareUnknownPropertyError;
            REPLY(Float32,currentVolume());
        case kPongDeviceVolumeDecibels:
            if(!deviceGainProperty(o,a))return kAudioHardwareUnknownPropertyError;
            REPLY(Float32,toDb(currentVolume()));
        case kPongDeviceMute:
            if(!deviceGainProperty(o,a))return kAudioHardwareUnknownPropertyError;
            REPLY(UInt32,gControl?__atomic_load_n(&gControl->out_muted,__ATOMIC_ACQUIRE):atomic_load(&gMuted));
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kPlugIn);
        // Keep the board's display name separate from its stable UID.
        // VirtualAudio dispatches Apple's reserved UIDs before transport;
        // PongAudioDevice remains a generic driver device in 22H374.
        case kAudioObjectPropertyName: REPLY_STR("Pong Stereo Speakers");
        case kAudioObjectPropertyManufacturer: REPLY_STR("UniXNU");
        case kAudioDevicePropertyDeviceUID: REPLY_STR(kDeviceUID);
        case kAudioDevicePropertyModelUID: REPLY_STR(kModelUID);
        case kAudioObjectPropertyModelName: REPLY_STR("Pong Stereo Speaker");
        case kAudioObjectPropertySerialNumber: REPLY_STR("Pong-SM8475");
        case kAudioObjectPropertyFirmwareVersion: REPLY_STR("1.0");
        case kAudioObjectPropertyIdentify: REPLY(UInt32, 0);
        case kAudioObjectPropertyElementName: REPLY_STR("");
        case kAudioDevicePropertyConfigurationApplication: REPLY_STR("com.apple.Preferences");
        case 'accs': REPLY(UInt32, 0);
        case 'auto': REPLY(UInt32, 0);
        // Use upstream's driver-device routing class. BuiltIn requires
        // Apple port descriptions that this ASPL bridge does not provide.
        case kAudioDevicePropertyTransportType: REPLY(UInt32, kAudioDeviceTransportTypeThunderbolt);
        case kAudioDevicePropertyRelatedDevices: REPLY(AudioObjectID, kDevice);
        case kAudioDevicePropertyClockDomain: REPLY(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: REPLY(UInt32, ready());
        case kAudioDevicePropertyDeviceIsRunning: REPLY(UInt32, gIoUsers > 0);
        // Darwin24's kAudioDevicePropertySupportsIsolatedIO is a UInt32.
        // This ASPL implements shared output IO, without isolated callbacks.
        case 'siso': REPLY(UInt32, 0);
        // 22H374 kAudioDevicePropertyWantsDisplayRouting, also UInt32.
        // TFA9873 speakers are not an HDMI/DisplayPort audio endpoint.
        case 'drte': REPLY(UInt32, 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: REPLY(UInt32, ready() && a->mScope!=kAudioObjectPropertyScopeInput);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: REPLY(UInt32, ready() && a->mScope!=kAudioObjectPropertyScopeInput);
        case kAudioDevicePropertyLatency: REPLY(UInt32, latencyFrames());
        case kAudioDevicePropertySafetyOffset: REPLY(UInt32, latencyFrames()+2*SHENG_AUDIO_PERIOD_FRAMES);
        case kAudioDevicePropertyNominalSampleRate: REPLY(Float64, kRate);
        case kAudioDevicePropertyAvailableNominalSampleRates: REPLY(AudioValueRange, ((AudioValueRange){kRate, kRate}));
        case kAudioDevicePropertyIsHidden: REPLY(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: REPLY(UInt32, SHENG_AUDIO_RING_FRAMES);
        case kAudioDevicePropertyClockIsStable: REPLY(UInt32, 1);
        case kAudioDevicePropertyClockAlgorithm: REPLY(UInt32, kAudioDeviceClockAlgorithmRaw);
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if(a->mScope==kAudioObjectPropertyScopeInput) {*outDataSize=0;return 0;}
            if(inDataSize<2*sizeof(UInt32))return kAudioHardwareBadPropertySizeError;
            UInt32 *channels=outData;channels[0]=1;channels[1]=2;
            *outDataSize=2*sizeof(UInt32);return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            const UInt32 channels=a->mScope==kAudioObjectPropertyScopeInput?0:2;
            const UInt32 bytes=(UInt32)(offsetof(AudioChannelLayout,mChannelDescriptions)+channels*sizeof(AudioChannelDescription));
            if(inDataSize<bytes)return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout *layout=outData;memset(layout,0,bytes);
            layout->mChannelLayoutTag=kAudioChannelLayoutTag_UseChannelDescriptions;
            layout->mNumberChannelDescriptions=channels;
            if(channels) {layout->mChannelDescriptions[0].mChannelLabel=kAudioChannelLabel_Left;
                layout->mChannelDescriptions[1].mChannelLabel=kAudioChannelLabel_Right;}
            *outDataSize=bytes;return 0;
        }
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList: {
            AudioObjectID list[3];
            UInt32 n = objects(kDevice, a->mScope, list), out = 0;
            for (UInt32 i = 0; i < n; ++i) {
                if(s==kAudioObjectPropertyOwnedObjects && q && qs) {
                    if(qs%sizeof(AudioClassID))return kAudioHardwareBadPropertySizeError;
                    const AudioClassID child=list[i]==kVolume?kAudioVolumeControlClassID:
                        list[i]==kMute?kAudioMuteControlClassID:kAudioStreamClassID;
                    const AudioClassID base=list[i]==kVolume?kAudioLevelControlClassID:
                        list[i]==kMute?kAudioBooleanControlClassID:kAudioObjectClassID;
                    bool accepted=false;
                    for(UInt32 k=0;k<qs/sizeof(AudioClassID);k++) {
                        AudioClassID wanted=((const AudioClassID *)q)[k];
                        if(wanted==child || wanted==base || wanted==kAudioObjectClassID ||
                           ((list[i]==kVolume || list[i]==kMute) && wanted==kAudioControlClassID))accepted=true;
                    }
                    if(!accepted)continue;
                }
                if (s == kAudioDevicePropertyStreams && list[i] != kOutStream) continue;
                if (s == kAudioObjectPropertyControlList && list[i] == kOutStream) continue;
                if ((out + 1) * sizeof(AudioObjectID) > inDataSize) break;
                ((AudioObjectID *)outData)[out++] = list[i];
            }
            *outDataSize = out * (UInt32)sizeof(AudioObjectID); return 0;
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kOutStream:
    case kInStream: {
        const UInt32 channels = o == kOutStream ? 2 : 1;
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kDevice);
        case kAudioObjectPropertyName: if (o == kOutStream) REPLY_STR("Pong Speaker Output"); REPLY_STR("Microphone");
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioStreamPropertyIsActive: REPLY(UInt32, atomic_load(&gOutActive));
        case kAudioStreamPropertyDirection: REPLY(UInt32, o == kInStream);
        case kAudioStreamPropertyTerminalType: REPLY(UInt32, o == kOutStream ? kAudioStreamTerminalTypeSpeaker : kAudioStreamTerminalTypeMicrophone);
        case kAudioStreamPropertyStartingChannel: REPLY(UInt32, 1);
        case kAudioStreamPropertyLatency: REPLY(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: REPLY(AudioStreamBasicDescription, format(channels));
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription r = {format(channels), {kRate, kRate}};
            REPLY(AudioStreamRangedDescription, r);
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    }
    case kVolume:
        switch (s) {
        case kAudioObjectPropertyName: REPLY_STR("Pong Speaker Volume");
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioLevelControlClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioVolumeControlClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kDevice);
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioControlPropertyScope: REPLY(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: REPLY(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
        case kAudioLevelControlPropertyScalarValue: REPLY(Float32, currentVolume());
        case kAudioLevelControlPropertyDecibelValue: REPLY(Float32, toDb(currentVolume()));
        case kAudioLevelControlPropertyDecibelRange: REPLY(AudioValueRange, ((AudioValueRange){-96.0, 0.0}));
        case kAudioLevelControlPropertyConvertScalarToDecibels:
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            *(Float32 *)outData = toDb(*(Float32 *)outData); *outDataSize = sizeof(Float32); return 0;
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            *(Float32 *)outData = *(Float32 *)outData <= -96.0f ? 0.0f : powf(10.0f, *(Float32 *)outData / 20.0f); *outDataSize = sizeof(Float32); return 0;
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kMute:
        switch(s) {
        case kAudioObjectPropertyName: REPLY_STR("Pong Speaker Mute");
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID,kAudioBooleanControlClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID,kAudioMuteControlClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID,kDevice);
        case kAudioObjectPropertyOwnedObjects: *outDataSize=0;return 0;
        case kAudioControlPropertyScope: REPLY(AudioObjectPropertyScope,kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: REPLY(AudioObjectPropertyElement,kAudioObjectPropertyElementMain);
        case kAudioBooleanControlPropertyValue:
            REPLY(UInt32,gControl?__atomic_load_n(&gControl->out_muted,__ATOMIC_ACQUIRE):atomic_load(&gMuted));
        default:return kAudioHardwareUnknownPropertyError;
        }
    default:
        return kAudioHardwareBadObjectError;
    }
}

// Keep missing Darwin24 properties distinguishable from a dead DSP, without
// flooding the console or returning invented values to retain a route.
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
    const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 size,
    UInt32 *written, void *data) {
    OSStatus r=propertyData(d,o,pid,a,qs,q,size,written,data);
    if(r) {
        static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
        static struct {AudioObjectID object;UInt32 selector,scope;OSStatus error;} seen[48];
        static unsigned count;
        pthread_mutex_lock(&lock);
        unsigned i=0;
        for(;i<count;i++)if(seen[i].object==o && seen[i].selector==a->mSelector &&
            seen[i].scope==a->mScope && seen[i].error==r)break;
        if(i==count && count<48) {
            seen[count].object=o;seen[count].selector=a->mSelector;
            seen[count].scope=a->mScope;seen[count++].error=r;
            UInt32 s=a->mSelector;
            syslog(LOG_NOTICE,"PongAudio HAL: property object=%u selector=%c%c%c%c scope=%x error=%d",
                o,(char)(s>>24),(char)(s>>16),(char)(s>>8),(char)s,a->mScope,(int)r);
        }
        pthread_mutex_unlock(&lock);
    }
    return r;
}

// ---- IO ----------------------------------------------------------------------------------------------------------------------------
static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client) {
    (void)d; (void)client;
    if (o != kDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    OSStatus r = 0;
    if (gIoUsers == 0) {
        if (connectService()) r = kAudioHardwareNotRunningError;
        else if (call(ShengAudioStartOutput) != KERN_SUCCESS) r = kAudioHardwareNotRunningError;
        else {
            setHostClock();
            unsigned i;
            for(i=0;i<200 && ready() && !(gControl->state&ShengAudioOutputRunning);i++)usleep(10000);
            if(!ready() || !(gControl->state&ShengAudioOutputRunning)) {
                call(ShengAudioStopOutput);r=kAudioHardwareNotRunningError;
            } else {
                uint64_t host=mach_absolute_time(),anchor=gControl->out_anchor_host;
                gZeroOrigin=host>anchor?(uint64_t)((Float64)(host-anchor)/gHostTicksPerFrame):0;
                gZeroSeed++;
            }
        }

    }
    if (!r) gIoUsers++;
    pthread_mutex_unlock(&gLock);
    syslog(r?LOG_ERR:LOG_NOTICE,"PongAudio HAL: StartIO result=%d",r);
    return r;
}
static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client) {
    (void)d; (void)client;
    if (o != kDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    if (gIoUsers && --gIoUsers == 0) {call(ShengAudioStopOutput);}
    pthread_mutex_unlock(&gLock);
    return 0;
}
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, Float64 *sampleTime, UInt64 *hostTime, UInt64 *seed) {
    (void)d; (void)o; (void)client;
    if (!ready()) return kAudioHardwareNotRunningError;
    // HAL queries this clock while discovering an idle device too. The
    // shared anchor is established when the physical service is published.
    uint32_t seq=0;
    uint64_t anchor=0;
    bool stable=false;
    for(unsigned attempt=0;attempt<128;attempt++) {
        seq=__atomic_load_n(&gControl->out_seq,__ATOMIC_ACQUIRE);
        if(seq&1)continue;
        anchor=__atomic_load_n(&gControl->out_anchor_host,__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if(seq==__atomic_load_n(&gControl->out_seq,__ATOMIC_RELAXED)){stable=true;break;}
    }
    if(!stable)return kAudioHardwareNotRunningError;
    const uint64_t now = mach_absolute_time();
    const Float64 period = gHostTicksPerFrame * SHENG_AUDIO_RING_FRAMES;
    const uint64_t origin=anchor+(uint64_t)((Float64)gZeroOrigin*gHostTicksPerFrame);
    const uint64_t wraps = now > origin ? (uint64_t)((Float64)(now - origin) / period) : 0;
    *sampleTime = (Float64)(gZeroOrigin+wraps * SHENG_AUDIO_RING_FRAMES);
    *hostTime = origin + (uint64_t)((Float64)wraps * period);
    *seed = 1 + seq / 2 + gZeroSeed;
    return 0;
}
static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, Boolean *will, Boolean *inPlace) {
    (void)d; (void)o; (void)client;
    *will = op == kAudioServerPlugInIOOperationWriteMix;
    *inPlace = true;
    return 0;
}
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *info) {
    (void)d; (void)o; (void)client; (void)op; (void)frames; (void)info; return 0;
}
static inline int16_t toS16(Float32 v) {if(!isfinite(v))v=0; v *= 32767.0f; return (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);}
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, AudioObjectID stream, UInt32 client, UInt32 op, UInt32 frames,
                              const AudioServerPlugInIOCycleInfo *info, void *main, void *secondary) {
    (void)d; (void)o; (void)client; (void)secondary;
    if (!ready()) return kAudioHardwareNotRunningError;
    if (op == kAudioServerPlugInIOOperationWriteMix && stream == kOutStream) {
        const Float32 *src = main;
        uint64_t t = (uint64_t)info->mOutputTime.mSampleTime;
        const bool muted=gControl?__atomic_load_n(&gControl->out_muted,__ATOMIC_ACQUIRE)!=0:atomic_load(&gMuted)!=0;
        const Float32 gain=muted?0.0f:currentVolume();
        const Boolean active = atomic_load(&gOutActive);
        for (UInt32 i = 0; i < frames; ++i, ++t) {
            int16_t *dst = gOut + 2 * (t % SHENG_AUDIO_RING_FRAMES);
            dst[0] = active ? toS16(src[2 * i] * gain) : 0;
            dst[1] = active ? toS16(src[2 * i + 1] * gain) : 0;
        }
    }

    return 0;
}
static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *info) {
    (void)d; (void)o; (void)client; (void)op; (void)frames; (void)info; return 0;
}

static AudioServerPlugInDriverInterface gInterface = {
    NULL, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice, AddDeviceClient, RemoveDeviceClient,
    PerformDeviceConfigurationChange, AbortDeviceConfigurationChange, HasProperty, IsPropertySettable, GetPropertyDataSize,
    GetPropertyData, SetPropertyData, StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginIOOperation, DoIOOperation, EndIOOperation};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;

static HRESULT QueryInterface(void *self, REFIID uuid, LPVOID *out) {
    if (!out) return E_POINTER;
    *out = NULL;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
    if (!requested) return E_NOINTERFACE;
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID)) {
        AddRef(self);
        *out = &gInterfacePtr;
        r = S_OK;
    }
    CFRelease(requested);
    return r;
}

__attribute__((visibility("default"))) void *PongAudio_Create(CFAllocatorRef allocator, CFUUIDRef type) {
    (void)allocator;
    if (!type || !CFEqual(type, kAudioServerPlugInTypeUUID)) return NULL;
    syslog(LOG_NOTICE,"PongAudio HAL: AudioServerPlugIn factory accepted");
    /* A supported AudioServerPlugIn type has a stable IUnknown interface.
     * Hardware readiness governs DeviceList, independently of factory load.
     * This bundle must be installed as .driver, never as a legacy AHP plugin. */
    if (!gHostTicksPerFrame) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        gHostTicksPerFrame = 1e9 / kRate * tb.denom / tb.numer;
    }
    // Prepare an already-existing physical endpoint before handing out its
    // interface. Darwin24 may query DeviceList on the factory interface;
    // an empty initial list must not hide a service that was ready all along.
    // Registry lookup is nonwaiting, graph/stream start stays in StartIO,
    // and the host callback is not used until Initialize supplies it.
    pthread_mutex_lock(&gLock);(void)connectService();pthread_mutex_unlock(&gLock);
    return &gInterfacePtr;
}
