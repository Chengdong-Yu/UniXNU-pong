/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Shared native route selection. In the ASPL this runs only on its worker,
 * after Initialize and PropertiesChanged returned and without a driver lock.
 * AudioObject IDs belong to this process's HALS_System; a separate helper
 * selecting its own default devices does not prove audiomxd's route.
 */
#pragma once
#include <CoreAudio/AudioHardwareBase.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/sysctl.h>
#include <mach-o/dyld.h>
#ifndef PONG_ROUTE_LOG
#define PONG_ROUTE_LOG(...) fprintf(stderr,__VA_ARGS__)
#endif

// AudioHardwareBase.h is the shared ASPL header; the iPhone SDK omits
// AudioHardware.h. These system-object selectors are also present in the
// selected Darwin24 CoreAudio runtime.
enum { PongSystemObject=1, PongDevices='dev#', PongDefaultOutput='dOut', PongDefaultSystemOutput='sOut' };

static OSStatus (*getSize)(AudioObjectID,const AudioObjectPropertyAddress *,UInt32,const void *,UInt32 *);
static OSStatus (*getData)(AudioObjectID,const AudioObjectPropertyAddress *,UInt32,const void *,UInt32 *,void *);
static OSStatus (*setData)(AudioObjectID,const AudioObjectPropertyAddress *,UInt32,const void *,UInt32,const void *);
static OSStatus (*embeddedSetData)(AudioObjectID,const AudioObjectPropertyAddress *,UInt32,const void *,UInt32,const void *);
static int (*makeConnectedPortRoutable)(UInt32);
static OSStatus (*configureNativeRoute)(CFDictionaryRef,UInt32,CFTypeRef,UInt32 *);
// Embedded MediaExperience installs its own HAL setter in gCMSM. Its
// native route configuration uses that callback, not the generic client
// AudioObjectSetPropertyData path which rejects rtcf with 'what' on 22H374.
static void resolveEmbeddedSetter(void) {
    static int attempted;
    if(attempted)return;
    attempted=1;
    void *media=dlopen("/System/Library/PrivateFrameworks/MediaExperience.framework/MediaExperience",RTLD_NOW|RTLD_LOCAL);
    if(!media)return;
    void *setter=dlsym(media,"CMSMVAUtility_AudioObjectSetPropertyData");
    makeConnectedPortRoutable=dlsym(media,"CMSMVAUtility_MakeConnectedPortRoutable");
    configureNativeRoute=dlsym(media,"vaemSetRouteConfigurationDictionaryOnVAD");
    if(!setter) {
        char version[32]={0};size_t size=sizeof(version);
        if(sysctlbyname("kern.osversion",version,&size,NULL,0) || strcmp(version,"22H374"))return;
        // Local-symbol adapter for this supplied cache only. Verify the
        // actual callback thunk contract before invoking its six-arg ABI.
        static const unsigned char thunk[]={0x28,0x44,0x30,0xf0,0x08,0xe1,0x31,0x91,
            0x06,0x15,0x40,0xf9,0x46,0x00,0x00,0xb4,0xc0,0x00,0x1f,0xd6,
            0x00,0x00,0x80,0x52,0xc0,0x03,0x5f,0xd6};
        for(uint32_t i=0;i<_dyld_image_count();i++) {
            const char *name=_dyld_get_image_name(i);
            if(name && strstr(name,"/MediaExperience.framework/MediaExperience") &&
               !strcmp(strstr(name,"/MediaExperience.framework/MediaExperience"),"/MediaExperience.framework/MediaExperience")) {
                const unsigned char *candidate=(const unsigned char *)_dyld_get_image_header(i)+0x7c14;
                if(!memcmp(candidate,thunk,sizeof(thunk)))setter=(void *)candidate;
                static const unsigned char routable[]={0xf4,0x4f,0xbe,0xa9,0xfd,0x7b,0x01,0xa9,
                    0xfd,0x43,0x00,0x91,0xf4,0x03,0x00,0xaa,0x08,0x3a,0x30,0xd0,
                    0x08,0xe1,0x31,0x91,0x03,0xb5,0x40,0xf9,0x21,0x00,0x80,0x52};
                candidate=(const unsigned char *)_dyld_get_image_header(i)+0x14cde8;
                if(!makeConnectedPortRoutable && !memcmp(candidate,routable,sizeof(routable)))
                    makeConnectedPortRoutable=(void *)candidate;
                // Use MediaExperience's own initialized plug-in identity and
                // response handling. Public HAL object IDs must not be
                // substituted for the embedded route manager's identity.
                static const unsigned char route[]={0xff,0x03,0x02,0xd1,0xf8,0x5f,0x04,0xa9,
                    0xf6,0x57,0x05,0xa9,0xf4,0x4f,0x06,0xa9,0xfd,0x7b,0x07,0xa9,
                    0xfd,0xc3,0x01,0x91,0xf6,0x03,0x03,0xaa};
                candidate=(const unsigned char *)_dyld_get_image_header(i)+0x700ec;
                if(!configureNativeRoute && !memcmp(candidate,route,sizeof(route)))
                    configureNativeRoute=(void *)candidate;
                break;
            }
        }
    }
    if(setter){embeddedSetData=setter;PONG_ROUTE_LOG("Pong audio route: native MediaExperience HAL setter selected\n");}
}
static AudioObjectID pluginID(CFStringRef bundle) {
    // 22H374 vaemGetVirtualAudioPlugin uses an AudioValueTranslation
    // payload for pibi, not a CFString qualifier and a device-sized reply.
    AudioObjectID result=0;
    struct {const void *input;UInt32 inputBytes;void *output;UInt32 outputBytes;}
        translation={&bundle,sizeof(bundle),&result,sizeof(result)};
    _Static_assert(sizeof(translation)==32,"22H374 plug-in lookup ABI");
    AudioObjectPropertyAddress p={'pibi',kAudioObjectPropertyScopeGlobal,0};
    UInt32 bytes=sizeof(translation);
    OSStatus status=getData(PongSystemObject,&p,0,NULL,&bytes,&translation);
    if(status || bytes!=sizeof(translation) || !result)return 0;
    UInt32 kind=0;bytes=sizeof(kind);
    p=(AudioObjectPropertyAddress){kAudioObjectPropertyClass,kAudioObjectPropertyScopeGlobal,0};
    return !getData(result,&p,0,NULL,&bytes,&kind) && kind==kAudioPlugInClassID?result:0;
}
static int liveOutput(AudioObjectID device) {
    AudioObjectPropertyAddress p={kAudioDevicePropertyDeviceIsAlive,kAudioObjectPropertyScopeGlobal,0};
    UInt32 alive=0,n=sizeof(alive);
    if(getData(device,&p,0,NULL,&n,&alive) || !alive)return 0;
    p=(AudioObjectPropertyAddress){kAudioDevicePropertyStreams,kAudioObjectPropertyScopeOutput,0};
    n=0;return !getSize(device,&p,0,NULL,&n) && n>=sizeof(AudioObjectID);
}
static AudioObjectID nativeUID(CFStringRef uid) {
    AudioObjectPropertyAddress p={'uidd',kAudioObjectPropertyScopeGlobal,0};
    AudioObjectID result=0;UInt32 bytes=sizeof(result);
    return !getData(PongSystemObject,&p,sizeof(uid),&uid,&bytes,&result) && result && liveOutput(result)?result:0;
}
static AudioObjectID findOutput(void) {
    // Prefer the real kernel PCM endpoint when CoreAudio has discovered
    // it. Its existence and output stream are checked, never fabricated.
    AudioObjectID native=nativeUID(CFSTR("Speaker"));
    if(!native)native=nativeUID(CFSTR("PongSpeakerNative"));
    if(native)return native;
    // Embedded CoreAudio can hide a physical driver from the default
    // aggregate's initial list. Ask the native UID translator first.
    CFStringRef requested=CFSTR("PongAudioDevice");
    AudioObjectPropertyAddress translate={'uidd',kAudioObjectPropertyScopeGlobal,0};
    AudioObjectID translated=0;UInt32 size=sizeof(translated);
    if(!getData(PongSystemObject,&translate,sizeof(requested),&requested,&size,&translated) &&
       translated && liveOutput(translated))return translated;
    // Ask the actual ASPL host as well: a freshly published device need
    // not yet be in the system object's cached aggregate device list.
    CFStringRef bundle=CFSTR("org.unixnu.pong.audio");
    AudioObjectID plugin=pluginID(bundle);
    if(plugin) {
        AudioObjectPropertyAddress list={kAudioPlugInPropertyDeviceList,kAudioObjectPropertyScopeGlobal,0};
        AudioObjectID actual[16];UInt32 n=sizeof(actual);
        if(!getData(plugin,&list,0,NULL,&n,actual) && n<=sizeof(actual))
            for(UInt32 i=0;i<n/sizeof(actual[0]);i++)if(liveOutput(actual[i]))return actual[i];
    }
    AudioObjectPropertyAddress p={PongDevices,kAudioObjectPropertyScopeGlobal,0};
    AudioObjectID devices[64];UInt32 bytes=sizeof(devices);
    OSStatus status=getData(PongSystemObject,&p,0,NULL,&bytes,devices);
    if(status || bytes>sizeof(devices)) {
        static unsigned errors;
        if(errors++<3)PONG_ROUTE_LOG("Pong audio route: device enumeration status=%d bytes=%u UID=%u\n",(int)status,bytes,translated);
        return 0;
    }
    for(UInt32 i=0;i<bytes/sizeof(devices[0]);++i) {
        CFStringRef uid=NULL;UInt32 n=sizeof(uid);
        p=(AudioObjectPropertyAddress){kAudioDevicePropertyDeviceUID,kAudioObjectPropertyScopeGlobal,0};
        if(getData(devices[i],&p,0,NULL,&n,&uid) || !uid)continue;
        int match=CFEqual(uid,CFSTR("PongAudioDevice"));CFRelease(uid);
        if(match && liveOutput(devices[i]))return devices[i];
    }
    return 0;
}
/* 22H374 vaemSetRouteConfigurationDictionaryOnVAD sends this 32-byte
 * request to 'rtcf' on the VirtualAudio PLUGIN. gCMSM+0x80 is filled by
 * vaemGetVirtualAudioPlugin's pibi translation, not the VAD device ID.
 * The device exposes vcsp, whereas the plugin owns prts/rtcf. */
