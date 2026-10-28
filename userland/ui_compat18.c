/* SPDX-License-Identifier: MIT
 * iPadOS 18 runtime adapter. No 20G81 QuartzCore/Settings addresses or vtables.
 * Device input reuses the actual Pong kernel event queue and runtime HID APIs.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <notify.h>
#include "touch.h"

static bool software_composition;
static bool scanout_format;
static bool renderer_installed;
static bool format_installed;
static bool personas_installed;
static bool springboard_process;
static bool phone_ui_process;
static bool skip_setup;
static IMP setup_reason_original;
static bool activation_installed;
static ptrdiff_t activation_offset;
static bool launchservices_process, launchservices_installed;
static bool installd_process, installd_directory_installed;
static bool pluginkit_process, pluginkit_directory_attempted;
static CFURLRef ls_base, ls_system, ls_user, ls_shared;
static CFURLRef installd_library;
static CFURLRef pluginkit_container;
static IMP battery_original;
static struct {Class cls;IMP original;} statusbar_factories[5];
static unsigned statusbar_factory_count;
static Class statusbar_provider;
static bool statusbar_fallback_bridge_installed;
static bool statusbar_diagnostic_item_removed;
static char statusbar_delegate_key;
static char statusbar_rebuild_key;
static IMP statusbar_fallback_get,statusbar_fallback_set,statusbar_fallback_responds,statusbar_fallback_setup_original;
static Class (*phone_selector_original)(id,id);
static bool phone_selector_installed;
static IMP phone_prepare_original;
static Ivar phone_provider_ivar;
static IMP phone_layout_original;
static bool (*phone_gestalt_bool_original)(CFStringRef);
static CFMutableDictionaryRef (*service_matching)(const char *);
static uint32_t (*matching_service)(uint32_t,CFDictionaryRef);
static int (*release_service)(uint32_t);
static CFURLRef container_root, container_library;
static void launchservices_directories(void);
static void installd_directory(void);
static void pluginkit_directory(void);
static void phone_selector_image(const struct mach_header *,intptr_t);
static void phone_gestalt_image(const struct mach_header *,intptr_t);
static void phone_repair_actual_bar(void *);
static void stui_repair_bar(id);
static bool boot_option(const char *option);
static bool own_directory(const char *path);
void pong_boot_snapshot_start(const char *name);
void pong_settings18_start(void);

static char abi_type_kind(const char *type)
{
    /* Objective-C qualifiers describe ownership/direction, not the register
     * ABI. Darwin24 declares NSError ** as o^@; checking its first byte
     * against '^' silently disabled the PosterBoard storage adapter. */
    if(!type)return 0;
    while(*type && strchr("rnNoORV",*type))type++;
    return *type;
}
static bool method_abi(Method method,const char *result,unsigned argc,const char *arguments)
{
    if(!method || method_getNumberOfArguments(method)!=argc+2)return false;
    char *type=method_copyReturnType(method);
    char kind=abi_type_kind(type);
    bool valid=kind && strchr(result,kind);
    free(type);
    for(unsigned i=0;valid && i<argc;i++) {
        type=method_copyArgumentType(method,i+2);
        valid=abi_type_kind(type)==arguments[i];
        free(type);
    }
    return valid;
}

/* The adapted launch cache has one canonical LaunchDaemons directory. That
 * loses launchd's Angel classification for PosterBoard: RunningBoard chose
 * the SystemXPCService 6 MiB defaults instead of its stock Angel policy.
 * Use an identity view ONLY for this synchronous policy lookup. The original
 * identity, bundle, PID, entitlements and service registration stay native.
 * RunningBoard still constructs its own limits from the matching board plist.
 */
