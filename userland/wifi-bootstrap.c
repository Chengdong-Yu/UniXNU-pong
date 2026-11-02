/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Initialize the existing MobileWiFi manager after a real driver device is
 * discoverable. All power requests go to stock wifid, not a Pong UI/API.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <net/if.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <spawn.h>
#include <string.h>
extern char **environ;
#include <unistd.h>

static void timeout(int signal) {
    (void)signal;
    const char text[]="Pong Wi-Fi bootstrap: native manager deadline; boot continues\n";
    (void)write(STDERR_FILENO,text,sizeof(text)-1);_exit(1);
}
/* configd's interface namer and wifid consume the same native interface
 * object. En0 is already attached to DLIL by this board driver; give that
 * actual object a native configuration before asking wifid for devices.
 * Never manufacture a device dictionary or an enabled Wi-Fi UI state. */
static int configure_interface(void) {
    if(!if_nametoindex("en0")){fputs("Pong Wi-Fi bootstrap: DLIL en0 not attached yet\n",stderr);return 1;}
    io_service_t entry=IOServiceGetMatchingService(kIOMainPortDefault,
        IOBSDNameMatching(kIOMainPortDefault,0,"en0"));
    if(!entry) {
        /* This interface is self-named in DLIL. IOBSD matching need not
         * include it before InterfaceNamer's first pass; use its actual
         * IOKit class and validate the physical BSD name instead. */
        entry=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("IO80211Interface"));
        if(entry) {
            CFTypeRef name=IORegistryEntryCreateCFProperty(entry,CFSTR("BSD Name"),NULL,0);
            Boolean valid=name && CFGetTypeID(name)==CFStringGetTypeID() && CFEqual(name,CFSTR("en0"));
            if(name)CFRelease(name);
            if(!valid){IOObjectRelease(entry);entry=0;}
        }
    }
    if(!entry){fputs("Pong Wi-Fi bootstrap: actual en0 registry service unavailable\n",stderr);return 1;}
    if(!IOObjectConformsTo(entry,"IO80211Interface")){IOObjectRelease(entry);return 1;}
    void *sc=dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration",RTLD_NOW|RTLD_LOCAL);
    if(!sc){IOObjectRelease(entry);return 1;}
    CFTypeRef (*interfaceCreate)(io_registry_entry_t)=dlsym(sc,"_SCNetworkInterfaceCreateWithIONetworkInterfaceObject");
    CFStringRef (*interfaceType)(CFTypeRef)=dlsym(sc,"SCNetworkInterfaceGetInterfaceType");
    CFStringRef (*interfaceName)(CFTypeRef)=dlsym(sc,"SCNetworkInterfaceGetBSDName");
    CFTypeRef (*prefsCreate)(CFAllocatorRef,CFStringRef,CFStringRef)=dlsym(sc,"SCPreferencesCreate");
    Boolean (*prefsLock)(CFTypeRef,Boolean)=dlsym(sc,"SCPreferencesLock");
    Boolean (*prefsUnlock)(CFTypeRef)=dlsym(sc,"SCPreferencesUnlock");
    Boolean (*commit)(CFTypeRef)=dlsym(sc,"SCPreferencesCommitChanges");
    Boolean (*apply)(CFTypeRef)=dlsym(sc,"SCPreferencesApplyChanges");
    CFArrayRef (*servicesCopy)(CFTypeRef)=dlsym(sc,"SCNetworkServiceCopyAll");
    CFTypeRef (*serviceInterface)(CFTypeRef)=dlsym(sc,"SCNetworkServiceGetInterface");
    CFTypeRef (*serviceCreate)(CFTypeRef,CFTypeRef)=dlsym(sc,"SCNetworkServiceCreate");
    Boolean (*defaults)(CFTypeRef)=dlsym(sc,"SCNetworkServiceEstablishDefaultConfiguration");
    Boolean (*enable)(CFTypeRef,Boolean)=dlsym(sc,"SCNetworkServiceSetEnabled");
    CFTypeRef (*setCopy)(CFTypeRef)=dlsym(sc,"SCNetworkSetCopyCurrent");
    CFTypeRef (*setCreate)(CFTypeRef)=dlsym(sc,"SCNetworkSetCreate");
    Boolean (*setCurrent)(CFTypeRef)=dlsym(sc,"SCNetworkSetSetCurrent");
    Boolean (*setAdd)(CFTypeRef,CFTypeRef)=dlsym(sc,"SCNetworkSetAddService");
    if(!interfaceCreate||!interfaceType||!interfaceName||!prefsCreate||!prefsLock||!prefsUnlock||
       !commit||!apply||!servicesCopy||!serviceInterface||!serviceCreate||!defaults||!enable||
       !setCopy||!setCreate||!setCurrent||!setAdd){IOObjectRelease(entry);return 1;}
    CFTypeRef interface=interfaceCreate(entry);IOObjectRelease(entry);
    if(!interface){fputs("Pong Wi-Fi bootstrap: SC could not construct the actual en0 interface\n",stderr);return 1;}
    CFStringRef name=interfaceName(interface),type=interfaceType(interface);
    if(!name||!type||!CFEqual(name,CFSTR("en0"))||!CFEqual(type,CFSTR("IEEE80211"))){
        fputs("Pong Wi-Fi bootstrap: native SC rejected physical wireless interface\n",stderr);CFRelease(interface);return 1;
    }
    CFTypeRef prefs=prefsCreate(NULL,CFSTR("Pong physical WLAN configuration"),NULL);
    int result=1;
    if(prefs && prefsLock(prefs,false)) {
        CFArrayRef services=servicesCopy(prefs);Boolean found=false;
        if(services)for(CFIndex i=0;i<CFArrayGetCount(services);i++) {
            CFTypeRef candidate=serviceInterface(CFArrayGetValueAtIndex(services,i));
            CFStringRef bsd=candidate?interfaceName(candidate):NULL;
            if(bsd && CFEqual(bsd,name)){found=true;break;}
        }
        if(services)CFRelease(services);
        if(found)result=0;
        else {
            CFTypeRef set=setCopy(prefs);Boolean current=set!=NULL;
            if(!set){set=setCreate(prefs);current=set && setCurrent(set);}
            CFTypeRef service=current?serviceCreate(prefs,interface):NULL;
            if(service && defaults(service) && enable(service,true) && setAdd(set,service) && commit(prefs) && apply(prefs))result=0;
            if(service)CFRelease(service);
            if(set)CFRelease(set);
        }
        (void)prefsUnlock(prefs);
    }
    if(prefs)CFRelease(prefs);
    fprintf(stderr,"Pong Wi-Fi bootstrap: real en0 native configuration result=%d\n",result);
    CFRelease(interface);return result;
}
/* Publish the actual DLIL interface before asking configd to apply a
 * service. SCPreferencesApplyChanges may wait for InterfaceNamer, while
 * wifid itself waits for this interface-arrival notification. */
