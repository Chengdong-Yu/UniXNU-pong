/* Pong 20G81 software UI adaptations, derived from HackiOS inject.c/inject_sb.c
 * (MIT). Hardware drivers and identity are supplied independently by UniXNU.
 * No 20F66 vtables, object-field offsets, battery or GPU driver are used.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CGGeometry.h>
#include <dlfcn.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach-o/dyld.h>
#include <libkern/OSCacheControl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include "touch.h"
#include "icons_cpu.h"

extern const void *_dyld_get_shared_cache_range(size_t *);
static pthread_mutex_t hooks_lock = PTHREAD_MUTEX_INITIALIZER;
static bool is_springboard, is_media, is_settings;
static IMP orb_original, persona_original, snapshot_original, activation_original, setup_original;
static ptrdiff_t activation_offset = -1;
static IMP portrait_original;
static IMP poster_original, wallpaper_original;
static IMP battery_original;
static IMP statusbar_factory_original;
static Class statusbar_pong_provider;
static IMP watch_unlock_original;
static IMP settings_bundle_original, settings_cellular_original, settings_carrier_original;
static IMP settings_hotspot_original;
static IMP about_bootstrap_original, about_sim_original, about_wifi_original, about_bt_original;
static IMP about_number_original, about_coverage_original, about_version_original;
static IMP system_name_original;
static IMP localized_name_original;
static bool about_radios_excluded;
static void (*settings_previous_exception)(id);
static bool settings_exception_installed;
static int settings_diagnostic_mib[CTL_MAXNAME];
static size_t settings_diagnostic_mib_count;
static uintptr_t settings_cache_base;

static void settings_record(const char *text)
{
    if (settings_diagnostic_mib_count && text)
        (void)sysctl(settings_diagnostic_mib,(u_int)settings_diagnostic_mib_count,
                     NULL,NULL,(void *)text,strnlen(text,1023));
}

static void settings_fault(int number,siginfo_t *info,void *saved)
{
    char line[384];
    ucontext_t *context=saved;
    uint64_t pc=0,lr=0,fp=0;
    if (context && context->uc_mcontext) {
        pc=context->uc_mcontext->__ss.__pc;
        lr=context->uc_mcontext->__ss.__lr;
        fp=context->uc_mcontext->__ss.__fp;
    }
    int n=snprintf(line,sizeof(line),
        "fatal signal=%d address=%p pc=0x%llx lr=0x%llx fp=0x%llx cache_base=0x%lx",
        number,info?info->si_addr:NULL,(unsigned long long)pc,
        (unsigned long long)lr,(unsigned long long)fp,(unsigned long)settings_cache_base);
    if (n>0) settings_record(line);
    /* Read bounded frames through the kernel instead of dereferencing a
     * possibly corrupt user stack in the fault handler (HackiOS strategy,
     * with failed reads terminating the walk). */
    uint64_t cursor=fp;
    for (unsigned i=0;i<10 && cursor>=0x100000000ULL && !(cursor&7);i++) {
        uint64_t frame[2]={0};vm_size_t copied=0;
        if (vm_read_overwrite(mach_task_self(),(vm_address_t)cursor,sizeof(frame),
                (vm_address_t)frame,&copied)!=KERN_SUCCESS || copied!=sizeof(frame)) break;
        snprintf(line,sizeof(line),"fatal frame[%u] return=0x%llx",i,(unsigned long long)frame[1]);
        settings_record(line);
        if (frame[0]<=cursor || frame[0]-cursor>0x100000) break;
        cursor=frame[0];
    }
    /* Same fault-record-and-reraise strategy as HackiOS; never resume at a
     * faulting instruction or convert a fatal error into apparent success. */
    signal(number,SIG_DFL);
    raise(number);
}

static void settings_record_init(void)
{
    size_t bytes=0;
    settings_cache_base=(uintptr_t)_dyld_get_shared_cache_range(&bytes);
    settings_diagnostic_mib_count=CTL_MAXNAME;
    if (sysctlnametomib("kern.pong_diagnostic",settings_diagnostic_mib,
                       &settings_diagnostic_mib_count)!=0) settings_diagnostic_mib_count=0;
    settings_record("Preferences adapter entered 20G81/v94");
    struct sigaction action;
    memset(&action,0,sizeof(action));
    action.sa_sigaction=settings_fault;
    action.sa_flags=SA_SIGINFO|SA_RESETHAND;
    const int numbers[]={SIGSEGV,SIGBUS,SIGABRT,SIGILL,SIGTRAP};
    for (unsigned i=0;i<sizeof(numbers)/sizeof(numbers[0]);i++)
        (void)sigaction(numbers[i],&action,NULL);
}
static CFMutableDictionaryRef (*service_matching)(const char *);
static uint32_t (*matching_service)(uint32_t,CFDictionaryRef);
static int (*release_service)(uint32_t);
static void say(const char *fmt, ...);

static void say(const char *fmt, ...)
{
    char b[768]; va_list ap; va_start(ap, fmt);
    int n=vsnprintf(b,sizeof(b),fmt,ap); va_end(ap);
    if (n>0) {
        (void)write(2,b,(size_t)n<sizeof(b)?(size_t)n:sizeof(b)-1);
        if (is_settings) settings_record(b);
    }
}