static IMP poster_jetsam_original;
static Class poster_jetsam_identity;
static char poster_jetsam_source_key;
static id poster_jetsam_source(id self)
{ return objc_getAssociatedObject(self,&poster_jetsam_source_key); }
static unsigned char poster_jetsam_service_type(id self,SEL selector)
{ (void)self;(void)selector;return 3; /* native osServiceType: Angel */ }
static BOOL poster_jetsam_is_application(id self,SEL selector)
{ (void)self;(void)selector;return NO; }
static id poster_jetsam_label(id self,SEL selector)
{ return ((id(*)(id,SEL))objc_msgSend)(poster_jetsam_source(self),selector); }
static id poster_jetsam_forward(id self,SEL selector,SEL query)
{ (void)selector;(void)query;return poster_jetsam_source(self); }
static BOOL poster_jetsam_responds(id self,SEL selector,SEL query)
{ (void)selector;return ((BOOL(*)(id,SEL,SEL))objc_msgSend)(poster_jetsam_source(self),sel_registerName("respondsToSelector:"),query); }
static id poster_jetsam_properties(id self,SEL selector,int pid,id identity,id bundle,BOOL platform)
{
    SEL label_query=sel_registerName("consistentLaunchdJobLabel");
    Method label=identity?class_getInstanceMethod(object_getClass(identity),label_query):NULL;
    if(method_abi(label,"@",0,"")) {
        id value=((id(*)(id,SEL))objc_msgSend)(identity,label_query);
        if(value && CFEqual((CFTypeRef)value,CFSTR("com.apple.PosterBoard"))) {
            id view=((id(*)(id,SEL))objc_msgSend)((id)poster_jetsam_identity,sel_registerName("new"));
            if(view) {
                objc_setAssociatedObject(view,&poster_jetsam_source_key,identity,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                id result=((id(*)(id,SEL,int,id,id,BOOL))poster_jetsam_original)(self,selector,pid,view,bundle,platform);
                ((void(*)(id,SEL))objc_msgSend)(view,sel_registerName("release"));
                static unsigned reported;
                if(__atomic_fetch_add(&reported,1,__ATOMIC_RELAXED)<3)
                    fprintf(stderr,"Pong Poster18: native Angel memory policy selected for PosterBoard\n");
                return result;
            }
        }
    }
    return ((id(*)(id,SEL,int,id,id,BOOL))poster_jetsam_original)(self,selector,pid,identity,bundle,platform);
}
static void poster_jetsam_image(const struct mach_header *header,intptr_t slide)
{
    (void)header;(void)slide;
    if(poster_jetsam_original)return;
    Class manager=objc_lookUpClass("RBJetsamPropertyManager");
    Class identity=objc_lookUpClass("RBSProcessIdentity");
    SEL query=sel_registerName("jetsamPropertiesForProcess:identity:bundleProperties:isPlatformBinary:");
    Method method=manager?class_getInstanceMethod(manager,query):NULL;
    Method type=identity?class_getInstanceMethod(identity,sel_registerName("osServiceType")):NULL;
    if(!method_abi(method,"@",4,"i@@B") || !method_abi(type,"C",0,""))return;
    Class view=objc_allocateClassPair(objc_lookUpClass("NSObject"),"PongPosterJetsamIdentity18",0);
    if(!view)return;
    if(!class_addMethod(view,sel_registerName("osServiceType"),(IMP)poster_jetsam_service_type,method_getTypeEncoding(type)) ||
       !class_addMethod(view,sel_registerName("isApplication"),(IMP)poster_jetsam_is_application,"B16@0:8") ||
       !class_addMethod(view,sel_registerName("consistentLaunchdJobLabel"),(IMP)poster_jetsam_label,"@16@0:8") ||
       !class_addMethod(view,sel_registerName("forwardingTargetForSelector:"),(IMP)poster_jetsam_forward,"@24@0:8:16") ||
       !class_addMethod(view,sel_registerName("respondsToSelector:"),(IMP)poster_jetsam_responds,"B24@0:8:16")) {
        objc_disposeClassPair(view);return;
    }
    objc_registerClassPair(view);poster_jetsam_identity=view;
    poster_jetsam_original=method_setImplementation(method,(IMP)poster_jetsam_properties);
}

/* Phone identity still uses Pong's real screen. The native provider factory
 * falls back to a diagnostic label for an unlisted width/height tuple. Reuse
 * its real 1080px phone layout only for this main display, with the factory's
 * normal initialization and the supplied 22H374 Objective-C signatures. */
static double phone_reference_scale(id self,SEL selector)
{
    (void)self;(void)selector;
    /* This class method also runs during UIKit's screen/provider discovery.
     * Reentering +mainScreen here can wait on that same initialization. Pong's
     * ADT and framebuffer use scale 3; no UIKit calls are needed to report it. */
    return 3.0;
}
/* 22H374 can retain a diagnostic provider selected before +load completes.
 * Give that provider a real, separately allocated phone layout. Its 24-byte
 * instance cannot be changed into a 240-byte DynamicSplit object in place.
 * All regions, placements, clocks and battery views still come from UIKit;
 * none are drawn by this adapter and no hardware values are synthesized. */
static id phone_fallback_delegate(id self)
{
    id delegate=objc_getAssociatedObject(self,&statusbar_delegate_key);
    if(delegate || !statusbar_provider)return delegate;
    delegate=((id(*)(id,SEL))objc_msgSend)((id)statusbar_provider,sel_registerName("new"));
    if(!delegate)return nil;
    id bar=((id(*)(id,SEL))statusbar_fallback_get)(self,sel_registerName("statusBar"));
    ((void(*)(id,SEL,id))objc_msgSend)(delegate,sel_registerName("setStatusBar:"),bar);
    objc_setAssociatedObject(self,&statusbar_delegate_key,delegate,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    ((void(*)(id,SEL))objc_msgSend)(delegate,sel_registerName("release"));
    // This weak statusBar link also finds server-owned bars absent from
    // UIApplication.windows. Rebuild once after the current native update
    // unwinds; swapping an in-use provider synchronously is unsafe.
    if(bar && !objc_getAssociatedObject(bar,&statusbar_rebuild_key)) {
        objc_setAssociatedObject(bar,&statusbar_rebuild_key,(id)kCFBooleanTrue,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        id retained=((id(*)(id,SEL))objc_msgSend)(bar,sel_registerName("retain"));
        dispatch_async_f(dispatch_get_main_queue(),retained,phone_repair_actual_bar);
    }
    return objc_getAssociatedObject(self,&statusbar_delegate_key);
}
static void phone_fallback_set_bar(id self,SEL selector,id bar)
{
    ((void(*)(id,SEL,id))statusbar_fallback_set)(self,selector,bar);
    id delegate=phone_fallback_delegate(self);
    if(delegate)((void(*)(id,SEL,id))objc_msgSend)(delegate,selector,bar);
}
static id phone_fallback_setup(id self,SEL selector,id container)
{
    id delegate=phone_fallback_delegate(self);
    if(!delegate)return ((id(*)(id,SEL,id))statusbar_fallback_setup_original)(self,selector,container);
    id regions=((id(*)(id,SEL,id))objc_msgSend)(delegate,selector,container);
    static unsigned reported;
    if(__atomic_fetch_add(&reported,1,__ATOMIC_RELAXED)<3)
        fprintf(stderr,"Pong UI18: diagnostic provider replaced by native phone regions\n");
    return regions;
}
static id phone_fallback_placements(id self,SEL selector,id region)
{ return ((id(*)(id,SEL,id))objc_msgSend)(phone_fallback_delegate(self),selector,region); }
static id phone_fallback_style(id self,SEL selector,long long style)
{ return ((id(*)(id,SEL,long long))objc_msgSend)(phone_fallback_delegate(self),selector,style); }
static id phone_fallback_clock(id self,SEL selector)
{ return ((id(*)(id,SEL))objc_msgSend)(phone_fallback_delegate(self),selector); }
static BOOL phone_fallback_responds(id self,SEL selector,SEL query)
{
    if(((BOOL(*)(id,SEL,SEL))statusbar_fallback_responds)(self,selector,query))return YES;
    return ((BOOL(*)(id,SEL,SEL))objc_msgSend)(phone_fallback_delegate(self),selector,query);
}
static id phone_fallback_forward(id self,SEL selector,SEL query)
{
    (void)selector;
    id delegate=phone_fallback_delegate(self);
    return delegate && ((BOOL(*)(id,SEL,SEL))objc_msgSend)(delegate,sel_registerName("respondsToSelector:"),query)?delegate:nil;
}
static BOOL phone_fallback_class_flag(id self,SEL selector)
{ (void)self;return ((BOOL(*)(id,SEL))objc_msgSend)((id)statusbar_provider,selector); }
static CGSize phone_fallback_class_size(id self,SEL selector,long long orientation)
{ (void)self;return ((CGSize(*)(id,SEL,long long))objc_msgSend)((id)statusbar_provider,selector,orientation); }
static void phone_fallback_policy(void)
{
    if(statusbar_fallback_bridge_installed || !statusbar_provider)return;
    Class fallback=objc_lookUpClass("_UIStatusBarVisualProvider_Fallback");
    const char *names[]={"statusBar","setStatusBar:","setupInContainerView:",
        "orderedDisplayItemPlacementsInRegionWithIdentifier:","styleAttributesForStyle:","clockFont"};
    const char *results[]={"@","v","@","@","@","@"};
    const unsigned counts[]={0,1,1,1,1,0};
    const char *args[]={"","@","@","@","q",""};
    Method methods[6];
    for(unsigned i=0;i<6;i++) {
        SEL sel=sel_registerName(names[i]);
        methods[i]=fallback?class_getInstanceMethod(fallback,sel):NULL;
        if(!method_abi(methods[i],results[i],counts[i],args[i]) ||
           !method_abi(class_getInstanceMethod(statusbar_provider,sel),results[i],counts[i],args[i]))return;
    }
    SEL responds=sel_registerName("respondsToSelector:"),forward=sel_registerName("forwardingTargetForSelector:");
    Method rm=class_getInstanceMethod(fallback,responds),fm=class_getInstanceMethod(fallback,forward);
    if(!method_abi(rm,"Bc",1,":") || !method_abi(fm,"@",1,":"))return;
    Class meta=object_getClass((id)fallback);
    const char *flags[]={"scalesWithScreenNativeScale","requiresIterativeOverflowLayout"};
    for(unsigned i=0;i<2;i++)if(!method_abi(class_getClassMethod(fallback,sel_registerName(flags[i])),"Bc",0,"") ||
        !method_abi(class_getClassMethod(statusbar_provider,sel_registerName(flags[i])),"Bc",0,""))return;
    SEL size=sel_registerName("intrinsicContentSizeForOrientation:");
    Method sm=class_getClassMethod(fallback,size),ps=class_getClassMethod(statusbar_provider,size);
    if(!method_abi(sm,"{",1,"q") || !method_abi(ps,"{",1,"q"))return;
    statusbar_fallback_get=method_getImplementation(methods[0]);
    statusbar_fallback_set=method_setImplementation(methods[1],(IMP)phone_fallback_set_bar);
    statusbar_fallback_setup_original=method_setImplementation(methods[2],(IMP)phone_fallback_setup);
    method_setImplementation(methods[3],(IMP)phone_fallback_placements);
    method_setImplementation(methods[4],(IMP)phone_fallback_style);
    method_setImplementation(methods[5],(IMP)phone_fallback_clock);
    statusbar_fallback_responds=method_getImplementation(rm);
    class_addMethod(fallback,responds,(IMP)phone_fallback_responds,method_getTypeEncoding(rm));
    class_addMethod(fallback,forward,(IMP)phone_fallback_forward,method_getTypeEncoding(fm));
    for(unsigned i=0;i<2;i++)method_setImplementation(class_getClassMethod(fallback,sel_registerName(flags[i])),(IMP)phone_fallback_class_flag);
    method_setImplementation(sm,(IMP)phone_fallback_class_size);
    class_addMethod(meta,sel_registerName("referenceScreenScale"),(IMP)phone_reference_scale,"d16@0:8");
    statusbar_fallback_bridge_installed=true;
    fprintf(stderr,"Pong UI18: rdar diagnostic layout removed; native clock/battery layout connected\n");
}
static BOOL phone_diagnostic_item_enabled(id self,SEL selector,id item,id data)
{ (void)self;(void)selector;(void)item;(void)data;return NO; }
static void phone_remove_diagnostic_item(void)
{
    if(statusbar_diagnostic_item_removed)return;
    Class cls=objc_lookUpClass("_UIStatusBarFallbackItem");
    Method method=cls?class_getInstanceMethod(cls,sel_registerName("canEnableDisplayItem:fromData:")):NULL;
    if(!method_abi(method,"Bc",2,"@@"))return;
    method_setImplementation(method,(IMP)phone_diagnostic_item_enabled);
    statusbar_diagnostic_item_removed=true;
}
static Class phone_statusbar(unsigned index,id self,SEL selector,id screen,id info)
{
    /* Resolve the known Pong screen before entering the native candidate
     * enumerator, matching the order used before services42. */
    // UIKit uses nil for its default display during screen construction.
    // This adapter is already restricted to Pong; do not reenter mainScreen.
    if(!screen && statusbar_provider)return statusbar_provider;
    if(screen && statusbar_provider) {
        CGRect bounds=((CGRect(*)(id,SEL))objc_msgSend)(screen,sel_registerName("nativeBounds"));
        double scale=((double(*)(id,SEL))objc_msgSend)(screen,sel_registerName("nativeScale"));
        if(((bounds.size.width==1080 && bounds.size.height==2412) ||
            (bounds.size.height==1080 && bounds.size.width==2412)) && scale>0 && scale<=4) {
            static unsigned reported;
            if(__atomic_fetch_add(&reported,1,__ATOMIC_RELAXED)<3)
                fprintf(stderr,"Pong UI18: actual 1080x2412 phone status bar selected\n");
            return statusbar_provider;
        }
    }
    return ((Class(*)(id,SEL,id,id))statusbar_factories[index].original)(self,selector,screen,info);
}
/* Darwin24 compares a candidate's IMP with its parent's IMP. Each factory
 * must have a distinct wrapper, even though the Pong screen policy is shared. */
#define PHONE_FACTORY(N) static Class phone_factory##N(id s,SEL c,id screen,id info) \
    { return phone_statusbar(N,s,c,screen,info); }
PHONE_FACTORY(0)
PHONE_FACTORY(1)
PHONE_FACTORY(2)
PHONE_FACTORY(3)
#undef PHONE_FACTORY
static void phone_presentation(void)
{
    if(!phone_ui_process || statusbar_factory_count)return;
    char product[64]={0};size_t size=sizeof(product);
    if(sysctlbyname("hw.product",product,&size,NULL,0) || strcmp(product,"iPhone17,1"))return;
    phone_remove_diagnostic_item();
    Class base=objc_lookUpClass("_UIStatusBarVisualProvider_DynamicSplitMedium");
    if(!base)base=objc_lookUpClass("_UIStatusBarVisualProvider_Split1080");
    if(!base)return;
    // DynamicSplitMedium does not declare referenceScreenScale in 22H374.
    // It is an optional class query, which this concrete Pong subclass adds.
    // Requiring it on the base silently skipped the entire provider adapter.
    Class provider=objc_allocateClassPair(base,"PongStatusBar18Phone1080",0);
    if(!provider)return;
    if(!class_addMethod(object_getClass((id)provider),sel_registerName("referenceScreenScale"),(IMP)phone_reference_scale,"d16@0:8")) {
        objc_disposeClassPair(provider);return;
    }
    objc_registerClassPair(provider);statusbar_provider=provider;
    phone_fallback_policy();
    // Keep _prepareVisualProviderIfNeeded's native class selection branch:
    // it initializes scale/reference/overflow flags before provider init.
    // Darwin24's C selector compares each subclass IMP with its parent.
    // Hooking every class to the same IMP makes it discard all candidates.
    // Keep the root implementation intact and use distinct child IMPs.
    const char *names[]={"_UIStatusBarVisualProvider_iOS","_UIStatusBarVisualProvider_Phone",
        "_UIStatusBarVisualProvider_Split","_UIStatusBarVisualProvider_DynamicSplit"};
    const IMP replacements[]={(IMP)phone_factory0,(IMP)phone_factory1,(IMP)phone_factory2,(IMP)phone_factory3};
    SEL selector=sel_registerName("visualProviderSubclassForScreen:visualProviderInfo:");
    /* services43 rendered with only the iOS factory adapted. The broader
     * services44 overrides ran before any scanout and never completed the
     * main-queue wake. Keep child factory discovery native during boot;
     * the extended candidate policy remains an explicit development option. */
    unsigned count=boot_option("pong_statusbar_factories=1")?4:1;
    for(unsigned i=0;i<count;i++) {
        Class factory=objc_lookUpClass(names[i]);
        Method method=factory?class_getClassMethod(factory,selector):NULL;
        if(!method_abi(method,"#",2,"@@"))continue;
        // Do not mutate an inherited method through a child class handle.
        unsigned n=0;Method *own=class_copyMethodList(object_getClass((id)factory),&n);
        bool declared=false;
        for(unsigned j=0;j<n;j++)if(method_getName(own[j])==selector)declared=true;
        free(own);if(!declared)continue;
        statusbar_factories[i].cls=factory;
        statusbar_factories[i].original=method_setImplementation(method,replacements[i]);
        statusbar_factory_count++;
    }
    fprintf(stderr,"Pong UI18: native phone status bar factories installed=%u\n",
        statusbar_factory_count);
}

/* This restore supplies iPad artwork. Use 22H374 WallpaperKit's own
 * compatibility selectors for its real catalogs on Pong, rather than clone
 * collection UUIDs or change the user's selected iPhone identity. */
static bool wallpaper_process,wallpaper_paths_process,wallpaper_policy_installed;
static BOOL (*wallpaper_original_collection)(id,SEL,id);
static BOOL wallpaper_collection_path(id self,SEL selector,id url)
{
    if(!wallpaper_original_collection(self,selector,url))return NO;
    if(!url || CFGetTypeID((CFTypeRef)url)!=CFURLGetTypeID())return YES;
    CFStringRef path=CFURLCopyFileSystemPath((CFURLRef)url,kCFURLPOSIXPathStyle);
    char original[PATH_MAX],phone[PATH_MAX];
    bool usable=path && CFStringGetCString(path,original,sizeof original,kCFStringEncodingUTF8);
    if(path)CFRelease(path);
    if(!usable)return YES;
    const char *prefix="/Library/Wallpaper/Collections/";
    if(strncmp(original,prefix,strlen(prefix)))return YES;
    char *suffix=strstr(original,"~ipad.wallpaperCollection");
    if(!suffix || strcmp(suffix,"~ipad.wallpaperCollection"))return YES;
    // _loadSystemWallpaperCollections keys by UUID. An iPad directory
    // scanned after its phone wrapper overwrote the adapted screen variant.
    // Only exclude the duplicate when the matching real wrapper is present.
    int n=snprintf(phone,sizeof phone,"%.*s~iphone.wallpaperCollection/WallpaperCollection.plist",
        (int)(suffix-original),original);
    return n>0 && (size_t)n<sizeof phone && access(phone,R_OK)==0?NO:YES;
}
static BOOL wallpaper_resource_compat(id self,SEL selector)
{ (void)self;(void)selector;return YES; }
static void wallpaper_resource_policy(void)
{
    if(wallpaper_paths_process && !wallpaper_original_collection) {
        Class cls=objc_lookUpClass("WKWallpaperRepresentingCollection");
        Method method=cls?class_getClassMethod(cls,sel_registerName("shouldLoadWallpaperCollectionAtURL:")):NULL;
        if(method_abi(method,"Bc",1,"@")) {
            wallpaper_original_collection=(BOOL (*)(id,SEL,id))method_setImplementation(method,(IMP)wallpaper_collection_path);
            fprintf(stderr,"Pong UI18: native wallpaper scan prefers installed phone path per UUID\n");
        }
    }
    if(!wallpaper_process || wallpaper_policy_installed)return;
    Class cls=objc_lookUpClass("NSUserDefaults");
    Method product=cls?class_getInstanceMethod(cls,sel_registerName("wk_ignoreProductTypesForWallpaperBundle")):NULL;
    Method screen=cls?class_getInstanceMethod(cls,sel_registerName("wk_ignoreLogicalScreenClassForWallpaperBundle")):NULL;
    if(!method_abi(product,"Bc",0,"") || !method_abi(screen,"Bc",0,""))return;
    method_setImplementation(product,(IMP)wallpaper_resource_compat);
    method_setImplementation(screen,(IMP)wallpaper_resource_compat);
    wallpaper_policy_installed=true;
    fprintf(stderr,"Pong UI18: original 22H374 wallpaper catalogs enabled for Pong screen\n");
}

/* Same single-user container policy as the working iOS16 port. Native
 * UID containers remain in use. Do not disable launchd's Darwin24 proxy
 * personas, which are separate from per-user MCM persona containers. */
static BOOL no_container_personas(id self,SEL selector)
{ (void)self;(void)selector;return NO; }

/* 22H374 SBLockdownManager still stores its local UI state as an int.
 * Its original method maps Activated/FactoryActivated to 2. Discover the
 * ivar at runtime, rather than copying the old OS's +8 offset. This is a
 * development SpringBoard setup policy, not a mobileactivationd/SEP reply. */
static void setup_activation(id self,SEL selector)
{
    (void)selector;
    const int state=2;
    memcpy((char *)self+activation_offset,&state,sizeof(state));
    static unsigned reported;
    if(__atomic_fetch_add(&reported,1,__ATOMIC_RELAXED)==0)
        fprintf(stderr,"Pong UI18: development SpringBoard activation UI selected\n");
}

/* The supplied 22H374 method is B24@0:8Q16, as on 20G81. Keep its actual
 * transition/notification implementation and return value, passing the
 * no-assistant reason requested by the Pong bring-up boot option. */
static BOOL setup_reason(id self,SEL selector,unsigned long long reason)
{
    static unsigned reported;
    if(reason && __atomic_fetch_add(&reported,1,__ATOMIC_RELAXED)<3)
        fprintf(stderr,"Pong UI18: development setup reason %llu skipped\n",reason);
    return ((BOOL(*)(id,SEL,unsigned long long))setup_reason_original)(self,selector,0);
}

static void springboard_setup_policy(void)
{
    if(!springboard_process || !skip_setup)return;
    if(!activation_installed) {
        Class cls=objc_lookUpClass("SBLockdownManager");
        Method method=cls?class_getInstanceMethod(cls,sel_registerName("_setupActivationState")):NULL;
        Ivar ivar=cls?class_getInstanceVariable(cls,"_state"):NULL;
        const char *type=ivar?ivar_getTypeEncoding(ivar):NULL;
        ptrdiff_t offset=ivar?ivar_getOffset(ivar):-1;
        if(method_abi(method,"v",0,"") && type && type[0] && strchr("iI",type[0]) &&
           offset>=(ptrdiff_t)sizeof(void *) &&
           (size_t)offset+sizeof(int)<=class_getInstanceSize(cls)) {
            activation_offset=offset;
            method_setImplementation(method,(IMP)setup_activation);
            activation_installed=true;
            fprintf(stderr,"Pong UI18: 22H374 activation UI policy installed\n");
        }
    }
    if(!setup_reason_original) {
        Class cls=objc_lookUpClass("SBSetupManager");
        Method method=cls?class_getInstanceMethod(cls,sel_registerName("_setSetupRequiredReason:")):NULL;
        if(method_abi(method,"B",1,"Q")) {
            setup_reason_original=method_setImplementation(method,(IMP)setup_reason);
            fprintf(stderr,"Pong UI18: 22H374 Setup Assistant UI policy installed\n");
        }
    }
}

/* Reuse the working iOS16 policy, with 22H374's actual method ABI.
 * The method normally queries connectedDevices before returning nil when
 * there is no internal battery. Avoid its powerd wait only while this port
 * has no published physical power source. Keep the original path whenever
 * an IOPMPowerSource is present; never invent charge/health values. */
static id internal_battery(id self,SEL selector)
{
    CFMutableDictionaryRef match=service_matching("IOPMPowerSource");
    if(!match)return ((id(*)(id,SEL))battery_original)(self,selector);
    uint32_t source=matching_service(0,match); /* consumes match */
    if(source) {
        release_service(source);
        return ((id(*)(id,SEL))battery_original)(self,selector);
    }
    static unsigned reported;
    if(__atomic_fetch_add(&reported,1,__ATOMIC_RELAXED)==0)
        fprintf(stderr,"Pong UI18: no physical power source; omit unavailable battery status\n");
    return nil;
}

static void springboard_platform_policy(void)
{
    if(!springboard_process || battery_original)return;
    Class cls=objc_lookUpClass("BCBatteryDeviceController");
    Method method=cls?class_getInstanceMethod(cls,sel_registerName("sb_deviceInternalBattery")):NULL;
    if(!method_abi(method,"@",0,""))return;
    service_matching=(__typeof__(service_matching))dlsym(RTLD_DEFAULT,"IOServiceMatching");
    matching_service=(__typeof__(matching_service))dlsym(RTLD_DEFAULT,"IOServiceGetMatchingService");
    release_service=(__typeof__(release_service))dlsym(RTLD_DEFAULT,"IOObjectRelease");
    if(!service_matching || !matching_service || !release_service)return;
    battery_original=method_setImplementation(method,(IMP)internal_battery);
}

static void container_persona_policy(void)
{
    if(personas_installed)return;
    Class cls=objc_lookUpClass("MCMUserIdentityCache");
    Method method=cls?class_getClassMethod(cls,sel_registerName("personasAreSupported")):NULL;
    if(!method_abi(method,"Bc",0,""))return;
    method_setImplementation(method,(IMP)no_container_personas);
    personas_installed=true;
    fprintf(stderr,"Pong UI18: native single-user container paths selected\n");
}

/* A new unencrypted Data has no persona.kb. In 22H374 --init can finish
 * without creating it, but the daemon's disk loader then aborts on ENOENT.
 * Let the matching executable's own platform class create and serialize
 * its empty initial registry. No plist schema, users, key material or
 * persona IDs are fabricated; existing registries are never replaced. */
static void initialize_persona_manifest(void)
{
    const char *path="/private/var/keybags/persona.kb";
    struct stat st;
    if(geteuid()!=0 || lstat(path,&st)==0 || errno!=ENOENT)return;
    if(lstat("/private/var/keybags",&st)!=0 || !S_ISDIR(st.st_mode) ||
       st.st_uid!=0 || (st.st_mode&0077))return;
    Class cls=objc_lookUpClass("UMDPersonaManifest");
    SEL make=sel_registerName("manifest"),encode=sel_registerName("dataValue");
    if(!method_abi(cls?class_getClassMethod(cls,make):NULL,"@",0,""))return;
    id manifest=((id(*)(id,SEL))objc_msgSend)((id)cls,make);
    if(!manifest || !method_abi(class_getInstanceMethod(object_getClass(manifest),encode),"@",0,""))return;
    id data=((id(*)(id,SEL))objc_msgSend)(manifest,encode);
    if(!data || CFGetTypeID((CFTypeRef)data)!=CFDataGetTypeID())return;
    CFIndex length=CFDataGetLength((CFDataRef)data);
    if(length<=0 || length>4*1024*1024)return;
    /* Publish only a complete registry. A crash during the old direct
     * O_EXCL write left a truncated persona.kb that all later boots kept. */
    char temporary[]="/private/var/keybags/.persona.kb.pong.XXXXXX";
    int fd=mkstemp(temporary);
    if(fd<0)return;
    (void)fcntl(fd,F_SETFD,FD_CLOEXEC);
    const UInt8 *bytes=CFDataGetBytePtr((CFDataRef)data);
    size_t written=0;
    while(written<(size_t)length) {
        ssize_t n=write(fd,bytes+written,(size_t)length-written);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)break;
        written+=(size_t)n;
    }
    int saved=0;
    if(written!=(size_t)length || fsync(fd)!=0)saved=errno?errno:EIO;
    if(close(fd)!=0 && !saved)saved=errno;
    bool published=false;
    if(!saved) {
        if(link(temporary,path)==0)published=true;
        else if(errno!=EEXIST)saved=errno;
    }
    (void)unlink(temporary);
    if(saved) {
        fprintf(stderr,"Pong UI18: native persona registry write failed errno=%d\n",saved);
    } else if(published) {
        int directory=open("/private/var/keybags",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(directory>=0) {
            if(fsync(directory)!=0)
                fprintf(stderr,"Pong UI18: persona registry published; directory sync errno=%d\n",errno);
            (void)close(directory);
        }
        fprintf(stderr,"Pong UI18: 22H374 serialized initial persona registry (%ld bytes)\n",(long)length);
    }
}

static bool executable_image_range(const struct mach_header_64 *header,intptr_t slide,
                                   uintptr_t target,size_t bytes)
{
    if(!bytes || (uintptr_t)(header+1)>UINTPTR_MAX-header->sizeofcmds)return false;
    const struct load_command *lc=(const void *)(header+1);
    const char *end=(const char *)lc+header->sizeofcmds;
    for(uint32_t i=0;i<header->ncmds;i++) {
        if((const char *)lc+sizeof(*lc)>end || lc->cmdsize<sizeof(*lc) ||
           lc->cmdsize>(size_t)(end-(const char *)lc))return false;
        if(lc->cmd==LC_SEGMENT_64 && lc->cmdsize>=sizeof(struct segment_command_64)) {
            const struct segment_command_64 *segment=(const void *)lc;
            const vm_prot_t needed=VM_PROT_READ|VM_PROT_EXECUTE;
            uintptr_t start;
            if((segment->initprot&needed)==needed) {
                if(slide>=0) {
                    if(segment->vmaddr>UINTPTR_MAX-(uintptr_t)slide)return false;
                    start=(uintptr_t)segment->vmaddr+(uintptr_t)slide;
                } else {
                    uintptr_t amount=0-(uintptr_t)slide;
                    if(segment->vmaddr<amount)return false;
                    start=(uintptr_t)segment->vmaddr-amount;
                }
                if(segment->vmsize<=UINTPTR_MAX-start && target>=start &&
                   target-start<=segment->vmsize && bytes<=segment->vmsize-(target-start))return true;
            }
        }
        lc=(const void *)((const char *)lc+lc->cmdsize);
    }
    return false;
}

static bool install_compositor_code(const struct mach_header *header,intptr_t slide,uintptr_t offset,
    const uint32_t *expected,const uint32_t *replacement,size_t bytes,const char *label)
{
    if(offset>UINTPTR_MAX-(uintptr_t)header || !bytes || bytes>vm_page_size)return false;
    uintptr_t target=(uintptr_t)header+offset;
    if(!executable_image_range((const void *)header,slide,target,bytes)) {
        fprintf(stderr,"Pong UI18: %s outside executable image range; SKIP\n",label);return false;
    }
    if(memcmp((const void *)target,expected,bytes)) {
        fprintf(stderr,"Pong UI18: 22H374 %s entry differs; SKIP\n",label);return false;
    }
    const vm_address_t page=target&~((vm_address_t)vm_page_size-1);
    if(target-page>vm_page_size-bytes)return false;
    kern_return_t kr=vm_protect(mach_task_self(),page,vm_page_size,FALSE,
                               VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY);
    if(kr!=KERN_SUCCESS) {
        fprintf(stderr,"Pong UI18: %s protection failed %d\n",label,kr);return false;
    }
    memcpy((void *)target,replacement,bytes);
    sys_icache_invalidate((void *)target,bytes);
    kr=vm_protect(mach_task_self(),page,vm_page_size,FALSE,VM_PROT_READ|VM_PROT_EXECUTE);
    if(kr!=KERN_SUCCESS) {
        memcpy((void *)target,expected,bytes);
        sys_icache_invalidate((void *)target,bytes);
        (void)vm_protect(mach_task_self(),page,vm_page_size,FALSE,VM_PROT_READ|VM_PROT_EXECUTE);
        fprintf(stderr,"Pong UI18: %s RX restore failed %d; original restored\n",label,kr);
        return false;
    }
    fprintf(stderr,"Pong UI18: 22H374 %s selected\n",label);return true;
}

/* This build's generated deviceSupportsDynamicIsland getter calls
 * MGGetBoolAnswer with this exact key. Keep every other answer native: in
 * particular this does not advertise Face ID, telephony or camera hardware.
 * Identity/artwork/home gestures are provided by the ADT, not by this hook.
 */
static bool phone_gestalt_bool(CFStringRef key)
{
    if(key && (CFEqual(key,CFSTR("YlEtTtHlNesRBMal1CqRaA")) ||
               CFEqual(key,CFSTR("DeviceSupportsDynamicIsland"))))return true;
    return phone_gestalt_bool_original(key);
}
static void phone_gestalt_image(const struct mach_header *header,intptr_t slide)
{
    if(!phone_ui_process || phone_gestalt_bool_original || header->magic!=MH_MAGIC_64)return;
    static const uint8_t uuid[16]={0xbe,0xed,0x77,0xde,0x98,0x7a,0x34,0x7e,
                                 0xad,0xa9,0x88,0x63,0x4a,0x6b,0xe5,0x6a};
    const struct mach_header_64 *h=(const void *)header;
    const struct load_command *lc=(const void *)(h+1);
    const char *end=(const char *)lc+h->sizeofcmds;bool matched=false;
    for(uint32_t i=0;i<h->ncmds;i++) {
        if((const char *)lc+sizeof(*lc)>end || lc->cmdsize<sizeof(*lc) ||
           lc->cmdsize>(size_t)(end-(const char *)lc))return;
        if(lc->cmd==LC_UUID && lc->cmdsize==sizeof(struct uuid_command) &&
           !memcmp(((const struct uuid_command *)lc)->uuid,uuid,16))matched=true;
        lc=(const void *)((const char *)lc+lc->cmdsize);
    }
    if(!matched)return;
    char product[64]={0};size_t size=sizeof(product);
    if(sysctlbyname("hw.product",product,&size,NULL,0) || strcmp(product,"iPhone17,1"))return;
    const uintptr_t offset=0xf00,entry=(uintptr_t)header+offset;
    // Four position-independent prologue instructions from MGGetBoolAnswer.
    static const uint32_t expected[]={0xd100c3ff,0xa9014ff4,0xa9027bfd,0x910083fd};
    if(!executable_image_range((const void *)header,slide,entry,sizeof(expected)) ||
       memcmp((const void *)entry,expected,sizeof(expected)))return;
    vm_address_t trampoline=0;
    if(vm_allocate(mach_task_self(),&trampoline,vm_page_size,VM_FLAGS_ANYWHERE)!=KERN_SUCCESS)return;
    uint32_t back[]={0x58000050,0xd61f0200,(uint32_t)(entry+16),(uint32_t)((entry+16)>>32)};
    memcpy((void *)trampoline,expected,16);memcpy((void *)(trampoline+16),back,16);
    sys_icache_invalidate((void *)trampoline,32);
    if(vm_protect(mach_task_self(),trampoline,vm_page_size,FALSE,VM_PROT_READ|VM_PROT_EXECUTE)!=KERN_SUCCESS) {
        (void)vm_deallocate(mach_task_self(),trampoline,vm_page_size);return;
    }
    const uintptr_t replacement=(uintptr_t)phone_gestalt_bool;
    uint32_t jump[]={0x58000050,0xd61f0200,(uint32_t)replacement,(uint32_t)(replacement>>32)};
    phone_gestalt_bool_original=(bool(*)(CFStringRef))trampoline;
    if(!install_compositor_code(header,slide,offset,expected,jump,sizeof(expected),"MobileGestalt Dynamic Island")) {
        phone_gestalt_bool_original=NULL;(void)vm_deallocate(mach_task_self(),trampoline,vm_page_size);
    }
}

/* 22H374 _prepareVisualProviderIfNeeded directly calls this C selector,
 * bypassing the iOS class factory used by 20G81. Adapt that selection point
 * before UIKit computes provider flags/scale, preserving the native result
 * for supported layouts and every screen other than Pong's real panel. */
static Class phone_selected_class(id screen,id info)
{
    Class selected=phone_selector_original(screen,info);
    Class fallback=objc_lookUpClass("_UIStatusBarVisualProvider_Fallback");
    static unsigned decisions;
    if(__atomic_fetch_add(&decisions,1,__ATOMIC_RELAXED)<2)
        fprintf(stderr,"Pong UI18: native provider decision=%s screen=%s\n",
            selected?class_getName(selected):"nil",screen?object_getClassName(screen):"default");
    if((selected && selected!=fallback) || !statusbar_provider)return selected;
    /* UIKit also asks for its default display with nil while UIScreen is
     * being initialized. Calling mainScreen here would recurse into that
     * initialization; use the same Pong-only default as the class factory. */
    if(screen) {
        CGRect bounds=((CGRect(*)(id,SEL))objc_msgSend)(screen,sel_registerName("nativeBounds"));
        double scale=((double(*)(id,SEL))objc_msgSend)(screen,sel_registerName("nativeScale"));
        if(!((bounds.size.width==1080 && bounds.size.height==2412) ||
             (bounds.size.width==2412 && bounds.size.height==1080)) || scale!=3.0)return selected;
    }
    static unsigned reported;
    if(__atomic_fetch_add(&reported,1,__ATOMIC_RELAXED)<3)
        fprintf(stderr,"Pong UI18: Darwin24 selector chose native 1080 phone provider\n");
    return statusbar_provider;
}
static void phone_prepare_provider(id self,SEL selector)
{
    /* A class assigned before the selector was adapted skips the selector
     * entirely in Darwin24. Re-run native discovery only for an unbuilt
     * diagnostic provider; do not replace a live provider or pre-set its
     * scale/overflow flags. Both accessors and the ivar are from this OS's
     * runtime metadata, not a 20G81 offset. */
    Class assigned=((Class(*)(id,SEL))objc_msgSend)(self,sel_registerName("_visualProviderClass"));
    Class fallback=objc_lookUpClass("_UIStatusBarVisualProvider_Fallback");
    id current=object_getIvar(self,phone_provider_ivar);
    if(current && object_getClass(current)==fallback) {
        // Native callers inside UIKit can invoke this entry directly. Tear
        // down an already built diagnostic region before native discovery
        // recomputes scale/overflow flags and constructs the phone provider.
        ((void(*)(id,SEL))objc_msgSend)(self,sel_registerName("resetVisualProvider"));
        object_setIvarWithStrongDefault(self,phone_provider_ivar,nil);
        ((void(*)(id,SEL,Class))objc_msgSend)(self,sel_registerName("_setVisualProviderClass:"),Nil);
    }
    if(assigned==fallback && !object_getIvar(self,phone_provider_ivar)) {
        ((void(*)(id,SEL,Class))objc_msgSend)(self,sel_registerName("_setVisualProviderClass:"),Nil);
    }
    ((void(*)(id,SEL))phone_prepare_original)(self,selector);
}
static void phone_repair_actual_bar(void *context)
{
    id bar=(id)context;
    Class cls=objc_lookUpClass("_UIStatusBar");
    if(phone_prepare_original && phone_provider_ivar && cls &&
       ((BOOL(*)(id,SEL,Class))objc_msgSend)(bar,sel_registerName("isKindOfClass:"),cls)) {
        id current=object_getIvar(bar,phone_provider_ivar);
        if(current && object_getClass(current)==objc_lookUpClass("_UIStatusBarVisualProvider_Fallback")) {
            phone_prepare_provider(bar,sel_registerName("_prepareVisualProviderIfNeeded"));
            ((void(*)(id,SEL))objc_msgSend)(bar,sel_registerName("setNeedsLayout"));
            current=object_getIvar(bar,phone_provider_ivar);
            fprintf(stderr,"Pong UI18: actual status bar rebuilt provider=%s\n",current?object_getClassName(current):"nil");
        }
    }
    ((void(*)(id,SEL))objc_msgSend)(bar,sel_registerName("release"));
}
static void phone_layout_provider(id self,SEL selector)
{
    // A status-bar server window need not belong to UIApplication.windows.
    // Rebuild an early fallback at its first layout, on the actual view,
    // using native teardown and preparation rather than changing its class.
    id current=object_getIvar(self,phone_provider_ivar);
    Class fallback=objc_lookUpClass("_UIStatusBarVisualProvider_Fallback");
    if(current && object_getClass(current)==fallback) {
        SEL reset=sel_registerName("resetVisualProvider");
        ((void(*)(id,SEL))objc_msgSend)(self,reset);
        object_setIvarWithStrongDefault(self,phone_provider_ivar,nil);
        ((void(*)(id,SEL,Class))objc_msgSend)(self,sel_registerName("_setVisualProviderClass:"),Nil);
        ((void(*)(id,SEL))phone_prepare_original)(self,sel_registerName("_prepareVisualProviderIfNeeded"));
        current=object_getIvar(self,phone_provider_ivar);
        fprintf(stderr,"Pong UI18: status layout rebuilt provider=%s\n",current?object_getClassName(current):"nil");
    }
    ((void(*)(id,SEL))phone_layout_original)(self,selector);
}
static void phone_prepare_policy(const struct mach_header *header,intptr_t slide)
{
    if(phone_prepare_original || !phone_selector_installed)return;
    Class cls=objc_lookUpClass("_UIStatusBar");
    Method prepare=cls?class_getInstanceMethod(cls,sel_registerName("_prepareVisualProviderIfNeeded")):NULL;
    Method get=cls?class_getInstanceMethod(cls,sel_registerName("_visualProviderClass")):NULL;
    Method set=cls?class_getInstanceMethod(cls,sel_registerName("_setVisualProviderClass:")):NULL;
    Ivar ivar=cls?class_getInstanceVariable(cls,"_visualProvider"):NULL;
    const char *type=ivar?ivar_getTypeEncoding(ivar):NULL;
    if(!method_abi(prepare,"v",0,"") || !method_abi(get,"#",0,"") ||
       !method_abi(set,"v",1,"#") || !type || type[0]!='@')return;
    // In 22H374 UIKit's own status paths also call the native IMP directly;
    // a method-table replacement alone leaves those cached callers intact.
    // This exact four-instruction prologue contains no PC-relative operand.
    const uintptr_t offset=0x1860f2fa0ULL-0x184f96000ULL;
    const uintptr_t entry=(uintptr_t)header+offset;
    static const uint32_t expected[]={0xd102c3ff,0x6d0623e9,0xa9075ff8,0xa90857f6};
    if(!executable_image_range((const void *)header,slide,entry,sizeof(expected)) ||
       memcmp((const void *)entry,expected,sizeof(expected)))return;
    vm_address_t trampoline=0;
    if(vm_allocate(mach_task_self(),&trampoline,vm_page_size,VM_FLAGS_ANYWHERE)!=KERN_SUCCESS)return;
    uint32_t back[]={0x58000050,0xd61f0200,(uint32_t)(entry+16),(uint32_t)((entry+16)>>32)};
    memcpy((void *)trampoline,expected,16);memcpy((void *)(trampoline+16),back,16);
    sys_icache_invalidate((void *)trampoline,32);
    if(vm_protect(mach_task_self(),trampoline,vm_page_size,FALSE,VM_PROT_READ|VM_PROT_EXECUTE)!=KERN_SUCCESS) {
        (void)vm_deallocate(mach_task_self(),trampoline,vm_page_size);return;
    }
    const uintptr_t replacement=(uintptr_t)phone_prepare_provider;
    uint32_t jump[]={0x58000050,0xd61f0200,(uint32_t)replacement,(uint32_t)(replacement>>32)};
    phone_provider_ivar=ivar;
    phone_prepare_original=(IMP)trampoline;
    if(!install_compositor_code(header,slide,offset,expected,jump,sizeof(expected),"phone provider prepare entry")) {
        phone_prepare_original=NULL;(void)vm_deallocate(mach_task_self(),trampoline,vm_page_size);return;
    }
    method_setImplementation(prepare,(IMP)phone_prepare_provider);
    SEL layout=sel_registerName("layoutSubviews"),reset=sel_registerName("resetVisualProvider");
    Method lm=class_getInstanceMethod(cls,layout),rm=class_getInstanceMethod(cls,reset);
    if(method_abi(lm,"v",0,"") && method_abi(rm,"v",0,"")) {
        phone_layout_original=method_getImplementation(lm);
        if(!class_addMethod(cls,layout,(IMP)phone_layout_provider,method_getTypeEncoding(lm)))
            method_setImplementation(lm,(IMP)phone_layout_provider);
    }
}

/* A status view may be built by UIKit/SpringBoard +load before the injected
 * runtime's constructor. Factory hooks cannot change that existing object.
 * On the main queue, rebuild only a real fallback provider using 22H374's
 * native region teardown, strong ivar ownership and initialization path.
 * Native scale/reference/overflow flags are computed by UIKit again.
 */
static void phone_repair_view(id view,unsigned depth,unsigned *remaining)
{
    if(!view || !depth || !*remaining)return;
    --*remaining;
    stui_repair_bar(view);
    Class bar=objc_lookUpClass("_UIStatusBar");
    if(bar && ((BOOL(*)(id,SEL,Class))objc_msgSend)(view,sel_registerName("isKindOfClass:"),bar) &&
       phone_prepare_original && phone_provider_ivar) {
        id current=object_getIvar(view,phone_provider_ivar);
        Class fallback=objc_lookUpClass("_UIStatusBarVisualProvider_Fallback");
        if(current && object_getClass(current)==fallback) {
            SEL reset=sel_registerName("resetVisualProvider"),prepare=sel_registerName("_prepareVisualProviderIfNeeded");
            Method teardown=class_getInstanceMethod(bar,reset);
            if(method_abi(teardown,"v",0,"")) {
                ((void(*)(id,SEL))objc_msgSend)(view,reset);
                object_setIvarWithStrongDefault(view,phone_provider_ivar,nil);
                ((void(*)(id,SEL,Class))objc_msgSend)(view,sel_registerName("_setVisualProviderClass:"),Nil);
                ((void(*)(id,SEL))phone_prepare_original)(view,prepare);
                ((void(*)(id,SEL))objc_msgSend)(view,sel_registerName("setNeedsLayout"));
                id installed=object_getIvar(view,phone_provider_ivar);
                fprintf(stderr,"Pong UI18: existing status provider rebuilt %s -> %s\n",
                    class_getName(fallback),installed?object_getClassName(installed):"nil");
            }
        }
    }
    id children=((id(*)(id,SEL))objc_msgSend)(view,sel_registerName("subviews"));
    if(!children)return;
    CFIndex count=CFArrayGetCount((CFArrayRef)children);
    for(CFIndex i=0;i<count && *remaining;i++)
        phone_repair_view((id)CFArrayGetValueAtIndex((CFArrayRef)children,i),depth-1,remaining);
}
static void phone_repair_existing(void *unused)
{
    (void)unused;
    if(!phone_ui_process || !phone_selector_installed)return;
    Class cls=objc_lookUpClass("UIApplication");
    id app=cls?((id(*)(id,SEL))objc_msgSend)((id)cls,sel_registerName("sharedApplication")):nil;
    id windows=app?((id(*)(id,SEL))objc_msgSend)(app,sel_registerName("windows")):nil;
    if(!windows)return;
    unsigned remaining=2048;CFIndex count=CFArrayGetCount((CFArrayRef)windows);
    for(CFIndex i=0;i<count && remaining;i++)
        phone_repair_view((id)CFArrayGetValueAtIndex((CFArrayRef)windows,i),16,&remaining);
}
static void phone_selector_image(const struct mach_header *header,intptr_t slide)
{
    static const struct mach_header *uikit;
    static intptr_t uikit_slide;
    static bool attempted;
    if(!phone_ui_process || phone_selector_installed || attempted || header->magic!=MH_MAGIC_64)return;
    static const uint8_t uuid[16]={0x15,0x0b,0x91,0xd9,0xf3,0x7e,0x35,0x9f,
                                 0xad,0x9d,0x95,0x26,0xa4,0x3b,0x81,0xaa};
    const struct mach_header_64 *h=(const void *)header;
    const struct load_command *lc=(const void *)(h+1);
    const char *end=(const char *)lc+h->sizeofcmds;
    for(uint32_t i=0;i<h->ncmds;i++) {
        if((const char *)lc+sizeof(*lc)>end || lc->cmdsize<sizeof(*lc) ||
           lc->cmdsize>(size_t)(end-(const char *)lc))return;
        if(lc->cmd==LC_UUID && lc->cmdsize==sizeof(struct uuid_command) &&
           !memcmp(((const struct uuid_command *)lc)->uuid,uuid,16)){uikit=header;uikit_slide=slide;}
        lc=(const void *)((const char *)lc+lc->cmdsize);
    }
    if(!uikit || !statusbar_provider)return;
    attempted=true;
    const uintptr_t offset=0x1860cbb90ULL-0x184f96000ULL;
    const uintptr_t entry=(uintptr_t)uikit+offset;
    // These four instructions only adjust SP/save registers; no PC-relative
    // operand is relocated. The rest of the original selector runs unchanged.
    static const uint32_t expected[]={0xd10443ff,0x6d0a23e9,0xa90b6ffc,0xa90c67fa};
    if(!executable_image_range((const void *)uikit,uikit_slide,entry,sizeof(expected)) ||
       memcmp((const void *)entry,expected,sizeof(expected)))return;
    vm_address_t trampoline=0;
    if(vm_allocate(mach_task_self(),&trampoline,vm_page_size,VM_FLAGS_ANYWHERE)!=KERN_SUCCESS)return;
    uint32_t back[]={0x58000050,0xd61f0200,(uint32_t)(entry+16),(uint32_t)((entry+16)>>32)};
    memcpy((void *)trampoline,expected,16);memcpy((void *)(trampoline+16),back,16);
    sys_icache_invalidate((void *)trampoline,32);
    if(vm_protect(mach_task_self(),trampoline,vm_page_size,FALSE,VM_PROT_READ|VM_PROT_EXECUTE)!=KERN_SUCCESS){
        (void)vm_deallocate(mach_task_self(),trampoline,vm_page_size);return;
    }
    const uintptr_t replacement=(uintptr_t)phone_selected_class;
    uint32_t jump[]={0x58000050,0xd61f0200,(uint32_t)replacement,(uint32_t)(replacement>>32)};
    phone_selector_original=(Class(*)(id,id))trampoline;
    phone_selector_installed=install_compositor_code(uikit,uikit_slide,offset,
        expected,jump,sizeof(expected),"phone provider selector");
    if(!phone_selector_installed){phone_selector_original=NULL;(void)vm_deallocate(mach_task_self(),trampoline,vm_page_size);}
    else phone_prepare_policy(uikit,uikit_slide);
}

/* Darwin24's system status bar moved out of UIKitCore. It has distinct
 * STUI classes and a 368-byte DynamicSplit provider, not the 240-byte UIKit
 * one. Select and initialize that native implementation; never mix objects,
 * copy a 20G81 offset or replace a small fallback object's isa. */
static Class stui_provider;
static Ivar stui_provider_ivar;
static IMP stui_select_original,stui_prepare_original,stui_layout_original;
static Class stui_selected_class(id screen,id info)
{
    if(stui_provider) {
        bool pong=!screen;
        if(screen) {
            CGRect bounds=((CGRect(*)(id,SEL))objc_msgSend)(screen,sel_registerName("nativeBounds"));
            double scale=((double(*)(id,SEL))objc_msgSend)(screen,sel_registerName("nativeScale"));
            pong=((bounds.size.width==1080 && bounds.size.height==2412) ||
                  (bounds.size.width==2412 && bounds.size.height==1080)) && scale==3.0;
        }
        if(pong) {
            static unsigned reported;
            if(__atomic_fetch_add(&reported,1,__ATOMIC_RELAXED)<3)
                fprintf(stderr,"Pong UI18: SystemStatusUI selected native Pong clock/battery layout\n");
            return stui_provider;
        }
    }
    return ((Class(*)(id,id))stui_select_original)(screen,info);
}
static void stui_prepare_provider(id self,SEL selector)
{
    Class fallback=objc_lookUpClass("STUIStatusBarVisualProvider_Fallback");
    id current=object_getIvar(self,stui_provider_ivar);
    Class assigned=((Class(*)(id,SEL))objc_msgSend)(self,sel_registerName("_visualProviderClass"));
    if(current && object_getClass(current)==fallback) {
        ((void(*)(id,SEL))objc_msgSend)(self,sel_registerName("resetVisualProvider"));
        object_setIvarWithStrongDefault(self,stui_provider_ivar,nil);
        ((void(*)(id,SEL,Class))objc_msgSend)(self,sel_registerName("_setVisualProviderClass:"),Nil);
    } else if(!current && assigned==fallback) {
        ((void(*)(id,SEL,Class))objc_msgSend)(self,sel_registerName("_setVisualProviderClass:"),Nil);
    }
    ((void(*)(id,SEL))stui_prepare_original)(self,selector);
}
static void stui_repair_bar(id bar)
{
    if(!stui_prepare_original || !stui_provider_ivar)return;
    Class cls=objc_lookUpClass("STUIStatusBar");
    if(!cls || !((BOOL(*)(id,SEL,Class))objc_msgSend)(bar,sel_registerName("isKindOfClass:"),cls))return;
    id current=object_getIvar(bar,stui_provider_ivar);
    if(current && object_getClass(current)==objc_lookUpClass("STUIStatusBarVisualProvider_Fallback")) {
        stui_prepare_provider(bar,sel_registerName("_prepareVisualProviderIfNeeded"));
        ((void(*)(id,SEL))objc_msgSend)(bar,sel_registerName("setNeedsLayout"));
    }
}
static void stui_layout_provider(id self,SEL selector)
{
    stui_repair_bar(self);
    ((void(*)(id,SEL))stui_layout_original)(self,selector);
}
static bool stui_install_entry(const struct mach_header *h,intptr_t slide,uintptr_t offset,
    const uint32_t expected[4],IMP replacement,IMP *saved,const char *name)
{
    const uintptr_t entry=(uintptr_t)h+offset;
    if(!executable_image_range((const void *)h,slide,entry,16) || memcmp((const void *)entry,expected,16))return false;
    vm_address_t code=0;
    if(vm_allocate(mach_task_self(),&code,vm_page_size,VM_FLAGS_ANYWHERE)!=KERN_SUCCESS)return false;
    uint32_t back[]={0x58000050,0xd61f0200,(uint32_t)(entry+16),(uint32_t)((entry+16)>>32)};
    memcpy((void *)code,expected,16);memcpy((void *)(code+16),back,16);
    sys_icache_invalidate((void *)code,32);
    if(vm_protect(mach_task_self(),code,vm_page_size,FALSE,VM_PROT_READ|VM_PROT_EXECUTE)!=KERN_SUCCESS) {
        (void)vm_deallocate(mach_task_self(),code,vm_page_size);return false;
    }
    const uintptr_t target=(uintptr_t)replacement;
    uint32_t jump[]={0x58000050,0xd61f0200,(uint32_t)target,(uint32_t)(target>>32)};
    *saved=(IMP)code;
    if(install_compositor_code(h,slide,offset,expected,jump,16,name))return true;
    *saved=NULL;(void)vm_deallocate(mach_task_self(),code,vm_page_size);return false;
}
static void stui_phone_image(const struct mach_header *header,intptr_t slide)
{
    if(!phone_ui_process || stui_select_original || header->magic!=MH_MAGIC_64)return;
    static const uint8_t uuid[]={0x0e,0x5c,0x36,0xcc,0x88,0x46,0x36,0x1c,
                                0x85,0x8b,0xba,0x51,0x0f,0xf8,0xdc,0x0d};
    const struct mach_header_64 *h=(const void *)header;
    const struct load_command *lc=(const void *)(h+1);
    const char *end=(const char *)lc+h->sizeofcmds;bool matched=false;
    for(uint32_t i=0;i<h->ncmds;i++) {
        if((const char *)lc+sizeof(*lc)>end || lc->cmdsize<sizeof(*lc) || lc->cmdsize>(size_t)(end-(const char *)lc))return;
        if(lc->cmd==LC_UUID && lc->cmdsize==sizeof(struct uuid_command))
            matched=!memcmp(((const struct uuid_command *)lc)->uuid,uuid,16);
        lc=(const void *)((const char *)lc+lc->cmdsize);
    }
    if(!matched)return;
    Class cls=objc_lookUpClass("STUIStatusBar"),base=objc_lookUpClass("STUIStatusBarVisualProvider_DynamicSplitMedium");
    Method prepare=cls?class_getInstanceMethod(cls,sel_registerName("_prepareVisualProviderIfNeeded")):NULL;
    Method reset=cls?class_getInstanceMethod(cls,sel_registerName("resetVisualProvider")):NULL;
    Method set=cls?class_getInstanceMethod(cls,sel_registerName("_setVisualProviderClass:")):NULL;
    Ivar ivar=cls?class_getInstanceVariable(cls,"_visualProvider"):NULL;
    if(!base || !method_abi(prepare,"v",0,"") || !method_abi(reset,"v",0,"") ||
       !method_abi(set,"v",1,"#") || !ivar || ivar_getTypeEncoding(ivar)[0]!='@')return;
    Class provider=objc_allocateClassPair(base,"PongSystemStatus18Phone1080",0);
    if(!provider)return;
    if(!class_addMethod(object_getClass((id)provider),sel_registerName("referenceScreenScale"),(IMP)phone_reference_scale,"d16@0:8")) {
        objc_disposeClassPair(provider);return;
    }
    objc_registerClassPair(provider);stui_provider=provider;stui_provider_ivar=ivar;
    // The matched four-instruction prologues only save SP/registers.
    static const uint32_t select_code[]={0xd102c3ff,0x6d0523e9,0xa90667fa,0xa9075ff8};
    if(!stui_install_entry(header,slide,0x1d1d5ef68ULL-0x1d1d5d000ULL,select_code,
        (IMP)stui_selected_class,&stui_select_original,"SystemStatusUI provider selector"))return;
    static const uint32_t prepare_code[]={0xd102c3ff,0x6d0623e9,0xa9075ff8,0xa90857f6};
    if(stui_install_entry(header,slide,0x1d1d61eb8ULL-0x1d1d5d000ULL,prepare_code,
        (IMP)stui_prepare_provider,&stui_prepare_original,"SystemStatusUI provider prepare")) {
        method_setImplementation(prepare,(IMP)stui_prepare_provider);
        SEL layout=sel_registerName("layoutSubviews");Method lm=class_getInstanceMethod(cls,layout);
        if(method_abi(lm,"v",0,"")) {
            stui_layout_original=method_getImplementation(lm);
            if(!class_addMethod(cls,layout,(IMP)stui_layout_provider,method_getTypeEncoding(lm)))
                method_setImplementation(lm,(IMP)stui_layout_provider);
        }
    }
    Class fallback=objc_lookUpClass("STUIStatusBarFallbackItem");
    Method diagnostic=fallback?class_getInstanceMethod(fallback,sel_registerName("canEnableDisplayItem:fromData:")):NULL;
    if(method_abi(diagnostic,"Bc",2,"@@"))method_setImplementation(diagnostic,(IMP)phone_diagnostic_item_enabled);
}

static bool boot_option(const char *key)
{
    char args[2048];size_t size=sizeof(args)-1;
    if(sysctlbyname("kern.bootargs",args,&size,NULL,0))return false;
    args[size<sizeof(args)?size:sizeof(args)-1]='\0';
    const size_t length=strlen(key);
    for(const char *p=args;(p=strstr(p,key));p+=length) {
        if((p==args || p[-1]==' ' || p[-1]=='\t') &&
           (p[length]=='\0' || p[length]==' ' || p[length]=='\t'))return true;
    }
    return false;
}

/* 22H374 QuartzCore, UUID FAF7D7EA-18F0-3C2D-816C-AC56D16C40A3.
 * Its AccelServer::render_update calls renderer(), and on nullptr branches
 * to Server::render_update -> sw_renderer() -> the original SWContext.
 * Replace the method body before the constructor's direct call, as on iOS16.
 * This selects a real software backend; no Metal device is fabricated.
 * Match this image's UUID and entry instructions, never a 20G81 address.
 */
static void compositor_image(const struct mach_header *header,intptr_t slide)
{
    phone_gestalt_image(header,slide);
    phone_presentation();
    phone_selector_image(header,slide);
    stui_phone_image(header,slide);
    wallpaper_resource_policy();
    container_persona_policy();
    springboard_platform_policy();
    springboard_setup_policy();
    launchservices_directories();
    installd_directory();
    pluginkit_directory();
    if((!software_composition && !scanout_format) || ((!software_composition || renderer_installed) && format_installed) || header->magic!=MH_MAGIC_64)return;
    const struct mach_header_64 *h=(const struct mach_header_64 *)header;
    static const unsigned char expected_uuid[16]={
        0xfa,0xf7,0xd7,0xea,0x18,0xf0,0x3c,0x2d,0x81,0x6c,0xac,0x56,0xd1,0x6c,0x40,0xa3};
    const struct load_command *lc=(const void *)(h+1);
    const char *end=(const char *)lc+h->sizeofcmds;
    bool matched=false;
    for(uint32_t i=0;i<h->ncmds;i++) {
        if((const char *)lc+sizeof(*lc)>end || lc->cmdsize<sizeof(*lc) ||
           lc->cmdsize>(size_t)(end-(const char *)lc))return;
        if(lc->cmd==LC_UUID && lc->cmdsize==sizeof(struct uuid_command))
            matched=memcmp(((const struct uuid_command *)lc)->uuid,expected_uuid,16)==0;
        lc=(const void *)((const char *)lc+lc->cmdsize);
    }
    if(!matched)return;
    static const uint32_t expected[]={0xa9be4ff4,0xa9017bfd};
    static const uint32_t cpu_renderer[]={0xd2800000,0xd65f03c0}; /* return nullptr */
    if(software_composition && !renderer_installed)renderer_installed=install_compositor_code(header,slide,
        0x184393468ULL-0x184339000ULL,expected,cpu_renderer,sizeof(expected),"stock CPU compositor");
    /* The 18.x IOMFBDisplay format method branches to this implementation.
     * Match its own prologue, not the four-byte thunk or 20G81 vtable.
     * MiniIOMFB's CPU backend consumes uncompressed eight-bit BGRA only. */
    static const uint32_t format_expected[]={0xa9bd57f6,0xa9014ff4,0xa9027bfd};
    static const uint32_t bgra_format[]={0x528a4820,0x72a848e0,0xd65f03c0};
    if(!format_installed)format_installed=install_compositor_code(header,slide,
        0x184396060ULL-0x184339000ULL,format_expected,bgra_format,sizeof(format_expected),"software scanout BGRA");
}

static id singleton(const char *name)
{
    Class cls=objc_lookUpClass(name);SEL selector=sel_registerName("sharedInstance");
    return method_abi(cls?class_getClassMethod(cls,selector):NULL,"@",0,"")?
        ((id(*)(id,SEL))objc_msgSend)((id)cls,selector):nil;
}

/* Use the already-created controller. The 22H374 volumeControl getter is an
 * ivar load; preserve that ownership rather than creating a controller from a
 * boot timer. Resolve the object ivar afresh on the main queue, without
 * constructing it. */
static id volume18_control(void)
{
    Class cls=objc_lookUpClass("SpringBoard");
    SEL shared=sel_registerName("sharedApplication");
    if(!method_abi(cls?class_getClassMethod(cls,shared):NULL,"@",0,""))return nil;
    id app=((id(*)(id,SEL))objc_msgSend)((id)cls,shared);
    Class application=app?object_getClass(app):Nil;
    Ivar ivar=application?class_getInstanceVariable(application,"_volumeControl"):NULL;
    const char *type=ivar?ivar_getTypeEncoding(ivar):NULL;
    ptrdiff_t offset=ivar?ivar_getOffset(ivar):-1;
    if(!type || type[0]!='@' || offset<0 ||
       (size_t)offset+sizeof(id)>class_getInstanceSize(application))return nil;
    id control=object_getIvar(app,ivar);
    Class expected=objc_lookUpClass("SBVolumeControl");
    for(Class actual=control?object_getClass(control):Nil;actual;actual=class_getSuperclass(actual))
        if(actual==expected)return control;
    return nil;
}

static struct { uint64_t presses; bool held; } volume18_keys[2];
static bool volume18_ready;
static int volume18_ready_token=-1;
static const char *volume18_names[]={"dev.unixnu.pong.volume18.increase","dev.unixnu.pong.volume18.decrease"};
static bool volume18_step(unsigned index)
{
    id control=volume18_control();
    SEL action=sel_registerName(index?"decreaseVolume":"increaseVolume");
    if(!control || !method_abi(class_getInstanceMethod(object_getClass(control),action),"v",0,""))return false;
    ((void(*)(id,SEL))objc_msgSend)(control,action);
    return true;
}
static void volume18_repeat(void *context)
{
    uintptr_t value=(uintptr_t)context;unsigned index=value&1;
    if(!volume18_keys[index].held || volume18_keys[index].presses!=(value>>1))return;
    if(volume18_step(index))dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,100*NSEC_PER_MSEC),
        dispatch_get_main_queue(),context,volume18_repeat);
}
static void volume18_prepare(void *context)
{
    if(volume18_ready)return;
    unsigned attempt=(unsigned)(uintptr_t)context;
    id control=volume18_control();
    Class cls=control?object_getClass(control):Nil;
    if(!cls ||
       !method_abi(class_getInstanceMethod(cls,sel_registerName("increaseVolume")),"v",0,"") ||
       !method_abi(class_getInstanceMethod(cls,sel_registerName("decreaseVolume")),"v",0,"")) {
        if(attempt<30)dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),
            dispatch_get_main_queue(),(void *)(uintptr_t)(attempt+1),volume18_prepare);
        else fprintf(stderr,"Pong volume18: matching SBVolumeControl ABI unavailable; keep consumer HID\n");
        return;
    }
    for(unsigned i=0;i<2;i++) {
        int token=-1;
        if(notify_register_dispatch(volume18_names[i],&token,dispatch_get_main_queue(),^(int changed){
            uint64_t state=0;if(notify_get_state(changed,&state))return;
            uint64_t count=state>>1,old=volume18_keys[i].presses;
            volume18_keys[i].held=(state&1)!=0;volume18_keys[i].presses=count;
            // A restarted backboardd begins a new counter. Consume the new
            // click, never replay an old queue or synthesize a device report.
            uint64_t steps=count>=old?count-old:count;
            if(steps>8)steps=8;
            for(uint64_t n=0;n<steps;n++)(void)volume18_step(i);
            if(steps && volume18_keys[i].held)dispatch_after_f(
                dispatch_time(DISPATCH_TIME_NOW,400*NSEC_PER_MSEC),dispatch_get_main_queue(),
                (void *)(uintptr_t)((count<<1)|i),volume18_repeat);
            fprintf(stderr,"Pong volume18: native %s presses=%llu held=%u\n",i?"decrease":"increase",
                (unsigned long long)steps,volume18_keys[i].held);
        })!=NOTIFY_STATUS_OK)return;
    }
    if(notify_register_check("dev.unixnu.pong.volume18.ready",&volume18_ready_token)!=NOTIFY_STATUS_OK)return;
    volume18_ready=true;(void)notify_set_state(volume18_ready_token,1);
    fprintf(stderr,"Pong volume18: physical volume keys routed to 22H374 SBVolumeControl; power uses HID\n");
}