static int publish_interface(void) {
    if(!if_nametoindex("en0"))return 1;
    io_service_t entry=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("IO80211Interface"));
    if(!entry)return 1;
    CFTypeRef name=IORegistryEntryCreateCFProperty(entry,CFSTR("BSD Name"),NULL,0);IOObjectRelease(entry);
    if(!name || CFGetTypeID(name)!=CFStringGetTypeID() || !CFEqual(name,CFSTR("en0"))){if(name)CFRelease(name);return 1;}
    void *sc=dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration",RTLD_NOW|RTLD_LOCAL);
    CFTypeRef (*create)(CFAllocatorRef,CFStringRef,void *,void *)=sc?dlsym(sc,"SCDynamicStoreCreate"):NULL;
    CFTypeRef (*copy)(CFTypeRef,CFStringRef)=sc?dlsym(sc,"SCDynamicStoreCopyValue"):NULL;
    Boolean (*set)(CFTypeRef,CFStringRef,CFPropertyListRef)=sc?dlsym(sc,"SCDynamicStoreSetValue"):NULL;
    if(!create||!copy||!set){CFRelease(name);return 1;}
    CFTypeRef store=create(NULL,CFSTR("Pong real WLAN arrival"),NULL,NULL);
    if(!store){CFRelease(name);return 1;}
    CFTypeRef old=copy(store,CFSTR("State:/Network/Interface"));
    CFMutableDictionaryRef state=old && CFGetTypeID(old)==CFDictionaryGetTypeID()?
        CFDictionaryCreateMutableCopy(NULL,0,old):CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CFTypeRef list=CFDictionaryGetValue(state,CFSTR("Interfaces"));
    CFMutableArrayRef names=list && CFGetTypeID(list)==CFArrayGetTypeID()?
        CFArrayCreateMutableCopy(NULL,0,list):CFArrayCreateMutable(NULL,0,&kCFTypeArrayCallBacks);
    if(!CFArrayContainsValue(names,CFRangeMake(0,CFArrayGetCount(names)),name))CFArrayAppendValue(names,name);
    CFDictionarySetValue(state,CFSTR("Interfaces"),names);
    int result=set(store,CFSTR("State:/Network/Interface"),state)?0:1;
    CFRelease(names);CFRelease(state);if(old)CFRelease(old);CFRelease(store);CFRelease(name);
    fprintf(stderr,"Pong Wi-Fi bootstrap: real interface arrival publication=%d\n",result);return result;
}