/* Check runtime metadata instead of assuming 16.5 implementation addresses. */
static IMP hook(const char *clsname, const char *selname, bool meta,
                const char *returns, const char *arg, IMP replacement)
{
    Class cls=objc_getClass(clsname);
    Method m=cls?(meta?class_getClassMethod(cls,sel_registerName(selname)):
                         class_getInstanceMethod(cls,sel_registerName(selname))):NULL;
    static Method rejected[16];
    static unsigned rejected_count;
    if (!m) return NULL; /* retry while a framework has not loaded */
    for (unsigned i=0;i<rejected_count;i++) if (rejected[i]==m) return NULL;
    char *r=method_copyReturnType(m), *a=arg?method_copyArgumentType(m,2):NULL;
    bool valid=method_getNumberOfArguments(m)==(arg?3u:2u) &&
               r && r[0] && strchr(returns,r[0]) && (!arg || (a && a[0] && strchr(arg,a[0])));
    if (!valid) {
        say("Pong UI: incompatible ABI %s %s (%s); SKIP\n",clsname,selname,method_getTypeEncoding(m));
        if (rejected_count<sizeof(rejected)/sizeof(rejected[0])) rejected[rejected_count++]=m;
    }
    free(r); free(a);
    if (!valid) return NULL;
    IMP old=method_getImplementation(m);
    if (!old || old==replacement) return NULL;
    method_setImplementation(m,replacement);
    say("Pong UI: installed %s %s\n",clsname,selname);
    return old;
}

static BOOL no_personas(id self, SEL sel) { (void)self;(void)sel;return NO; }

/* Actual20G81's _UIStatusBar fallback displays rdar://45025538 when its
 * Apple screen table cannot match Pong's width/scale/corner-radius tuple.
 * Select the stock1080 layout for this actual main panel before provider
 * construction; keep real display geometry, global identity and other screens.
 * NSObject runtime metadata and the public UIScreen geometry ABI are used,
 * never an offset into UIKit's private instance fields.
 */
static double statusbar_reference_scale(id self,SEL sel)
{
    (void)self;(void)sel;
    id screen=((id(*)(id,SEL))objc_msgSend)((id)objc_getClass("UIScreen"),sel_registerName("mainScreen"));
    return ((double(*)(id,SEL))objc_msgSend)(screen,sel_registerName("nativeScale"));
}
static Class statusbar_factory(id self,SEL sel,id screen,id info)
{
    Class selected=((Class(*)(id,SEL,id,id))statusbar_factory_original)(self,sel,screen,info);
    Class fallback=objc_getClass("_UIStatusBarVisualProvider_Fallback");
    if(selected && selected!=fallback)return selected;
    id main=((id(*)(id,SEL))objc_msgSend)((id)objc_getClass("UIScreen"),sel_registerName("mainScreen"));
    if(screen && screen==main && statusbar_pong_provider) {
        CGRect bounds=((CGRect(*)(id,SEL))objc_msgSend)(screen,sel_registerName("nativeBounds"));
        double scale=((double(*)(id,SEL))objc_msgSend)(screen,sel_registerName("nativeScale"));
        double small=bounds.size.width<bounds.size.height?bounds.size.width:bounds.size.height;
        double large=bounds.size.width>bounds.size.height?bounds.size.width:bounds.size.height;
        if(small==1080.0 && large==2412.0 && scale>0.0 && scale<=4.0) {
            static unsigned reported;
            if(__atomic_fetch_add(&reported,1,__ATOMIC_RELAXED)<3)
                say("Pong UI: actual1080x2412 status bar provider selected; native scale %.3f\n",scale);
            return statusbar_pong_provider;
        }
    }
    return selected;
}
static void install_statusbar_hook(void)
{
    if(statusbar_factory_original)return;
    Class factory=objc_getClass("_UIStatusBarVisualProvider_iOS"),base=objc_getClass("_UIStatusBarVisualProvider_Split1080");
    if(!factory || !base)return;
    Method method=class_getClassMethod(factory,sel_registerName("visualProviderSubclassForScreen:visualProviderInfo:"));
    if(!method || strcmp(method_getTypeEncoding(method),"#32@0:8@16@24"))return;
    Method reference=class_getClassMethod(base,sel_registerName("referenceScreenScale"));
    if(!reference || strcmp(method_getTypeEncoding(reference),"d16@0:8"))return;
    if(!statusbar_pong_provider) {
        statusbar_pong_provider=objc_allocateClassPair(base,"PongStatusBarVisualProvider1080",0);
        if(!statusbar_pong_provider)return;
        if(!class_addMethod(object_getClass((id)statusbar_pong_provider),sel_registerName("referenceScreenScale"),
                            (IMP)statusbar_reference_scale,"d16@0:8")) {
            objc_disposeClassPair(statusbar_pong_provider);statusbar_pong_provider=NULL;return;
        }
        objc_registerClassPair(statusbar_pong_provider);
    }
    /* Return through the real factory, so UIKit still computes its scale /
     * reference-scale / iterative-overflow flags normally. Pre-setting the
     * status bar's private class property would bypass that initialization. */
    statusbar_factory_original=method_getImplementation(method);
    method_setImplementation(method,(IMP)statusbar_factory);
    say("Pong UI:20G81 status bar factory compatibility installed\n");
}

/* The supplied system is an iPad image running with the requested iPhone
 * identity. Use its actual 20G81 resources, with a private resource-only
 * bundle that omits panes needing an unported baseband or absent phone app.
 * This affects Settings only; the original framework and system identity stay
 * available to every other process. No cached 16.5 method addresses are used.
 */
static id settings_bundle(id self, SEL sel)
{
    static dispatch_once_t once;
    static id resources;
    dispatch_once(&once, ^{
        resources=((id(*)(id,SEL,id))objc_msgSend)((id)objc_getClass("NSBundle"),
            sel_registerName("bundleWithPath:"),(id)CFSTR("/private/var/pong/PreferencesUI.framework"));
        if (resources) resources=((id(*)(id,SEL))objc_msgSend)(resources,sel_registerName("retain"));
        say("Pong Settings: compatible resource bundle %s\n",resources?"selected":"unavailable; using stock");
    });
    return resources?resources:((id(*)(id,SEL))settings_bundle_original)(self,sel);
}