static void wake_home(void *unused)
{
    (void)unused;
    id screen=((id(*)(id,SEL))objc_msgSend)((id)objc_lookUpClass("UIScreen"),sel_registerName("mainScreen"));
    if(screen) {
        CGRect native=((CGRect(*)(id,SEL))objc_msgSend)(screen,sel_registerName("nativeBounds"));
        CGRect logical=((CGRect(*)(id,SEL))objc_msgSend)(screen,sel_registerName("bounds"));
        double scale=((double(*)(id,SEL))objc_msgSend)(screen,sel_registerName("scale"));
        double nativeScale=((double(*)(id,SEL))objc_msgSend)(screen,sel_registerName("nativeScale"));
        fprintf(stderr,"Pong UI18: screen native=%.0fx%.0f logical=%.2fx%.2f scale=%.2f nativeScale=%.2f\n",
            native.size.width,native.size.height,logical.size.width,logical.size.height,scale,nativeScale);
    }
    id backlight=singleton("SBBacklightController"),lock=singleton("SBLockScreenManager");
    SEL hold=sel_registerName("preventIdleSleepForNumberOfSeconds:");
    if(backlight && method_abi(class_getInstanceMethod(object_getClass(backlight),hold),"v",1,"f"))
        ((void(*)(id,SEL,float))objc_msgSend)(backlight,hold,300.0f);
    SEL wake=sel_registerName("turnOnScreenFullyWithBacklightSource:");
    if(backlight && method_abi(class_getInstanceMethod(object_getClass(backlight),wake),"v",1,"q"))
        ((void(*)(id,SEL,long long))objc_msgSend)(backlight,wake,0);
    SEL locked=sel_registerName("isUILocked"),unlock=sel_registerName("unlockUIFromSource:withOptions:");
    if(lock && method_abi(class_getInstanceMethod(object_getClass(lock),locked),"Bc",0,"") &&
       ((BOOL(*)(id,SEL))objc_msgSend)(lock,locked) &&
       method_abi(class_getInstanceMethod(object_getClass(lock),unlock),"Bc",2,"i@")) {
        BOOL accepted=((BOOL(*)(id,SEL,int,id))objc_msgSend)(lock,unlock,24,nil);
        fprintf(stderr,"Pong UI18: native home request accepted=%d\n",accepted);
    }
    fprintf(stderr,"Pong UI18: main queue wake completed backlight=%d lockscreen=%d\n",backlight!=nil,lock!=nil);
}