static int routePhysicalOutput(AudioObjectID plugin,AudioObjectID vad,AudioObjectID physical) {
    // Match the actual endpoint transport. Built-in output is a speaker
    // port; ptbo is only for the legacy external-driver transport.
    AudioObjectPropertyAddress transportProperty={kAudioDevicePropertyTransportType,kAudioObjectPropertyScopeGlobal,0};
    UInt32 transport=0,transportBytes=sizeof(transport);
    if(getData(physical,&transportProperty,0,NULL,&transportBytes,&transport))return 1;
    const UInt32 portType=transport=='bltn'?'pspk':'ptbo';
    CFArrayRef ports=NULL;UInt32 bytes=sizeof(ports);
    // The 22H374 vaemGetVADPortIDFromVADPortType caller queries 'prts'
    // in global scope; ptbo itself identifies the output port. Using outp
    // here rejected the native query before any route was selected.
    AudioObjectPropertyAddress property={'prts',kAudioObjectPropertyScopeGlobal,0};
    OSStatus status=getData(plugin,&property,sizeof(portType),&portType,&bytes,&ports);
    if(status || !ports) {
        static unsigned failures;
        if(failures++<3)PONG_ROUTE_LOG("Pong audio route: native global ports plugin=%u VAD=%u status=%d bytes=%u\n",plugin,vad,(int)status,bytes);
        return 1;
    }
    if(bytes!=sizeof(ports) || CFGetTypeID(ports)!=CFArrayGetTypeID() || !CFArrayGetCount(ports)){
        static unsigned empty;
        if(empty++<3)PONG_ROUTE_LOG("Pong audio route: native global ports plugin=%u VAD=%u returned no output port\n",plugin,vad);
        CFRelease(ports);return 1;
    }
    // prts returns AudioObjectIDs (the stock vaem caller reads CFNumber).
    // Resolve the actual port's device UID through puid before overriding it;
    // allowing a transport type alone leaves the built-in/null route selected.
    CFStringRef uid=NULL;bytes=sizeof(uid);
    property=(AudioObjectPropertyAddress){kAudioDevicePropertyDeviceUID,kAudioObjectPropertyScopeGlobal,0};
    if(getData(physical,&property,0,NULL,&bytes,&uid) || !uid){CFRelease(ports);return 1;}
    CFTypeRef selectedPort=NULL;UInt32 selectedPortID=0;
    for(CFIndex i=0;i<CFArrayGetCount(ports);i++) {
        CFTypeRef candidate=CFArrayGetValueAtIndex(ports,i);UInt32 port=0;
        if(CFGetTypeID(candidate)!=CFNumberGetTypeID() ||
            !CFNumberGetValue(candidate,kCFNumberSInt32Type,&port) || !port)continue;
        CFStringRef portUID=NULL;bytes=sizeof(portUID);
        property=(AudioObjectPropertyAddress){'puid',kAudioObjectPropertyScopeGlobal,0};
        if(!getData(port,&property,0,NULL,&bytes,&portUID) && portUID) {
            if(CFGetTypeID(portUID)==CFStringGetTypeID()) {
                // A generic driver may namespace its output's UID by direction.
                // Match the published physical UID, never the first arbitrary port.
                CFRange found=CFStringFind(portUID,uid,0);
                if(CFEqual(uid,portUID) || found.location!=kCFNotFound){selectedPort=candidate;selectedPortID=port;}
            }
            CFRelease(portUID);
        }
        if(selectedPort)break;
    }
    CFRelease(uid);
    if(!selectedPort){CFRelease(ports);return 1;}
    // A discovered physical device is not yet an eligible system route.
    // Use the OS's connected-port transition before choosing it on the VAD.
    static UInt32 madeRoutable;
    if(makeConnectedPortRoutable && madeRoutable!=selectedPortID && makeConnectedPortRoutable(selectedPortID))
        madeRoutable=selectedPortID;
    CFArrayRef selected=CFArrayCreate(NULL,&selectedPort,1,&kCFTypeArrayCallBacks);
    CFNumberRef type=CFNumberCreate(NULL,kCFNumberSInt32Type,&portType);
    const void *value=type;
    CFArrayRef allowed=CFArrayCreate(NULL,&value,1,&kCFTypeArrayCallBacks);
    CFMutableDictionaryRef config=CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(config,CFSTR("allowed port types"),allowed);
    CFDictionarySetValue(config,CFSTR("overridden ports"),selected);
    CFDictionarySetValue(config,CFSTR("routable ports"),selected);
    /* Keep session categories/modes under the native policy. This selects
     * an actually connected external-driver port, not a fabricated built-in
     * speaker or microphone description. */
    CFDictionaryRef response=NULL;
    struct {CFDictionaryRef configuration;UInt64 responseCapacity;CFDictionaryRef *response;UInt64 responseBytes;}
        request={config,sizeof(response),&response,sizeof(response)};
    _Static_assert(sizeof(request)==32,"22H374 VirtualAudio route ABI");
    property=(AudioObjectPropertyAddress){'rtcf',kAudioObjectPropertyScopeGlobal,0};
    // rtcf changes native route policy; selecting it repeatedly once/second
    // tears down the same ports while audiomxd is still building the graph.
    static AudioObjectID routedPlugin,routedVAD,routedPhysical;static UInt32 routedPort;
    const int changed=routedPlugin!=plugin || routedVAD!=vad || routedPhysical!=physical || routedPort!=selectedPortID;
    UInt32 reason=0;int acceptedByNative=0;
    status=changed?setData(plugin,&property,0,NULL,sizeof(request),&request):0;
    if(changed && status && configureNativeRoute && !strcmp(getprogname(),"audiomxd")) {
        status=configureNativeRoute(config,0,NULL,&reason);
        acceptedByNative=!status;
    }
    if(changed && status) {
        // Distinguish a missing HAL property from a rejection inside the
        // real VirtualAudio route callback. Never turn either into success.
        static unsigned failures;
        if(failures++<2) {
            UInt32 expected=0;
            OSStatus shape=getSize(plugin,&property,0,NULL,&expected);
            Boolean writable=0;
            OSStatus (*isSettable)(AudioObjectID,const AudioObjectPropertyAddress *,Boolean *)=
                dlsym(RTLD_DEFAULT,"AudioObjectIsPropertySettable");
            OSStatus access=isSettable?isSettable(plugin,&property,&writable):kAudioHardwareUnknownPropertyError;
            PONG_ROUTE_LOG("Pong audio route: rtcf dispatch size-status=%d bytes=%u settable-status=%d writable=%u\n",
                (int)shape,expected,(int)access,writable);
        }
    }
    // The stock 22H374 caller also requires the returned route description;
    // outer HAL success without it is not confirmation of an accepted route.
    if(!status && changed && !acceptedByNative && (!response || CFGetTypeID(response)!=CFDictionaryGetTypeID()))
        status=kAudioHardwareIllegalOperationError;
    // iPadOS18 rejects dOut/sOut writes with 'nope'. Its own route graph,
    // not the desktop HAL default selectors, owns session output and gain.
    UInt32 volume=0;bytes=sizeof(volume);
    property=(AudioObjectPropertyAddress){'vcsp',kAudioObjectPropertyScopeOutput,0};
    OSStatus observed=getData(vad,&property,0,NULL,&bytes,&volume);
    if(!status && !observed && volume && changed){routedPlugin=plugin;routedVAD=vad;routedPhysical=physical;routedPort=selectedPortID;}
    static unsigned reports;
    if(changed || reports++<3 || volume)
        PONG_ROUTE_LOG("Pong audio route: plugin=%u VAD=%u physical=%u connected ports=%ld route=%d volume status=%d capability=%u\n",
            plugin,vad,physical,(long)CFArrayGetCount(ports),(int)status,(int)observed,volume);
    if(response)CFRelease(response);
    CFRelease(config);CFRelease(selected);CFRelease(allowed);CFRelease(type);CFRelease(ports);
    return status || observed || !volume;
}
static int pong_audio_select_native_route(void) {
    void *hal=dlopen("/System/Library/Frameworks/CoreAudio.framework/CoreAudio",RTLD_NOW|RTLD_LOCAL);
    if(!hal){PONG_ROUTE_LOG("Pong audio route: CoreAudio unavailable\n");return 1;}
    getSize=dlsym(hal,"AudioObjectGetPropertyDataSize");getData=dlsym(hal,"AudioObjectGetPropertyData");
    setData=dlsym(hal,"AudioObjectSetPropertyData");
    if(!getSize||!getData||!setData)return 1;
    resolveEmbeddedSetter();
    for(unsigned attempt=0;attempt<30;++attempt) {
        AudioObjectID physical=findOutput();
        if(physical) {
            AudioObjectPropertyAddress p={PongDefaultOutput,kAudioObjectPropertyScopeGlobal,0};
            AudioObjectID current=0;UInt32 bytes=sizeof(current);
            if(getData(PongSystemObject,&p,0,NULL,&bytes,&current))current=0;
            AudioObjectID native=nativeUID(CFSTR("VirtualAudioDevice_Default"));
            AudioObjectID plugin=pluginID(CFSTR("com.apple.audio.CoreAudio.VirtualAudio"));
            AudioObjectID vad=native?native:(current!=physical?current:0);
            if(plugin && vad && !routePhysicalOutput(plugin,vad,physical))return 0;
            if(attempt<3 || !(attempt%10))PONG_ROUTE_LOG("Pong audio route: physical=%u current=%u native=%u; waiting for real VAD driver port\n",physical,current,native);
        }
        usleep(1000000);
    }
    PONG_ROUTE_LOG("Pong audio route: no native output route confirmed; physical endpoint remains published\n");return 1;
}