/* No CommCenter/baseband implementation is shipped by this Pong port.
 * Leave cellular state uninitialised/unavailable instead of repeatedly
 * querying a service which the saved Preferences log cannot look up.
 */
static void settings_no_cellular(id self, SEL sel)
{
    (void)self;(void)sel;
    static unsigned reported;
    if (__atomic_fetch_add(&reported,1,__ATOMIC_RELAXED)==0)
        say("Pong Settings: baseband panes/refresh skipped (no CommCenter transport)\n");
}

/* Actual20G81 GeneralSettingsUI: loadSpecifiers begins with the synchronous
 * _getBootStrapIccid query, then asks PSSimStatusCache for subscription
 * contexts/equipment; later it queries Wi-Fi/BT addresses and cloud coverage.
 * Pong has no baseband/WLAN/BT transport. Keep those values unavailable in
 * Preferences rather than contacting absent providers on the page's thread.
 * No fake SIM, MAC, serial, capacity or successful hardware service is made.
 * v93's background-exit watchdog is not proof of one particular wait; this
 * is a scoped compatibility repair, still requiring a real page-open result.
 */
static id about_unavailable(id self,SEL sel)
{
    (void)self;
    say("Pong About: %s unavailable (transport not ported)\n",sel_getName(sel));
    return nil;
}
static void about_no_optional_query(id self,SEL sel)
{
    (void)self;
    say("Pong About: skip optional %s\n",sel_getName(sel));
}
static BOOL about_no_sim(id self,SEL sel,id specifier)
{(void)self;(void)sel;(void)specifier;return NO;}
static id about_local_version(id self,SEL sel)
{
    (void)sel;
    id value=((id(*)(id,SEL))objc_msgSend)(self,sel_registerName("_productVersionStringFallback"));
    if(value && ((BOOL(*)(id,SEL,id))objc_msgSend)(value,sel_registerName("isKindOfClass:"),
            (id)objc_getClass("NSString")))
        return ((id(*)(id,SEL,id,id))objc_msgSend)(value,
            sel_registerName("stringByReplacingOccurrencesOfString:withString:"),
            (id)CFSTR("iPadOS"),(id)CFSTR("iOS"));
    return value;
}
/* OS display name only: keep the real16.6.1 version/build and Pong's existing
 * iPhone DT identity, size/scale and device-class policy unchanged. */
static id ios_system_name(id self,SEL sel)
{(void)self;(void)sel;return (id)CFSTR("iOS");}

/*20G81 General.loctable contains separate IPADOS_VERSION/IOS_VERSION
 * labels. A numeric softwareVersionString alone cannot rename the row.
 * Translate only the OS branding in strings returned to Preferences. */
static id ios_localized_name(id self,SEL sel,id key,id value,id table)
{
    id text=((id(*)(id,SEL,id,id,id))localized_name_original)(self,sel,key,value,table);
    if(text && CFStringFind((CFStringRef)text,CFSTR("iPadOS"),0).location!=kCFNotFound)
        return ((id(*)(id,SEL,id,id))objc_msgSend)(text,
            sel_registerName("stringByReplacingOccurrencesOfString:withString:"),
            (id)CFSTR("iPadOS"),(id)CFSTR("iOS"));
    return text;
}
static void install_os_label_hook(void)
{
    if(localized_name_original)return;
    Method method=class_getInstanceMethod(objc_getClass("NSBundle"),
        sel_registerName("localizedStringForKey:value:table:"));
    if(!method || strcmp(method_getTypeEncoding(method),"@40@0:8@16@24@32"))return;
    localized_name_original=method_getImplementation(method);
    method_setImplementation(method,(IMP)ios_localized_name);
    say("Pong Settings: localized iOS display name installed\n");
}
static void install_about_radio_policy(void)
{
    if(about_radios_excluded || settings_cache_base<0x180000000ULL)return;
    Class cls=objc_getClass("PSGAboutDataSource");
    Method method=cls?class_getInstanceMethod(cls,sel_registerName("loadSpecifiers")):NULL;
    uintptr_t slide=settings_cache_base-0x180000000ULL;
    if(!method || (uintptr_t)method_getImplementation(method)!=0x204b12b28ULL+slide)return;
    /* Actual20G81 loadSpecifiers reads MG keys cellular-data, bluetooth and
     * wi-fi. Exclude these hardware sections locally; identity and MG answers
     * in every other function remain intact. Returning empty subscriptions
     * while keeping cellular-data=true would index element0 of an empty array
     * at0x204b12e90, so preserve the stock no-cellular branch instead.
     */
    const struct {uintptr_t address;uint32_t original,replacement;} words[]={
        {0x204b12bacULL,0xaa0003f9,0xaa1f03f9}, /* mov x25,x0 -> xzr */
        {0x204b12bbcULL,0xb900e3e0,0xb900e3ff}, /* local Bluetooth flag */
        {0x204b12bccULL,0xb900dbe0,0xb900dbff}, /* local Wi-Fi flag */
    };
    for(unsigned i=0;i<3;i++)
        if(*(const uint32_t *)(words[i].address+slide)!=words[i].original) {
            say("Pong About: actual20G81 radio branch differs; SKIP\n");return;
        }
    vm_address_t page=(words[0].address+slide)&~((vm_address_t)vm_page_size-1);
    if(words[2].address+slide-page>vm_page_size-4)return;
    kern_return_t result=vm_protect(mach_task_self(),page,vm_page_size,FALSE,
        VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY);
    if(result!=KERN_SUCCESS){say("Pong About: private branch mapping failed %d\n",result);return;}
    for(unsigned i=0;i<3;i++)__atomic_store_n((uint32_t *)(words[i].address+slide),words[i].replacement,__ATOMIC_RELEASE);
    sys_icache_invalidate((void *)page,vm_page_size);
    result=vm_protect(mach_task_self(),page,vm_page_size,FALSE,VM_PROT_READ|VM_PROT_EXECUTE);
    if(result!=KERN_SUCCESS) {
        for(unsigned i=0;i<3;i++)__atomic_store_n((uint32_t *)(words[i].address+slide),words[i].original,__ATOMIC_RELEASE);
        sys_icache_invalidate((void *)page,vm_page_size);
        (void)vm_protect(mach_task_self(),page,vm_page_size,FALSE,VM_PROT_READ|VM_PROT_EXECUTE);
        say("Pong About: execute restore failed, original branches restored\n");return;
    }
    about_radios_excluded=true;
    say("Pong About: actual20G81 local no-radio branches installed; identity retained\n");
}
static void install_about_hooks(void)
{
    install_about_radio_policy();
    if(!about_bootstrap_original)
        about_bootstrap_original=hook("PSGAboutDataSource","_getBootStrapIccid",false,"v",NULL,(IMP)about_no_optional_query);
    if(!about_sim_original)
        about_sim_original=hook("PSGAboutDataSource","shouldShowSIMSpecifier:",false,"Bc","@",(IMP)about_no_sim);
    if(!about_wifi_original)
        about_wifi_original=hook("PSGAboutDataSource","_macAddress",false,"@",NULL,(IMP)about_unavailable);
    if(!about_bt_original)
        about_bt_original=hook("PSGAboutDataSource","_bluetoothMACAddress",false,"@",NULL,(IMP)about_unavailable);
    if(!about_number_original)
        about_number_original=hook("PSGAboutDataSource","_myNumber",false,"@",NULL,(IMP)about_unavailable);
    if(!about_coverage_original)
        about_coverage_original=hook("PSGAboutDataSource","updateNDOSpecifiers",false,"v",NULL,(IMP)about_no_optional_query);
    if(!about_version_original && class_getInstanceMethod(objc_getClass("PSGAboutDataSource"),sel_registerName("_productVersionStringFallback")))
        about_version_original=hook("PSGAboutDataSource","softwareVersionString",false,"@",NULL,(IMP)about_local_version);
}