static bool own_directory(const char *path)
{
    if(mkdir(path,0700) && errno!=EEXIST)return false;
    struct stat st;
    return lstat(path,&st)==0 && S_ISDIR(st.st_mode) && st.st_uid==geteuid();
}

static id installation_library(id self,SEL selector)
{ (void)self;(void)selector;return (id)installd_library; }

static id plugin_state_container(id self,SEL selector)
{ (void)self;(void)selector;return (id)pluginkit_container; }

/* The services20 exception stack ends at 22H374's
 * -[PKDAnnotationStore containerURL] + 0xd8: its class-10 com.apple.pkd
 * query fails with error 52. Give this daemon's annotation store a real
 * directory on Data. Keep the stock database filename, serialization,
 * extension discovery and app container/security providers unchanged.
 * A system or mobile instance uses its own UID-owned home, never another
 * user's container. An unavailable directory retains the native failure.
 */
static void pluginkit_directory(void)
{
    if(!pluginkit_process || pluginkit_directory_attempted)return;
    const uid_t uid=geteuid();
    if(uid!=0 && uid!=501)return;
    Class cls=objc_lookUpClass("PKDAnnotationStore");
    Method method=cls?class_getInstanceMethod(cls,sel_registerName("containerURL")):NULL;
    if(!method_abi(method,"@",0,""))return;
    pluginkit_directory_attempted=true;
    const char *home=uid==0?"/private/var/root/Library":"/private/var/mobile/Library";
    char parent[128],path[160];
    snprintf(parent,sizeof(parent),"%s/Pong",home);
    snprintf(path,sizeof(path),"%s/PlugInKit",parent);
    if(!own_directory(home) || !own_directory(parent) || !own_directory(path)) {
        fprintf(stderr,"Pong PK18: annotation directory unavailable uid=%u errno=%d\n",uid,errno);
        return;
    }
    pluginkit_container=CFURLCreateFromFileSystemRepresentation(NULL,
        (const UInt8 *)path,strlen(path),true);
    if(!pluginkit_container)return;
    method_setImplementation(method,(IMP)plugin_state_container);
    fprintf(stderr,"Pong PK18: native annotations use %s uid=%u\n",path,uid);
}

