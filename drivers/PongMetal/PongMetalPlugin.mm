/* SPDX-License-Identifier: MIT */
/*20G81 stock Metal discovery over Pong's real IOAcceleratorES provider.
 * The OS loads this bundle and registers the returned native device.
 * No manual catalog mutation or iOS17 QuartzCore offsets.
 */
#import <Foundation/Foundation.h>
#import <CoreFoundation/CoreFoundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <initializer_list>
#include <mach/mach.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "PongMetalAbi.h"
#include "NativeBridgeMetalFrontend.h"
#include "NativeAdrenoABI.h"

@interface _MTLDevice : NSObject
@end
@interface PongAdrenoMetalDevice : _MTLDevice
- (id)initWithAcceleratorPort:(unsigned)port;
@end

namespace {
using Open=kern_return_t(*)(unsigned,mach_port_t,unsigned,unsigned *);
using Close=kern_return_t(*)(unsigned);
using Call=kern_return_t(*)(unsigned,unsigned,const void *,size_t,void *,size_t *);
using Property=CFTypeRef(*)(unsigned,CFStringRef,CFAllocatorRef,unsigned);
using Registry=kern_return_t(*)(unsigned,uint64_t *);
static id device;
static void *iokit;
}
static void report(const char *phase,int status)
{
    char line[256];snprintf(line,sizeof(line),"Pong Metal: %s status=0x%x\n",phase,status);
    (void)write(2,line,strlen(line));
    size_t count=CTL_MAXNAME;int mib[CTL_MAXNAME];
    if(!sysctlnametomib("kern.pong_diagnostic",mib,&count))
        (void)sysctl(mib,(u_int)count,NULL,NULL,line,strlen(line));
}
#include "PongMetalScanout.h"
#include "PongQuartzCorePolicy.h"
namespace {
static void missing(const char *type,const char *selector)
{
    static unsigned count;
    if(__atomic_fetch_add(&count,1,__ATOMIC_RELAXED)>=12)return;
    char text[192];snprintf(text,sizeof(text),"missing %.70s %.90s",type,selector);
    report(text,-1);
}
static bool propertyTrue(Property read,unsigned port,CFStringRef key)
{
    CFTypeRef value=read(port,key,kCFAllocatorDefault,0);
    bool yes=value && CFGetTypeID(value)==CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)value);
    if(value)CFRelease(value);
    return yes;
}
static bool query(Call call,unsigned connection,unsigned selector,void *output,size_t bytes)
{
    size_t size=bytes;
    kern_return_t result=call(connection,selector,NULL,0,output,&size);
    if(result || size!=bytes){report("native ABI query refused",result?result:-1);return false;}
    return true;
}
static bool backend(unsigned port,uint64_t *registryID)
{
    char version[64]={};size_t length=sizeof(version);
    if(sysctlbyname("kern.osversion",version,&length,NULL,0) || strcmp(version,"20G81")) {
        report("guest build is not20G81",-1);return false;
    }
    if(!PongMetalDeviceAbiAvailable(objc_getClass("_MTLDevice"))) {
        report("20G81 _MTLDevice ABI unavailable",-1);return false;
    }
    if(!iokit)iokit=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);
    if(!iokit)return false;
    auto open=(Open)dlsym(iokit,"IOServiceOpen");auto close=(Close)dlsym(iokit,"IOServiceClose");
    auto call=(Call)dlsym(iokit,"IOConnectCallStructMethod");
    auto read=(Property)dlsym(iokit,"IORegistryEntryCreateCFProperty");
    auto registry=(Registry)dlsym(iokit,"IORegistryEntryGetRegistryEntryID");
    if(!open || !close || !call || !read || !registry)return false;
    /* CPReady is published only after real completed init fences. Enabled
     * requests this entry; it does not claim runtime/renderer qualification.
     * Runtime ABI and the hardware below must independently agree.
     */
    if(!propertyTrue(read,port,CFSTR("PongAdrenoCPReady")) ||
       !propertyTrue(read,port,CFSTR("PongMetal20G81Enabled"))) {
        report("CP unavailable or Metal disabled; CPU display remains available",-1);return false;
    }
    unsigned connection=0;kern_return_t result=open(port,mach_task_self(),0x4147,&connection);
    if(result){report("native connection refused",result);return false;}
    NativeAdrenoAbiParams params={};NativeAdrenoAbiDeviceInfo info={};
    NativeAdrenoAbiStatus status={};NativeAdrenoAbiTimestamp time={};
    bool ready=query(call,connection,NativeAdrenoMethodQuery,&params,sizeof(params)) &&
        query(call,connection,NativeAdrenoMethodGetDeviceInfo,&info,sizeof(info)) &&
        query(call,connection,NativeAdrenoMethodGetStatus,&status,sizeof(status)) &&
        query(call,connection,NativeAdrenoMethodGetTimestamp,&time,sizeof(time));
    result=close(connection);
    if(result){report("native connection close failed",result);return false;}
    if(!ready || params.abi_version!=NativeAdrenoAbiVersion || params.input_bits<31 ||
       params.input_bits>48 || params.output_bits<32 || params.output_bits>48 ||
       info.chip_id!=0x07030002 || info.gmem_bytes!=0x200000 ||
       !info.va_size || (info.va_start&4095) || (info.va_size&4095) ||
       info.va_start>UINT64_MAX-info.va_size || status.faults || !time.ticks) {
        report("Pong hardware/ABI prerequisites failed",-1);return false;
    }
    return registry(port,registryID)==KERN_SUCCESS && *registryID;
}
static id openDevice(unsigned port)
{
    uint64_t registryID=0;if(!backend(port,&registryID))return nil;
    // DYLD injection does not load the executable through NSBundle. Locate
    // its sibling with dladdr as upstream does, independent of bundle lookup.
    Dl_info location={};
    if(!dladdr((const void *)&openDevice,&location) || !location.dli_fname)return nil;
    NSString *loader=[[@(location.dli_fname) stringByDeletingLastPathComponent]
        stringByAppendingPathComponent:@"libvulkan_freedreno.dylib"];
    if(access(loader.fileSystemRepresentation,R_OK)){report("Turnip library absent",-1);return nil;}
    NSString *process=[NSProcessInfo processInfo].processName;
    NSMutableArray<NSString *> *candidates=[NSMutableArray new];
    // Keep separate process caches as in Sheng's native Metal clients.
    if(const struct passwd *user=getpwuid(getuid());user && user->pw_dir && user->pw_dir[0])
        [candidates addObject:[@(user->pw_dir) stringByAppendingPathComponent:
            [NSString stringWithFormat:@"Library/Caches/com.apple.%@/PongMetal",process]]];
    for(int key:{_CS_DARWIN_USER_CACHE_DIR,_CS_DARWIN_USER_TEMP_DIR}) {
        char path[1024]={};
        if(confstr(key,path,sizeof(path)) && path[0])
            [candidates addObject:[@(path) stringByAppendingPathComponent:@"PongMetal"]];
    }
    NSString *base=[NSSearchPathForDirectoriesInDomains(NSCachesDirectory,NSUserDomainMask,YES) firstObject];
    if(base)[candidates addObject:[[base stringByAppendingPathComponent:@"PongMetal"] stringByAppendingPathComponent:process]];
    // A7xx sysmem/no concurrent binning until Pong's BV/BR scheduling is ready.
    // Plain IOSurface rows match Pong's current framebuffer/scanout contract.
    setenv("TU_DEBUG","sysmem,nocb",0);
    setenv("PONG_IOSURFACE_PADDING","1",1);
    // Pong implements NativeAdreno directly, not Sheng's AGX selector tunnel.
    setenv("TU_DARWIN_SERVICE","NativeAdrenoGpuPublished",1);
    setenv("TU_DARWIN_OPEN_TYPE","0x4147",1);
    setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION","1",1);
    CFPreferencesSetAppValue(CFSTR("CAEnableDeepFramebuffer"),kCFBooleanFalse,kCFPreferencesCurrentApplication);
    CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"),kCFBooleanFalse,kCFPreferencesCurrentApplication);
    CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"),kCFBooleanFalse,kCFPreferencesCurrentApplication);
    id result=nil;
    for(NSString *cache in candidates) {
        NSError *error=nil;
        if(![[NSFileManager defaultManager] createDirectoryAtPath:cache withIntermediateDirectories:YES attributes:nil error:&error] ||
           access(cache.fileSystemRepresentation,W_OK))continue;
        @try {
            result=native_bridge_metal_open(loader.fileSystemRepresentation,cache.fileSystemRepresentation,registryID,missing);
        } @catch(NSException *exception) {
            NSLog(@"Pong Metal: device initialization failed: %@",exception);return nil;
        }
        if(result)break;
    }
    if(result) {
        const int policy=PongApplyQuartzCorePolicy20G81();
        if(policy)report("20G81 decoded-image policy unavailable",policy);
    }
    report(result?"real Vulkan-backed device created":"runtime refused device",result?0:-1);
    return result;
}