static void settings_exception(id exception)
{
    SEL utf8=sel_registerName("UTF8String");
    const char *selectors[]={"name","reason","callStackSymbols"};
    int fd=open("/private/var/mobile/Library/Logs/CrashReporter/Pong-Preferences.log",
                O_WRONLY|O_CREAT|O_TRUNC,0600);
    for (unsigned i=0;i<3;i++) {
        id value=((id(*)(id,SEL))objc_msgSend)(exception,sel_registerName(selectors[i]));
        if (i==2) value=((id(*)(id,SEL))objc_msgSend)(value,sel_registerName("description"));
        const char *s=value?((const char *(*)(id,SEL))objc_msgSend)(value,utf8):NULL;
        if (!s) s="(unavailable)";
        settings_record(selectors[i]);
        /* Each sysctl copy is bounded, including a long exception stack. */
        for (size_t at=0,bytes=strnlen(s,16384);at<bytes;at+=900) {
            char part[901];
            size_t count=bytes-at<900?bytes-at:900;
            memcpy(part,s+at,count);part[count]=0;settings_record(part);
        }
        say("Pong Settings exception %s: %.600s\n",selectors[i],s);
        if (fd>=0) {
            (void)write(fd,selectors[i],strlen(selectors[i]));
            (void)write(fd,": ",2);
            (void)write(fd,s,strnlen(s,16384));
            (void)write(fd,"\n",1);
        }
    }
    if (fd>=0) { (void)fsync(fd);close(fd); }
    /* Record the exception, then retain Foundation's ordinary fatal path. */
    if (settings_previous_exception) settings_previous_exception(exception);
}

static void install_settings_hooks(void)
{
    install_os_label_hook();
    install_about_hooks();
    if (!settings_bundle_original && access("/private/var/pong/PreferencesUI.framework/Settings.plist",R_OK)==0)
        settings_bundle_original=hook("PSUIPrefsListController","bundle",false,"@",NULL,(IMP)settings_bundle);
    if (!settings_cellular_original)
        settings_cellular_original=hook("PSUIPrefsListController","fetchCellularDataEnabled",false,"v",NULL,(IMP)settings_no_cellular);
    if (!settings_carrier_original)
        settings_carrier_original=hook("PSUIPrefsListController","_newCarrierNotification",false,"v",NULL,(IMP)settings_no_cellular);
    if (!settings_hotspot_original)
        settings_hotspot_original=hook("PSUIPrefsListController","updatePersonalHotspot",false,"v",NULL,(IMP)settings_no_cellular);
    if (!settings_exception_installed) {
        void (*set_handler)(void(*)(id))=dlsym(RTLD_DEFAULT,"NSSetUncaughtExceptionHandler");
        void (*(*get_handler)(void))(id)=dlsym(RTLD_DEFAULT,"NSGetUncaughtExceptionHandler");
        if (set_handler && get_handler) {
            settings_previous_exception=get_handler();
            set_handler(settings_exception);
            settings_exception_installed=true;
        }
    }
}