/* 22H374 InstalledContentLibrary's daemonUserDataLibraryDirectory queries
 * class-10 com.apple.installd with a persona, and fatally terminates when
 * the query fails. Use the actual _installd home from this build's passwd
 * on our single-user Data. Its original currentUserDataDirectory and
 * currentUserCachesDirectory append MobileInstallation and Caches to this
 * NSURL. Keep all installation, signature and app-container operations.
 * Resolve the Objective-C method at runtime; never write private ivars or
 * reuse the iOS16 framework's addresses.
 */
static void installd_directory(void)
{
    if(!installd_process || installd_directory_installed || geteuid()!=33)return;
    Class cls=objc_lookUpClass("MIDaemonConfiguration");
    Method method=cls?class_getInstanceMethod(cls,
        sel_registerName("daemonUserDataLibraryDirectory")):NULL;
    if(!method_abi(method,"@",0,""))return;
    const char *dirs[]={"/private/var/installd","/private/var/installd/Library",
        "/private/var/installd/Library/MobileInstallation",
        "/private/var/installd/Library/Caches"};
    for(unsigned i=0;i<sizeof(dirs)/sizeof(dirs[0]);i++) {
        if(!own_directory(dirs[i])) {
            fprintf(stderr,"Pong MI18: daemon directory unavailable %s errno=%d\n",dirs[i],errno);
            return;
        }
    }
    installd_library=CFURLCreateFromFileSystemRepresentation(NULL,
        (const UInt8 *)dirs[1],strlen(dirs[1]),true);
    if(!installd_library)return;
    method_setImplementation(method,(IMP)installation_library);
    installd_directory_installed=true;
    fprintf(stderr,"Pong MI18: native installd uses writable _installd Library\n");
}