/* 22H374 SCNetworkInterfaceCopyAll waits for Plugin:InterfaceNamer to
 * contain one of its native terminal markers. Pong's physical interface
 * is already named/attached by DLIL, but the Apple naming plugin never
 * completes this handshake on the custom IONetworkStack. That held wifid's
 * dispatch_once owner, its native manager queue, then SpringBoard's sync
 * wifi_manager_open. Report the actual bounded IOKit wait outcome, never
 * claim QUIET when services are still busy. TIMEOUT is the native fallback
 * marker SC explicitly handles; it still enumerates the real interfaces.
 */
static int publish_namer_result(void) {
    if(!if_nametoindex("en0"))return 1;
    mach_timespec_t deadline={5,0};
    kern_return_t waited=IOKitWaitQuiet(kIOMainPortDefault,&deadline);
    if(waited!=KERN_SUCCESS && waited!=kIOReturnTimeout) {
        fprintf(stderr,"Pong Wi-Fi bootstrap: IOKit enumeration wait failed=%d\n",waited);return 1;
    }
    void *sc=dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration",RTLD_NOW|RTLD_LOCAL);
    CFTypeRef (*create)(CFAllocatorRef,CFStringRef,void *,void *)=sc?dlsym(sc,"SCDynamicStoreCreate"):NULL;
    CFTypeRef (*copy)(CFTypeRef,CFStringRef)=sc?dlsym(sc,"SCDynamicStoreCopyValue"):NULL;
    Boolean (*set)(CFTypeRef,CFStringRef,CFPropertyListRef)=sc?dlsym(sc,"SCDynamicStoreSetValue"):NULL;
    if(!create||!copy||!set)return 1;
    CFTypeRef store=create(NULL,CFSTR("Pong self-named network completion"),NULL,NULL);
    if(!store)return 1;
    CFStringRef key=CFSTR("Plugin:InterfaceNamer");CFTypeRef old=copy(store,key);
    CFMutableDictionaryRef record=old && CFGetTypeID(old)==CFDictionaryGetTypeID()?
        CFDictionaryCreateMutableCopy(NULL,0,old):
        CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    int result=1;
    if(record) {
        if(CFDictionaryContainsKey(record,CFSTR("*QUIET*")) ||
           CFDictionaryContainsKey(record,CFSTR("*COMPLETE*")) ||
           CFDictionaryContainsKey(record,CFSTR("*TIMEOUT*")))result=0;
        else {
            CFDictionarySetValue(record,waited==KERN_SUCCESS?CFSTR("*QUIET*"):CFSTR("*TIMEOUT*"),kCFBooleanTrue);
            result=set(store,key,record)?0:1;
        }
        CFRelease(record);
    }
    if(old)CFRelease(old);CFRelease(store);
    fprintf(stderr,"Pong Wi-Fi bootstrap: real self-named en0 completion wait=%d publish=%d\n",waited,result);
    return result;
}
int main(int argc,char **argv) {
    signal(SIGALRM,timeout);setvbuf(stderr,NULL,_IONBF,0);
    if(argc==2 && !strcmp(argv[1],"--configure-only")) {
        alarm(20);fputs("Pong Wi-Fi bootstrap: optional native SC service configuration begins\n",stderr);
        (void)publish_interface();
        (void)publish_namer_result();
        return configure_interface();
    }

    const char *marker="/private/var/.pong-wifi-native-bootstrap97-requested";
    alarm(60);
    /* The real physical registration is the prerequisite. Do not make a
     * native power request depend on SCPreferences or CopyDevices, both
     * of which previously waited for this same initialization. */
    unsigned ready=0;
    for(unsigned i=0;i<40;i++) {
        if(if_nametoindex("en0")) {
            io_service_t device=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("IO80211Interface"));
            if(device){IOObjectRelease(device);ready=1;break;}
        }
        usleep(250000);
    }
    if(!ready){fputs("Pong Wi-Fi bootstrap: real WLAN not registered; no power request\n",stderr);return 1;}

    // InterfaceNamer/configd and wifid can wait for one another. Dispatch
    // the real interface arrival/configuration BEFORE MobileWiFi's sync
    // manager reconnect; doing this after SetPower left it unreachable on
    // services103 (manager deadline, despite en0 and completed scans).
    // The separate worker has its own deadline, so it cannot stall boot.
    pid_t configuration=0;
    char *child[]={"/pong/wifi-bootstrap","--configure-only",NULL};
    int spawned=posix_spawn(&configuration,child[0],NULL,NULL,child,environ);
    fprintf(stderr,"Pong Wi-Fi bootstrap: native SC worker spawn=%d before manager reconnect\n",spawned);

    void *lib=dlopen("/System/Library/PrivateFrameworks/MobileWiFi.framework/MobileWiFi",RTLD_NOW|RTLD_LOCAL);
    if(!lib){fputs("Pong Wi-Fi bootstrap: MobileWiFi unavailable\n",stderr);return 1;}
    CFTypeRef (*create)(CFAllocatorRef,unsigned)=dlsym(lib,"WiFiManagerClientCreate");
    void (*schedule)(CFTypeRef,CFRunLoopRef,CFStringRef)=dlsym(lib,"WiFiManagerClientScheduleWithRunLoop");
    CFArrayRef (*devices)(CFTypeRef)=dlsym(lib,"WiFiManagerClientCopyDevices");
    CFTypeRef (*property)(CFTypeRef,CFStringRef)=dlsym(lib,"WiFiManagerClientCopyProperty");
    void (*power)(CFTypeRef,Boolean)=dlsym(lib,"WiFiManagerClientSetPower");
    if(!create||!schedule||!devices||!property||!power)return 1;
    CFTypeRef manager=create(kCFAllocatorDefault,0);
    if(!manager){fputs("Pong Wi-Fi bootstrap: native manager creation failed\n",stderr);return 1;}
    schedule(manager,CFRunLoopGetCurrent(),kCFRunLoopDefaultMode);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.25,false);
    CFTypeRef airplane=property(manager,CFSTR("AirplaneMode"));
    Boolean blocked=airplane && CFGetTypeID(airplane)==CFBooleanGetTypeID() && CFBooleanGetValue(airplane);
    if(airplane)CFRelease(airplane);
    if(blocked){fputs("Pong Wi-Fi bootstrap: airplane mode retained\n",stderr);return 0;}
    power(manager,true);
    fputs("Pong Wi-Fi bootstrap: native system power-on requested for registered Pong WLAN\n",stderr);
    for(unsigned attempt=0;attempt<40;attempt++) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.5,false);
        CFArrayRef list=devices(manager);
        CFIndex count=list && CFGetTypeID(list)==CFArrayGetTypeID()?CFArrayGetCount(list):0;
        if(list)CFRelease(list);
        if(!count)continue;
        fprintf(stderr,"Pong Wi-Fi bootstrap: native manager discovered %ld device(s)\n",(long)count);
        CFTypeRef allowed=property(manager,CFSTR("AllowEnable"));
        int observed=-1;
        if(allowed && CFGetTypeID(allowed)==CFBooleanGetTypeID())observed=CFBooleanGetValue(allowed);
        if(allowed)CFRelease(allowed);
        fprintf(stderr,"Pong Wi-Fi bootstrap: native AllowEnable=%d (not an association result)\n",observed);
        int fd=open(marker,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
        if(fd>=0){const char sent[]="native request dispatched\n";(void)write(fd,sent,sizeof(sent)-1);close(fd);}
        alarm(0);return 0;
    }
    fputs("Pong Wi-Fi bootstrap: native power request dispatched, but no system device discovery yet\n",stderr);return 1;
}