/* This port has no paired-watch/Bluetooth transport. 20G81's delayed unlock
 * otherwise queries AutoUnlock through SCDynamicStore on the main thread,
 * exactly where v84's retained stack waits. Keep ordinary local unlocking. */
static BOOL no_watch_unlock(id self, SEL sel)
{
    (void)self;(void)sel;
    static unsigned reported;
    if (__atomic_fetch_add(&reported,1,__ATOMIC_RELAXED)==0)
        say("Pong UI: watch AutoUnlock unavailable on this platform; retain local unlock\n");
    return NO;
}

/* HackiOS's development UI route, with the ivar's current runtime offset/type
 * instead of its 20F66 +8 layout. This affects SpringBoard's setup gate only;
 * it neither fabricates SEP identity nor changes mobileactivationd's answer. */
static void setup_activation(id self, SEL sel)
{
    (void)sel;
    int state = 2;
    memcpy((char *)self + activation_offset, &state, sizeof(state));
    say("Pong UI: development SpringBoard activation gate selected\n");
}

/* 20G81 returns BOOL. Preserve the original transition result/notifications. */
static BOOL set_setup_reason(id self, SEL sel, unsigned long long reason)
{
    if (reason) say("Pong UI: optional Setup Assistant reason %llu skipped for bring-up\n",reason);
    return ((BOOL(*)(id,SEL,unsigned long long))setup_original)(self,sel,0);
}

/* HackiOS's software-only UI policy. The poster editor and switcher's wash
 * need rendering facilities absent from this CPU presenter. The main
 * wallpaper, scene services and ordinary app switcher remain available. */
static BOOL poster_switcher_allowed(id self, SEL sel, id *explanation)
{
    (void)self;(void)sel;
    if (explanation) *explanation=(id)CFSTR("Poster editing unavailable during software display bring-up");
    return NO;
}

static void wallpaper_overlay(id self, SEL sel) { (void)self;(void)sel; }

/* The 20G81 status provider already accepts nil for an absent internal
 * battery. With no actual power-source driver on Pong, do not synchronously
 * ask powerd for one on SpringBoard's main thread. A real published source
 * keeps the original BatteryCenter path and its actual data. */
static id internal_battery(id self, SEL sel)
{
    CFMutableDictionaryRef match=service_matching("IOPMPowerSource");
    if (!match) return ((id(*)(id,SEL))battery_original)(self,sel);
    uint32_t source=matching_service(0,match); /* consumes the matching dictionary */
    if (source) {
        release_service(source);
        return ((id(*)(id,SEL))battery_original)(self,sel);
    }
    static unsigned reported;
    if (__atomic_fetch_add(&reported,1,__ATOMIC_RELAXED)==0)
        say("Pong UI: no physical IOPMPowerSource; leave battery status unavailable without a powerd wait\n");
    return nil;
}

static long snapshot_format(id self, SEL sel, id snapshot)
{
    (void)self;(void)sel;
    SEL opaque=sel_registerName("isImageOpaque");
    if (snapshot && class_getInstanceMethod(object_getClass(snapshot),opaque) &&
        ((BOOL(*)(id,SEL))objc_msgSend)(snapshot,opaque)) return 3; /* JPEG */
    return 0; /* PNG: software-readable, preserves alpha */
}

static void prewarm_orb(id self, SEL sel)
{
    id (*create)(void)=(id(*)(void))dlsym(RTLD_DEFAULT,"MTLCreateSystemDefaultDevice");
    if (!create || create()==nil) {
        /* Siri's optional Metal animation must not abort software UI startup. */
        say("Pong UI: Siri orb Metal prewarm SKIP (no Metal device)\n");
        return;
    }
    ((void(*)(id,SEL))orb_original)(self,sel);
}

static void prewarm_portrait(id self, SEL sel, int version, BOOL styles)
{
    id (*create)(void)=(id(*)(void))dlsym(RTLD_DEFAULT,"MTLCreateSystemDefaultDevice");
    if (!create || create()==nil) {
        say("Pong UI: optional camera-filter GPU prewarm skipped (software-only platform)\n");
        return;
    }
    ((void(*)(id,SEL,int,BOOL))portrait_original)(self,sel,version,styles);
}

static void install_media_hook(void)
{
    Class cls=objc_getClass("BWCoreImageFilterRenderer");
    Method m=cls?class_getClassMethod(cls,sel_registerName("prewarmPortraitFilterVersion:semanticStyleFilters:")):NULL;
    if (!m || method_getNumberOfArguments(m)!=4) return;
    char *r=method_copyReturnType(m),*a=method_copyArgumentType(m,2),*b=method_copyArgumentType(m,3);
    bool valid=r && r[0]=='v' && a && a[0] && strchr("iI",a[0]) &&
               b && b[0] && strchr("cB",b[0]);
    free(r); free(a); free(b);
    if (!valid) return;
    portrait_original=method_getImplementation(m);
    method_setImplementation(m,(IMP)prewarm_portrait);
    say("Pong UI: installed software-only camera filter prewarm policy\n");
}