static id ls_directory(id self,SEL selector)
{
    (void)self;
    if(selector==sel_registerName("systemContainerBaseURL"))return (id)ls_base;
    if(selector==sel_registerName("systemContainerURL"))return (id)ls_system;
    if(selector==sel_registerName("systemGroupContainerURL"))return (id)ls_shared;
    return (id)ls_user;
}

/* 22H374 databaseContainerURL selects userContainerURL for an ordinary lsd.
 * LS_DATABASE_DIR covers only __LSDefaultsBaseSystemContainerURL, so it
 * cannot avoid userContainerURL's class-10 persona query -> abort path.
 * Apply the port's single-user storage policy to this server's own state,
 * preserving the original database/schema, app containers and XPC methods.
 * These are real directories, not successful replies from a fake MCM daemon.
 */
static void launchservices_directories(void)
{
    if(!launchservices_process || launchservices_installed || geteuid()!=501)return;
    Class cls=objc_lookUpClass("_LSDefaults");
    const char *names[]={"systemContainerBaseURL","systemContainerURL",
        "userContainerURL","systemGroupContainerURL"};
    Method methods[4];
    for(unsigned i=0;i<4;i++) {
        methods[i]=cls?class_getInstanceMethod(cls,sel_registerName(names[i])):NULL;
        if(!method_abi(methods[i],"@",0,""))return;
    }
    const char *dirs[]={
        "/private/var/mobile/Library/Pong",
        "/private/var/mobile/Library/Pong/LaunchServices",
        "/private/var/mobile/Library/Pong/LaunchServices/Library",
        "/private/var/mobile/Library/Pong/LaunchServices/Library/Caches",
        "/private/var/mobile/Library/Pong/LaunchServices/User",
        "/private/var/mobile/Library/Pong/LaunchServices/User/Library",
        "/private/var/mobile/Library/Pong/LaunchServices/User/Library/Caches",
        "/private/var/mobile/Library/Pong/LaunchServices/Shared",
        "/private/var/mobile/Library/Pong/LaunchServices/Shared/Library",
        "/private/var/mobile/Library/Pong/LaunchServices/Shared/Library/Caches"
    };
    for(unsigned i=0;i<sizeof(dirs)/sizeof(dirs[0]);i++) {
        if(!own_directory(dirs[i])) {
            fprintf(stderr,"Pong LS18: writable directory unavailable %s errno=%d\n",dirs[i],errno);
            return;
        }
    }
    const char *paths[]={dirs[1],dirs[3],dirs[6],dirs[9]};
    CFURLRef urls[4]={0};
    for(unsigned i=0;i<4;i++) {
        urls[i]=CFURLCreateFromFileSystemRepresentation(NULL,(const UInt8 *)paths[i],strlen(paths[i]),true);
        if(!urls[i]) {
            for(unsigned j=0;j<i;j++)CFRelease(urls[j]);
            return;
        }
    }
    ls_base=urls[0];ls_system=urls[1];ls_user=urls[2];ls_shared=urls[3];
    for(unsigned i=0;i<4;i++)method_setImplementation(methods[i],(IMP)ls_directory);
    launchservices_installed=true;
    fprintf(stderr,"Pong LS18: native lsd database uses writable single-user paths\n");
}