static bool selectedClient()
{
    const char *process=getprogname();
    if(!process || !*process)return false;
    // Sheng uses publish/clients seed files to enable real device discovery.
    // The root-owned client seed is the single publication policy. A second
    // compiled list used to silently reject the wallpaper processes.
    // The extension view is the same root-owned seed under a system-readable
    // path. Sandboxed poster/widget clients cannot necessarily read /var/pong.
    for(const char *directory:{"/System/Library/Extensions","/private/var/pong"}) {
        char marker[256],clients[256];
        snprintf(marker,sizeof(marker),"%s/PongAdrenoMetal.publish",directory);
        snprintf(clients,sizeof(clients),"%s/PongAdrenoMetal.clients",directory);
        struct stat st={};
        if(lstat(marker,&st) || !S_ISREG(st.st_mode) || st.st_uid || (st.st_mode&022))continue;
        FILE *file=fopen(marker,"r");if(!file)continue;
        const bool publish=fgetc(file)=='1';fclose(file);if(!publish)return false;
        if(lstat(clients,&st) || !S_ISREG(st.st_mode) || st.st_uid || (st.st_mode&022))continue;
        file=fopen(clients,"r");if(!file)continue;
        char line[80];bool selected=false;
        while(fgets(line,sizeof(line),file)) {
            line[strcspn(line,"\r\n")]=0;
            if(!strcmp(line,process)){selected=true;break;}
        }
        fclose(file);return selected;
    }
    return false;
}
static void startPresentation(id candidate,unsigned port)
{
    if(strcmp(getprogname(),"backboardd"))return;
    using Ref=kern_return_t(*)(unsigned);
    auto retain=(Ref)dlsym(iokit,"IOObjectRetain"),release=(Ref)dlsym(iokit,"IOObjectRelease");
    if(!retain || !release || retain(port))return;
    // Leave the stock catalog's construction lock before creating the
    // presenter. Its actual client retains the device and scanout backing.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE,0),^{
        pongscanout::start(iokit,port,(id<MTLDevice>)candidate);release(port);
    });
    // Bounded observations of real application work, not synthetic renders.
    // RAM cache journals disappear on REC; retain these few status lines.
    for(unsigned seconds:{5u,20u,60u})
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,int64_t(seconds)*NSEC_PER_SEC),
            dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
                char status[240]={};native_bridge_metal_status(status,sizeof(status));
                report(status,0);
            });
}
}
@implementation PongAdrenoMetalDevice
- (id)initWithAcceleratorPort:(unsigned)port
{
    self=[super init];if(!self || !port || !selectedClient())return nil;
    @synchronized([PongAdrenoMetalDevice class]) {
        if(!device) {
            device=openDevice(port);
            if(device){report("real device returned to stock20G81 discovery",0);startPresentation(device,port);}
        }
        return device;
    }
}
@end