static void install_objc_hooks(void)
{
    pthread_mutex_lock(&hooks_lock);
    if (!persona_original)
        persona_original=hook("MCMUserIdentityCache","personasAreSupported",true,"Bc",NULL,(IMP)no_personas);
    if (is_settings) install_settings_hooks();
    if (is_springboard || is_settings) {
        install_statusbar_hook();
        if(!system_name_original)
            system_name_original=hook("UIDevice","systemName",false,"@",NULL,(IMP)ios_system_name);
    }
    if (is_media && !portrait_original) install_media_hook();
    if (is_springboard) {
        if (!watch_unlock_original)
            watch_unlock_original=hook("SFAutoUnlockManager","autoUnlockEnabled",true,
                                       "Bc",NULL,(IMP)no_watch_unlock);
        if (!battery_original) {
            service_matching=(__typeof__(service_matching))dlsym(RTLD_DEFAULT,"IOServiceMatching");
            matching_service=(__typeof__(matching_service))dlsym(RTLD_DEFAULT,"IOServiceGetMatchingService");
            release_service=(__typeof__(release_service))dlsym(RTLD_DEFAULT,"IOObjectRelease");
            if (service_matching && matching_service && release_service)
                battery_original=hook("BCBatteryDeviceController","sb_deviceInternalBattery",false,
                                      "@",NULL,(IMP)internal_battery);
        }
        if (!activation_original) {
            Class cls=objc_getClass("SBLockdownManager");
            Ivar ivar=cls?class_getInstanceVariable(cls,"_state"):NULL;
            const char *type=ivar?ivar_getTypeEncoding(ivar):NULL;
            ptrdiff_t off=ivar?ivar_getOffset(ivar):-1;
            if (type && type[0] && strchr("iI",type[0]) && off>=(ptrdiff_t)sizeof(void *) &&
                (size_t)off+sizeof(int)<=class_getInstanceSize(cls)) {
                activation_offset=off;
                activation_original=hook("SBLockdownManager","_setupActivationState",false,"v",NULL,(IMP)setup_activation);
            }
        }
        if (!setup_original)
            setup_original=hook("SBSetupManager","_setSetupRequiredReason:",false,"Bc","Q",(IMP)set_setup_reason);
        if (!poster_original)
            poster_original=hook("SBMainDisplayPolicyAggregator","_allowsCapabilityPosterSwitcherWithExplanation:",
                                 false,"Bc","^",(IMP)poster_switcher_allowed);
        if (!wallpaper_original)
            wallpaper_original=hook("SBAppSwitcherPageView","_createOrDestroyWallpaperOverlayView",
                                    false,"v",NULL,(IMP)wallpaper_overlay);
        if (!orb_original)
            orb_original=hook("SiriPresentationViewController","prewarmOrbViewShaders",false,"v",NULL,(IMP)prewarm_orb);
        if (!snapshot_original)
            snapshot_original=hook("XBApplicationSnapshotManifestImpl","_outputFormatForSnapshot:",true,"qQ", "@",(IMP)snapshot_format);
    }
    pthread_mutex_unlock(&hooks_lock);
}

static void image_added(const struct mach_header *h, intptr_t slide)
{ (void)h;(void)slide;install_objc_hooks(); }

/* HackiOS's delayed unlock is needed while Pong has no input path into the
 * iOS lock screen. Use the actual 20G81 int argument (20F66 used long in the
 * reference's cast), and keep the ordinary authentication decision intact.
 * A bounded idle assertion also keeps a bring-up screen visible without an
 * Apple HID wake source. No private object offsets are used here. */
static bool method_abi(Method method, const char *result, unsigned argc, const char *arguments)
{
    if (!method || method_getNumberOfArguments(method)!=argc+2) return false;
    char *type=method_copyReturnType(method);
    bool good=type && type[0] && strchr(result,type[0]);
    free(type);
    for (unsigned i=0;good && i<argc;i++) {
        type=method_copyArgumentType(method,i+2);
        good=type && type[0] && type[0]==arguments[i];
        free(type);
    }
    return good;
}

static id existing_singleton(const char *name)
{
    Class cls=objc_getClass(name);
    SEL sel=sel_registerName("sharedInstance");
    if (!method_abi(cls?class_getClassMethod(cls,sel):NULL,"@",0,"")) return nil;
    return ((id(*)(id,SEL))objc_msgSend)((id)cls,sel);
}

static int bool_property(id object, const char *name)
{
    SEL sel=sel_registerName(name);
    if (!object || !method_abi(class_getInstanceMethod(object_getClass(object),sel),"Bc",0,"")) return -1;
    return ((BOOL(*)(id,SEL))objc_msgSend)(object,sel)?1:0;
}

static void show_ui_state(const char *phase, id manager, id backlight)
{
    say("Pong UI: %s locked=%d screen=%d dim=%d pending-unblank=%d\n",phase,
        bool_property(manager,"isUILocked"),bool_property(backlight,"screenIsOn"),
        bool_property(backlight,"screenIsDim"),bool_property(backlight,"isPendingScreenUnblankAfterCACommit"));
}

static void present_home(void *unused)
{
    (void)unused;
    id manager=existing_singleton("SBLockScreenManager");
    id backlight=existing_singleton("SBBacklightController");
    show_ui_state("main queue reached",manager,backlight);
    SEL hold=sel_registerName("preventIdleSleepForNumberOfSeconds:");
    if (backlight && method_abi(class_getInstanceMethod(object_getClass(backlight),hold),"v",1,"f"))
        ((void(*)(id,SEL,float))objc_msgSend)(backlight,hold,300.0f);
    SEL wake=sel_registerName("turnOnScreenFullyWithBacklightSource:");
    if (backlight && method_abi(class_getInstanceMethod(object_getClass(backlight),wake),"v",1,"q"))
        ((void(*)(id,SEL,long long))objc_msgSend)(backlight,wake,0);
    SEL unlock=sel_registerName("unlockUIFromSource:withOptions:");
    if (bool_property(manager,"isUILocked")==1 &&
        method_abi(class_getInstanceMethod(object_getClass(manager),unlock),"Bc",2,"i@")) {
        BOOL accepted=((BOOL(*)(id,SEL,int,id))objc_msgSend)(manager,unlock,24,nil);
        say("Pong UI: HackiOS delayed home request accepted=%d\n",accepted);
    }
    show_ui_state("home request returned",manager,backlight);
}