/* Keep the real fatal signal. Retain bounded frames because ReportCrash's
 * osanalytics backend is absent in this port; never resume after SIGABRT. */
static void lsd_abort(int number,siginfo_t *info,void *saved)
{
    (void)info;
    ucontext_t *context=saved;
    char line[160];uint64_t fp=0;
    if(context && context->uc_mcontext) {
        int n=snprintf(line,sizeof(line),"Pong LS18: fatal signal=%d pc=%llx lr=%llx\n",
            number,(unsigned long long)context->uc_mcontext->__ss.__pc,
            (unsigned long long)context->uc_mcontext->__ss.__lr);
        if(n>0)(void)write(2,line,(size_t)n<sizeof(line)?(size_t)n:sizeof(line)-1);
        fp=context->uc_mcontext->__ss.__fp;
    }
    for(unsigned i=0;i<12 && fp>=0x100000000ULL && !(fp&15);i++) {
        uint64_t frame[2];vm_size_t size=0;
        if(vm_read_overwrite(mach_task_self(),(vm_address_t)fp,sizeof(frame),
            (vm_address_t)frame,&size)!=KERN_SUCCESS || size!=sizeof(frame))break;
        int n=snprintf(line,sizeof(line),"Pong LS18: fatal frame[%u]=%llx\n",i,(unsigned long long)frame[1]);
        if(n>0)(void)write(2,line,(size_t)n<sizeof(line)?(size_t)n:sizeof(line)-1);
        if(frame[0]<=fp || frame[0]-fp>0x100000)break;
        fp=frame[0];
    }
    signal(number,SIG_DFL);raise(number);
}


/* Preserve the actual PosterBoard fatal signal and a small persistent stack.
 * The restored catalog and eligibility policy are the functional changes;
 * this retains a usable cause if the native 22H374 renderer still rejects it.
 */
static int poster_log=-1;
static bool poster_directory_installed;
static CFURLRef poster_data_root,poster_legacy_root;
static IMP poster_base_original,poster_store_original;
static IMP poster_database_original;
static IMP poster_resource_original,poster_resource_tree_original,poster_resource_update_original;

/* The Pong Data volume is ordinary unencrypted APFS. Its real inode class
 * is 0 and it does not advertise MNT_CPROTECT. It cannot persist a class-D
 * encryption policy. Adapt PosterBoard's expected attributes to that volume
 * capability instead of claiming a successful protection class transition.
 * Reachability, POSIX access, directory checks and SQLite validation still
 * run in the original 22H374 methods. Encrypted volumes are untouched. */
static bool poster_unprotected_data(id url)
{
    UInt8 path[1024];
    if(!url || !CFURLGetFileSystemRepresentation((CFURLRef)url,true,path,sizeof(path)))return false;
    const char *p=(const char *)path;
    const char *roots[]={"/private/var/mobile/Library/Application Support/PRBPosterExtensionDataStore",
        "/var/mobile/Library/Application Support/PRBPosterExtensionDataStore",
        "/private/var/mobile/Library/Pong/PosterBoard", "/var/mobile/Library/Pong/PosterBoard"};
    bool scoped=false;
    for(unsigned i=0;i<sizeof(roots)/sizeof(roots[0]);i++) {
        size_t n=strlen(roots[i]);
        if(!strncmp(p,roots[i],n) && (!p[n] || p[n]=='/'))scoped=true;
    }
    struct stat st;struct statfs fs;
    if(!scoped || lstat(p,&st) || S_ISLNK(st.st_mode) || st.st_uid!=geteuid() ||
       statfs(p,&fs) || strcmp(fs.f_fstypename,"apfs") ||
       (strcmp(fs.f_mntonname,"/private/var") && strcmp(fs.f_mntonname,"/var")))return false;
    // Darwin24 bsd/sys/mount.h: MNT_CPROTECT = 0x00000080.
    return !(fs.f_flags&0x00000080u);
}
static CFMutableDictionaryRef poster_volume_resources(id url,id resources)
{
    const CFStringRef *key=dlsym(RTLD_DEFAULT,"NSURLFileProtectionKey");
    if(!resources || CFGetTypeID((CFTypeRef)resources)!=CFDictionaryGetTypeID() ||
       !key || !*key || !CFDictionaryContainsKey((CFDictionaryRef)resources,*key) ||
       !poster_unprotected_data(url))return NULL;
    CFMutableDictionaryRef actual=CFDictionaryCreateMutableCopy(NULL,0,(CFDictionaryRef)resources);
    if(actual)CFDictionaryRemoveValue(actual,*key);
    static unsigned reported;
    if(actual && __atomic_fetch_add(&reported,1,__ATOMIC_RELAXED)<3)
        fprintf(stderr,"Pong Poster18: native resource validation uses unencrypted Data capabilities\n");
    return actual;
}
static BOOL poster_resource_values(id self,SEL selector,id values,id *error)
{
    CFMutableDictionaryRef actual=poster_volume_resources(self,values);
    BOOL result=((BOOL(*)(id,SEL,id,id *))poster_resource_original)(self,selector,actual?(id)actual:values,error);
    if(actual)CFRelease(actual);
    return result;
}
static BOOL poster_resource_tree(id self,SEL selector,id values,id *nonconforming,id *error)
{
    CFMutableDictionaryRef actual=poster_volume_resources(self,values);
    BOOL result=((BOOL(*)(id,SEL,id,id *,id *))poster_resource_tree_original)(self,selector,actual?(id)actual:values,nonconforming,error);
    if(actual)CFRelease(actual);
    return result;
}
static BOOL poster_resource_update(id self,SEL selector,id values,id *error)
{
    CFMutableDictionaryRef actual=poster_volume_resources(self,values);
    BOOL result=((BOOL(*)(id,SEL,id,id *))poster_resource_update_original)(self,selector,actual?(id)actual:values,error);
    if(actual)CFRelease(actual);
    return result;
}
static void poster_volume_policy(void)
{
    if(poster_resource_original)return;
    Class cls=objc_lookUpClass("NSURL");
    Method single=cls?class_getInstanceMethod(cls,sel_registerName("pbf_URLIsReachableAndConformToResourceValues:error:")):NULL;
    Method tree=cls?class_getInstanceMethod(cls,sel_registerName("pbf_recursivelyValidateContentsAreReachableAndConformToResourceValues:URLsNotConformingToAttributes:error:")):NULL;
    Method update=cls?class_getInstanceMethod(cls,sel_registerName("pbf_recursivelyUpdateResourceValues:error:")):NULL;
    if(!method_abi(single,"Bc",2,"@^") || !method_abi(tree,"Bc",3,"@^^") || !method_abi(update,"Bc",2,"@^"))return;
    poster_resource_original=method_setImplementation(single,(IMP)poster_resource_values);
    poster_resource_tree_original=method_setImplementation(tree,(IMP)poster_resource_tree);
    poster_resource_update_original=method_setImplementation(update,(IMP)poster_resource_update);
}

/* The restore can leave a current-version directory without its database.
 * 22H374's option 1 validates/opens an existing database and does not create
 * it. Its native option 8 path opens SQLite read/write/create and runs the
 * matching schema setup. Use that path only for an absent Poster database
 * below a real mobile-owned Data directory; preserve every existing store.
 */
static id poster_database(id self,SEL selector,id url,unsigned long long options,id *error)
{
    UInt8 path[1024];
    if(options==1 && url && CFURLGetFileSystemRepresentation((CFURLRef)url,true,path,sizeof(path))) {
        const char *p=(const char *)path;
        const char *native="/private/var/mobile/Library/Application Support/PRBPosterExtensionDataStore/";
        const char *port="/private/var/mobile/Library/Pong/PosterBoard/";
        const char *native_alias="/var/mobile/Library/Application Support/PRBPosterExtensionDataStore/";
        const char *port_alias="/var/mobile/Library/Pong/PosterBoard/";
        bool own_path=!strncmp(p,native,strlen(native)) || !strncmp(p,port,strlen(port)) ||
            !strncmp(p,native_alias,strlen(native_alias)) || !strncmp(p,port_alias,strlen(port_alias));
        struct stat st;
        if(own_path && lstat(p,&st)!=0 && errno==ENOENT) {
            char parent[sizeof(path)];strlcpy(parent,p,sizeof(parent));
            char *slash=strrchr(parent,'/');if(slash)*slash=0;
            if(slash && lstat(parent,&st)==0 && S_ISDIR(st.st_mode) && st.st_uid==geteuid()) {
                options|=8;
                fprintf(stderr,"Pong Poster18: initialize absent database with native schema %s\n",p);
            }
        }
    }
    id result=((id(*)(id,SEL,id,unsigned long long,id *))poster_database_original)(self,selector,url,options,error);
    if(!result) {
        id text=error && *error?((id(*)(id,SEL))objc_msgSend)(*error,sel_registerName("description")):nil;
        char detail[768]={0};if(text)CFStringGetCString((CFStringRef)text,detail,sizeof(detail),kCFStringEncodingUTF8);
        fprintf(stderr,"Pong Poster18: native database initialization failed: %s errno=%d\n",detail,errno);
    }
    return result;
}

/* The 22H374 scene constructor requires the real PBF data store. Its
 * _baseDataStoreURL: can resolve an unavailable shared/container home on
 * this single-user port. Use a UID-owned directory on the mounted Data,
 * keeping PBF's database, migration, role configuration and renderer intact. */
