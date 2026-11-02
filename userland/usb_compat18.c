/* SPDX-License-Identifier: MIT
 * Native 22H374 USB consumer adaptation to the actual Pong Mux service.
 * No fabricated Apple driver, pairing response, certificate or trust result.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <unistd.h>

extern CFTypeRef MGCopyAnswer(CFStringRef);
extern bool MGGetBoolAnswer(CFStringRef);
extern int32_t MGGetSInt32Answer(CFStringRef,int32_t);
extern int64_t MGGetSInt64Answer(CFStringRef,int64_t);
extern CFDictionaryRef MGCopyMultipleAnswers(CFArrayRef,CFDictionaryRef);
extern const CFStringRef kMGQDiskUsageTotalDataCapacity;
extern const CFStringRef kMGQDiskUsageTotalDataAvailable;
extern const CFStringRef kMGQDiskUsageTotalDiskCapacity;
#include "PongGestaltKeys.h"
static CFDictionaryRef identity;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static void load_identity(void) {
    int fd=open("/pong/usb-board-identity.plist",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return;
    struct stat st;unsigned char bytes[4096];ssize_t length=-1;
    if(!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==0 && !(st.st_mode&022) &&
       st.st_size>0 && st.st_size<=(off_t)sizeof(bytes))length=read(fd,bytes,(size_t)st.st_size);
    close(fd);if(length<=0 || length!=st.st_size)return;
    CFDataRef data=CFDataCreate(NULL,bytes,length);
    CFPropertyListRef plist=data?CFPropertyListCreateWithData(NULL,data,kCFPropertyListImmutable,NULL,NULL):NULL;
    if(data)CFRelease(data);
    if(plist && CFGetTypeID(plist)==CFDictionaryGetTypeID())identity=(CFDictionaryRef)plist;
    else if(plist)CFRelease(plist);
}
CFTypeRef pong_usb_copy_board_identity(CFStringRef key) {
    key=canonical_key(key);
    if(!key)return NULL;
    // These are software identity values derived from this Pong's real
    // Qualcomm serial. Never supply Apple ECID/fuses/activation capabilities.
    const char *process=getprogname();
    bool usb=process && (!strcmp(process,"lockdownd") || !strcmp(process,"usb-mux"));
    if(usb && (CFEqual(key,CFSTR("UniqueChipID")) || CFEqual(key,CFSTR("UniqueDeviceID")) ||
       CFEqual(key,CFSTR("SerialNumber")) || CFEqual(key,CFSTR("ChipID")) ||
       CFEqual(key,CFSTR("ModelNumber")) || CFEqual(key,CFSTR("RegionInfo")) ||
       CFEqual(key,CFSTR("ModelName")) || CFEqual(key,CFSTR("MarketingName")) ||
       CFEqual(key,CFSTR("RegulatoryModelNumber")) || CFEqual(key,CFSTR("HardwareModel")) ||
       CFEqual(key,CFSTR("MLBSerialNumber")) || CFEqual(key,CFSTR("BatterySerialNumber")) ||
       CFEqual(key,CFSTR("FrontCameraModuleSerialNumString")) || CFEqual(key,CFSTR("BackCameraModuleSerialNumString")) ||
       CFEqual(key,CFSTR("FrontFacingCameraModuleSerialNumber")) || CFEqual(key,CFSTR("RearFacingCameraModuleSerialNumber")) ||
       CFEqual(key,CFSTR("PanelSerialNumber")) || CFEqual(key,CFSTR("RawPanelSerialNumber")))) {
        pthread_once(&once,load_identity);
        CFTypeRef value=identity?CFDictionaryGetValue(identity,key):NULL;
        if(value)return CFRetain(value);
    }
    return NULL;
}
static CFTypeRef power_answer(CFStringRef key) {
    const struct {CFStringRef key,property;} names[]={
        {CFSTR("HasBattery"),CFSTR("BatteryInstalled")},
        {CFSTR("GasGaugeCapability"),CFSTR("BatteryInstalled")},
        {CFSTR("BatteryCurrentCapacity"),CFSTR("CurrentCapacity")},
        {CFSTR("BatteryIsCharging"),CFSTR("IsCharging")},
        {CFSTR("BatteryIsFullyCharged"),CFSTR("FullyCharged")},
        {CFSTR("ExternalPowerSourceConnected"),CFSTR("ExternalConnected")},
        {CFSTR("ExternalConnected"),CFSTR("ExternalConnected")},
        {CFSTR("ExternalChargeCapability"),CFSTR("ExternalChargeCapable")},
        {CFSTR("ExternalChargeCapable"),CFSTR("ExternalChargeCapable")},
    };
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++)if(CFEqual(key,names[i].key)) {
        io_service_t battery=IOServiceGetMatchingService(0,IOServiceMatching("IOPMPowerSource"));
        if(!battery)return NULL;
        CFTypeRef value=IORegistryEntryCreateCFProperty(battery,names[i].property,NULL,0);
        IOObjectRelease(battery);
        if(value && CFGetTypeID(value)!=CFBooleanGetTypeID() && CFGetTypeID(value)!=CFNumberGetTypeID()) {
            CFRelease(value);return NULL;
        }
        return value;
    }
    return NULL;
}
static CFTypeRef playback_answer(CFStringRef key) {
    if(!CFEqual(key,CFSTR("AudioPlaybackCapability")))return NULL;
    io_iterator_t iterator=0;
    if(IOServiceGetMatchingServices(0,IOServiceMatching("IOAudio2Device"),&iterator))return NULL;
    io_service_t service;bool found=false;
    while((service=IOIteratorNext(iterator))) {
        CFTypeRef uid=IORegistryEntryCreateCFProperty(service,CFSTR("device UID"),NULL,0);
        CFTypeRef streams=IORegistryEntryCreateCFProperty(service,CFSTR("output streams"),NULL,0);
        found=uid && CFGetTypeID(uid)==CFStringGetTypeID() && CFEqual(uid,CFSTR("Speaker")) &&
              streams && CFGetTypeID(streams)==CFArrayGetTypeID() && CFArrayGetCount(streams)>0;
        if(uid)CFRelease(uid);if(streams)CFRelease(streams);IOObjectRelease(service);
        if(found)break;
    }
    IOObjectRelease(iterator);
    return found?CFRetain(kCFBooleanTrue):NULL;
}
static CFTypeRef storage_answer(CFStringRef key) {
    if(CFEqual(key,CFSTR("TotalDiskCapacity"))) {
        CFMutableDictionaryRef match=IOServiceMatching("PongUfsBlock");
        if(!match)return NULL;
        const void *keys[]={CFSTR("PongDataWindow")},*values[]={kCFBooleanTrue};
        CFDictionaryRef properties=CFDictionaryCreate(NULL,keys,values,1,
            &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
        if(!properties){CFRelease(match);return NULL;}
        CFDictionarySetValue(match,CFSTR("IOPropertyMatch"),properties);CFRelease(properties);
        io_service_t data=IOServiceGetMatchingService(0,match);if(!data)return NULL;
        CFTypeRef size=IORegistryEntrySearchCFProperty(data,kIOServicePlane,CFSTR("Size"),NULL,kIORegistryIterateRecursively);
        IOObjectRelease(data);
        if(size && CFGetTypeID(size)!=CFNumberGetTypeID()){CFRelease(size);return NULL;}
        return size;
    }
    const bool dataCapacity=CFEqual(key,CFSTR("TotalDataCapacity")),dataFree=CFEqual(key,CFSTR("TotalDataAvailable"));
    if(!dataCapacity && !dataFree)return NULL;
    struct statfs fs;
    if(statfs("/private/var",&fs) || !fs.f_bsize)return NULL;
    const uint64_t blocks=dataCapacity?fs.f_blocks:fs.f_bavail;
    if(blocks>INT64_MAX/fs.f_bsize)return NULL;
    const int64_t bytes=(int64_t)(blocks*fs.f_bsize);
    return CFNumberCreate(NULL,kCFNumberSInt64Type,&bytes);
}
static CFTypeRef disk_usage_answer(CFStringRef question) {
    /* 22H374 lockdownd requests MG's DiskUsage dictionary, then uses the
     * exported kMGQDiskUsage... keys. Individual answer overrides never
     * reach that path. Retain the native dictionary and its other policy
     * values; replace only Data's measured capacity/free bytes with SInt64.
     */
    CFTypeRef original=MGCopyAnswer(question);
    if(original && CFGetTypeID(original)!=CFDictionaryGetTypeID())return original;
    CFTypeRef capacity=storage_answer(CFSTR("TotalDataCapacity"));
    CFTypeRef available=storage_answer(CFSTR("TotalDataAvailable"));
    if(!capacity || !available) {
        if(capacity)CFRelease(capacity);if(available)CFRelease(available);
        return original;
    }
    CFMutableDictionaryRef result=original?CFDictionaryCreateMutableCopy(NULL,0,original):
        CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    if(result) {
        CFDictionarySetValue(result,kMGQDiskUsageTotalDataCapacity,capacity);
        CFDictionarySetValue(result,kMGQDiskUsageTotalDataAvailable,available);
        CFTypeRef disk=storage_answer(CFSTR("TotalDiskCapacity"));
        if(disk){CFDictionarySetValue(result,kMGQDiskUsageTotalDiskCapacity,disk);CFRelease(disk);}
        if(original)CFRelease(original);
    }
    CFRelease(capacity);CFRelease(available);
    return result?result:original;
}
static CFTypeRef board_answer(CFStringRef key) {
    if(!key)return NULL;
    key=canonical_key(key);
    if(CFEqual(key,CFSTR("DiskUsage")))return disk_usage_answer(key);
    CFTypeRef value=pong_usb_copy_board_identity(key);
    if(!value)value=power_answer(key);
    if(!value)value=playback_answer(key);
    if(!value)value=storage_answer(key);
    return value;
}
static CFTypeRef pong_answer(CFStringRef key) {
    CFTypeRef value=board_answer(key);
    return value?value:MGCopyAnswer(key);
}
static bool pong_bool(CFStringRef key) {
    CFTypeRef value=board_answer(key);if(!value)return MGGetBoolAnswer(key);
    bool result=false;
    if(CFGetTypeID(value)==CFBooleanGetTypeID())result=CFBooleanGetValue(value);
    else if(CFGetTypeID(value)==CFNumberGetTypeID()){int64_t n=0;CFNumberGetValue(value,kCFNumberSInt64Type,&n);result=n!=0;}
    CFRelease(value);return result;
}
static int64_t pong_int64(CFStringRef key,int64_t fallback) {
    CFTypeRef value=board_answer(key);if(!value)return MGGetSInt64Answer(key,fallback);
    int64_t result=fallback;
    if(CFGetTypeID(value)==CFNumberGetTypeID())CFNumberGetValue(value,kCFNumberSInt64Type,&result);
    else if(CFGetTypeID(value)==CFBooleanGetTypeID())result=CFBooleanGetValue(value);
    CFRelease(value);return result;
}
static int32_t pong_int32(CFStringRef key,int32_t fallback) {
    CFTypeRef value=board_answer(key);if(!value)return MGGetSInt32Answer(key,fallback);
    int64_t result=fallback;
    if(CFGetTypeID(value)==CFNumberGetTypeID())CFNumberGetValue(value,kCFNumberSInt64Type,&result);
    else if(CFGetTypeID(value)==CFBooleanGetTypeID())result=CFBooleanGetValue(value);
    CFRelease(value);return result<INT32_MIN || result>INT32_MAX?fallback:(int32_t)result;
}
static CFDictionaryRef pong_multiple(CFArrayRef keys,CFDictionaryRef options) {
    CFDictionaryRef original=MGCopyMultipleAnswers(keys,options);
    if(!keys || CFGetTypeID(keys)!=CFArrayGetTypeID())return original;
    CFMutableDictionaryRef result=original?CFDictionaryCreateMutableCopy(NULL,0,original):
        CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    if(!result)return original;
    if(original)CFRelease(original);
    for(CFIndex i=0;i<CFArrayGetCount(keys);i++) {
        CFTypeRef key=CFArrayGetValueAtIndex(keys,i);
        if(!key || CFGetTypeID(key)!=CFStringGetTypeID())continue;
        CFTypeRef value=board_answer(key);
        if(value){CFDictionarySetValue(result,key,value);CFRelease(value);}
    }
    return result;
}
static CFMutableDictionaryRef pong_matching(const char *name) {
    const char *process=getprogname();
    if(process && !strcmp(process,"lockdownd") && name &&
       (!strcmp(name,"AppleUSBDeviceMux") || !strcmp(name,"IOUSBDeviceInterface"))) {
        // Native PTP property filters still exclude our actual Mux function.
        // IsActive and its interest events come from the DWC3 configuration.
        return IOServiceMatching("PongUsbMuxTransport");
    }
    return IOServiceMatching(name);
}
__attribute__((used,section("__DATA,__interpose")))
static const struct {const void *replacement,*original;} interpose[]={
    {(const void *)pong_answer,(const void *)MGCopyAnswer},
    {(const void *)pong_bool,(const void *)MGGetBoolAnswer},
    {(const void *)pong_int32,(const void *)MGGetSInt32Answer},
    {(const void *)pong_int64,(const void *)MGGetSInt64Answer},
    {(const void *)pong_multiple,(const void *)MGCopyMultipleAnswers},
    {(const void *)pong_matching,(const void *)IOServiceMatching},
};