/* Bounded snapshots distinguish an idle event loop from startup still running.
 * Do not suspend it: it might own malloc/Objective-C locks needed by us.
 * Read frame records through Mach with bounds; never dereference another
 * thread's stack as if it were stable. This is bounded, not per-frame trace. */
static mach_port_t ui_main_thread;
static uint64_t ui_main_turns;
static uintptr_t ui_cache_slide;

static void main_turn(CFRunLoopObserverRef observer, CFRunLoopActivity activity, void *info)
{
    (void)observer;(void)activity;(void)info;
    __atomic_fetch_add(&ui_main_turns,1,__ATOMIC_RELAXED);
}

static void main_snapshot(void *context)
{
    arm_thread_state64_t state;
    mach_msg_type_number_t count=ARM_THREAD_STATE64_COUNT;
    kern_return_t result=thread_get_state(ui_main_thread,ARM_THREAD_STATE64,(thread_state_t)&state,&count);
    uint64_t turns=__atomic_load_n(&ui_main_turns,__ATOMIC_RELAXED);
    if (result!=KERN_SUCCESS) {
        say("Pong UI: main snapshot %lu result=%x turns=%llu\n",(unsigned long)context,result,turns);
        return;
    }
    uintptr_t pc=arm_thread_state64_get_pc(state),lr=arm_thread_state64_get_lr(state);
    uintptr_t fp=arm_thread_state64_get_fp(state);
    say("Pong UI: main snapshot %lu turns=%llu pc=%lx lr=%lx (cache unslid)\n",
        (unsigned long)context,turns,pc-ui_cache_slide,lr-ui_cache_slide);
    for (unsigned i=0;i<12 && fp && (fp&15)==0;i++) {
        uint64_t frame[2]; vm_size_t got=0;
        if (vm_read_overwrite(mach_task_self(),fp,sizeof(frame),(vm_address_t)frame,&got)!=KERN_SUCCESS ||
            got!=sizeof(frame)) break;
        say("Pong UI: main frame %u lr=%llx (cache unslid)\n",i,frame[1]-ui_cache_slide);
        if (frame[0]<=fp || frame[0]-fp>1024*1024) break;
        fp=frame[0];
    }
}

static void start_home_transition(void)
{
    ui_main_thread=mach_thread_self();
    size_t length=0;
    const void *cache=_dyld_get_shared_cache_range(&length);
    ui_cache_slide=cache?(uintptr_t)cache-0x180000000ULL:0;
    CFRunLoopObserverRef observer=CFRunLoopObserverCreate(NULL,kCFRunLoopBeforeWaiting,true,2000001,main_turn,NULL);
    if (observer) { CFRunLoopAddObserver(CFRunLoopGetMain(),observer,kCFRunLoopCommonModes);CFRelease(observer); }
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_SEC),dispatch_get_main_queue(),NULL,present_home);
    dispatch_queue_t queue=dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_LOW,0);
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,60*NSEC_PER_SEC),queue,(void *)60,main_snapshot);
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,120*NSEC_PER_SEC),queue,(void *)120,main_snapshot);
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,300*NSEC_PER_SEC),queue,(void *)300,main_snapshot);
}

/* Addresses resolved from the supplied 20G81 QuartzCore, not from 20F66.
 * The replacement preserves the real first result; a missing GPU is cached so
 * CoreAnimation can continue using its existing sw_renderer rather than retry.
 */
static uintptr_t original_renderer, original_format, original_ready, original_ignore;
static uintptr_t original_begin, original_render;
static bool renderer_unavailable;
static void *renderer(void *server)
{
    if (__atomic_load_n(&renderer_unavailable,__ATOMIC_ACQUIRE)) return NULL;
    void *r=((void *(*)(void *))original_renderer)(server);
    if (!r) {
        __atomic_store_n(&renderer_unavailable,true,__ATOMIC_RELEASE);
        say("Pong UI: CoreAnimation renderer unavailable; retain software renderer\n");
    }
    return r;
}

/* A CPU-only scanout consumes uncompressed 8-bit BGRA. Do not select the
 * compressed/half-float formats that the original Apple product supports.
 * The slot and original method are from this supplied 20G81 QuartzCore. */
static uint32_t framebuffer_format(void *display)
{
    uint32_t wanted=((uint32_t(*)(void *))original_format)(display);
    static unsigned calls;
    if (__atomic_fetch_add(&calls,1,__ATOMIC_RELAXED)==0)
        say("Pong UI: framebuffer format requested=%08x selected=42475241 (software BGRA)\n",wanted);
    return 0x42475241; /* 'BGRA', the actual MiniIOMobileFramebuffer format */
}

/* Bounded observations accompany the format/selection repairs. Preserve the
 * original readiness decision; never force an unavailable display ready. */
static bool display_ready(void *display)
{
    bool ready=((bool(*)(void *))original_ready)(display);
    static unsigned calls;
    unsigned n=__atomic_fetch_add(&calls,1,__ATOMIC_RELAXED);
    if (n<3) {
        uint64_t flags; memcpy(&flags,(char *)display+0x9f2,sizeof(flags));
        say("Pong UI: display ready=%d flags=%llx\n",ready,(unsigned long long)flags);
    }
    return ready;
}