static void poster_directory_protection(id url)
{
    if(poster_unprotected_data(url))return;
    UInt8 path[1024];
    if(!url || !CFURLGetFileSystemRepresentation((CFURLRef)url,true,path,sizeof(path)))return;
    const char *p=(const char *)path;
    const char *roots[]={"/private/var/mobile/Library/Application Support/PRBPosterExtensionDataStore",
        "/var/mobile/Library/Application Support/PRBPosterExtensionDataStore",
        "/private/var/mobile/Library/Pong/PosterBoard/", "/var/mobile/Library/Pong/PosterBoard/"};
    bool owned=false;for(unsigned i=0;i<sizeof(roots)/sizeof(roots[0]);i++) {
        size_t n=strlen(roots[i]);
        if(!strncmp(p,roots[i],n) && (roots[i][n-1]=='/' || !p[n] || p[n]=='/'))owned=true;
    }
    if(!owned)return;
    const CFStringRef *key=dlsym(RTLD_DEFAULT,"NSURLFileProtectionKey");
    const CFStringRef *value=dlsym(RTLD_DEFAULT,"NSURLFileProtectionNone");
    if(!key || !value || !*key || !*value)return;
    // PFPosterPathURLResourceValues requires IsReadable=YES and protection
    // None. A writable directory with inherited class C still fails this
    // native check. Set the actual filesystem attribute; do not bypass it.
    // 22H374's minimum version is 59 but its current version is 61.
    // Resolve both from the loaded framework. Never create or rename a
    // version directory here: the real migrator owns that transition.
    Class store=objc_lookUpClass("PBFPosterExtensionDataStore");
    SEL minimum=sel_registerName("minimumDataStoreVersion"),current=sel_registerName("dataStoreVersion");
    if(!method_abi(store?class_getClassMethod(store,minimum):NULL,"Qq",0,"") ||
       !method_abi(class_getClassMethod(store,current),"Qq",0,""))return;
    unsigned long long first=((unsigned long long(*)(id,SEL))objc_msgSend)((id)store,minimum);
    unsigned long long last=((unsigned long long(*)(id,SEL))objc_msgSend)((id)store,current);
    if(first>last || last-first>16)return;
    const char *suffixes[]={"","/Extensions","/GalleryCache"};
    unsigned count=1+(unsigned)(last-first+1)*3;
    for(unsigned i=0;i<count;i++) {
        char target[1200];
        if(!i)strlcpy(target,p,sizeof(target));
        else snprintf(target,sizeof(target),"%s/%llu%s",p,first+(i-1)/3,suffixes[(i-1)%3]);
        struct stat st;if(lstat(target,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=geteuid())continue;
        CFURLRef directory=CFURLCreateFromFileSystemRepresentation(NULL,(const UInt8 *)target,strlen(target),true);
        if(!directory)continue;
        SEL get=sel_registerName("getResourceValue:forKey:error:"),set=sel_registerName("setResourceValue:forKey:error:");
        Class cls=object_getClass((id)directory);
        Method gm=class_getInstanceMethod(cls,get),sm=class_getInstanceMethod(cls,set);
        if(method_abi(gm,"Bc",3,"^@^") && method_abi(sm,"Bc",3,"@@^")) {
            id current=nil,error=nil;
            BOOL readable=((BOOL(*)(id,SEL,id *,id,id *))objc_msgSend)((id)directory,get,&current,(id)*key,&error);
            if(!readable || !current || !CFEqual((CFTypeRef)current,*value)) {
                error=nil;
                BOOL applied=((BOOL(*)(id,SEL,id,id,id *))objc_msgSend)((id)directory,set,(id)*value,(id)*key,&error);
                char detail[512]={0};id text=error?((id(*)(id,SEL))objc_msgSend)(error,sel_registerName("description")):nil;
                if(text)CFStringGetCString((CFStringRef)text,detail,sizeof(detail),kCFStringEncodingUTF8);
                static unsigned reported;
                if(!applied || __atomic_fetch_add(&reported,1,__ATOMIC_RELAXED)<8)
                    fprintf(stderr,"Pong Poster18: native directory protection %s %s %s\n",applied?"set":"failed",target,detail);
            }
        }
        CFRelease(directory);
    }
}
static void poster_base_directory_protection(id base)
{
    // PBF's base is Application Support (or the distinct shared/legacy
    // fallback), not PRBPosterExtensionDataStore itself. Derive the same
    // store root as its native NSURL category before touching attributes.
    UInt8 path[1024];
    if(!base || !CFURLGetFileSystemRepresentation((CFURLRef)base,true,path,sizeof(path)))return;
    const char *p=(const char *)path;
    const char *allowed[]={"/private/var/mobile/Library/Application Support",
        "/var/mobile/Library/Application Support",
        "/private/var/mobile/Library/Pong/PosterBoard/Shared",
        "/private/var/mobile/Library/Pong/PosterBoard/Legacy",
        "/var/mobile/Library/Pong/PosterBoard/Shared",
        "/var/mobile/Library/Pong/PosterBoard/Legacy"};
    bool owned=false;
    for(unsigned i=0;i<sizeof(allowed)/sizeof(allowed[0]);i++)
        if(!strcmp(p,allowed[i]))owned=true;
    if(!owned)return;
    struct stat st;
    if(lstat(p,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=geteuid() || access(p,W_OK|X_OK))return;
    CFURLRef root=CFURLCreateCopyAppendingPathComponent(NULL,(CFURLRef)base,
        CFSTR("PRBPosterExtensionDataStore"),true);
    if(root){poster_directory_protection((id)root);CFRelease(root);}
}
static id poster_base_url(id self,SEL selector,BOOL use_shared)
{
    id original=((id(*)(id,SEL,BOOL))poster_base_original)(self,selector,use_shared);
    UInt8 path[1024];
    if(original && CFURLGetFileSystemRepresentation((CFURLRef)original,true,path,sizeof(path)) &&
       access((const char *)path,W_OK|X_OK)==0) {
        poster_base_directory_protection(original);return original;
    }
    id selected=(id)(use_shared?poster_data_root:poster_legacy_root);
    poster_base_directory_protection(selected);return selected;
}
static id poster_store(id self,SEL selector)
{
    (void)selector;id error=nil;
    /* The stock dataStore accessor is a tail call to this exact method with
     * a null error pointer. Preserve the operation and retain its real error. */
    id store=((id(*)(id,SEL,id *))objc_msgSend)(self,sel_registerName("_dataStoreWithError:"),&error);
    if(!store) {
        id description=error?((id(*)(id,SEL))objc_msgSend)(error,sel_registerName("description")):nil;
        char detail[768]={0};
        if(description)CFStringGetCString((CFStringRef)description,detail,sizeof(detail),kCFStringEncodingUTF8);
        fprintf(stderr,"Pong Poster18: native DataStore unavailable: %s\n",detail);
        if(poster_log>=0){(void)write(poster_log,detail,strlen(detail));(void)write(poster_log,"\n",1);}
    }
    return store;
}
static void poster_image(const struct mach_header *header,intptr_t slide)
{
    (void)header;(void)slide;
    wallpaper_resource_policy();
    // MCM may arrive after PosterFoundation. Keep applying its real
    // single-user policy on later image loads, even once PBF is adapted.
    container_persona_policy();
    poster_volume_policy();
    if(poster_directory_installed)return;
    if(geteuid()!=501)return;
    Class cls=objc_lookUpClass("PBFPosterExtensionDataStoreXPCServiceGlue");
    Method base=cls?class_getInstanceMethod(cls,sel_registerName("_baseDataStoreURL:")):NULL;
    Method store=cls?class_getInstanceMethod(cls,sel_registerName("dataStore")):NULL;
    Method error=cls?class_getInstanceMethod(cls,sel_registerName("_dataStoreWithError:")):NULL;
    if(!method_abi(base,"@",1,"B") || !method_abi(store,"@",0,"") ||
       !method_abi(error,"@",1,"^"))return;
    const char *parent="/private/var/mobile/Library/Pong";
    const char *path="/private/var/mobile/Library/Pong/PosterBoard";
    const char *shared="/private/var/mobile/Library/Pong/PosterBoard/Shared";
    const char *legacy="/private/var/mobile/Library/Pong/PosterBoard/Legacy";
    if(!own_directory(parent) || !own_directory(path) ||
       !own_directory(shared) || !own_directory(legacy))return;
    // Native migration compares the old per-user and new shared locations;
    // do not alias those two distinct roots to the same directory.
    poster_data_root=CFURLCreateFromFileSystemRepresentation(NULL,(const UInt8 *)shared,strlen(shared),true);
    poster_legacy_root=CFURLCreateFromFileSystemRepresentation(NULL,(const UInt8 *)legacy,strlen(legacy),true);
    if(!poster_data_root || !poster_legacy_root)return;
    poster_base_original=method_setImplementation(base,(IMP)poster_base_url);
    poster_store_original=method_setImplementation(store,(IMP)poster_store);
    Class database=objc_lookUpClass("PBFPosterExtensionDataStoreSQLiteDatabase");
    Method initializer=database?class_getInstanceMethod(database,sel_registerName("initWithURL:options:error:")):NULL;
    if(method_abi(initializer,"@",3,"@Q^")) {
        poster_database_original=method_setImplementation(initializer,(IMP)poster_database);
    }
    poster_directory_installed=true;
    container_persona_policy();
    fprintf(stderr,"Pong Poster18: native DataStore fallback uses writable %s\n",path);
}
static void poster_fatal(int number,siginfo_t *info,void *saved)
{
    (void)info;ucontext_t *context=saved;
    char line[176];uint64_t fp=0;
    if(context && context->uc_mcontext) {
        int n=snprintf(line,sizeof(line),"Pong Poster18: fatal=%d pc=%llx lr=%llx\n",number,
            (unsigned long long)context->uc_mcontext->__ss.__pc,
            (unsigned long long)context->uc_mcontext->__ss.__lr);
        if(n>0 && n<(int)sizeof(line)){(void)write(2,line,(size_t)n);if(poster_log>=0)(void)write(poster_log,line,(size_t)n);}
        fp=context->uc_mcontext->__ss.__fp;
    }
    for(unsigned i=0;i<12 && fp>=0x100000000ULL && !(fp&15);i++) {
        uint64_t frame[2];vm_size_t bytes=0;
        if(vm_read_overwrite(mach_task_self(),(vm_address_t)fp,sizeof(frame),(vm_address_t)frame,&bytes)!=KERN_SUCCESS || bytes!=sizeof(frame))break;
        int n=snprintf(line,sizeof(line),"Pong Poster18: frame[%u]=%llx\n",i,(unsigned long long)frame[1]);
        if(n>0 && n<(int)sizeof(line)){(void)write(2,line,(size_t)n);if(poster_log>=0)(void)write(poster_log,line,(size_t)n);}
        if(frame[0]<=fp || frame[0]-fp>0x100000)break;
        fp=frame[0];
    }
    if(poster_log>=0)(void)fsync(poster_log);
    signal(number,SIG_DFL);raise(number);
}
static void poster_fatal_capture(void)
{
    poster_log=open("/private/var/mobile/Library/Logs/Pong-Poster18.log",O_WRONLY|O_CREAT|O_APPEND,0644);
    Dl_info image={0};
    if(dladdr((const void *)objc_msgSend,&image)) {
        char line[192];int n=snprintf(line,sizeof(line),"Pong Poster18: pid=%d libobjc base=%p\n",getpid(),image.dli_fbase);
        if(n>0 && n<(int)sizeof(line) && poster_log>=0)(void)write(poster_log,line,(size_t)n);
    }
    struct sigaction action={0};action.sa_sigaction=poster_fatal;
    action.sa_flags=SA_SIGINFO|SA_RESETHAND;sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTRAP,&action,NULL);(void)sigaction(SIGABRT,&action,NULL);
}

static id process_root(id self,SEL selector)
{ (void)self;(void)selector;return (id)container_root; }
static id process_library(id self,SEL selector)
{ (void)self;(void)selector;return (id)container_library; }

/* The 22H374 backboardd trace enters this provider with a nil system
 * container Library string. Supply the port's actual, process-owned Data
 * directories. Its original cachesPath appends Caches to libraryPath.
 * This is confined to backboardd, with no global NSURL/exception overrides
 * and no changes to launchd's native system-proxy personas.
 */
static void backboard_container(void)
{
    Class cls=objc_lookUpClass("BSSystemContainerForCurrentProcessPathProvider");
    SEL root_sel=sel_registerName("defaultPath"),lib_sel=sel_registerName("libraryPath");
    Method root=cls?class_getInstanceMethod(cls,root_sel):NULL;
    Method lib=cls?class_getInstanceMethod(cls,lib_sel):NULL;
    if(!root || !lib || method_getNumberOfArguments(root)!=2 || method_getNumberOfArguments(lib)!=2)return;
    char *root_type=method_copyReturnType(root),*lib_type=method_copyReturnType(lib);
    bool valid=root_type && lib_type && root_type[0]=='@' && lib_type[0]=='@';
    free(root_type);free(lib_type);
    if(!valid)return;
    const char *base="/private/var/mobile/Library/Pong";
    const char *path="/private/var/mobile/Library/Pong/backboardd";
    const char *library="/private/var/mobile/Library/Pong/backboardd/Library";
    if(!own_directory(base) || !own_directory(path) || !own_directory(library) ||
       !own_directory("/private/var/mobile/Library/Pong/backboardd/Library/Caches")) {
        fprintf(stderr,"Pong UI18: backboardd Data directories unavailable errno=%d\n",errno);return;
    }
    container_root=CFURLCreateFromFileSystemRepresentation(NULL,(const UInt8 *)path,strlen(path),true);
    container_library=CFURLCreateFromFileSystemRepresentation(NULL,(const UInt8 *)library,strlen(library),true);
    if(!container_root || !container_library)return;
    class_replaceMethod(cls,root_sel,(IMP)process_root,method_getTypeEncoding(root));
    class_replaceMethod(cls,lib_sel,(IMP)process_library,method_getTypeEncoding(lib));
    fprintf(stderr,"Pong UI18: backboardd container uses writable %s\n",path);
}

__attribute__((constructor)) static void pong_ui18_start(void)
{
    const char *name=getprogname();
    if(!name)return;
    phone_ui_process=!strcmp(name,"Preferences") || !strcmp(name,"SpringBoard");
    // Install the capability answer before status-provider +initialize can
    // cache it during the regular all-images callback below.
    if(phone_ui_process)for(uint32_t i=0,n=_dyld_image_count();i<n;i++)
        phone_gestalt_image(_dyld_get_image_header(i),_dyld_get_image_vmaddr_slide(i));
    if(phone_ui_process) {
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,15*NSEC_PER_SEC),dispatch_get_main_queue(),NULL,phone_repair_existing);
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,35*NSEC_PER_SEC),dispatch_get_main_queue(),NULL,phone_repair_existing);
    }
    /* services42 enabled unsupported restore variants globally. Keep the
     * known phone catalog wrappers and leave native defaults unchanged. */
    wallpaper_paths_process=phone_ui_process || !strcmp(name,"PosterBoard") ||
        !strcmp(name,"CollectionsPoster") || !strcmp(name,"LegacyPoster") ||
        !strcmp(name,"WallpaperSettingsIntents");
    wallpaper_process=boot_option("pong_wallpaper_catalog_override=1") && wallpaper_paths_process;
    (void)dup2(2,1);setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
    fprintf(stderr,"Pong iPadOS18 adapter: %s (22H374)\n",name);
    if(strcmp(name,"Preferences")==0)pong_settings18_start();
    // Keep SpringBoard/compositor adapters out of the poster renderer.
    // Only its native storage path and single-user identity are adapted.
    if(!strcmp(name,"PosterBoard")){
        poster_fatal_capture();_dyld_register_func_for_add_image(poster_image);
        if(boot_option("pong_poster_snapshot=1"))pong_boot_snapshot_start(name);
        return;
    }
    if(strcmp(name,"runningboardd")==0) {
        _dyld_register_func_for_add_image(poster_jetsam_image);
        if(boot_option("pong_boot_snapshot=1"))pong_boot_snapshot_start(name);
        return;
    }
    installd_process=strcmp(name,"installd")==0;
    pluginkit_process=strcmp(name,"pkd")==0;
    if(strcmp(name,"lsd")==0) {
        launchservices_process=true;
        struct sigaction action={0};
        action.sa_sigaction=lsd_abort;action.sa_flags=SA_SIGINFO|SA_RESETHAND;
        sigemptyset(&action.sa_mask);
        (void)sigaction(SIGABRT,&action,NULL);
    }
    if(strcmp(name,"usermanagerd")==0) {
        initialize_persona_manifest();return;
    }
    if(strcmp(name,"SpringBoard")==0) {
        springboard_process=true;
        // The consumer reports are accepted but physical volume remains
        // unresponsive on this imported iPad board. Route those transitions
        // to the existing 22H374 controller after checking its actual ABI.
        // Power stays on HID; backboardd sends each volume transition once.
        if(!boot_option("pong_volume_direct=0"))
            dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC),dispatch_get_main_queue(),NULL,volume18_prepare);
        skip_setup=boot_option("pong_skip_setup=1");
        CFPreferencesSetAppValue(CFSTR("SBDisableAutoDim"),kCFBooleanTrue,kCFPreferencesCurrentApplication);
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_SEC),dispatch_get_main_queue(),NULL,wake_home);
    }
    if(strcmp(name,"backboardd")==0) {
        software_composition=boot_option("pong_ui_software=1");
        scanout_format=true;
        backboard_container();
        pong_touch_start();
    }
    _dyld_register_func_for_add_image(compositor_image);
    if(boot_option("pong_boot_snapshot=1"))pong_boot_snapshot_start(name);
}