static bool ignore_update(void *display, void *update)
{
    bool ignore=((bool(*)(void *,void *))original_ignore)(display,update);
    static unsigned calls, reports;
    static int last=-1;
    unsigned n=__atomic_fetch_add(&calls,1,__ATOMIC_RELAXED);
    int previous=__atomic_exchange_n(&last,ignore,__ATOMIC_RELAXED);
    if ((n<3 || previous!=ignore) && __atomic_fetch_add(&reports,1,__ATOMIC_RELAXED)<8) {
        uint64_t flags; uint32_t damage;
        memcpy(&flags,(char *)display+0x9f2,sizeof(flags));
        memcpy(&damage,(char *)update+0xf4,sizeof(damage));
        say("Pong UI: display ignore update=%d flags=%llx damage=%x idle=%x\n",ignore,
            (unsigned long long)flags,damage,(unsigned)((unsigned char *)display)[0x233]);
    }
    return ignore;
}

static uintptr_t begin_update(void *display, void *update)
{
    static unsigned calls;
    unsigned n=__atomic_fetch_add(&calls,1,__ATOMIC_RELAXED);
    if (n<3) say("Pong UI: display begin update enter\n");
    uintptr_t result=((uintptr_t(*)(void *,void *))original_begin)(display,update);
    if (n<3) say("Pong UI: display begin update return=%lx\n",(unsigned long)result);
    return result;
}

static uintptr_t render_update(void *server, void *update, unsigned *flags)
{
    static unsigned calls;
    unsigned n=__atomic_fetch_add(&calls,1,__ATOMIC_RELAXED);
    if (n<3) say("Pong UI: compositor render update enter\n");
    uintptr_t result=((uintptr_t(*)(void *,void *,unsigned *))original_render)(server,update,flags);
    if (n<3) say("Pong UI: compositor render update return flags=%x\n",flags?*flags:0);
    return result;
}

static bool replace_virtual(const char *name, uintptr_t slot_address, uintptr_t method_address,
                            void *replacement, uintptr_t *original)
{
    size_t length=0;
    uintptr_t base=(uintptr_t)_dyld_get_shared_cache_range(&length);
    if (!base || length<sizeof(uintptr_t) || base>UINTPTR_MAX-length) return false;
    uintptr_t slide=base-0x180000000ULL;
    uintptr_t slot=slot_address+slide, expected=method_address+slide;
    if (slot<base || slot>base+length-sizeof(uintptr_t)) return false;
    uintptr_t *entry=(uintptr_t *)slot;
    Dl_info info;
    if (*entry!=expected || !dladdr((void *)expected,&info) || !info.dli_fname ||
        !strstr(info.dli_fname,"/QuartzCore.framework/QuartzCore")) {
        say("Pong UI: 20G81 QuartzCore %s ABI unavailable; SKIP\n",name);return false;
    }
    vm_address_t page=slot&~((vm_address_t)vm_page_size-1);
    kern_return_t kr=vm_protect(mach_task_self(),page,vm_page_size,FALSE,
                               VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY);
    if (kr!=KERN_SUCCESS) {say("Pong UI: %s table protection failed %d\n",name,kr);return false;}
    *original=expected; /* publish the original before installing the wrapper */
    __atomic_store_n(entry,(uintptr_t)replacement,__ATOMIC_RELEASE);
    kr=vm_protect(mach_task_self(),page,vm_page_size,FALSE,VM_PROT_READ);
    say("Pong UI: 20G81 %s installed (protection=%d)\n",name,kr);
    return true;
}

static void install_renderer_hook(void)
{
    replace_virtual("software renderer fallback",0x1d586e000ULL,0x18826a418ULL,
                    (void *)renderer,&original_renderer);
    replace_virtual("software framebuffer format",0x1d586c7c0ULL,0x1884aa47cULL,
                    (void *)framebuffer_format,&original_format);
    replace_virtual("display readiness",0x1d586c7c8ULL,0x1882787f4ULL,
                    (void *)display_ready,&original_ready);
    replace_virtual("display update gate",0x1d586c850ULL,0x188268b60ULL,
                    (void *)ignore_update,&original_ignore);
    replace_virtual("display begin update",0x1d586c890ULL,0x188265c74ULL,
                    (void *)begin_update,&original_begin);
    /* EAGLServer's 20G81 render_update slot; no graphics driver is added. */
    replace_virtual("compositor render update",0x1d586dfd8ULL,0x1882df668ULL,
                    (void *)render_update,&original_render);
}

__attribute__((constructor)) static void pong_ui_init(void)
{
    const char *name=getprogname();
    if (!name) return;
    is_springboard=strcmp(name,"SpringBoard")==0;
    is_media=strcmp(name,"mediaserverd")==0;
    is_settings=strcmp(name,"Preferences")==0;
    bool backboard=strcmp(name,"backboardd")==0;
    if (!is_springboard && !backboard && !is_media && !is_settings && strncmp(name,"containermanagerd",17)!=0) return;
    /* HackiOS exposes the actual driver's last printf before a process aborts. */
    (void)dup2(2,1); setvbuf(stdout,NULL,_IONBF,0); setvbuf(stderr,NULL,_IONBF,0);
    say("Pong UI: 20G81 compatibility library entered %s\n",name);
    if (is_settings) {
        settings_record_init();
        pong_settings_icons_start(settings_record);
    }
    if (is_springboard) {
        CFPreferencesSetAppValue(CFSTR("BSUIVibrancyEffectViewDisabled"),kCFBooleanTrue,kCFPreferencesCurrentApplication);
        /* 20G81 SpringBoard reads this preference. Set it before UIApplication
         * startup: the delayed wake cannot run until the main queue is free. */
        CFPreferencesSetAppValue(CFSTR("SBDisableAutoDim"),kCFBooleanTrue,kCFPreferencesCurrentApplication);
    }
    if (is_springboard) start_home_transition();
    if (backboard) {install_renderer_hook();pong_touch_start();}
    _dyld_register_func_for_add_image(image_added);
}
