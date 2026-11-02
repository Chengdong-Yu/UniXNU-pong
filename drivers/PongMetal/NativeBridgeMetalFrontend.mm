// SPDX-License-Identifier: MIT
// Native Metal API frontend over the bridge (bridge/NativeMetalObjects.h).
// Pong fork of Sheng's frontend over the native A730 kernel transport.
// The device is a dynamically allocated subclass of the target's own _MTLDevice that
// only adds this file's implementations; every other object is an own class answering the Metal protocol methods it implements.
// Every result comes from the runtime and the GPU: a refused call returns nil/NO with the runtime's reason, an encoder call the runtime
// refuses fails its command buffer (status Error, handlers run) instead of being dropped. A selector this frontend does not implement is
// reported through the missing-selector hook before the usual exception, so a hardware run names the gap.
// Shared events work within this process (no cross-process handle yet).
// Argument buffers, indirect commands and drawables/presentation remain incomplete.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <map>
#include <tuple>
#include <pthread.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <mach/mach.h>
#include "bridge/NativeMetalObjects.h"
#include "NativeBridgeMetalFrontend.h"
#include "SharedShaderCache.h"
#include "QueuePriority.h"
#include "NativeGpuTrace.h"
#include <CommonCrypto/CommonDigest.h>
#include <sys/sysctl.h>
#include "PongMetalAbi.h"

extern "C" void sys_dcache_flush(void *start, size_t length);

static NmbDevice *gDevice;
static id gDeviceObject;
static bool (*gFrameTransport)(uint32_t,const void *,uint32_t);
static std::atomic<uint64_t> gFrameNumber{0},gFrameLost{0};
static bool frameTraceOn(){static const bool on=getenv("SHENG_GPU_TRACE")!=nullptr;return on;}
template<typename... T> static void frameEvent(uint32_t event,T... args) {
 if(!frameTraceOn() || !gFrameTransport)return;
 NativeGpuTraceRecord rec{};rec.event=event;const uint64_t values[]={uint64_t(args)...};
 static_assert(sizeof...(args)<=12,"frame event fields");
 memcpy(rec.value,values,sizeof(values));
 if(!gFrameTransport(NativeGpuTraceMethod,&rec,sizeof(rec)))++gFrameLost;
 else if(uint64_t lost=gFrameLost.exchange(0)) {
  NativeGpuTraceRecord dropped{};dropped.event=41;dropped.value[0]=lost;
  if(!gFrameTransport(NativeGpuTraceMethod,&dropped,sizeof(dropped)))gFrameLost+=lost;
 }
}
static void frameRuntime(uint32_t event,uint64_t id,uint64_t a,uint64_t b){frameEvent(event,id,a,b);}
static void frameText(uint32_t event,const char *text) {
 if(!frameTraceOn() || !gFrameTransport || !text)return;
 NativeGpuTraceRecord rec{};rec.event=event;strlcpy((char *)rec.value,text,sizeof(rec.value));
 (void)gFrameTransport(NativeGpuTraceMethod,&rec,sizeof(rec));
}



static uint64_t gRegistryID;
static NSString *gName;
static std::string gLoaderPath;
static std::atomic<uint32_t> gPriorityForeground{1},gPriorityBackground{3};
static NBQueuePriorities gQueuePriorities;
static std::mutex gPriorityLock;
static bool forwardGpuPriority(uint32_t foreground,uint32_t background) {
 using SetPriority=bool(*)(uint32_t,uint32_t);
 static std::atomic<SetPriority> setter{nullptr};
 SetPriority fn=setter.load();
 if(!fn && !gLoaderPath.empty()) {
  void *library=dlopen(gLoaderPath.c_str(),RTLD_NOW|RTLD_NOLOAD);
  if(library){fn=(SetPriority)dlsym(library,"tu_darwin_set_priority");if(fn)setter.store(fn);dlclose(library);}
 }
 if(!fn || !fn(foreground,background))return false;
 gPriorityForeground.store(foreground);gPriorityBackground.store(background);return true;
}
static void (*gMissing)(const char *, const char *);

// What this device has been asked and refused, for the plugin's status line (native_bridge_metal_status): commits, render/compute pipelines made,
// refusals, and the last two refusals' texts. Counters only; nothing here changes a result.
static std::atomic<uint64_t> gCommits,gCommitFailures,gPipelines,gProblems,gCompleted;
// What the render server actually asks of the GPU (int-141: 59 commits in 200 s, and a desktop that behaves like CPU compositing): render passes,
// draws, the pixels of their first colour attachment, compute dispatches, blits and the largest pass (pixels).
static std::atomic<uint64_t> gPasses,gDraws,gPassPixels,gDispatches,gBlits,gBiggestPass,gFullScreenPasses;
// Where the time goes on the CPU side of the bridge: pipeline, library and function creation (count, total and slowest, in microseconds), and the
// last commits with what each held and how long the GPU took (commit to completion).
static uint64_t nowUs() {return clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1000;}
struct Timed {std::atomic<uint64_t> count{0},total{0},slowest{0};
 void add(uint64_t began){const uint64_t took=nowUs()-began;count++;total+=took;uint64_t seen=slowest.load();while(took>seen && !slowest.compare_exchange_weak(seen,took)){}}};
static Timed gTimePipeline,gTimeLibrary,gTimeFunction,gTimeEncodeToCommit;
static std::atomic<uint64_t> gHistFrameGpu[10],gHistOtherGpu[10],gHistFrameEncode[10];
static void histAdd(std::atomic<uint64_t> *bins,uint64_t us) {unsigned b=0;const uint64_t ms=us/1000;while(b<9 && ms>=(1ull<<b))++b;bins[b]++;}
extern "C" { __attribute__((weak)) volatile uint32_t native_prime_functions_resumed; }
static Timed gTimeWaitScheduled,gTimeWaitCompleted;
static Timed gTimeResourceWait;
static std::atomic<uint64_t> gResourceChecks,gResourceSkipped;
static Timed gTimeSurfaceFlush;
static std::atomic<uint64_t> gSurfaceFlushBytes,gSurfaceFlushDuplicates;
struct EncodedWork {uint32_t passes,draws,dispatches,blits;uint64_t pixels;};
struct CommitRecord {uint64_t committedUs=0,callbackUs=0,ageUs=0;int64_t scheduledUs=-1;uint32_t index=0;EncodedWork work={};};
static CommitRecord gCommitRing[32];
static std::mutex gCommitLock;
// Upstream shader priming uses actual recent commits to yield to animation.
extern "C" unsigned native_bridge_metal_recent_commits(uint64_t windowUs) {
 const uint64_t now=nowUs();unsigned count=0;
 std::lock_guard<std::mutex> lock(gCommitLock);
 for(const CommitRecord &r:gCommitRing)if(r.index && r.committedUs<=now && now-r.committedUs<=windowUs)++count;
 return count;
}
static std::mutex gProblemLock;
static char gLastProblems[2][200];
static void noteProblem(NSString *what) {
 gProblems++;
 std::lock_guard<std::mutex> lock(gProblemLock);
 memcpy(gLastProblems[1],gLastProblems[0],sizeof gLastProblems[0]);
 strlcpy(gLastProblems[0],what.UTF8String?:"?",sizeof gLastProblems[0]);
}
#define NBLOG(...) do{NSString *nbText=[NSString stringWithFormat:__VA_ARGS__];NSLog(@"%@",nbText);noteProblem(nbText);}while(0)
// The pixel formats textures were asked for (first eight distinct), with how many were refused: native_bridge_metal_formats.
static uint32_t gFormats[8];static unsigned gFormatCount;static std::atomic<uint64_t> gTextureRefusals;
// Textures over IOSurfaces: made, refused, and how many of the requests named a planar surface.
static std::atomic<uint64_t> gSurfaceTextures,gSurfaceRefusals,gSurfacePlanar;
// The last refused surface texture: the check that refused it (1 plane, 2 base, 3 plane range, 4 import, 5 texture), the surface's format,
// planes, requested plane, plane offset, plane row bytes and allocation, and the descriptor's size and pixel format.
struct SurfaceRefusal {uint32_t step,format,planes,plane,pixelFormat;uint64_t offset,row,allocation,width,height;};
static SurfaceRefusal gSurfaceLastRefusal;
static void refuseSurface(uint32_t step,IOSurfaceRef surface,MTLTextureDescriptor *d,NSUInteger plane,uint64_t offset,uint64_t row) {
 gSurfaceRefusals++;
 SurfaceRefusal r={step,surface?IOSurfaceGetPixelFormat(surface):0,surface?(uint32_t)IOSurfaceGetPlaneCount(surface):0,(uint32_t)plane,
                   d?(uint32_t)d.pixelFormat:0,offset,row,surface?IOSurfaceGetAllocSize(surface):0,d?d.width:0,d?d.height:0};
 std::lock_guard<std::mutex> lock(gProblemLock);gSurfaceLastRefusal=r;
}
static void noteFormat(uint32_t format,bool made) {
 std::lock_guard<std::mutex> lock(gProblemLock);
 if(!made)gTextureRefusals++;
 for(unsigned i=0;i<gFormatCount;i++)if(gFormats[i]==format)return;
 if(gFormatCount<8)gFormats[gFormatCount++]=format;
}
static NSError *nbError(NSString *why) {
 return [NSError errorWithDomain:@"org.blu-sharky.native-metal" code:-1 userInfo:@{NSLocalizedDescriptionKey:why?:@"refused"}];
}
static NSString *lastError() {return [NSString stringWithUTF8String:nmb_last_error()];}
// The render pipeline journal: one line per render pipeline creation (when, on which thread, how long, the outcome, both functions with
// every constant value, the attachments, blending and vertex descriptor) appended to pipelines.log in the runtime's cache directory, which
// the board log exports with the caches (CacheExport.h): every pipeline a boot made, or was refused, can be made again on the host.
static std::mutex gJournalLock;
static std::string gJournalPath;
static uint64_t gJournalBegan;
static bool gJournalVerbose;
static void journal(const std::string &line) {
 if(gJournalPath.empty())return;
 std::lock_guard<std::mutex> lock(gJournalLock);
 if(FILE *f=fopen(gJournalPath.c_str(),"a")){fwrite(line.data(),1,line.size(),f);fputc('\n',f);fclose(f);}
}

@interface NBObject : NSObject
@property(atomic,copy) NSString *label;
@end
@implementation NBObject
- (id<MTLDevice>)device {return gDeviceObject;}
- (void)doesNotRecognizeSelector:(SEL)selector {
 if(gMissing)gMissing(class_getName(object_getClass(self)),sel_getName(selector));
 noteProblem([NSString stringWithFormat:@"missing %s %s",class_getName(object_getClass(self)),sel_getName(selector)]);
 [super doesNotRecognizeSelector:selector];
}
@end

// ---- resources ----
// The wait takes a strong snapshot of already-committed buffers; new commits require the
// caller's usual resource synchronization. Storage dependency checks are in the runtime.
static NSMutableSet *gInFlight;
static void waitForResource(id resource);
// Resource SPI the runtime has no state for: no protected content (Metal's default options), and no owner transfer
// (memory attribution is the kernel driver's, by the importing task), refused as not supported.
#define NB_RESOURCE_SPI(TYPE) \
- (uint64_t)protectionOptions {return 0;} \
- (id<MTLHeap>)heap {return _parentHeap;} \
- (BOOL)isAliasable {return nmb_##TYPE##_is_aliasable(_handle);} \
- (void)makeAliasable {if(!nmb_##TYPE##_make_aliasable(_handle))NBLOG(@"native-metal: makeAliasable refused: %@",lastError());} \
- (int)setOwnerWithIdentity:(unsigned)identity {(void)identity;return 46; /* KERN_NOT_SUPPORTED */} \
- (void)waitUntilComplete {waitForResource(self);}
@interface NBBuffer : NBObject
@property(nonatomic) NmbBuffer *handle;
@property(nonatomic) MTLResourceOptions options;
@property(nonatomic) void *wrapped;
@property(nonatomic,strong) id<MTLHeap> parentHeap;   // the heap that made the buffer, nil otherwise
@property(nonatomic) NSUInteger heapOffset;
@end
@interface NBTexture : NBObject
@property(nonatomic) NmbTexture *handle;
@property(nonatomic) NmbTextureDescriptor descriptor;
@property(nonatomic,strong) NBBuffer *buffer;
@property(nonatomic) IOSurfaceRef surface;
@property(nonatomic) NSUInteger offset,rowBytes;
@property(nonatomic,strong) id<MTLTexture> parent;
@property(nonatomic,strong) id<MTLHeap> parentHeap;
@property(nonatomic) NSUInteger heapOffset;
@end
@interface NBHeap : NBObject
@property(nonatomic) NmbHeap *handle;
@property(nonatomic) MTLResourceOptions options;
@property(nonatomic) MTLHeapType heapType;
@end
// A no-copy import keeps its pages until the runtime frees it, which can be after the buffer object goes (a command buffer still using
// it). The owner's release (Metal's deallocator, an IOSurface's mapping) waits for that, as Metal's does: run earlier, the owner could
// unmap or reuse pages the GPU still reads or writes.
static std::atomic<uint64_t> gNoCopyAll,gNoCopyMade,gNoCopyReleased,gSurfaceImportsMade,gSurfaceImportsReleased;
static void releaseBlock(void *context,void *pointer,uint64_t length) {
 gNoCopyReleased++;
 void (^deallocator)(void *,NSUInteger)=(__bridge_transfer void (^)(void *,NSUInteger))context;deallocator(pointer,(NSUInteger)length);
}
static void releaseSurface(void *context,void *,uint64_t) {gSurfaceImportsReleased++;CFRelease((IOSurfaceRef)context);}
@implementation NBBuffer
- (void)dealloc {
 nmb_buffer_release(_handle);
}
- (NSUInteger)length {return nmb_buffer_length(_handle);}
- (void *)contents {return nmb_buffer_contents(_handle);}
- (uint64_t)gpuAddress {return nmb_buffer_gpu_address(_handle);}
- (MTLResourceOptions)resourceOptions {return _options;}
- (MTLStorageMode)storageMode {return (MTLStorageMode)((_options&MTLResourceStorageModeMask)>>MTLResourceStorageModeShift);}
- (MTLCPUCacheMode)cpuCacheMode {return (MTLCPUCacheMode)((_options&MTLResourceCPUCacheModeMask)>>MTLResourceCPUCacheModeShift);}
- (MTLHazardTrackingMode)hazardTrackingMode {return (MTLHazardTrackingMode)nmb_buffer_hazard_tracking_mode(_handle);}
- (void)didModifyRange:(NSRange)range {(void)range;}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
 const uint64_t answer=nmb_buffer_set_purgeable_state(_handle,state);
 if(!answer)NBLOG(@"native-metal: buffer setPurgeableState refused: %@",lastError());
 return (MTLPurgeableState)answer;
}
- (NSUInteger)allocatedSize {return nmb_buffer_length(_handle);}
- (IOSurfaceRef)iosurface {return nullptr;}   // a buffer is never the backing of a surface here (surfaces back textures)
NB_RESOURCE_SPI(buffer)
@end
// Feature branch 0f11e64/547312c: keep recent imports across frames, keyed
// by allocation identity and mapping. Retain the buffer that owns the surface,
// never an unretained caller wrapper. Pong bounds the hold to 64 MiB/64 entries
// and one second; pressure drops it immediately. Releases occur outside this
// lock because freeing a BO may wait for GPU retirement.
static NSMapTable<NSValue *,NBBuffer *> *gSurfaceImports;
static std::mutex gSurfaceImportLock;
struct HeldImport {NBBuffer *buffer;uint64_t usedUs,bytes;};
static auto &gHeldImports=*new std::map<std::tuple<uint64_t,uint64_t,uint64_t>,HeldImport>;
static void pruneHeldImports(bool all,std::vector<NBBuffer *> &gone) {
 const uint64_t now=nowUs();uint64_t bytes=0;
 for(auto it=gHeldImports.begin();it!=gHeldImports.end();) {
  if(all || now-it->second.usedUs>1000000) {
   gone.push_back(it->second.buffer);it=gHeldImports.erase(it);
  }else {bytes+=it->second.bytes;++it;}
 }
 while(gHeldImports.size()>64 || bytes>64u*1024u*1024u) {
  auto oldest=gHeldImports.begin();
  for(auto it=gHeldImports.begin();it!=gHeldImports.end();++it)
   if(it->second.usedUs<oldest->second.usedUs)oldest=it;
  bytes-=oldest->second.bytes;gone.push_back(oldest->second.buffer);gHeldImports.erase(oldest);
 }
}
static void trimHeldImports(bool all) {
 std::vector<NBBuffer *> gone;
 {std::lock_guard<std::mutex> lock(gSurfaceImportLock);pruneHeldImports(all,gone);}
}
static bool trimOnDeviceMemoryFailure() {
 // Device VA can be exhausted before the OS raises system memory pressure.
 // Evict only our optional holds and retry once; in-flight owners stay alive.
 const char *error=nmb_last_error();
 if(!error || !(strstr(error,"OutOfMemory") || strstr(error,"OutOfDeviceMemory") ||
    strstr(error,"OutOfHostMemory") || strstr(error,"OUT_OF_DEVICE_MEMORY") ||
    strstr(error,"OUT_OF_HOST_MEMORY")))return false;
 trimHeldImports(true);if(gDevice)nmb_device_release_kept_memory(gDevice);return true;
}
extern "C" void native_bridge_metal_release_idle_imports(void) {
 // Drop optional compositor cache holds on display idle. Live textures,
 // command buffers and the presenter's three pinned sources retain their
 // own backing. Never release an uncertain/in-flight DMA owner here.
 trimHeldImports(true);
 if(gDevice)nmb_device_release_kept_memory(gDevice);
}
static void watchImportPressure() {
 static dispatch_once_t once;
 static dispatch_source_t source; // ARC must retain the active pressure source.
 dispatch_once(&once,^{
  source=dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE,0,
   DISPATCH_MEMORYPRESSURE_WARN|DISPATCH_MEMORYPRESSURE_CRITICAL,dispatch_get_global_queue(QOS_CLASS_UTILITY,0));
  if(!source)return;
  dispatch_source_set_event_handler(source,^{
   trimHeldImports(true);if(gDevice)nmb_device_release_kept_memory(gDevice);
  });
  dispatch_activate(source);
 });
}
static void holdImport(uint64_t identity,uint64_t address,uint64_t bytes,NBBuffer *buffer,std::vector<NBBuffer *> &gone) {
 gHeldImports[{identity,address,bytes}]={buffer,nowUs(),bytes};pruneHeldImports(false,gone);
}
static NBBuffer *surfaceBuffer(IOSurfaceRef surface,void *base,size_t bytes) {
 struct Key {uint64_t identity,address,bytes;};
 const Key value={IOSurfaceGetID(surface),(uintptr_t)base,bytes};
 NSValue *key=value.identity?[NSValue valueWithBytes:&value objCType:@encode(Key)]:nil;
 std::vector<NBBuffer *> gone; // declared before the lock: release after unlock
 std::lock_guard<std::mutex> lock(gSurfaceImportLock);
 if(!gSurfaceImports)gSurfaceImports=[NSMapTable strongToWeakObjectsMapTable];
 NBBuffer *buffer=key?[gSurfaceImports objectForKey:key]:nil;
 if(buffer){if(key)holdImport(value.identity,value.address,value.bytes,buffer,gone);return buffer;}
 // The surface keeps its CPU mapping (the imported pages) until the import is freed.
 NmbBuffer *handle=nmb_buffer_new_no_copy_releasing(gDevice,base,bytes,MTLResourceStorageModeShared,releaseSurface,(void *)CFRetain(surface));
 if(!handle){CFRelease(surface);NBLOG(@"native-metal: surface import refused: %@",lastError());return nil;}
 gSurfaceImportsMade++;buffer=[NBBuffer new];buffer.handle=handle;buffer.options=MTLResourceStorageModeShared;buffer.wrapped=base;
 if(key) {
  // Bound metadata without retaining old frames. Evicting an association is
  // harmless: its owners still retain the actual import until their own end.
  if(gSurfaceImports.count>=256) {
   for(NSValue *old in gSurfaceImports.keyEnumerator.allObjects)
    if(![gSurfaceImports objectForKey:old])[gSurfaceImports removeObjectForKey:old];
   if(gSurfaceImports.count>=256)[gSurfaceImports removeObjectForKey:gSurfaceImports.keyEnumerator.nextObject];
  }
  [gSurfaceImports setObject:buffer forKey:key];
  holdImport(value.identity,value.address,value.bytes,buffer,gone);
 }
 return buffer;
}
// Upstream default: every Metal client uses one sample; explicit override is retained.
static bool singleSample() {
 static const bool enabled=[] {
  if(const char *value=getenv("NATIVE_METAL_SINGLE_SAMPLE"))return strcmp(value,"0")!=0;
  return true;
 }();
 return enabled;
}
static uint64_t packedSwizzle(MTLTextureSwizzleChannels s) {
 return (1ull<<32)|(uint64_t)s.red|((uint64_t)s.green<<8)|((uint64_t)s.blue<<16)|((uint64_t)s.alpha<<24);
}
static bool textureDescriptor(MTLTextureDescriptor *d,NmbTextureDescriptor *out) {
 if(!d)return false;
 *out={(uint64_t)d.textureType,(uint64_t)d.pixelFormat,d.width,d.height,d.depth,d.mipmapLevelCount,d.sampleCount,d.arrayLength,(uint64_t)d.usage,(uint64_t)d.resourceOptions,packedSwizzle(d.swizzle)};
 if(singleSample() && out->samples>1) {
  out->samples=1;
  if(out->type==MTLTextureType2DMultisample)out->type=MTLTextureType2D;
  if(out->type==MTLTextureType2DMultisampleArray)out->type=MTLTextureType2DArray;
  if((out->options&MTLResourceStorageModeMask)==MTLResourceStorageModeMemoryless)
   out->options=(out->options&~MTLResourceStorageModeMask)|MTLResourceStorageModePrivate;
 }
 return true;
}
// 22H374 texture-layout SPI, read from the supplied AGXTextureLayout metadata.
// This is our own linear host layout, never an invented AGX tiled layout.
// The backend already supports linear textures over imported host buffers.
struct NBFormatInfo {const char *name;uint64_t flags,castClass,words[4];};
extern "C" NBFormatInfo MTLPixelFormatGetInfo(NSUInteger);
struct NBLayoutWatermark {uint64_t words[2];};
@interface NBTextureLayout : NSObject
@property(nonatomic,copy) MTLTextureDescriptor *descriptor;
@property(nonatomic) NSUInteger rowBytes,pixelBytes,size,alignment;
+ (instancetype)layoutForDescriptor:(MTLTextureDescriptor *)descriptor;
@end
@implementation NBTextureLayout
+ (instancetype)layoutForDescriptor:(MTLTextureDescriptor *)d {
 if(!d || d.textureType!=MTLTextureType2D || d.sampleCount!=1 || d.mipmapLevelCount!=1 ||
    d.depth!=1 || d.arrayLength!=1 || !d.width || !d.height || d.width>16384 || d.height>16384 ||
    d.storageMode==MTLStorageModeMemoryless)return nil;
 NBFormatInfo info=MTLPixelFormatGetInfo(d.pixelFormat);
 if(!info.name || (info.flags&0x400) || !info.words[0] || info.words[0]>16)return nil;
 const uint64_t row=nmb_device_linear_bytes_per_row(gDevice,d.pixelFormat,d.width);
 const NSUInteger page=(NSUInteger)getpagesize();
 if(!row || !page || row>NSUIntegerMax/d.height || row*d.height>NSUIntegerMax-(page-1))return nil;
 NBTextureLayout *layout=[NBTextureLayout new];layout.descriptor=d;
 layout.pixelBytes=info.words[0];layout.rowBytes=row;layout.alignment=page;
 layout.size=((row*d.height+page-1)/page)*page;return layout;
}
- (NBLayoutWatermark)watermark {
 return {{UINT64_C(0x504f4e474c494e31),
  ((uint64_t)_descriptor.pixelFormat<<48)|((uint64_t)_descriptor.width<<24)|_descriptor.height}};
}
- (MTLSize)copyGranularity {return MTLSizeMake(1,1,1);}
- (void)initializeTextureMemory:(void *)memory {if(memory)memset(memory,0,_size);}
- (void)finalizeTextureMemory:(void *)memory {(void)memory; /* linear layout has no tiling metadata */}
- (BOOL)validRegion:(MTLRegion)region slice:(NSUInteger)slice level:(NSUInteger)level {
 return !slice && !level && !region.origin.z && region.size.depth==1 &&
  region.origin.x<=_descriptor.width && region.size.width<=_descriptor.width-region.origin.x &&
  region.origin.y<=_descriptor.height && region.size.height<=_descriptor.height-region.origin.y;
}
- (void)copyFromLinearBytes:(const void *)linear linearOffset:(NSUInteger)offset
 linearBytesPerRow:(NSUInteger)row linearBytesPerImage:(NSUInteger)image
 toTextureMemory:(void *)memory textureSlice:(NSUInteger)slice textureLevel:(NSUInteger)level textureRegion:(MTLRegion)region {
 (void)image;
 if(!linear || !memory || ![self validRegion:region slice:slice level:level])return;
 const NSUInteger bytes=region.size.width*_pixelBytes;
 if(row<bytes || offset>NSUIntegerMax-bytes ||
    (region.size.height>1 && row>(NSUIntegerMax-offset-bytes)/(region.size.height-1)))return;
 for(NSUInteger y=0;y<region.size.height;++y)
  std::memmove((uint8_t *)memory+(region.origin.y+y)*_rowBytes+region.origin.x*_pixelBytes,
   (const uint8_t *)linear+offset+y*row,bytes);
}
- (void)copyFromTextureMemory:(const void *)memory textureSlice:(NSUInteger)slice textureLevel:(NSUInteger)level
 textureRegion:(MTLRegion)region toLinearBytes:(void *)linear linearOffset:(NSUInteger)offset
 linearBytesPerRow:(NSUInteger)row linearBytesPerImage:(NSUInteger)image {
 (void)image;
 if(!linear || !memory || ![self validRegion:region slice:slice level:level])return;
 const NSUInteger bytes=region.size.width*_pixelBytes;
 if(row<bytes || offset>NSUIntegerMax-bytes ||
    (region.size.height>1 && row>(NSUIntegerMax-offset-bytes)/(region.size.height-1)))return;
 for(NSUInteger y=0;y<region.size.height;++y)
  std::memmove((uint8_t *)linear+offset+y*row,
   (const uint8_t *)memory+(region.origin.y+y)*_rowBytes+region.origin.x*_pixelBytes,bytes);
}
@end
@implementation NBTexture
- (NSUInteger)allocatedSize {return nmb_texture_allocated_size(_handle);}
- (MTLTextureSwizzleChannels)swizzle {
 const uint64_t s=_descriptor.swizzle;
 return MTLTextureSwizzleChannelsMake((MTLTextureSwizzle)(s&255),(MTLTextureSwizzle)((s>>8)&255),(MTLTextureSwizzle)((s>>16)&255),(MTLTextureSwizzle)((s>>24)&255));
}
- (void)dealloc {nmb_texture_release(_handle);if(_surface)CFRelease(_surface);}
- (MTLTextureType)textureType {return (MTLTextureType)_descriptor.type;}
- (MTLPixelFormat)pixelFormat {return (MTLPixelFormat)_descriptor.format;}
- (NSUInteger)width {return _descriptor.width;}
- (NSUInteger)height {return _descriptor.height;}
- (NSUInteger)depth {return _descriptor.depth;}
- (NSUInteger)mipmapLevelCount {return _descriptor.levels;}
- (NSUInteger)sampleCount {return _descriptor.samples;}
- (NSUInteger)arrayLength {return _descriptor.array_length;}
- (MTLTextureUsage)usage {return (MTLTextureUsage)_descriptor.usage;}
- (MTLResourceOptions)resourceOptions {return (MTLResourceOptions)_descriptor.options;}
- (MTLStorageMode)storageMode {return (MTLStorageMode)((_descriptor.options&MTLResourceStorageModeMask)>>MTLResourceStorageModeShift);}
- (MTLCPUCacheMode)cpuCacheMode {return (MTLCPUCacheMode)((_descriptor.options&MTLResourceCPUCacheModeMask)>>MTLResourceCPUCacheModeShift);}
- (MTLHazardTrackingMode)hazardTrackingMode {return (MTLHazardTrackingMode)nmb_texture_hazard_tracking_mode(_handle);}
- (BOOL)isFramebufferOnly {return NO;}
// CoreImage in 22H374 queries this Metal SPI before choosing its render
// destination. These are ordinary textures/IOSurface imports, never a
// CAMetalDrawable's presentation texture. Views preserve that distinction.
- (BOOL)isDrawable {return NO;}
- (IOSurfaceRef)iosurface {return _surface;}
- (NSUInteger)iosurfacePlane {return 0;}
- (id<MTLBuffer>)buffer {return _surface?nil:(id<MTLBuffer>)_buffer;}
- (NSUInteger)bufferOffset {return _offset;}
- (NSUInteger)bufferBytesPerRow {return _rowBytes;}
- (id<MTLTexture>)parentTexture {return _parent;}
- (NSUInteger)parentRelativeLevel {return nmb_texture_parent_relative_level(_handle);}
- (NSUInteger)parentRelativeSlice {return nmb_texture_parent_relative_slice(_handle);}
- (NSUInteger)rotation {return 0;}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
 const uint64_t answer=nmb_texture_set_purgeable_state(_handle,state);
 if(!answer)NBLOG(@"native-metal: texture setPurgeableState refused: %@",lastError());
 return (MTLPurgeableState)answer;
}
NB_RESOURCE_SPI(texture)
- (id<MTLTexture>)view:(NmbTexture *)handle {
 if(!handle){NBLOG(@"native-metal: texture view refused: %@",lastError());return nil;}
 NBTexture *view=[NBTexture new];view.handle=handle;frameEvent(35,(uintptr_t)handle,(uintptr_t)_handle,_descriptor.format,_descriptor.swizzle,_descriptor.samples,_descriptor.type,_descriptor.usage,_surface?IOSurfaceGetPixelFormat(_surface):0);NmbTextureDescriptor descriptor;nmb_texture_describe(handle,&descriptor);view.descriptor=descriptor;
 view.parent=(id<MTLTexture>)self;view.buffer=_buffer;view.offset=_offset;view.rowBytes=_rowBytes;
 view.parentHeap=_parentHeap;view.heapOffset=_heapOffset;
 if(_surface)view.surface=(IOSurfaceRef)CFRetain(_surface);
 return (id<MTLTexture>)view;
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format {
 return [self view:nmb_texture_new_view(_handle,format,0,nullptr,nullptr,true)];
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format textureType:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices {
 const uint64_t l[2]={levels.location,levels.length},s[2]={slices.location,slices.length};
 return [self view:nmb_texture_new_view(_handle,format,type,l,s,false)];
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format textureType:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices swizzle:(MTLTextureSwizzleChannels)swizzle {
 const uint64_t l[2]={levels.location,levels.length},r[2]={slices.location,slices.length};
 return [self view:nmb_texture_new_view_swizzle(_handle,format,type,l,r,packedSwizzle(swizzle))];
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice withBytes:(const void *)bytes bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)image {
 const uint64_t region[6]={r.origin.x,r.origin.y,r.origin.z,r.size.width,r.size.height,r.size.depth};
 const uint64_t length=(r.size.depth>1?image*(r.size.depth-1):0)+row*r.size.height;
 if(!nmb_texture_replace(_handle,region,level,slice,bytes,row,image,length))NBLOG(@"native-metal: replaceRegion refused: %@",lastError());
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level withBytes:(const void *)bytes bytesPerRow:(NSUInteger)row {
 [self replaceRegion:r mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:row bytesPerImage:row*r.size.height];
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)image fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice {
 const uint64_t region[6]={r.origin.x,r.origin.y,r.origin.z,r.size.width,r.size.height,r.size.depth};
 const uint64_t length=(r.size.depth>1?image*(r.size.depth-1):0)+row*r.size.height;
 if(!nmb_texture_get_bytes(_handle,bytes,row,image,length,region,level,slice))NBLOG(@"native-metal: getBytes refused: %@",lastError());
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level {
 [self getBytes:bytes bytesPerRow:row bytesPerImage:row*r.size.height fromRegion:r mipmapLevel:level slice:0];
}
@end
// A command can use many independent imports/views of one IOSurface. Their
// owners keep the mapping live through this phase. Maintain each exact CPU
// range once; no address identity or decision is carried into another phase
// or command, and distinct/partially overlapping ranges retain their calls.
#include "PongSurfaceHandoff.h"
using HostRanges=std::vector<std::pair<uint64_t,uint64_t>>;
static void flushSurfaces(NSMutableSet *surfaces,const HostRanges &kernelSynced) {
 if(!surfaces.count)return;
 std::vector<std::pair<uintptr_t,size_t>> ranges;
 ranges.reserve(surfaces.count);
 for(NBTexture *texture in surfaces)if(texture.surface)
  ranges.emplace_back(reinterpret_cast<uintptr_t>(IOSurfaceGetBaseAddress(texture.surface)),IOSurfaceGetAllocSize(texture.surface));
 std::sort(ranges.begin(),ranges.end());
 const auto last=std::unique(ranges.begin(),ranges.end());
 gSurfaceFlushDuplicates+=(uint64_t)(ranges.end()-last);
 // Views can describe overlapping subranges of the same live mapping.
 // Merge only covered virtual ranges inside this one maintenance phase;
 // aliases at different VAs and subsequent commands still get maintenance.
 // This removes repeated large cache walks without skipping coherency.
 std::vector<std::pair<uintptr_t,size_t>> merged;
 for(auto it=ranges.begin();it!=last;++it) {
  if(!it->first || !it->second || it->second>UINTPTR_MAX-it->first)continue;
  if(!merged.empty()) {
   auto &previous=merged.back();const uintptr_t end=previous.first+previous.second;
   if(it->first<=end){previous.second=std::max(end,it->first+it->second)-previous.first;continue;}
  }
  merged.push_back(*it);
 }
 for(const auto &range:merged) {
  // Only omit the user VA flush when this exact phase successfully
  // maintained a retained physical import covering the entire surface.
  // NotFound/unsupported, copied backings and uncovered ranges retain the
  // conservative path. Kernel physical maintenance includes CPU aliases.
  bool covered=false;
  for(const auto &synced:kernelSynced)
   if(range.first>=synced.first && range.second<=synced.second &&
      range.first-synced.first<=synced.second-range.second){covered=true;break;}
  if(covered)continue;
  const uint64_t began=nowUs();
  sys_dcache_flush(reinterpret_cast<void *>(range.first),range.second);
  gSurfaceFlushBytes+=range.second;gTimeSurfaceFlush.add(began);
 }
}
// Publish only the active command's retained host imports. The kernel
// matches full live import extents and keeps the old policy for every other
// buffer. Phase1 is issued only after actual runtime completion, before
// CPU/display ownership is returned; other imports remain conservative.
static bool syncActiveImports(NSMutableSet *surfaces,NSMutableSet *owners,uint32_t phase,HostRanges &kernelSynced) {
 using Sync=int32_t(*)(uint64_t,uint64_t,uint32_t);
 using Batch=int32_t(*)(const uint64_t *,uint32_t,uint32_t,uint64_t *);
 static std::atomic<Sync> cached{nullptr};Sync fn=cached.load();
 static std::atomic<Batch> cachedBatch{nullptr};Batch batch=cachedBatch.load();
 if(!fn && !gLoaderPath.empty()) {
  void *lib=dlopen(gLoaderPath.c_str(),RTLD_NOW|RTLD_NOLOAD);
  if(lib){fn=(Sync)dlsym(lib,"tu_darwin_sync_host_range");if(fn)cached.store(fn);
   batch=(Batch)dlsym(lib,"tu_darwin_sync_host_batch");if(batch)cachedBatch.store(batch);dlclose(lib);}
 }
 if(!fn)return true; // older matched kernel/Turnip retains all-import maintenance
 std::vector<std::pair<uint64_t,uint64_t>> ranges;
 auto addBuffer=[&](NBBuffer *buffer) {
  if(buffer.wrapped && buffer.length && buffer.length<=UINT64_MAX-4095)
   ranges.emplace_back((uintptr_t)buffer.wrapped,(uint64_t(buffer.length)+4095)&~uint64_t(4095));
 };
 auto addTexture=[&](NBTexture *texture) {
  for(unsigned depth=0;texture && depth<64;texture=texture.parent,++depth)addBuffer(texture.buffer);
 };
 for(id o in owners) {
  if([o isKindOfClass:[NBBuffer class]])addBuffer(o);
  else if([o isKindOfClass:[NBTexture class]])addTexture(o);
 }
 for(id o in surfaces)if([o isKindOfClass:[NBTexture class]])addTexture(o);
 std::sort(ranges.begin(),ranges.end());ranges.erase(std::unique(ranges.begin(),ranges.end()),ranges.end());
 // One retained command can have dozens of surface views. Submit their
 // ownership phase in one bounded call instead of reacquiring both process
 // and global device locks and rescanning the full BO table for every view.
 // Complete-range reply bits alone can replace a surface's CPU VA flush.
 if(batch)for(size_t first=0;first<ranges.size();first+=64) {
  const uint32_t count=uint32_t(std::min<size_t>(64,ranges.size()-first));
  uint64_t words[128]={},covered=0;
  for(unsigned i=0;i<count;++i){words[2*i]=ranges[first+i].first;words[2*i+1]=ranges[first+i].second;}
  const int32_t result=batch(words,count,phase,&covered);
  if(uint32_t(result)==0xe00002c7u) {batch=nullptr;break;} // older negotiated ABI: retain method19
  if(result){NBLOG(@"native-metal: batched host ownership sync refused: 0x%x",uint32_t(result));return false;}
  for(unsigned i=0;i<count;++i)if(covered&(UINT64_C(1)<<i))kernelSynced.push_back(ranges[first+i]);
 }
 if(batch)return true;
 for(const auto &range:ranges) {
  const int32_t result=fn(range.first,range.second,phase);
  if(!result)kernelSynced.push_back(range);
  // No matching host import means the runtime used an owned/copy backing,
  // which still receives its normal kernel maintenance. Any real failure
  // refuses this command; do not silently omit a required publication.
  if(result && uint32_t(result)!=0xe00002c7u && uint32_t(result)!=0xe00002f0u) {
   NBLOG(@"native-metal: host ownership sync refused: 0x%x",uint32_t(result));return false;
  }
 }
 return true;
}
static NBTexture *textureOnBuffer(NBBuffer *buffer,MTLTextureDescriptor *d,NSUInteger offset,NSUInteger row,bool surface) {
 NmbTextureDescriptor descriptor;if(!buffer || !textureDescriptor(d,&descriptor))return nil;
 NmbTexture *handle=nmb_texture_new_on_buffer(buffer.handle,&descriptor,offset,row,surface);
 if(!handle){NBLOG(@"native-metal: texture over buffer refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;
 frameEvent(49,(uintptr_t)handle,d.sampleCount,descriptor.samples,d.pixelFormat,descriptor.format,d.width,d.height,packedSwizzle(d.swizzle));
 texture.buffer=buffer;texture.offset=offset;texture.rowBytes=row;return texture;
}
// A texture over a two-plane surface: the runtime reads plane 1 from the surface's own layout (two-plane RGB and YCbCr formats).
static NBTexture *textureOnSurface(NBBuffer *buffer,MTLTextureDescriptor *d,IOSurfaceRef surface,NSUInteger plane,NSUInteger offset,NSUInteger row) {
 NmbTextureDescriptor descriptor;if(!buffer || !textureDescriptor(d,&descriptor))return nil;
 const uint64_t base=(uintptr_t)IOSurfaceGetBaseAddress(surface),chroma=(uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface,1);
 // Preserve the producer's color matrix. Treating every video/image surface
 // as BT.601 changes the colors of BT.709/2020 wallpaper and weather images.
 static const CFStringRef *matrixKey=(const CFStringRef *)dlsym(RTLD_DEFAULT,"kIOSurfaceYCbCrMatrix");
 CFTypeRef matrix=IOSurfaceCopyValue(surface,matrixKey?*matrixKey:CFSTR("IOSurfaceYCbCrMatrix"));
 if(!matrix)matrix=IOSurfaceCopyValue(surface,CFSTR("YCbCrMatrix"));
 const char *matrixText=nullptr;
 if(matrix)matrixText=CFGetTypeID(matrix)==CFStringGetTypeID()?[( __bridge NSString *)matrix UTF8String]:"unsupported IOSurface matrix type";
 NmbTexture *handle=nmb_texture_new_on_surface(buffer.handle,&descriptor,offset,row,IOSurfaceGetPixelFormat(surface),plane,
  IOSurfaceGetWidth(surface),IOSurfaceGetHeight(surface),chroma>=base?chroma-base:0,IOSurfaceGetBytesPerRowOfPlane(surface,1),matrixText);
 if(matrix)CFRelease(matrix);
 if(!handle){NBLOG(@"native-metal: texture over surface refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;
 frameEvent(49,(uintptr_t)handle,d.sampleCount,descriptor.samples,d.pixelFormat,descriptor.format,d.width,d.height,packedSwizzle(d.swizzle));
 texture.buffer=buffer;texture.offset=offset;texture.rowBytes=row;return texture;
}
@implementation NBBuffer (Textures)
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset bytesPerRow:(NSUInteger)row {
 return (id<MTLTexture>)textureOnBuffer(self,d,offset,row,false);
}
- (id<MTLTexture>)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)image {
 NmbTextureDescriptor descriptor;if(!textureDescriptor(d,&descriptor))return nil;
 NmbTexture *handle=nmb_texture_new_linear(self.handle,&descriptor,offset,row,image);
 if(!handle){NBLOG(@"native-metal: linear texture refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;
 frameEvent(49,(uintptr_t)handle,d.sampleCount,descriptor.samples,d.pixelFormat,descriptor.format,d.width,d.height,packedSwizzle(d.swizzle));
 frameEvent(49,(uintptr_t)handle,d.sampleCount,descriptor.samples,d.pixelFormat,descriptor.format,d.width,d.height,packedSwizzle(d.swizzle));
 texture.buffer=self;texture.offset=offset;texture.rowBytes=row;return (id<MTLTexture>)texture;
}
@end

// ---- libraries, functions, states ----
@interface NBLibrary : NBObject
@property(nonatomic) NmbLibrary *handle;
@end
@interface NBFunction : NBObject
@property(nonatomic) NmbFunction *handle;
@property(nonatomic,copy) NSString *sourceName;
@property(nonatomic,copy) NSString *name;
@property(nonatomic) MTLFunctionType functionType;
@property(nonatomic) std::string constantsText;   // name:type:hex of each constant value it was specialized with (the journal's)
@property(nonatomic,strong) id pipelineLibrary;
@property(nonatomic,strong) id functionCache;
@end
@interface NBFunctionConstant : NSObject
@property(nonatomic,copy) NSString *name;
@property(nonatomic) MTLDataType type;
@property(nonatomic) NSUInteger index;
@property(nonatomic,getter=isRequired) BOOL required;
@end
@implementation NBFunctionConstant
@end
@implementation NBFunction
- (NSDictionary<NSString *,MTLFunctionConstant *> *)functionConstantsDictionary {
 NSMutableDictionary *result=[NSMutableDictionary dictionary];
 if(!_handle)return result;
 for(uint32_t i=0,n=nmb_function_constant_count(_handle);i<n;i++) {
  const size_t size=nmb_function_constant_name_size(_handle,i);if(!size)return nil;
  std::vector<char> name(size);uint64_t index=0,type=0;bool required=false;
  if(!nmb_function_constant_at(_handle,i,name.data(),name.size(),&index,&type,&required))return nil;
  NBFunctionConstant *c=[NBFunctionConstant new];c.name=@(name.data());c.index=index;c.type=(MTLDataType)type;c.required=required;
  result[c.name]=c;
 }
 return result;
}
- (void)dealloc {nmb_function_release(_handle);}
- (MTLFunctionOptions)options {return MTLFunctionOptionNone;}
@end
// The target's own accessors of MTLFunctionConstantValues (as the offline frontend reads them); their ABI is checked before any call.
@interface MTLFunctionConstantValues (NBOriginalConstantAccess)
- (NSArray *)newIndexedConstantArray;
- (NSArray *)newNamedConstantArray;
@end
@protocol NBOriginalConstantValue
- (NSUInteger)dataType;
- (void *)data;
- (NSUInteger)index;
- (NSString *)name;
@end
static bool methodABI(id object,SEL selector,const char *encoding) {
 Method method=class_getInstanceMethod(object_getClass(object),selector);
 return method && !strcmp(method_getTypeEncoding(method),encoding);
}
// Packed scalar components of a function constant. MSL's 3-wide storage has
// tail padding, but the AIR constant binder consumes exactly three lanes.
// Actual20G81 QuartzCore supplies UChar3 (51) at create_fragment_shader+0x868;
// rejecting every 3-wide type made ordinary desktop shader specialization nil.
static uint64_t constantBytes(uint64_t type) {
 switch(type) {
  case 3:case 29:case 33:return 4;
  case 4:case 30:case 34:return 8;
  case 5:case 31:case 35:return 12;
  case 6:case 32:case 36:return 16;
  case 16:case 37:case 41:case 121:return 2;
  case 17:case 38:case 42:case 122:return 4;
  case 18:case 39:case 43:case 123:return 6;
  case 19:case 40:case 44:case 124:return 8;
  case 45:case 49:case 53:return 1;
  case 46:case 50:case 54:return 2;
  case 47:case 51:case 55:return 3;
  case 48:case 52:case 56:return 4;
  case 81:case 85:return 8;
  case 82:case 86:return 16;
  case 83:case 87:return 24;
  case 84:case 88:return 32;
  default:return 0;
 }
}
static std::string quartzCoreCallers();
@implementation NBLibrary
- (void)dealloc {nmb_library_release(_handle);}
- (id<MTLFunction>)newFunctionWithZeroConstants:(NSString *)name {
 if(!name)return nil;
 NmbFunction *handle=nmb_function_new_zeroed(_handle,name.UTF8String);
 if(!handle)return nil;
 NBFunction *function=[NBFunction new];function.handle=handle;function.name=name;function.sourceName=name;
 function.functionType=(MTLFunctionType)nmb_function_type(handle);
 return (id<MTLFunction>)function;
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values error:(NSError **)error {
 if(!name || !values || !methodABI(values,@selector(newIndexedConstantArray),"@16@0:8") || !methodABI(values,@selector(newNamedConstantArray),"@16@0:8")) {
  NBLOG(@"native-metal: function %@ refused: constant values ABI",name);
  if(error)*error=nbError(@"unqualified function constant values");return nil;
 }
 MTLFunctionConstantValues *snapshot=[values copy];
 NSArray *indexed=[snapshot newIndexedConstantArray],*named=[snapshot newNamedConstantArray];
 std::vector<NmbConstant> constants;
 for(unsigned group=0;group<2;group++)for(id value in (group?named:indexed)) {
  if(!methodABI(value,@selector(dataType),"Q16@0:8") || !methodABI(value,@selector(data),"^v16@0:8") ||
     !methodABI(value,group?@selector(name):@selector(index),group?"@16@0:8":"Q16@0:8")) {
   NBLOG(@"native-metal: function %@ refused: constant value ABI",name);
   if(error)*error=nbError(@"unqualified constant value ABI");return nil;
  }
  id<NBOriginalConstantValue> v=value;
  const uint64_t bytes=constantBytes(v.dataType);
  if(!bytes || !v.data){NBLOG(@"native-metal: function %@ refused: constant type %lu",name,(unsigned long)v.dataType);if(error)*error=nbError([NSString stringWithFormat:@"constant type %lu is not qualified",(unsigned long)v.dataType]);return nil;}
  constants.push_back({v.dataType,group?0:v.index,group?v.name.UTF8String:nullptr,v.data,bytes});
 }
 const uint64_t functionBegan=nowUs();
 NmbFunction *handle=nmb_function_new_specialized(_handle,name.UTF8String,constants.data(),(uint32_t)constants.size());gTimeFunction.add(functionBegan);
 std::string text;
 if(gJournalVerbose || !handle)for(const NmbConstant &c:constants) {
  char head[32];if(!c.name)snprintf(head,sizeof head,"#%llu",(unsigned long long)c.index);
  text+=text.empty()?"":" ";text+=c.name?c.name:head;snprintf(head,sizeof head,":%llu:",(unsigned long long)c.type);text+=head;
  for(uint64_t i=0;i<c.length;i++){snprintf(head,sizeof head,"%02x",((const uint8_t *)c.bytes)[i]);text+=head;}
 }
 // One S line per specialization (int-155: every QuartzCore pipeline was made on its draw, and a refused generic function would leave its
 // generic pipeline unmade without a word): the function, its constants, the time, the refusal and QuartzCore's callers.
 if(!gJournalPath.empty() && (gJournalVerbose || !handle)) {
  char head[96];snprintf(head,sizeof head,"S t=%llu us=%llu ok=%d name=",(unsigned long long)((functionBegan-gJournalBegan)/1000),
      (unsigned long long)(nowUs()-functionBegan),handle?1:0);
  journal(head+std::string(name.UTF8String?:"?")+" k="+text+(handle?std::string():" err="+std::string(lastError().UTF8String?:"?"))+" | "+quartzCoreCallers());
 }
 if(!handle){
  NBLOG(@"native-metal: specialized function %@: %@",name,lastError());
  if(error)*error=nbError([NSString stringWithFormat:@"specialized function: %@",lastError()]);return nil;
 }
 NBFunction *function=[NBFunction new];function.handle=handle;function.name=name;function.sourceName=name;function.functionType=(MTLFunctionType)nmb_function_type(handle);
 function.constantsText=text;if(error)*error=nil;return (id<MTLFunction>)function;
}
- (void)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values completionHandler:(void (^)(id<MTLFunction>,NSError *))handler {
 if(!handler)return;
 NSString *savedName=[name copy];MTLFunctionConstantValues *snapshot=[values copy];
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{@autoreleasepool {
  NSError *error=nil;id<MTLFunction> function=[self newFunctionWithName:savedName constantValues:snapshot error:&error];
  handler(function,error);
 }});
}
#if PONG_GUEST_22H374
// 22H374 Core Image uses the private pipeline-library entry, whose native
// _MTLLibrary implementation forwards the library's functionCache. These
// are compilation caches, not another shader library. Compile the requested
// function/constants through our real runtime cache and keep attribution
// objects alive; no fabricated function/pipeline is returned on failure.
- (id<MTLFunction>)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values functionCache:(id)cache error:(NSError **)error {
 id<MTLFunction> result=values?[self newFunctionWithName:name constantValues:values error:error]:[self newFunctionWithName:name];
 if(result) {((NBFunction *)result).functionCache=cache;if(error)*error=nil;}
 else if(error && !values)*error=nbError([NSString stringWithFormat:@"function %@: %@",name,lastError()]);
 return result;
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values pipelineLibrary:(id)library error:(NSError **)error {
 id cache=nil;
 if(library && methodABI(library,sel_registerName("functionCache"),"@16@0:8"))
  cache=((id(*)(id,SEL))objc_msgSend)(library,sel_registerName("functionCache"));
 id<MTLFunction> result=[self newFunctionWithName:name constantValues:values functionCache:cache error:error];
 if(result)((NBFunction *)result).pipelineLibrary=library;
 return result;
}
- (void)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values pipelineLibrary:(id)library completionHandler:(void (^)(id<MTLFunction>,NSError *))handler {
 if(!handler)return;
 NSString *savedName=[name copy];MTLFunctionConstantValues *snapshot=[values copy];
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{@autoreleasepool {
  NSError *error=nil;id<MTLFunction> result=[self newFunctionWithName:savedName constantValues:snapshot pipelineLibrary:library error:&error];
  handler(result,error);
 }});
}
#endif
- (MTLLibraryType)type {return MTLLibraryTypeExecutable;}
// The descriptor's name and constants; its specialized name only labels Metal's function and its options ask for compile-time work the
// runtime does when the function is made.
- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d error:(NSError **)error {
 if(!d.name){if(error)*error=nbError(@"a function name is required");return nil;}
 NBFunction *function=(NBFunction *)(d.constantValues?[self newFunctionWithName:d.name constantValues:d.constantValues error:error]:[self newFunctionWithName:d.name]);
 if(!function){if(error && !d.constantValues)*error=nbError([NSString stringWithFormat:@"function %@: %@",d.name,lastError()]);return nil;}
 if(d.specializedName) {
  if(function.handle && !nmb_function_set_specialized_name(function.handle,d.specializedName.UTF8String)) {
   if(error)*error=nbError(lastError());return nil;
  }
  function.name=d.specializedName;
 }
 if(error)*error=nil;return (id<MTLFunction>)function;
}
- (void)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d completionHandler:(void (^)(id<MTLFunction>,NSError *))handler {
 if(!handler)return;MTLFunctionDescriptor *saved=[d copy];
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{@autoreleasepool {
  NSError *error=nil;id<MTLFunction> function=[self newFunctionWithDescriptor:saved error:&error];handler(function,error);
 }});
}
- (NSArray<NSString *> *)functionNames {
 NSMutableArray *names=[NSMutableArray array];
 for(uint32_t i=0,n=nmb_library_function_count(_handle);i<n;i++) {
  char name[256];nmb_library_function_name(_handle,i,name,sizeof name);[names addObject:[NSString stringWithUTF8String:name]];
 }
 return names;
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name {
 const uint64_t functionBegan=nowUs();
 NmbFunction *handle=name?nmb_function_new(_handle,name.UTF8String):nullptr;gTimeFunction.add(functionBegan);
 if(!handle)return nil;
 NBFunction *function=[NBFunction new];function.handle=handle;function.name=name;function.sourceName=name;function.functionType=(MTLFunctionType)nmb_function_type(handle);
 return (id<MTLFunction>)function;
}
@end
@interface NBComputePipeline : NBObject
@property(nonatomic) NmbComputePipeline *handle;
@end
@implementation NBComputePipeline
- (void)dealloc {nmb_compute_pipeline_release(_handle);}
- (NSUInteger)maxTotalThreadsPerThreadgroup {return nmb_compute_pipeline_max_threads(_handle);}
- (NSUInteger)threadExecutionWidth {return nmb_compute_pipeline_execution_width(_handle);}
@end
@interface NBRenderPipeline : NBObject
@property(nonatomic) NmbRenderPipeline *handle;
@property(nonatomic) uint32_t stateFormat;
@end
@implementation NBRenderPipeline
- (void)dealloc {nmb_render_pipeline_release(_handle);}
// The threadgroup size a tile dispatch of this pipeline could use: the device's limit (no tile shaders are offered).
- (NSUInteger)maxTotalThreadsPerThreadgroup {
 uint64_t s[3]={};if(!nmb_device_max_threads_per_threadgroup(gDevice,s))return 0;
 return s[0];
}
@end
@interface NBSampler : NBObject
@property(nonatomic) NmbSampler *handle;
@end
@implementation NBSampler
- (void)dealloc {nmb_sampler_release(_handle);}
- (NSUInteger)pixelFormat {return MTLPixelFormatInvalid;}   // SPI: a sampler built from a plain descriptor names no format
@end
@interface NBDepthStencil : NBObject
@property(nonatomic) NmbDepthStencil *handle;
@end
@implementation NBDepthStencil
- (void)dealloc {nmb_depth_stencil_release(_handle);}
@end

// ---- events and fences ----
@interface NBEvent : NBObject
@property(nonatomic) NmbEvent *handle;
@end
@implementation NBEvent
- (void)dealloc {nmb_event_release(_handle);}
@end
@interface NBSharedEvent : NBEvent
@end
static void notified(void *context,uint64_t value);
@implementation NBSharedEvent
- (uint64_t)signaledValue {return nmb_shared_event_value(self.handle);}
- (void)setSignaledValue:(uint64_t)value {frameEvent(52,(uintptr_t)self.handle,value,1);nmb_shared_event_set_value(self.handle,value);}
- (BOOL)waitUntilSignaledValue:(uint64_t)value timeoutMS:(uint64_t)milliseconds {frameEvent(52,(uintptr_t)self.handle,value,2,milliseconds);BOOL result=nmb_shared_event_wait(self.handle,value,milliseconds);frameEvent(52,(uintptr_t)self.handle,value,3,milliseconds,result);return result;}
// The block runs on the listener's dispatch queue (Metal's contract); the bridge's listener thread only forwards it there.
- (void)notifyListener:(MTLSharedEventListener *)listener atValue:(uint64_t)value block:(MTLSharedEventNotificationBlock)block {
 if(!block)return;
 dispatch_queue_t queue=listener.dispatchQueue?:dispatch_get_global_queue(QOS_CLASS_DEFAULT,0);
 NSArray *job=@[self,queue,[block copy]];
 if(!nmb_shared_event_notify(self.handle,value,notified,(__bridge_retained void *)job)) {
  (void)(__bridge_transfer NSArray *)(__bridge void *)job;
  NBLOG(@"native-metal: notifyListener refused: %@",lastError());
 }
}
// A handle for another process needs a kernel-backed event this bridge does not have yet: refused, never a handle that could not work.
- (MTLSharedEventHandle *)newSharedEventHandle {
 frameEvent(53,(uintptr_t)self.handle);
 if(gMissing)gMissing("NBSharedEvent","newSharedEventHandle (cross-process)");
 return nil;
}
@end
static void notified(void *context,uint64_t value) {
 NSArray *job=(__bridge_transfer NSArray *)context;
 id<MTLSharedEvent> event=job[0];MTLSharedEventNotificationBlock block=job[2];
 dispatch_async(job[1],^{block(event,value);});
}
@interface NBFence : NBObject
@property(nonatomic) NmbFence *handle;
@end
@implementation NBFence
- (void)dealloc {nmb_fence_release(_handle);}
@end

// ---- command buffers and encoders ----
@interface NBQueue : NBObject {NBQueuePriorities::Request _priority;}
@property(nonatomic) NmbQueue *handle;
@property(atomic,strong) dispatch_queue_t completionQueue;   // SPI: where command buffers' handlers run (nil: the runtime's thread)
@end
@interface NBCommandBuffer : NBObject {
 std::vector<mach_port_t> displayHandoffs;
@public
 EncodedWork encodedWork;   // the command buffer's single encoder thread, not global counter deltas
 uint64_t createdUs,traceNumber;
}
@property(nonatomic) NmbCommandBuffer *handle;
@property(nonatomic,strong) NBQueue *queue;
@property(atomic,copy) NSString *encodeFailure;
@property(atomic) BOOL committed,failedLocally,completionSyncFailed;
@property(nonatomic) uint64_t committedUs;
@property(nonatomic) uint32_t commitIndex;
@property(nonatomic,strong) NSMutableArray *scheduledHandlers,*completedHandlers;
@property(nonatomic,strong) NSMutableSet *surfaces;
@property(nonatomic,strong) NSMutableDictionary *traceUsage;
- (void)traceTexture:(NBTexture *)texture usage:(uint32_t)usage;
- (void)tracePhase:(uint32_t)phase;
// Runtime handles retain GPU storage, but only the Objective-C owner retains
// a no-copy deallocator, its memory owner and texture/heap ancestry.
@property(nonatomic,strong) NSMutableSet *resourceOwners;
// Darwin24 QuartzCore attaches a uint32 task-attribution list before encoding.
// Preserve that metadata without changing ownership of kernel BOs or tasks.
@property(atomic,copy) NSData *responsibleTaskIDs;
@property(nonatomic,strong) dispatch_group_t handlers;
@property(nonatomic,strong) dispatch_group_t schedulingHandlers;
@property(nonatomic) BOOL schedulingDelivered;
- (void)refuse:(NSString *)what;
@end
struct SingleSampleCopy {
 NmbTexture *source,*destination;
 uint64_t sourceLevel,sourceSlice,destinationLevel,destinationSlice;
 std::array<uint64_t,3> sourceOrigin,destinationOrigin,size;
};
@interface NBEncoder : NBObject
@property(nonatomic) NmbEncoder *handle;
@property(nonatomic) std::vector<SingleSampleCopy> resolveCopies;
@property(nonatomic,strong) NBCommandBuffer *commandBuffer;
@end
static void waitForResource(id resource) {
 const uint64_t began=nowUs();
 NSArray *pending;@synchronized(gInFlight){pending=gInFlight.allObjects;}
 for(NBCommandBuffer *commandBuffer in pending) {
  int dependency=-1;
  if([resource isKindOfClass:[NBBuffer class]])dependency=nmb_command_buffer_uses_buffer(commandBuffer.handle,[(NBBuffer *)resource handle]);
  else if([resource isKindOfClass:[NBTexture class]])dependency=nmb_command_buffer_uses_texture(commandBuffer.handle,[(NBTexture *)resource handle]);
  gResourceChecks++;
  // An unknown result retains the old conservative wait. False means no matching storage
  // (or GPU work already completed), not a fabricated completion for a pending dependency.
  if(dependency!=0)[(id<MTLCommandBuffer>)commandBuffer waitUntilCompleted];else gResourceSkipped++;
 }
 gTimeResourceWait.add(began);
}
@implementation NBEncoder
- (void)dealloc {if(_handle)nmb_encoder_discard(_handle);}
- (bool)check:(bool)ok what:(const char *)what {
 if(!ok)[_commandBuffer refuse:[NSString stringWithFormat:@"%s: %@",what,lastError()]];
 return ok;
}
- (void)endEncoding {
 NmbEncoder *handle=_handle;_handle=nullptr;
 if(!handle){[_commandBuffer refuse:@"endEncoding twice"];return;}
 if(![self check:nmb_encoder_end(handle) what:"endEncoding"])return;
 if(!_resolveCopies.empty()) {
  NmbEncoder *blit=nmb_blit_encoder_new(_commandBuffer.handle);
  if(!blit){[self check:false what:"single-sample resolve"];return;}
  for(const auto &c:_resolveCopies)
   [self check:nmb_blit_copy_texture(blit,c.source,c.sourceSlice,c.sourceLevel,c.sourceOrigin.data(),c.size.data(),
    c.destination,c.destinationSlice,c.destinationLevel,c.destinationOrigin.data()) what:"single-sample resolve"];
  [self check:nmb_encoder_end(blit) what:"single-sample resolve end"];
  _resolveCopies.clear();
 }
}
- (void)insertDebugSignpost:(NSString *)s {(void)s;}
- (void)pushDebugGroup:(NSString *)s {(void)s;}
- (void)popDebugGroup {}
// Stage 0 compute, 1 vertex, 2 fragment (the bridge's numbering).
- (void)buffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage {
 if(buffer && ![(id)buffer isKindOfClass:[NBBuffer class]]){[_commandBuffer refuse:@"foreign buffer"];return;}
 if(buffer)[_commandBuffer.resourceOwners addObject:buffer];
 [self check:nmb_encoder_set_buffer(_handle,stage,buffer?[(NBBuffer *)buffer handle]:nullptr,offset,(uint32_t)index) what:"setBuffer"];
}
- (void)texture:(id<MTLTexture>)texture index:(NSUInteger)index stage:(uint32_t)stage {
 if(texture && ![(id)texture isKindOfClass:[NBTexture class]]){[_commandBuffer refuse:@"foreign texture"];return;}
 if(texture){[_commandBuffer.resourceOwners addObject:texture];[_commandBuffer traceTexture:(NBTexture *)texture usage:stage==0 && (texture.usage&MTLTextureUsageShaderWrite)?9:1];}
 if(texture && [(NBTexture *)texture surface])[_commandBuffer.surfaces addObject:texture];
 [self check:nmb_encoder_set_texture(_handle,stage,texture?[(NBTexture *)texture handle]:nullptr,(uint32_t)index) what:"setTexture"];
}
- (void)sampler:(id<MTLSamplerState>)sampler index:(NSUInteger)index stage:(uint32_t)stage {
 if(sampler && ![(id)sampler isKindOfClass:[NBSampler class]]){[_commandBuffer refuse:@"foreign sampler"];return;}
 [self check:nmb_encoder_set_sampler(_handle,stage,sampler?[(NBSampler *)sampler handle]:nullptr,(uint32_t)index) what:"setSamplerState"];
}
- (void)bytes:(const void *)bytes length:(NSUInteger)length index:(NSUInteger)index stage:(uint32_t)stage {
 [self check:nmb_encoder_set_bytes(_handle,stage,bytes,length,(uint32_t)index) what:"setBytes"];
}
- (void)fence:(id<MTLFence>)fence stages:(uint64_t)stages update:(bool)update {
 frameEvent(51,_commandBuffer->traceNumber,(uintptr_t)(__bridge void *)fence,stages,update);
 if(![(id)fence isKindOfClass:[NBFence class]]){[_commandBuffer refuse:@"foreign fence"];return;}
 NmbFence *handle=[(NBFence *)fence handle];
 [self check:update?nmb_encoder_update_fence(_handle,handle,stages):nmb_encoder_wait_fence(_handle,handle,stages) what:update?"updateFence":"waitForFence"];
}
- (void)updateFence:(id<MTLFence>)fence {[self fence:fence stages:0 update:true];}
- (void)waitForFence:(id<MTLFence>)fence {[self fence:fence stages:0 update:false];}
- (void)offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage {
 [self check:nmb_encoder_set_buffer_offset(_handle,stage,offset,(uint32_t)index) what:"setBufferOffset"];
}
// A resource reached through an address/ID in another buffer is not a
// binding. Pass it to the runtime's actual residency table and retain its
// wrapper/IOSurface owner through completion, exactly like a direct binding.
- (bool)resourceList:(const id<MTLResource> __unsafe_unretained [])resources count:(NSUInteger)count
              into:(std::vector<NmbResourceRef> &)out {
 if(count && !resources){[_commandBuffer refuse:@"null resource list"];return false;}
 out.reserve(count);
 for(NSUInteger i=0;i<count;++i) {
  id resource=resources[i];
  if([resource isKindOfClass:[NBBuffer class]])out.push_back({[(NBBuffer *)resource handle],0,0});
  else if([resource isKindOfClass:[NBTexture class]])out.push_back({[(NBTexture *)resource handle],1,0});
  else {[_commandBuffer refuse:@"missing or foreign resident resource"];return false;}
 }
 return true;
}
- (void)holdResources:(const id<MTLResource> __unsafe_unretained [])resources count:(NSUInteger)count {
 for(NSUInteger i=0;i<count;++i) {
  id resource=resources[i];[_commandBuffer.resourceOwners addObject:resource];
  if([resource isKindOfClass:[NBTexture class]] && [(NBTexture *)resource surface])
   [_commandBuffer.surfaces addObject:resource];
 }
}
- (void)useResourceList:(const id<MTLResource> __unsafe_unretained [])resources count:(NSUInteger)count
                 usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages {
 std::vector<NmbResourceRef> values;
 if(![self resourceList:resources count:count into:values])return;
 if([self check:nmb_encoder_use_resources(_handle,values.data(),values.size(),usage,stages) what:"useResources"])
  [self holdResources:resources count:count];
}
- (void)useHeapList:(const id<MTLHeap> __unsafe_unretained [])heaps count:(NSUInteger)count stages:(MTLRenderStages)stages {
 if(count && !heaps){[_commandBuffer refuse:@"null heap list"];return;}
 std::vector<NmbHeap *> values;values.reserve(count);
 for(NSUInteger i=0;i<count;++i) {
  if(![(id)heaps[i] isKindOfClass:[NBHeap class]]){[_commandBuffer refuse:@"missing or foreign resident heap"];return;}
  values.push_back([(NBHeap *)heaps[i] handle]);
 }
 if([self check:nmb_encoder_use_heaps(_handle,values.data(),values.size(),stages) what:"useHeaps"])
  for(NSUInteger i=0;i<count;++i)[_commandBuffer.resourceOwners addObject:heaps[i]];
}
- (void)barrierResourceList:(const id<MTLResource> __unsafe_unretained [])resources count:(NSUInteger)count
                    after:(MTLRenderStages)after before:(MTLRenderStages)before {
 std::vector<NmbResourceRef> values;
 if(![self resourceList:resources count:count into:values])return;
 if([self check:nmb_encoder_barrier_resources(_handle,values.data(),values.size(),after,before) what:"memoryBarrierWithResources"])
  [self holdResources:resources count:count];
}
@end

@interface NBComputeEncoder : NBEncoder
@end
@implementation NBComputeEncoder
- (MTLDispatchType)dispatchType {return MTLDispatchTypeSerial;}
- (void)setComputePipelineState:(id<MTLComputePipelineState>)state {
 if(![(id)state isKindOfClass:[NBComputePipeline class]]){[self.commandBuffer refuse:@"foreign compute pipeline"];return;}
 [self check:nmb_compute_set_pipeline(self.handle,[(NBComputePipeline *)state handle]) what:"setComputePipelineState"];
}
- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i {[self buffer:b offset:o index:i stage:0];}
- (void)setBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {[self offset:o index:i stage:0];}
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger [])o withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self buffer:b[k] offset:o[k] index:r.location+k stage:0];
}
- (void)setBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {[self bytes:p length:n index:i stage:0];}
- (void)setTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {[self texture:t index:i stage:0];}
- (void)setTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self texture:t[k] index:r.location+k stage:0];
}
- (void)setSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {[self sampler:s index:i stage:0];}
- (void)setThreadgroupMemoryLength:(NSUInteger)n atIndex:(NSUInteger)i {
 [self check:nmb_compute_set_threadgroup_memory(self.handle,(uint32_t)n,(uint32_t)i) what:"setThreadgroupMemoryLength"];
}
- (void)dispatch:(MTLSize)size group:(MTLSize)group threads:(bool)threads {
 gDispatches++;self.commandBuffer->encodedWork.dispatches++;
 const uint64_t s[3]={size.width,size.height,size.depth},g[3]={group.width,group.height,group.depth};
 [self check:nmb_compute_dispatch(self.handle,s,g,threads) what:"dispatch"];
}
- (void)dispatchThreads:(MTLSize)t threadsPerThreadgroup:(MTLSize)g {[self dispatch:t group:g threads:true];}
- (void)dispatchThreadgroups:(MTLSize)t threadsPerThreadgroup:(MTLSize)g {[self dispatch:t group:g threads:false];}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u {
 const id<MTLResource> resources[]={r};[self useResourceList:resources count:1 usage:u stages:0];
}
- (void)useResources:(const id<MTLResource> __unsafe_unretained [])r count:(NSUInteger)n usage:(MTLResourceUsage)u {
 [self useResourceList:r count:n usage:u stages:0];
}
- (void)useHeap:(id<MTLHeap>)heap {const id<MTLHeap> heaps[]={heap};[self useHeapList:heaps count:1 stages:0];}
- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained [])heaps count:(NSUInteger)n {[self useHeapList:heaps count:n stages:0];}
- (void)memoryBarrierWithScope:(MTLBarrierScope)s {
 [self check:nmb_encoder_barrier_scope(self.handle,s,0,0) what:"memoryBarrierWithScope"];
}
- (void)memoryBarrierWithResources:(const id<MTLResource> __unsafe_unretained [])r count:(NSUInteger)n {
 [self barrierResourceList:r count:n after:0 before:0];
}
@end

static std::mutex gPassFormatLock;
static struct {uint64_t key,n;} gPassFormatRows[32];
static unsigned gPassFormatCount;
static void noteFormats(uint64_t key) {
 std::lock_guard<std::mutex> lock(gPassFormatLock);
 for(unsigned i=0;i<gPassFormatCount;i++)if(gPassFormatRows[i].key==key){gPassFormatRows[i].n++;return;}
 if(gPassFormatCount<32){gPassFormatRows[gPassFormatCount].key=key;gPassFormatRows[gPassFormatCount].n=1;gPassFormatCount++;}
}
@interface NBRenderEncoder : NBEncoder
@property(nonatomic) uint64_t passKey;
@end
@implementation NBRenderEncoder
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state {
 if(![(id)state isKindOfClass:[NBRenderPipeline class]]){[self.commandBuffer refuse:@"foreign render pipeline"];return;}
 noteFormats(self.passKey|((uint64_t)((NBRenderPipeline *)state).stateFormat<<32));
 [self check:nmb_render_set_pipeline(self.handle,[(NBRenderPipeline *)state handle]) what:"setRenderPipelineState"];
}
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i {[self buffer:b offset:o index:i stage:1];}
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i {[self buffer:b offset:o index:i stage:2];}
- (void)setVertexBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {[self offset:o index:i stage:1];}
- (void)setFragmentBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {[self offset:o index:i stage:2];}
- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger [])o withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self buffer:b[k] offset:o[k] index:r.location+k stage:1];
}
- (void)setFragmentBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger [])o withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self buffer:b[k] offset:o[k] index:r.location+k stage:2];
}
- (void)setVertexBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {[self bytes:p length:n index:i stage:1];}
- (void)setFragmentBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {[self bytes:p length:n index:i stage:2];}
- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {[self texture:t index:i stage:1];}
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {[self texture:t index:i stage:2];}
- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self texture:t[k] index:r.location+k stage:2];
}
- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {[self sampler:s index:i stage:1];}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {[self sampler:s index:i stage:2];}
- (void)setDepthStencilState:(id<MTLDepthStencilState>)state {
 if(state && ![(id)state isKindOfClass:[NBDepthStencil class]]){[self.commandBuffer refuse:@"foreign depth/stencil state"];return;}
 [self check:nmb_render_set_depth_stencil(self.handle,state?[(NBDepthStencil *)state handle]:nullptr) what:"setDepthStencilState"];
}
- (void)setViewport:(MTLViewport)v {
 const double values[6]={v.originX,v.originY,v.width,v.height,v.znear,v.zfar};
 [self check:nmb_render_set_viewport(self.handle,values) what:"setViewport"];
}
- (void)setViewports:(const MTLViewport *)v count:(NSUInteger)count {
 if(!v || !count || count>16){[self.commandBuffer refuse:@"invalid viewport array"];return;}
 std::vector<double> values;values.reserve(count*6);
 for(NSUInteger i=0;i<count;i++)values.insert(values.end(),{v[i].originX,v[i].originY,v[i].width,v[i].height,v[i].znear,v[i].zfar});
 [self check:nmb_render_set_viewports(self.handle,values.data(),count) what:"setViewports"];
}
- (void)setScissorRects:(const MTLScissorRect *)r count:(NSUInteger)count {
 if(!r || !count || count>16){[self.commandBuffer refuse:@"invalid scissor array"];return;}
 std::vector<uint64_t> values;values.reserve(count*4);
 for(NSUInteger i=0;i<count;i++)values.insert(values.end(),{r[i].x,r[i].y,r[i].width,r[i].height});
 [self check:nmb_render_set_scissors(self.handle,values.data(),count) what:"setScissorRects"];
}
- (void)setScissorRect:(MTLScissorRect)r {
 const uint64_t values[4]={r.x,r.y,r.width,r.height};
 [self check:nmb_render_set_scissor(self.handle,values) what:"setScissorRect"];
}
// The runtime takes cull mode, winding and fill mode together; this encoder keeps Metal's defaults until each is set.
- (void)raster {
 NSNumber *cull=objc_getAssociatedObject(self,"cull"),*winding=objc_getAssociatedObject(self,"winding"),*fill=objc_getAssociatedObject(self,"fill");
 [self check:nmb_render_set_raster(self.handle,cull.unsignedLongLongValue,winding.unsignedLongLongValue,fill.unsignedLongLongValue) what:"raster state"];
}
- (void)setCullMode:(MTLCullMode)m {objc_setAssociatedObject(self,"cull",@(m),OBJC_ASSOCIATION_RETAIN);[self raster];}
- (void)setFrontFacingWinding:(MTLWinding)w {objc_setAssociatedObject(self,"winding",@(w),OBJC_ASSOCIATION_RETAIN);[self raster];}
- (void)setTriangleFillMode:(MTLTriangleFillMode)f {objc_setAssociatedObject(self,"fill",@(f),OBJC_ASSOCIATION_RETAIN);[self raster];}
- (void)setDepthBias:(float)bias slopeScale:(float)slope clamp:(float)clamp {
 [self check:nmb_render_set_depth_bias(self.handle,bias,slope,clamp) what:"setDepthBias"];
}
- (void)setLineWidth:(float)width {[self check:nmb_render_set_line_width(self.handle,width) what:"setLineWidth"];}
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset {
 [self check:nmb_render_set_visibility_mode(self.handle,mode,offset) what:"setVisibilityResultMode"];
}
- (void)setFragmentTexture:(id<MTLTexture>)t atTextureIndex:(NSUInteger)ti samplerState:(id<MTLSamplerState>)s atSamplerIndex:(NSUInteger)si {
 [self texture:t index:ti stage:2];[self sampler:s index:si stage:2];
}
// Tile shaders and imageblock tile memory (Apple GPU families 4 and up) are not offered: supportsTileShaders answers no, and a call that
// reaches one anyway fails its command buffer.
- (NSUInteger)tileWidth {return 0;}
- (NSUInteger)tileHeight {return 0;}
- (void)setTileTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {(void)t;(void)i;[self.commandBuffer refuse:@"tile shaders are not supported"];}
- (void)setTileBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {(void)p;(void)n;(void)i;[self.commandBuffer refuse:@"tile shaders are not supported"];}
- (void)dispatchThreadsPerTile:(MTLSize)size {(void)size;[self.commandBuffer refuse:@"tile shaders are not supported"];}
- (void)dispatchThreadsPerTile:(MTLSize)size inRegion:(MTLRegion)region {(void)size;(void)region;[self.commandBuffer refuse:@"tile shaders are not supported"];}
- (void)setStencilReferenceValue:(uint32_t)v {[self check:nmb_render_set_stencil_reference(self.handle,v,v) what:"setStencilReferenceValue"];}
- (void)setStencilFrontReferenceValue:(uint32_t)f backReferenceValue:(uint32_t)b {
 [self check:nmb_render_set_stencil_reference(self.handle,f,b) what:"setStencilReferenceValues"];
}
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a {
 const float c[4]={r,g,b,a};[self check:nmb_render_set_blend_color(self.handle,c) what:"setBlendColor"];
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n instanceCount:(NSUInteger)i baseInstance:(NSUInteger)b {
 gDraws++;self.commandBuffer->encodedWork.draws++;
 [self check:nmb_render_draw(self.handle,t,(uint32_t)s,(uint32_t)n,(uint32_t)i,(uint32_t)b) what:"drawPrimitives"];
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n instanceCount:(NSUInteger)i {
 [self drawPrimitives:t vertexStart:s vertexCount:n instanceCount:i baseInstance:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n {
 [self drawPrimitives:t vertexStart:s vertexCount:n instanceCount:1 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)k indexBuffer:(id<MTLBuffer>)b indexBufferOffset:(NSUInteger)o
                instanceCount:(NSUInteger)i baseVertex:(NSInteger)v baseInstance:(NSUInteger)bi {
 if(![(id)b isKindOfClass:[NBBuffer class]]){[self.commandBuffer refuse:@"foreign index buffer"];return;}
 [self.commandBuffer.resourceOwners addObject:b];
 gDraws++;self.commandBuffer->encodedWork.draws++;
 [self check:nmb_render_draw_indexed(self.handle,t,(uint32_t)n,k,[(NBBuffer *)b handle],o,(uint32_t)i,(int32_t)v,(uint32_t)bi) what:"drawIndexedPrimitives"];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)k indexBuffer:(id<MTLBuffer>)b indexBufferOffset:(NSUInteger)o
                instanceCount:(NSUInteger)i {
 [self drawIndexedPrimitives:t indexCount:n indexType:k indexBuffer:b indexBufferOffset:o instanceCount:i baseVertex:0 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)k indexBuffer:(id<MTLBuffer>)b indexBufferOffset:(NSUInteger)o {
 [self drawIndexedPrimitives:t indexCount:n indexType:k indexBuffer:b indexBufferOffset:o instanceCount:1 baseVertex:0 baseInstance:0];
}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u stages:(MTLRenderStages)s {
 const id<MTLResource> resources[]={r};[self useResourceList:resources count:1 usage:u stages:s];
}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u {
 [self useResource:r usage:u stages:MTLRenderStageVertex|MTLRenderStageFragment];
}
- (void)useResources:(const id<MTLResource> __unsafe_unretained [])r count:(NSUInteger)n usage:(MTLResourceUsage)u stages:(MTLRenderStages)s {
 [self useResourceList:r count:n usage:u stages:s];
}
- (void)useResources:(const id<MTLResource> __unsafe_unretained [])r count:(NSUInteger)n usage:(MTLResourceUsage)u {
 [self useResourceList:r count:n usage:u stages:MTLRenderStageVertex|MTLRenderStageFragment];
}
- (void)useHeap:(id<MTLHeap>)heap stages:(MTLRenderStages)s {
 const id<MTLHeap> heaps[]={heap};[self useHeapList:heaps count:1 stages:s];
}
- (void)useHeap:(id<MTLHeap>)heap {[self useHeap:heap stages:MTLRenderStageVertex|MTLRenderStageFragment];}
- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained [])heaps count:(NSUInteger)n stages:(MTLRenderStages)s {
 [self useHeapList:heaps count:n stages:s];
}
- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained [])heaps count:(NSUInteger)n {
 [self useHeapList:heaps count:n stages:MTLRenderStageVertex|MTLRenderStageFragment];
}
- (void)memoryBarrierWithScope:(MTLBarrierScope)s afterStages:(MTLRenderStages)a beforeStages:(MTLRenderStages)b {
 [self check:nmb_encoder_barrier_scope(self.handle,s,a,b) what:"memoryBarrierWithScope"];
}
- (void)memoryBarrierWithResources:(const id<MTLResource> __unsafe_unretained [])r count:(NSUInteger)n
                    afterStages:(MTLRenderStages)a beforeStages:(MTLRenderStages)b {
 [self barrierResourceList:r count:n after:a before:b];
}
- (void)updateFence:(id<MTLFence>)fence afterStages:(MTLRenderStages)stages {[self fence:fence stages:stages update:true];}
- (void)waitForFence:(id<MTLFence>)fence beforeStages:(MTLRenderStages)stages {[self fence:fence stages:stages update:false];}
@end

@interface NBBlitEncoder : NBEncoder
@end
@implementation NBBlitEncoder
// The arguments are passed as a plain array: an NSArray literal raises on a nil element, and a nil resource (a refused allocation) must fail the
// command buffer like any invalid argument, not abort the host process.
#define NB_OBJECTS(...) ((__unsafe_unretained id[]){__VA_ARGS__})
- (bool)buffers:(__unsafe_unretained id *)objects count:(size_t)count {
 for(size_t i=0;i<count;i++)if(![objects[i] isKindOfClass:[NBBuffer class]]){[self.commandBuffer refuse:@"missing or foreign blit buffer"];return false;}
 for(size_t i=0;i<count;i++)[self.commandBuffer.resourceOwners addObject:objects[i]];
 return true;
}
- (bool)textures:(__unsafe_unretained id *)objects count:(size_t)count {
 for(size_t i=0;i<count;i++) {
  id o=objects[i];
  if(![o isKindOfClass:[NBTexture class]]){[self.commandBuffer refuse:@"missing or foreign blit texture"];return false;}
  [self.commandBuffer.resourceOwners addObject:o];
  [self.commandBuffer traceTexture:(NBTexture *)o usage:4];
  if([(NBTexture *)o surface])[self.commandBuffer.surfaces addObject:o];
 }
 return true;
}
- (void)copyFromBuffer:(id<MTLBuffer>)s sourceOffset:(NSUInteger)so toBuffer:(id<MTLBuffer>)d destinationOffset:(NSUInteger)dO size:(NSUInteger)n {
 if(![self buffers:NB_OBJECTS(s,d) count:2])return;
 [self check:nmb_blit_copy_buffer(self.handle,[(NBBuffer *)s handle],so,[(NBBuffer *)d handle],dO,n) what:"copyFromBuffer:toBuffer:"];
}
- (void)fillBuffer:(id<MTLBuffer>)b range:(NSRange)r value:(uint8_t)v {
 if(![self buffers:NB_OBJECTS(b) count:1])return;
 [self check:nmb_blit_fill_buffer(self.handle,[(NBBuffer *)b handle],r.location,r.location+r.length,v) what:"fillBuffer"];
}
- (void)copyFromTexture:(id<MTLTexture>)s sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)z
              toTexture:(id<MTLTexture>)d destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dO {
 if(![self textures:NB_OBJECTS(s,d) count:2])return;
 [self.commandBuffer traceTexture:(NBTexture *)s usage:1];[self.commandBuffer traceTexture:(NBTexture *)d usage:2];
 const uint64_t a[3]={so.x,so.y,so.z},size[3]={z.width,z.height,z.depth},b[3]={dO.x,dO.y,dO.z};
 [self check:nmb_blit_copy_texture(self.handle,[(NBTexture *)s handle],ss,sl,a,size,[(NBTexture *)d handle],ds,dl,b) what:"copyFromTexture:toTexture:"];
}
- (void)copyFromTexture:(id<MTLTexture>)s sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl toTexture:(id<MTLTexture>)d
 destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl sliceCount:(NSUInteger)ns levelCount:(NSUInteger)nl {
 if(![self textures:NB_OBJECTS(s,d) count:2])return;
 [self.commandBuffer traceTexture:(NBTexture *)s usage:1];[self.commandBuffer traceTexture:(NBTexture *)d usage:2];
 auto slices=[](id<MTLTexture> t)->NSUInteger {return t.arrayLength*((t.textureType==MTLTextureTypeCube || t.textureType==MTLTextureTypeCubeArray)?6:1);};
 if(sl>s.mipmapLevelCount || nl>s.mipmapLevelCount-sl || dl>d.mipmapLevelCount || nl>d.mipmapLevelCount-dl ||
    ss>slices(s) || ns>slices(s)-ss || ds>slices(d) || ns>slices(d)-ds) {[self.commandBuffer refuse:@"texture copy range out of bounds"];return;}
 for(NSUInteger slice=0;slice<ns;slice++)for(NSUInteger level=0;level<nl;level++) {
  const NSUInteger mip=sl+level;
  MTLSize size=MTLSizeMake(MAX(s.width>>mip,(NSUInteger)1),MAX(s.height>>mip,(NSUInteger)1),MAX(s.depth>>mip,(NSUInteger)1));
  [self copyFromTexture:s sourceSlice:ss+slice sourceLevel:mip sourceOrigin:MTLOriginMake(0,0,0) sourceSize:size
    toTexture:d destinationSlice:ds+slice destinationLevel:dl+level destinationOrigin:MTLOriginMake(0,0,0)];
 }
}
- (void)copyFromTexture:(id<MTLTexture>)s toTexture:(id<MTLTexture>)d {
 if(![self textures:NB_OBJECTS(s,d) count:2])return;
 [self.commandBuffer traceTexture:(NBTexture *)s usage:1];[self.commandBuffer traceTexture:(NBTexture *)d usage:2];
 const NSUInteger levels=MIN(s.mipmapLevelCount,d.mipmapLevelCount),slices=MIN(s.arrayLength,d.arrayLength);
 for(NSUInteger slice=0;slice<slices;slice++)for(NSUInteger level=0;level<levels;level++) {
  MTLSize size=MTLSizeMake(MAX(s.width>>level,(NSUInteger)1),MAX(s.height>>level,(NSUInteger)1),MAX(s.depth>>level,(NSUInteger)1));
  [self copyFromTexture:s sourceSlice:slice sourceLevel:level sourceOrigin:MTLOriginMake(0,0,0) sourceSize:size toTexture:d destinationSlice:slice
       destinationLevel:level destinationOrigin:MTLOriginMake(0,0,0)];
 }
}
- (void)copyFromBuffer:(id<MTLBuffer>)s sourceOffset:(NSUInteger)o sourceBytesPerRow:(NSUInteger)row sourceBytesPerImage:(NSUInteger)image
            sourceSize:(MTLSize)z toTexture:(id<MTLTexture>)d destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level
     destinationOrigin:(MTLOrigin)dO {
 if(![self buffers:NB_OBJECTS(s) count:1] || ![self textures:NB_OBJECTS(d) count:1])return;
 const uint64_t size[3]={z.width,z.height,z.depth},b[3]={dO.x,dO.y,dO.z};
 [self check:nmb_blit_buffer_to_texture(self.handle,[(NBBuffer *)s handle],o,row,image,size,[(NBTexture *)d handle],slice,level,b) what:"copyFromBuffer:toTexture:"];
}
- (void)copyFromTexture:(id<MTLTexture>)s sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)z
               toBuffer:(id<MTLBuffer>)d destinationOffset:(NSUInteger)o destinationBytesPerRow:(NSUInteger)row destinationBytesPerImage:(NSUInteger)image {
 if(![self textures:NB_OBJECTS(s) count:1] || ![self buffers:NB_OBJECTS(d) count:1])return;
 const uint64_t a[3]={so.x,so.y,so.z},size[3]={z.width,z.height,z.depth};
 [self check:nmb_blit_texture_to_buffer(self.handle,[(NBTexture *)s handle],slice,level,a,size,[(NBBuffer *)d handle],o,row,image) what:"copyFromTexture:toBuffer:"];
}
- (void)generateMipmapsForTexture:(id<MTLTexture>)t {
 if(![self textures:NB_OBJECTS(t) count:1])return;
 [self check:nmb_blit_generate_mipmaps(self.handle,[(NBTexture *)t handle]) what:"generateMipmapsForTexture"];
}
- (void)synchronizeResource:(id<MTLResource>)r {(void)r;}
@end

static void completed(void *context);
static void scheduled(void *context);
// Retain the surface's colour-space description and serialized-property hash with each sample.
static void captureColour(IOSurfaceRef surface,uint64_t capture) {
 for(unsigned kind=0;kind<2;++kind) {
  CFTypeRef property=IOSurfaceCopyValue(surface,kind?kIOSurfaceICCProfile:kIOSurfaceColorSpace);
  NativeGpuTraceRecord rec{};rec.event=56;rec.value[0]=IOSurfaceGetID(surface);rec.value[1]=capture;rec.value[2]=kind;
  if(property) {
   CFStringRef description=CFCopyDescription(property);
   if(description) {
    rec.value[3]=CFStringGetLength(description);
    char *text=(char *)&rec.value[4];CFIndex used=0;
    CFStringGetBytes(description,CFRangeMake(0,CFStringGetLength(description)),kCFStringEncodingUTF8,'?',false,(UInt8 *)text,63,&used);
    CFRelease(description);
   }
   CFDataRef data=kind && CFGetTypeID(property)==CFDataGetTypeID()?(CFDataRef)CFRetain(property):
       CFPropertyListCreateData(kCFAllocatorDefault,property,kCFPropertyListBinaryFormat_v1_0,0,nullptr);
   if(data) {
    NativeGpuTraceRecord digest{};digest.event=57;digest.value[0]=rec.value[0];digest.value[1]=capture;digest.value[2]=kind;
    digest.value[3]=CFDataGetLength(data);
    CC_SHA256(CFDataGetBytePtr(data),(CC_LONG)CFDataGetLength(data),(unsigned char *)&digest.value[4]);
    (void)gFrameTransport(NativeGpuTraceMethod,&digest,sizeof(digest));CFRelease(data);
   }
   CFRelease(property);
  }
  (void)gFrameTransport(NativeGpuTraceMethod,&rec,sizeof(rec));
 }
}
// Samples retain the IOSurface owner through the real completion callback.
static void captureFrameSurfaces(NBCommandBuffer *cb) {
 if(!frameTraceOn() || !gFrameTransport || !getenv("SHENG_GPU_PIXELS"))return;
 const char *name=getprogname();const uint32_t stage=name && strstr(name,"Poster")?1:name && !strcmp(name,"backboardd")?2:0;
 if(!stage)return;
 static std::mutex gate;static unsigned taken;static uint64_t last;
 std::unique_lock<std::mutex> lock(gate,std::try_to_lock);if(!lock.owns_lock())return;
 const uint64_t now=nowUs();if(taken>=3 || (last && now-last<30000000))return;
 NBTexture *chosen=nil;uint64_t pixels=0;
 for(NBTexture *t in cb.surfaces) {
  const uint32_t usage=[cb.traceUsage[[NSValue valueWithPointer:t.handle]] unsignedIntValue];
  if(!(usage&2) || !t.surface || IOSurfaceGetPlaneCount(t.surface)>1)continue;
  if(t.descriptor.width*t.descriptor.height>pixels){chosen=t;pixels=t.descriptor.width*t.descriptor.height;}
 }
 if(!chosen)return;
 const auto td=chosen.descriptor;const uint32_t bpp=td.format==115?8:(td.format==70 || td.format==71 || td.format==80 || td.format==81)?4:0;
 IOSurfaceRef surface=chosen.surface;const uint64_t stride=chosen.rowBytes,allocation=IOSurfaceGetAllocSize(surface);
 const uint8_t *base=(const uint8_t *)IOSurfaceGetBaseAddress(surface);
 if(!bpp || !base || !td.width || !td.height || td.width>16384 || td.height>16384 ||
    td.width!=IOSurfaceGetWidth(surface) || td.height!=IOSurfaceGetHeight(surface) ||
    stride<td.width*bpp || chosen.offset>allocation || stride*(td.height-1)+td.width*bpp>allocation-chosen.offset) {
  frameEvent(40,cb->traceNumber,stage,1,td.format,IOSurfaceGetID(surface));last=now;return;
 }
 const uint32_t gw=(uint32_t)std::min<uint64_t>(64,td.width),gh=(uint32_t)std::min<uint64_t>(64,td.height);
 std::vector<uint8_t> data(gw*gh*bpp),check(data.size());
 auto sample=[&](std::vector<uint8_t> &out) {
  for(uint32_t y=0;y<gh;++y)for(uint32_t x=0;x<gw;++x)
   memcpy(out.data()+(y*gw+x)*bpp,base+chosen.offset+(uint64_t(y)*td.height/gh)*stride+(uint64_t(x)*td.width/gw)*bpp,bpp);
 };
 sample(data);sample(check);
 NativeGpuPixelPiece piece{};auto &h=piece.header;
 h.magic=0x31585047;h.version=1;h.stage=stage;h.capture=cb->traceNumber;h.us=now;h.command=cb->traceNumber;
 h.surface=IOSurfaceGetID(surface);h.width=(uint32_t)td.width;h.height=(uint32_t)td.height;h.format=(uint32_t)td.format;
 h.colour_space=td.format==71 || td.format==81?2:1;h.source_stride=stride;h.grid_width=gw;h.grid_height=gh;
 h.row_bytes=gw*bpp;h.bytes_per_pixel=bpp;h.flags=1|8|(data!=check?4:0);h.bytes=(uint32_t)data.size();
 bool ok=true;
 for(uint32_t at=0;at<h.bytes && ok;at+=piece.bytes) {
  piece.offset=at;piece.bytes=std::min<uint32_t>(NativeGpuPixelChunk,h.bytes-at);memcpy(piece.data,data.data()+at,piece.bytes);
  ok=gFrameTransport(NativeGpuPixelMethod,&piece,sizeof(piece));
 }
 frameEvent(40,h.command,stage,ok?0:2,h.surface,h.format,h.bytes,h.flags);
 if(ok){++taken;captureColour(surface,h.capture);}last=now;
}

@implementation NBCommandBuffer
// Upstream 0f11e64: MPS stores per-command state here on Darwin24 too.
static char kUserDictionaryKey;
- (id)userDictionary {
 @synchronized(self) {
  NSMutableDictionary *dictionary=objc_getAssociatedObject(self,&kUserDictionaryKey);
  if(!dictionary){dictionary=[NSMutableDictionary dictionary];objc_setAssociatedObject(self,&kUserDictionaryKey,dictionary,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
  return dictionary;
 }
}
- (instancetype)init {
 if((self=[super init])){_scheduledHandlers=[NSMutableArray array];_completedHandlers=[NSMutableArray array];_surfaces=[NSMutableSet set];_resourceOwners=[NSMutableSet set];_handlers=dispatch_group_create();_schedulingHandlers=dispatch_group_create();}
 if(self){createdUs=nowUs();traceNumber=++gFrameNumber;if(frameTraceOn())_traceUsage=[NSMutableDictionary dictionary];}
 return self;
}
- (void)dealloc {nmb_command_buffer_release(_handle);}
- (void)traceTexture:(NBTexture *)texture usage:(uint32_t)usage {
 if(!frameTraceOn() || !texture)return;
 NSValue *key=[NSValue valueWithPointer:texture.handle];_traceUsage[key]=@([_traceUsage[key] unsignedIntValue]|usage);
 const auto td=texture.descriptor;IOSurfaceRef surface=texture.surface;
 frameEvent(31,traceNumber,(uintptr_t)texture.handle,surface?IOSurfaceGetID(surface):0,
    surface?(uintptr_t)IOSurfaceGetBaseAddress(surface):0,surface?IOSurfaceGetAllocSize(surface):0,
    td.width,td.height,texture.rowBytes,td.format,0,usage,texture.offset);
 frameEvent(35,(uintptr_t)texture.handle,(uintptr_t)[(NBTexture *)texture.parentTexture handle],td.format,td.swizzle,
    td.samples,td.type,td.usage,surface?IOSurfaceGetPixelFormat(surface):0);
}
- (void)tracePhase:(uint32_t)phase {
 if(!frameTraceOn())return;
 const uint64_t runtime=_handle?nmb_command_buffer_diagnostic_id(_handle):0;
 frameEvent(30,traceNumber,runtime,phase,self.status,encodedWork.passes,self.commitIndex,(uintptr_t)_handle);
 for(NBTexture *t in _surfaces)frameEvent(31,traceNumber,(uintptr_t)t.handle,t.surface?IOSurfaceGetID(t.surface):0,
    t.surface?(uintptr_t)IOSurfaceGetBaseAddress(t.surface):0,t.surface?IOSurfaceGetAllocSize(t.surface):0,
    t.descriptor.width,t.descriptor.height,t.rowBytes,t.descriptor.format,phase,
    [_traceUsage[[NSValue valueWithPointer:t.handle]] unsignedIntValue],t.offset);
}


- (id<MTLCommandQueue>)commandQueue {return (id<MTLCommandQueue>)_queue;}
- (BOOL)retainedReferences {return YES;}
#if PONG_GUEST_22H374
// Actual 22H374 Metal encoding: v28@0:8r^I16I24, not NSUInteger count.
- (void)setResponsibleTaskIDs:(const uint32_t *)identities count:(uint32_t)count {
 if(self.committed || (count && !identities) || count>4096) {
  [self refuse:@"invalid responsible task attribution list"];return;
 }
 self.responsibleTaskIDs=count?[NSData dataWithBytes:identities length:size_t(count)*sizeof(*identities)]:nil;
}
#endif
// Protected content is not supported (the runtime's AppleOnlyCall::SetProtectionOptions takes Metal's default only): any other option fails
// the command buffer.
- (uint64_t)protectionOptions {return 0;}
- (void)setProtectionOptions:(uint64_t)options {if(options)[self refuse:[NSString stringWithFormat:@"protection options 0x%llx",options]];}
- (void)refuse:(NSString *)what {
 if(!self.encodeFailure){self.encodeFailure=what;NBLOG(@"native-metal: command buffer refused: %@",what);}
}
- (MTLCommandBufferStatus)status {
 if(self.failedLocally || self.completionSyncFailed)return MTLCommandBufferStatusError;
 return (MTLCommandBufferStatus)nmb_command_buffer_status(_handle);
}
- (NSError *)error {
 if(self.failedLocally || self.completionSyncFailed)return nbError(self.encodeFailure);
 if(self.status!=MTLCommandBufferStatusError)return nil;
 char text[512]={};nmb_command_buffer_error(_handle,text,sizeof text);
 return nbError([NSString stringWithUTF8String:text]);
}
// Debug groups and cache hints name work for tools and the driver's caches; they change nothing the GPU computes here.
- (void)pushDebugGroup:(NSString *)name {(void)name;}
- (void)popDebugGroup {}
- (void)encodeCacheHintTag:(uint64_t)tag resourceGroups:(const id __unsafe_unretained [])groups count:(NSUInteger)count {(void)tag;(void)groups;(void)count;}
- (void)encodeCacheHintFinalize:(uint64_t)tag resourceGroups:(const id __unsafe_unretained [])groups count:(NSUInteger)count {(void)tag;(void)groups;(void)count;}
// The kernel driver's own submission times are not recorded: 0 is Metal's answer where they are unavailable.
- (CFTimeInterval)kernelStartTime {return 0;}
- (CFTimeInterval)kernelEndTime {return 0;}
- (BOOL)commitAndWaitUntilSubmitted {[(id<MTLCommandBuffer>)self commit];[self waitUntilScheduled];return !self.failedLocally;}
- (CFTimeInterval)GPUStartTime {return nmb_command_buffer_gpu_time(_handle,false);}
- (CFTimeInterval)GPUEndTime {return nmb_command_buffer_gpu_time(_handle,true);}
- (id)encoder:(NBEncoder *)encoder handle:(NmbEncoder *)handle what:(NSString *)what {
 if(self.committed){[self refuse:[what stringByAppendingString:@" after commit"]];return nil;}
 // RenderBox on 22H374 caches the encoder's IMPs with methodForSelector:.
 // A nil result becomes a null function call there. Keep the encoder object
 // while preserving the failure: null runtime handles reject every operation,
 // and encodeFailure completes this command buffer with status Error.
 if(!handle){[self refuse:[NSString stringWithFormat:@"%@: %@",what,lastError()]];encoder.commandBuffer=self;return encoder;}
 encoder.handle=handle;encoder.commandBuffer=self;return encoder;
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoder {
 return [self encoder:[NBComputeEncoder new] handle:self.committed?nullptr:nmb_compute_encoder_new(_handle) what:@"computeCommandEncoder"];
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)type {
 if(type!=MTLDispatchTypeSerial){[self refuse:@"concurrent compute dispatch"];return nil;}
 return [self computeCommandEncoder];
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoder {
 gBlits++;encodedWork.blits++;
 return [self encoder:[NBBlitEncoder new] handle:self.committed?nullptr:nmb_blit_encoder_new(_handle) what:@"blitCommandEncoder"];
}
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d {
 NmbRenderPassDescriptor pass={};
 std::vector<SingleSampleCopy> resolveCopies;
 {
  id<MTLTexture> first=d.colorAttachments[0].texture;
  if(first) {
   const uint64_t pixels=(uint64_t)first.width*first.height;
   gPasses++;gPassPixels+=pixels;encodedWork.passes++;encodedWork.pixels+=pixels;
   uint64_t seen=gBiggestPass.load();while(pixels>seen && !gBiggestPass.compare_exchange_weak(seen,pixels)){}
   if(pixels>=2000000)gFullScreenPasses++;
  }
 }
 unsigned traceAttachment=0;
 auto attachment=[&](MTLRenderPassAttachmentDescriptor *a,NmbPassAttachment &out)->bool {
  const unsigned attachmentIndex=traceAttachment++;
  if(!a.texture)return true;
  if(![(id)a.texture isKindOfClass:[NBTexture class]])return false;
  NBTexture *texture=(NBTexture *)a.texture;if(texture.surface)[_surfaces addObject:texture];
  [_resourceOwners addObject:texture];[self traceTexture:texture usage:4];
  const size_t copiesBefore=resolveCopies.size();
  out.texture=texture.handle;out.level=a.level;out.slice=a.slice;out.depth_plane=a.depthPlane;out.load=a.loadAction;out.store=a.storeAction;
  if(a.resolveTexture) {
   if(![(id)a.resolveTexture isKindOfClass:[NBTexture class]])return false;
   NBTexture *resolve=(NBTexture *)a.resolveTexture;
   [_resourceOwners addObject:resolve];[self traceTexture:resolve usage:4];if(resolve.surface)[_surfaces addObject:resolve];
   const bool resolves=a.storeAction==MTLStoreActionMultisampleResolve || a.storeAction==MTLStoreActionStoreAndMultisampleResolve;
   if(singleSample()) {
    if(resolves) {
     if(a.loadAction!=MTLLoadActionLoad && a.storeAction==MTLStoreActionMultisampleResolve) {
      // The source is discarded, so draw into the final attachment directly.
      out.texture=resolve.handle;out.level=a.resolveLevel;out.slice=a.resolveSlice;out.depth_plane=a.resolveDepthPlane;
     } else {
      // Load and StoreAndResolve retain the source's contents across passes.
      const uint64_t width=std::max<uint64_t>(1,texture.width>>a.level),height=std::max<uint64_t>(1,texture.height>>a.level);
      const uint64_t layers=d.renderTargetArrayLength?d.renderTargetArrayLength:1;
      for(uint64_t layer=0;layer<layers;layer++)
       resolveCopies.push_back({texture.handle,resolve.handle,a.level,a.slice+layer,a.resolveLevel,a.resolveSlice+layer,
          {0,0,a.depthPlane},{0,0,a.resolveDepthPlane},{width,height,1}});
     }
     out.store=MTLStoreActionStore;
    }
   } else {
    out.resolve_texture=resolve.handle;out.resolve_level=a.resolveLevel;
    out.resolve_slice=a.resolveSlice;out.resolve_depth_plane=a.resolveDepthPlane;
   }
  }

  [self traceTexture:texture usage:out.texture==texture.handle?(a.loadAction==MTLLoadActionLoad?3:2):0];
  if(a.resolveTexture) {
   NBTexture *resolve=(NBTexture *)a.resolveTexture;
   if(out.texture==resolve.handle || out.resolve_texture==resolve.handle || resolveCopies.size()>copiesBefore)
    [self traceTexture:resolve usage:2];
  }
  frameEvent(33,traceNumber,encodedWork.passes,attachmentIndex,(uintptr_t)texture.handle,(uintptr_t)out.texture,
      (uintptr_t)out.resolve_texture,a.texture.sampleCount,texture.descriptor.samples,a.loadAction,a.storeAction,out.load,out.store);
  frameEvent(34,traceNumber,encodedWork.passes,attachmentIndex,(uintptr_t)[(NBTexture *)a.resolveTexture handle],
      a.texture.pixelFormat,a.resolveTexture.pixelFormat,texture.descriptor.swizzle,resolveCopies.size(),
      singleSample(),a.level,a.slice,a.depthPlane);
  return true;
 };
 for(unsigned i=0;i<8;i++) {
  MTLRenderPassColorAttachmentDescriptor *c=d.colorAttachments[i];
  if(!attachment(c,pass.colors[i])){[self refuse:@"foreign color attachment"];return nil;}
  pass.colors[i].clear[0]=c.clearColor.red;pass.colors[i].clear[1]=c.clearColor.green;pass.colors[i].clear[2]=c.clearColor.blue;pass.colors[i].clear[3]=c.clearColor.alpha;
 }
 if(!attachment(d.depthAttachment,pass.depth) || !attachment(d.stencilAttachment,pass.stencil)){[self refuse:@"foreign depth/stencil attachment"];return nil;}
 pass.depth.clear[0]=d.depthAttachment.clearDepth;pass.stencil.clear[0]=d.stencilAttachment.clearStencil;
 pass.depth.resolve_filter=d.depthAttachment.depthResolveFilter;pass.stencil.resolve_filter=d.stencilAttachment.stencilResolveFilter;
 pass.width=d.renderTargetWidth;pass.height=d.renderTargetHeight;pass.array_length=d.renderTargetArrayLength;pass.samples=singleSample() && d.defaultRasterSampleCount?1:d.defaultRasterSampleCount;
 if(d.visibilityResultBuffer) {
  if(![(id)d.visibilityResultBuffer isKindOfClass:[NBBuffer class]]){[self refuse:@"foreign visibility result buffer"];return nil;}
  [_resourceOwners addObject:d.visibilityResultBuffer];
  pass.visibility=[(NBBuffer *)d.visibilityResultBuffer handle];
 }
 id<MTLRenderCommandEncoder> made=[self encoder:[NBRenderEncoder new] handle:self.committed?nullptr:nmb_render_encoder_new(_handle,&pass) what:@"renderCommandEncoder"];
 if(made) {
  ((NBRenderEncoder *)made).resolveCopies=std::move(resolveCopies);
  id<MTLTexture> t0=d.colorAttachments[0].texture;
  const uint64_t big=t0 && (uint64_t)t0.width*t0.height>=2000000;
  ((NBRenderEncoder *)made).passKey=(t0?(uint64_t)t0.pixelFormat:0)|((uint64_t)d.colorAttachments[0].loadAction<<16)|
   (uint64_t)([(id)t0 isKindOfClass:[NBTexture class]] && ((NBTexture *)t0).surface?1:0)<<18|big<<19;
 }
 return made;
}
- (void)addScheduledHandler:(MTLCommandBufferHandler)block {if(block)@synchronized(self){[_scheduledHandlers addObject:[block copy]];}}
- (void)addCompletedHandler:(MTLCommandBufferHandler)block {if(block)@synchronized(self){[_completedHandlers addObject:[block copy]];}}
// A drawable (RenderBox's CAMetalLayer path) is presented once the GPU has written it: this runtime's scheduling comes before completion.
- (void)presentDrawable:(id<MTLDrawable>)drawable {[self addCompletedHandler:^(id<MTLCommandBuffer>){[drawable present];}];}
- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)time {[self addCompletedHandler:^(id<MTLCommandBuffer>){[drawable presentAtTime:time];}];}
- (void)enqueue {}
- (void)event:(id<MTLEvent>)event value:(uint64_t)value signal:(bool)signal {
 if(![(id)event isKindOfClass:[NBEvent class]]){[self refuse:@"foreign event"];return;}
 if(self.committed){[self refuse:@"event encoded after commit"];return;}
 frameEvent(32,traceNumber,(uintptr_t)[(NBEvent *)event handle],value,signal);
 NmbEvent *handle=[(NBEvent *)event handle];
 if(!(signal?nmb_command_buffer_signal_event(_handle,handle,value):nmb_command_buffer_wait_event(_handle,handle,value)))
  [self refuse:[NSString stringWithFormat:@"%s: %@",signal?"encodeSignalEvent":"encodeWaitForEvent",lastError()]];
}
- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value {[self event:event value:value signal:true];}
- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value {[self event:event value:value signal:false];}
// The runtime has submitted the work. Deliver scheduling without waiting for GPU completion.
- (void)didSchedule {
 [self tracePhase:2];
 {
  const uint64_t took=nowUs()-self.committedUs;
  std::lock_guard<std::mutex> lock(gCommitLock);
  CommitRecord &r=gCommitRing[self.commitIndex%32];
  if(r.index==self.commitIndex)r.scheduledUs=took;
 }
 NSArray *handlers;
 @synchronized(self){
  _schedulingDelivered=YES;handlers=[_scheduledHandlers copy];[_scheduledHandlers removeAllObjects];
 }
 void (^run)(void)=^{
  for(MTLCommandBufferHandler block in handlers)block((id<MTLCommandBuffer>)self);
  dispatch_group_leave(self->_schedulingHandlers);
 };
 // Run on the runtime's scheduled-handler thread, preserving its ordering and own-wait
 // deadlock checks. The queue's completionQueue routes completed handlers only.
 run();
}
// After completion (or a refusal): invalidate the CPU's view of imported surfaces, then completed handlers, once.
- (void)finish {
 // Genuine runtime completion precedes this cache acquire, and writer
 // ownership/display callbacks follow it. Imported resources stay retained
 // across both phases. Nonparticipating resources keep kernel maintenance.
 const uint64_t syncBegan=nowUs();HostRanges kernelSynced;
 if(!self.failedLocally && self.status==MTLCommandBufferStatusCompleted &&
    !syncActiveImports(_surfaces,_resourceOwners,1,kernelSynced)) {
  self.completionSyncFailed=YES;self.encodeFailure=@"completed host-import acquire failed";
 }
 const uint64_t flushBegan=nowUs();flushSurfaces(_surfaces,kernelSynced);
 frameEvent(61,traceNumber,1,flushBegan-syncBegan,nowUs()-flushBegan,_surfaces.count,_resourceOwners.count,kernelSynced.size());
 [self tracePhase:3];
 if(!self.failedLocally && self.status==MTLCommandBufferStatusCompleted)captureFrameSurfaces(self);
 pongsurface::complete(displayHandoffs,self.completionSyncFailed || (!self.failedLocally && self.status!=MTLCommandBufferStatusCompleted));
 // Actual GPU completion (or a local refusal that never submitted) is the
 // release point. A retained completed command must not pin every old frame.
 [_surfaces removeAllObjects];[_resourceOwners removeAllObjects];
 @synchronized(gInFlight){[gInFlight removeObject:self];}
 NSArray *completedHandlers;
 @synchronized(self){
  // A frontend refusal or a failed submission never scheduled. Unblock its waiter without inventing a scheduled event.
  if(!_schedulingDelivered){_schedulingDelivered=YES;[_scheduledHandlers removeAllObjects];dispatch_group_leave(_schedulingHandlers);}
  completedHandlers=[_completedHandlers copy];[_completedHandlers removeAllObjects];
 }
 void (^run)(void)=^{
  for(MTLCommandBufferHandler block in completedHandlers)block((id<MTLCommandBuffer>)self);
  dispatch_group_leave(self->_handlers);
 };
 dispatch_queue_t queue=_queue.completionQueue;
 if(queue)dispatch_async(queue,run);else run();
}
- (void)commit {
 if(self.committed){NBLOG(@"native-metal: commit twice");return;}
 [self tracePhase:1];
 self.committed=YES;
 self.committedUs=nowUs();
 {
  std::lock_guard<std::mutex> lock(gCommitLock);
  // Assign once under the same lock as publication. A concurrent commit cannot reuse this index.
  const uint32_t index=(uint32_t)(gCommits.fetch_add(1)+1);self.commitIndex=index;
  CommitRecord &r=gCommitRing[index%32];
  r={};r.index=index;r.committedUs=self.committedUs;r.ageUs=self.committedUs-createdUs;r.work=encodedWork;
 }
 dispatch_group_enter(_handlers);
 dispatch_group_enter(_schedulingHandlers);
 @synchronized(gInFlight){[gInFlight addObject:self];}
 if(!self.encodeFailure && !pongsurface::begin(_surfaces,displayHandoffs))
  [self refuse:@"IOSurface CPU/GPU ownership handoff unavailable"];
 const uint64_t syncBegan=nowUs();
 HostRanges kernelSynced;
 if(!self.encodeFailure && !syncActiveImports(_surfaces,_resourceOwners,2,kernelSynced))
  [self refuse:@"explicit imported-page publication failed"];
 if(!self.encodeFailure) {
  const uint64_t flushBegan=nowUs();flushSurfaces(_surfaces,kernelSynced);
  frameEvent(61,traceNumber,2,flushBegan-syncBegan,nowUs()-flushBegan,_surfaces.count,_resourceOwners.count,kernelSynced.size());
  // Completion owns this reference; the runtime orders the non-owning scheduled callback before it.
  void *context=(__bridge_retained void *)self;
  if(nmb_command_buffer_on_completed(_handle,completed,context) &&
     nmb_command_buffer_on_scheduled(_handle,scheduled,context) && nmb_command_buffer_commit(_handle))return;
  (void)CFBridgingRelease(context);   // refused registration/commit cannot call the registered handlers
  [self refuse:[@"commit: " stringByAppendingString:lastError()]];
 }
 self.failedLocally=YES;gCommitFailures++;
 // This thread waits for real GPU completion and delivers Metal handlers.
 // Match the runtime's iOS user-interactive completion policy; cache cleanup
 // remains on its separate utility queue.
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE,0),^{[self finish];});
}
- (void)waitUntilCompleted {
 if(!self.committed)return;
 const uint64_t began=nowUs();
 if(!self.failedLocally)nmb_command_buffer_wait(_handle,0);
 dispatch_group_wait(_handlers,DISPATCH_TIME_FOREVER);
 gTimeWaitCompleted.add(began);
}
- (void)waitUntilScheduled {
 if(!self.committed)return;
 const uint64_t began=nowUs();
 if(!self.failedLocally && !nmb_command_buffer_wait_scheduled(_handle)){
  NBLOG(@"native-metal: waitUntilScheduled refused: %@",lastError());return;
 }
 dispatch_group_wait(_schedulingHandlers,DISPATCH_TIME_FOREVER);
 gTimeWaitScheduled.add(began);
}
@end
static void scheduled(void *context) {[(__bridge NBCommandBuffer *)context didSchedule];}
// The runtime's completion: the command buffer's own reference, taken at commit, is given back here.
static void completed(void *context) {
 gCompleted++;
 NBCommandBuffer *commandBuffer=(__bridge_transfer NBCommandBuffer *)context;
 const uint64_t callbackBegan=nowUs();
 int64_t scheduledUs=-1;
 {
  const uint64_t took=nowUs()-commandBuffer.committedUs;
  {
   const bool frame=commandBuffer->encodedWork.pixels>=2000000;
   histAdd(frame?gHistFrameGpu:gHistOtherGpu,took);
   if(frame)histAdd(gHistFrameEncode,commandBuffer.committedUs-commandBuffer->createdUs);
  }
  gTimeEncodeToCommit.count++;gTimeEncodeToCommit.total+=took;
  uint64_t seen=gTimeEncodeToCommit.slowest.load();while(took>seen && !gTimeEncodeToCommit.slowest.compare_exchange_weak(seen,took)){}
  std::lock_guard<std::mutex> lock(gCommitLock);
  CommitRecord &r=gCommitRing[commandBuffer.commitIndex%32];
  if(r.index==commandBuffer.commitIndex){r.callbackUs=took;scheduledUs=r.scheduledUs;}
 }
 [commandBuffer finish];
 // Bounded real frame records complement the kernel's CP start/end stamps.
 // Include completion-side ownership/cache work, which precedes presentation.
 const uint64_t finished=nowUs();
 static std::atomic<uint64_t> nextSlowLog{0};
 uint64_t next=nextSlowLog.load();
 if(finished-commandBuffer.committedUs>=33000 && finished>=next &&
    nextSlowLog.compare_exchange_strong(next,finished+250000)) {
  NSLog(@"Pong Metal slow: pid=%d commit=%u scheduled_us=%lld callback_us=%llu finish_us=%llu passes=%u draws=%u pixels=%llu fg=%u bg=%u",
   getpid(),commandBuffer.commitIndex,(long long)scheduledUs,
   (unsigned long long)(callbackBegan-commandBuffer.committedUs),(unsigned long long)(finished-callbackBegan),
   commandBuffer->encodedWork.passes,commandBuffer->encodedWork.draws,(unsigned long long)commandBuffer->encodedWork.pixels,
   gPriorityForeground.load(),gPriorityBackground.load());
  uint64_t stages[12]={};
  if(nmb_command_buffer_stage_summary(commandBuffer.handle,stages)) {
   auto elapsed=[&](unsigned end,unsigned begin)->long long {
    return stages[begin] && stages[end]>=stages[begin]?static_cast<long long>((stages[end]-stages[begin])/1000):-1;
   };
   NSLog(@"Pong GPU stages: pid=%d commit=%u run_us=%lld prepare_us=%lld hand_us=%lld inbox_us=%lld held_us=%lld submit_us=%lld poll_us=%lld seen_us=%lld held_mask=0x%llx",
    getpid(),commandBuffer.commitIndex,elapsed(1,0),elapsed(2,1),elapsed(3,2),elapsed(4,3),elapsed(5,4),
    elapsed(6,5),elapsed(8,7),elapsed(9,8),(unsigned long long)stages[11]);
  }
 }
 // Release IOSurface writer ownership before optional cache eviction. BO
 // destruction can wait for retirement and must not obstruct the completion
 // callback which the display is currently waiting for during wake.
 static std::atomic<bool> pruning{false};
 if(!pruning.exchange(true))dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
  trimHeldImports(false);pruning.store(false);
 });
}

@implementation NBQueue
- (instancetype)init {
 if((self=[super init])) {
  std::lock_guard<std::mutex> lock(gPriorityLock);
  gQueuePriorities.add(_priority);auto p=gQueuePriorities.aggregate();forwardGpuPriority(p.foreground,p.background);
 }
 return self;
}
- (void)dealloc {
 nmb_queue_release(_handle);
 std::lock_guard<std::mutex> lock(gPriorityLock);
 gQueuePriorities.remove(_priority);auto p=gQueuePriorities.aggregate();forwardGpuPriority(p.foreground,p.background);
}
- (id<MTLCommandBuffer>)commandBuffer {
 NmbCommandBuffer *handle=nmb_command_buffer_new(_handle);
 if(!handle){NBLOG(@"native-metal: commandBuffer refused: %@",lastError());return nil;}
 NBCommandBuffer *commandBuffer=[NBCommandBuffer new];commandBuffer.handle=handle;commandBuffer.queue=self;return (id<MTLCommandBuffer>)commandBuffer;
}
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences {return [self commandBuffer];}
- (void)setSubmissionQueue:(dispatch_queue_t)queue {(void)queue;}   // submission happens on the caller's thread here
// setCompletionQueue: is the property's setter.
// Forward the current aggregate for this process's shared Vulkan queue.
- (BOOL)updatePriority:(NSUInteger)value background:(BOOL)background {
 if(value>5 || !NBQueuePriorities::valid((uint32_t)value,background))return NO;
 std::lock_guard<std::mutex> lock(gPriorityLock);
 const auto old=_priority;gQueuePriorities.remove(old);
 if(background)_priority.background=(uint32_t)value;else _priority.foreground=(uint32_t)value;
 gQueuePriorities.add(_priority);const auto effective=gQueuePriorities.aggregate();
 const bool accepted=forwardGpuPriority(effective.foreground,effective.background);
 frameEvent(55,(uintptr_t)_handle,value,background,effective.foreground,effective.background,accepted);
 if(accepted)return YES;
 gQueuePriorities.remove(_priority);_priority=old;gQueuePriorities.add(old);return NO;
}
- (BOOL)setGPUPriority:(NSUInteger)priority {return [self updatePriority:priority background:NO];}
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority {return [self updatePriority:priority background:YES];}
- (NSUInteger)getGPUPriority {std::lock_guard<std::mutex> lock(gPriorityLock);return _priority.foreground;}
- (NSUInteger)getBackgroundGPUPriority {std::lock_guard<std::mutex> lock(gPriorityLock);return _priority.background;}
- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)d {(void)d;return [self commandBuffer];}
@end

// ---- the device ----
template <typename T> static T failWith(NSError **error,NSString *what,T value) {
 noteProblem([NSString stringWithFormat:@"%@: %@",what,lastError()]);
 if(error)*error=nbError([NSString stringWithFormat:@"%@: %@",what,lastError()]);
 return value;
}
// Which of QuartzCore's paths asked for a pipeline (int-153: every one was made synchronously on its render thread and no generic pipeline
// was ever asked for): its return addresses as offsets from its image header (header 0x189429000 in the 21H461 cache, so offset +
// 0x189429000 is the address in that image's disassembly), the global feature flags it ORs into every context's flags, and its
// CA_DISABLE_GENERIC_SHADERS, CA_FORCE_GENERIC_SHADERS and CA_DISABLE_PRECOMPILED_PIPELINES debug options (ca_debug_options +0x55..0x57,
// read only where dladdr places that address in QuartzCore).
static std::string quartzCoreCallers() {
 void *frames[24];const int n=backtrace(frames,24);
 std::string out="qc";char item[32];unsigned kept=0;uintptr_t header=0;
 for(int i=0;i<n && kept<8;i++) {
  Dl_info info{};
  if(!dladdr(frames[i],&info) || !info.dli_fname || !strstr(info.dli_fname,"/QuartzCore"))continue;
  header=(uintptr_t)info.dli_fbase;
  snprintf(item,sizeof item," %lx",(unsigned long)((uintptr_t)frames[i]-header));out+=item;++kept;
 }
 // Pong20G81 has a different QuartzCore layout. Do not read Sheng21H461
 // context fields or debug globals using private offsets from that system.
 static auto global=(uint32_t (*)(void))dlsym(RTLD_DEFAULT,"CARenderGetGlobalFeatureFlags");
 snprintf(item,sizeof item," global=%x",global?global():0xffffffffu);out+=item;
 return out;
}
// What a creation cost beyond its wall time (int-156: a translation the host does in 6 ms took 665 ms on the tablet, whose single core is
// as fast): this thread's CPU time (user and system, microseconds) and the task's page faults, page-ins and context switches. A creation
// whose CPU time is near its wall time computed; one whose faults are many paid the 4 KiB fault path; one far below it waited.
struct NBCost {
 uint64_t user=0,system=0;int64_t faults=0,pageins=0,switches=0;
 static NBCost now() {
  NBCost c;thread_basic_info_data_t t{};mach_msg_type_number_t n=THREAD_BASIC_INFO_COUNT;mach_port_t self=mach_thread_self();
  if(thread_info(self,THREAD_BASIC_INFO,(thread_info_t)&t,&n)==KERN_SUCCESS) {
   c.user=(uint64_t)t.user_time.seconds*1000000+t.user_time.microseconds;c.system=(uint64_t)t.system_time.seconds*1000000+t.system_time.microseconds;
  }
  mach_port_deallocate(mach_task_self(),self);
  task_events_info_data_t e{};n=TASK_EVENTS_INFO_COUNT;
  if(task_info(mach_task_self(),TASK_EVENTS_INFO,(task_info_t)&e,&n)==KERN_SUCCESS){c.faults=e.faults;c.pageins=e.pageins;c.switches=e.csw;}
  return c;
 }
 std::string since(const NBCost &b) const {
  char text[128];snprintf(text,sizeof text," cpu=%llu sys=%llu flt=%lld pin=%lld csw=%lld",(unsigned long long)(user+system-b.user-b.system),
      (unsigned long long)(system-b.system),(long long)(faults-b.faults),(long long)(pageins-b.pageins),(long long)(switches-b.switches));
  return text;
 }
};
// One journal line of a render pipeline creation begun at `began` (microseconds), `refused` its refusal or null. The label is the
// caller's name for the pipeline (QuartzCore's: its spec's encoded name).
static void journalRenderPipeline(MTLRenderPipelineDescriptor *d,uint64_t began,const char *refused,const std::string &cost) {
 if(gJournalPath.empty() || (!gJournalVerbose && !refused))return;
 const uint64_t ended=nowUs();uint64_t tid=0;pthread_threadid_np(nullptr,&tid);
 char head[256];
 snprintf(head,sizeof head,"R t=%llu tid=%llu main=%d us=%llu ok=%d samples=%lu topo=%lu ds=%lu/%lu",(unsigned long long)((began-gJournalBegan)/1000),
     (unsigned long long)tid,pthread_main_np(),(unsigned long long)(ended-began),refused?0:1,(unsigned long)d.rasterSampleCount,
     (unsigned long)d.inputPrimitiveTopology,(unsigned long)d.depthAttachmentPixelFormat,(unsigned long)d.stencilAttachmentPixelFormat);
 std::string line=head;
 line+=cost;
 line+=" label=";line+=d.label.length?d.label.UTF8String:"-";
 for(unsigned i=0;i<8;i++) {
  MTLRenderPipelineColorAttachmentDescriptor *a=d.colorAttachments[i];if(a.pixelFormat==MTLPixelFormatInvalid)continue;
  snprintf(head,sizeof head," c%u=%lu:%lu:%d:%lu:%lu:%lu:%lu:%lu:%lu",i,(unsigned long)a.pixelFormat,(unsigned long)a.writeMask,(int)a.blendingEnabled,
      (unsigned long)a.sourceRGBBlendFactor,(unsigned long)a.destinationRGBBlendFactor,(unsigned long)a.rgbBlendOperation,(unsigned long)a.sourceAlphaBlendFactor,
      (unsigned long)a.destinationAlphaBlendFactor,(unsigned long)a.alphaBlendOperation);
  line+=head;
 }
 if(MTLVertexDescriptor *v=d.vertexDescriptor)for(unsigned i=0;i<31;i++) {
  MTLVertexAttributeDescriptor *a=v.attributes[i];if(a.format==MTLVertexFormatInvalid)continue;
  MTLVertexBufferLayoutDescriptor *l=v.layouts[a.bufferIndex<31?a.bufferIndex:0];
  snprintf(head,sizeof head," a%u=%lu:%lu:%lu:%lu:%lu:%lu",i,(unsigned long)a.format,(unsigned long)a.offset,(unsigned long)a.bufferIndex,(unsigned long)l.stride,
      (unsigned long)l.stepFunction,(unsigned long)l.stepRate);
  line+=head;
 }
 NBFunction *vf=[(id)d.vertexFunction isKindOfClass:[NBFunction class]]?(NBFunction *)d.vertexFunction:nil;
 NBFunction *ff=[(id)d.fragmentFunction isKindOfClass:[NBFunction class]]?(NBFunction *)d.fragmentFunction:nil;
 line+=" | v ";line+=vf?vf.name.UTF8String:"-";line+=" ";line+=vf?vf.constantsText:"";
 line+=" | f ";line+=ff?ff.name.UTF8String:"-";line+=" ";line+=ff?ff.constantsText:"";
 if(refused){line+=" | refused ";line+=refused;}
 line+=" | ";line+=quartzCoreCallers();
 for(char &c:line)if(c=='\n')c=' ';
 journal(line);
}
// A render pipeline descriptor in the bridge's form; false, with the error, for a function of another device.
static bool renderDescriptor(MTLRenderPipelineDescriptor *d,NmbRenderPipelineDescriptor *p,NSError **error) {
 if(![(id)d.vertexFunction isKindOfClass:[NBFunction class]] || (d.fragmentFunction && ![(id)d.fragmentFunction isKindOfClass:[NBFunction class]])) {
  if(error)*error=nbError(@"foreign function");return false;
 }
 p->vertex=[(NBFunction *)d.vertexFunction handle];p->fragment=d.fragmentFunction?[(NBFunction *)d.fragmentFunction handle]:nullptr;
 for(unsigned i=0;i<8;i++) {
  MTLRenderPipelineColorAttachmentDescriptor *a=d.colorAttachments[i];
  p->colors[i]={(uint64_t)a.pixelFormat,(uint64_t)a.writeMask,(uint64_t)a.blendingEnabled,(uint64_t)a.sourceRGBBlendFactor,(uint64_t)a.destinationRGBBlendFactor,
      (uint64_t)a.rgbBlendOperation,(uint64_t)a.sourceAlphaBlendFactor,(uint64_t)a.destinationAlphaBlendFactor,(uint64_t)a.alphaBlendOperation};
 }
 p->depth_format=d.depthAttachmentPixelFormat;p->stencil_format=d.stencilAttachmentPixelFormat;p->samples=singleSample()?1:d.rasterSampleCount;
 p->topology=d.inputPrimitiveTopology;p->rasterization=d.rasterizationEnabled;
 if(MTLVertexDescriptor *v=d.vertexDescriptor) {
  for(unsigned i=0;i<31;i++) {
   MTLVertexAttributeDescriptor *a=v.attributes[i];if(a.format==MTLVertexFormatInvalid)continue;
   p->vertex_inputs=1;p->attributes[i]={(uint64_t)a.format,a.offset,a.bufferIndex};
   if(a.bufferIndex<31){MTLVertexBufferLayoutDescriptor *l=v.layouts[a.bufferIndex];p->layouts[a.bufferIndex]={l.stride,(uint64_t)l.stepFunction,l.stepRate};}
  }
 }
 return true;
}

// ---- heaps ----
// Metal heaps over the runtime's: an automatic heap places each resource, a placement heap takes an offset. Resources made from one keep it
// alive (their `heap`); a heap's resources share its storage mode, CPU cache mode and (here) hazard tracking mode.
@implementation NBHeap
- (void)dealloc {nmb_heap_release(_handle);}
- (NSUInteger)size {return nmb_heap_size(_handle);}
- (NSUInteger)usedSize {return nmb_heap_used_size(_handle);}
- (NSUInteger)currentAllocatedSize {return nmb_heap_allocated_size(_handle);}
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment {return nmb_heap_max_available_size(_handle,alignment);}
- (MTLResourceOptions)resourceOptions {return _options;}
- (MTLStorageMode)storageMode {return (MTLStorageMode)((_options&MTLResourceStorageModeMask)>>MTLResourceStorageModeShift);}
- (MTLCPUCacheMode)cpuCacheMode {return (MTLCPUCacheMode)((_options&MTLResourceCPUCacheModeMask)>>MTLResourceCPUCacheModeShift);}
- (MTLHazardTrackingMode)hazardTrackingMode {return (MTLHazardTrackingMode)((_options&MTLResourceHazardTrackingModeMask)>>MTLResourceHazardTrackingModeShift);}
- (MTLHeapType)type {return _heapType;}
- (uint64_t)protectionOptions {return 0;}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
 const uint64_t answer=nmb_heap_set_purgeable_state(_handle,state);
 if(!answer)NBLOG(@"native-metal: heap setPurgeableState refused: %@",lastError());
 return (MTLPurgeableState)answer;
}
- (id<MTLBuffer>)bufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(uint64_t)offset {
 NmbBuffer *handle=nmb_heap_new_buffer(_handle,length,options,offset);
 if(!handle){NBLOG(@"native-metal: heap buffer refused: %@",lastError());return nil;}
 NBBuffer *buffer=[NBBuffer new];buffer.handle=handle;buffer.options=options;buffer.parentHeap=(id<MTLHeap>)self;
 buffer.heapOffset=offset==UINT64_MAX?0:offset;return (id<MTLBuffer>)buffer;
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {return [self bufferWithLength:length options:options offset:UINT64_MAX];}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(NSUInteger)offset {
 return [self bufferWithLength:length options:options offset:offset];
}
- (id<MTLTexture>)textureWithDescriptor:(MTLTextureDescriptor *)d offset:(uint64_t)offset {
 NmbTextureDescriptor descriptor;if(!textureDescriptor(d,&descriptor))return nil;
 NmbTexture *handle=nmb_heap_new_texture(_handle,&descriptor,offset);
 if(!handle){NBLOG(@"native-metal: heap texture refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;
 frameEvent(49,(uintptr_t)handle,d.sampleCount,descriptor.samples,d.pixelFormat,descriptor.format,d.width,d.height,packedSwizzle(d.swizzle));
 texture.parentHeap=(id<MTLHeap>)self;texture.heapOffset=offset==UINT64_MAX?0:offset;return (id<MTLTexture>)texture;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d {return [self textureWithDescriptor:d offset:UINT64_MAX];}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset {return [self textureWithDescriptor:d offset:offset];}
@end
// Apple's SPI resource groups name resources for cache hints (encodeCacheHintTag:/Finalize:); a group only keeps its resources alive.
@interface NBResourceGroup : NBObject
@property(nonatomic,strong) NSArray *resources;
@end
@implementation NBResourceGroup
@end

// Creation and archive population must use the same complete supported descriptor.
static bool computeDescriptor(MTLComputePipelineDescriptor *d,NmbComputePipelineDescriptor *p,NSError **error) {
 if(!d || ![(id)d.computeFunction isKindOfClass:[NBFunction class]]){if(error)*error=nbError(@"foreign compute function");return false;}
 // Metal may materialize empty stage/linked descriptors on access. Preserve the
 // default meaning while refusing state this frontend cannot yet represent.
 MTLStageInputOutputDescriptor *stage=d.stageInputDescriptor;
 bool empty=!stage || (stage.indexType==MTLIndexTypeUInt16 && stage.indexBufferIndex==0);
 for(NSUInteger index=0;stage && index<31;++index) {
  MTLAttributeDescriptor *a=stage.attributes[index];MTLBufferLayoutDescriptor *b=stage.layouts[index];
  empty=empty && a.format==MTLAttributeFormatInvalid && !a.offset && !a.bufferIndex && !b.stride &&
        b.stepFunction==MTLStepFunctionPerVertex && b.stepRate==1;
 }
 if(!empty){if(error)*error=nbError(@"compute stage inputs are not implemented");return false;}
 MTLLinkedFunctions *linked=d.linkedFunctions;
 if(linked.functions.count || linked.binaryFunctions.count || linked.privateFunctions.count || linked.groups.count ||
    d.preloadedLibraries.count || d.insertLibraries.count) {
  if(error)*error=nbError(@"compute linked/dynamic libraries are not implemented");return false;
 }
 p->function=[(NBFunction *)d.computeFunction handle];p->max_threads=d.maxTotalThreadsPerThreadgroup;
 p->multiple_of_execution_width=d.threadGroupSizeIsMultipleOfThreadExecutionWidth;p->indirect=d.supportIndirectCommandBuffers;
 p->max_call_stack_depth=d.maxCallStackDepth;p->adding_binary_functions=d.supportAddingBinaryFunctions;
 for(NSUInteger index=0;index<31;++index)p->mutability[index]=d.buffers[index].mutability;
 return true;
}

// ---- binary archives ----
@interface NBBinaryArchive : NBObject
@property(nonatomic) NmbBinaryArchive *handle;
@end
@implementation NBBinaryArchive
- (void)dealloc {nmb_binary_archive_release(_handle);}
- (NSUInteger)options {return 0;}
- (BOOL)addRenderPipelineFunctionsWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {
 auto *p=new NmbRenderPipelineDescriptor();
 const bool ok=renderDescriptor(d,p,error) && nmb_binary_archive_add_render(_handle,p);delete p;
 if(!ok)return failWith<BOOL>(error,@"archive render pipeline",NO);
 if(error)*error=nil;return YES;
}
- (BOOL)addComputePipelineFunctionsWithDescriptor:(MTLComputePipelineDescriptor *)d error:(NSError **)error {
 NmbComputePipelineDescriptor p={};if(!computeDescriptor(d,&p,error))return NO;
 if(!nmb_binary_archive_add_compute_descriptor(_handle,&p))return failWith<BOOL>(error,@"archive compute pipeline",NO);
 if(error)*error=nil;return YES;
}
- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)error {
 if(!url.isFileURL){if(error)*error=nbError(@"a file URL is required");return NO;}
 if(!nmb_binary_archive_serialize(_handle,url.fileSystemRepresentation))return failWith<BOOL>(error,@"archive serialize",NO);
 if(error)*error=nil;return YES;
}
@end

static bool pipelineArchives(NSArray *list,std::vector<NmbBinaryArchive *> &archives,NSError **error) {
 for(id archive in list) {
  if(![archive isKindOfClass:[NBBinaryArchive class]]){if(error)*error=nbError(@"foreign binary archive");return false;}
  archives.push_back([(NBBinaryArchive *)archive handle]);
 }
 return true;
}

@interface NBDeviceMethods : NSObject
@end
@implementation NBDeviceMethods
- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(NSUInteger)format {
 return ((NSUInteger (*)(id,SEL,NSUInteger))objc_msgSend)(self,sel_registerName("minimumLinearTextureAlignmentForPixelFormat:"),format);
}
- (NSString *)name {return gName;}
- (uint64_t)registryID {return gRegistryID;}
- (NSUInteger)maxBufferLength {return nmb_device_max_buffer_length(gDevice);}
- (BOOL)hasUnifiedMemory {return YES;}
// Core Image's CIMetalDeviceGetLimits reads both (Apple's GPU devices answer them; _MTLDevice does not): without them every CIContext
// creation threw and iconservicesagent aborted while rasterizing symbol glyphs. The GPU has no memory of its own; it shares the system's.
- (uint64_t)dedicatedMemorySize {return 0;}
- (uint64_t)sharedMemorySize {return [NSProcessInfo processInfo].physicalMemory;}
// AdrenoMetal's CoreImage/MPS query, adapted to the four SP/CCU blocks of
// SM8475's A730 instead of the A740 board's eight-core presentation.
- (NSUInteger)gpuCoreCount {return 4;}
- (BOOL)isLowPower {return NO;}
- (BOOL)isRemovable {return NO;}
- (BOOL)isHeadless {return NO;}
// supportsFamily: and the other queries the runtime answers are installed from it (installAnswers); supportsFamily: also answers the target
// hardware's families (the bridge's TARGET_FAMILIES, journaled with their waived rows as F lines). No legacy feature set is claimed: the
// base implementation reads Apple tables this device does not have.
- (BOOL)supportsFeatureSet:(MTLFeatureSet)set {
 static const uint8_t families[]={1,2,1,2,3,1,2,3,1,2,3,4,1,2,3,4,5};
 if((NSUInteger)set>=sizeof families)return NO;
 return [(id<MTLDevice>)self supportsFamily:(MTLGPUFamily)(MTLGPUFamilyApple1+families[set]-1)];
}
- (NSUInteger)iosurfaceReadOnlyTextureAlignmentBytes {
 uint64_t value=0;
 return nmb_device_answer(gDevice,"minimumLinearTextureAlignmentForPixelFormat:",MTLPixelFormatRGBA16Float,&value)?NSUInteger(value):0;
}
// The original discovery (_MTLAddDevice) runs these four on every device it registers. The target's own versions fill the base class's limits
// and feature tables from a hardware profile (`featureProfile`, `indirectArgumentBufferCapabilities`, a table of Apple GPUs) that this device
// has not got: running them would claim an Apple GPU's capabilities. They are empty here, so a limit or feature the runtime measured is answered
// by this file's own answers (installAnswers) and any other stays the base class's neutral 0/NO. Nothing is claimed that nothing measured.
- (void)initLimits {}
- (void)initFeatureQueries {}
- (void)initGPUFamilySupport {}
- (void)initWorkarounds {}
- (NSString *)productName {return gName;}
- (NSString *)vendorName {char name[64]={};nmb_device_vendor_name(gDevice,name,sizeof name);return [NSString stringWithUTF8String:name];}
- (MTLSize)maxThreadsPerThreadgroup {
 uint64_t s[3]={};if(!nmb_device_max_threads_per_threadgroup(gDevice,s))NBLOG(@"native-metal: maxThreadsPerThreadgroup: %@",lastError());
 return MTLSizeMake(s[0],s[1],s[2]);
}
- (void)doesNotRecognizeSelector:(SEL)selector {
 if(gMissing)gMissing("NativeBridgeMetalDevice",sel_getName(selector));
 noteProblem([NSString stringWithFormat:@"missing device %s",sel_getName(selector)]);
 [super doesNotRecognizeSelector:selector];
}
- (id<MTLEvent>)newEvent {
 NmbEvent *handle=nmb_event_new(gDevice,false);if(!handle)return nil;
 NBEvent *event=[NBEvent new];event.handle=handle;return (id<MTLEvent>)event;
}
- (id<MTLSharedEvent>)newSharedEvent {
 NmbEvent *handle=nmb_event_new(gDevice,true);if(!handle)return nil;
 NBSharedEvent *event=[NBSharedEvent new];event.handle=handle;return (id<MTLSharedEvent>)event;
}
- (id<MTLFence>)newFence {
 NmbFence *handle=nmb_fence_new(gDevice);if(!handle)return nil;
 NBFence *fence=[NBFence new];fence.handle=handle;return (id<MTLFence>)fence;
}
- (NSUInteger)currentAllocatedSize {return nmb_device_current_allocated_size(gDevice);}
- (NSUInteger)recommendedMaxWorkingSetSize {
 // Match both real RAM and the connection's finite shared-32-bit VA lease.
 uint64_t bytes=0;size_t size=sizeof bytes;
 if(sysctlbyname("hw.memsize",&bytes,&size,nullptr,0))bytes=1ull<<30;
 uint64_t budget=std::min<uint64_t>(bytes/4,2ull<<30);
 if(!gLoaderPath.empty()) {
  void *lib=dlopen(gLoaderPath.c_str(),RTLD_NOW|RTLD_NOLOAD);
  if(lib){auto query=(uint64_t(*)(void))dlsym(lib,"tu_darwin_va_budget");
   const uint64_t lease=query?query():0;if(lease)budget=std::min(budget,lease*3/4);dlclose(lib);}
 }
 return (NSUInteger)budget;
}
- (id<MTLCommandQueue>)newCommandQueue {
 NmbQueue *handle=nmb_queue_new(gDevice);if(!handle)return nil;
 NBQueue *queue=[NBQueue new];queue.handle=handle;return (id<MTLCommandQueue>)queue;
}
- (id<MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor *)d {
 if(!d)return nil;
 NmbHeap *handle=nmb_heap_new(gDevice,d.type,d.size,(uint64_t)d.resourceOptions);
 if(!handle){NBLOG(@"native-metal: heap refused: %@",lastError());return nil;}
 NBHeap *heap=[NBHeap new];heap.handle=handle;heap.options=d.resourceOptions;heap.heapType=d.type;return (id<MTLHeap>)heap;
}
- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)d {
 NmbTextureDescriptor descriptor{};uint64_t answer[2]={};
 if(!textureDescriptor(d,&descriptor) || !nmb_device_heap_texture_size_and_align(gDevice,&descriptor,answer))
  NBLOG(@"native-metal: heap texture size refused: %@",lastError());
 return (MTLSizeAndAlign){answer[0],answer[1]};
}
- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
 uint64_t answer[2]={};
 if(!nmb_device_heap_buffer_size_and_align(gDevice,length,options,answer))NBLOG(@"native-metal: heap buffer size refused: %@",lastError());
 return (MTLSizeAndAlign){answer[0],answer[1]};
}
- (id)newResourceGroupFromResources:(const id __unsafe_unretained [])resources count:(NSUInteger)count {
 NBResourceGroup *group=[NBResourceGroup new];
 NSMutableArray *held=[NSMutableArray arrayWithCapacity:count];
 for(NSUInteger i=0;i<count;i++)if(resources[i])[held addObject:resources[i]];
 group.resources=held;return group;
}
// Match the layout object above: raw memory contains linear pixels, and is
// imported with its real row stride. Unsupported layouts still return nil.
- (id<MTLTexture>)newTextureWithBytesNoCopy:(void *)bytes length:(NSUInteger)length descriptor:(MTLTextureDescriptor *)d deallocator:(void (^)(void *,NSUInteger))deallocator {
 NBTextureLayout *layout=[NBTextureLayout layoutForDescriptor:d];
 if(layout && !length)length=layout.size;
 if(!layout || !bytes || d.storageMode!=MTLStorageModeShared || length<layout.size ||
    (uintptr_t)bytes%layout.alignment || length%layout.alignment) {
  NBLOG(@"native-metal: raw texture requires supported native linear layout and page-aligned host memory");return nil;
 }
 __block BOOL committed=NO;
 NBBuffer *buffer=(NBBuffer *)[(id<MTLDevice>)self newBufferWithBytesNoCopy:bytes length:length options:d.resourceOptions
  deallocator:^(void *pointer,NSUInteger size){if(committed && deallocator)deallocator(pointer,size);}];
 if(!buffer)return nil;
 NBTexture *texture=textureOnBuffer(buffer,d,0,layout.rowBytes,false);
 if(texture)committed=YES;
 return (id<MTLTexture>)texture;
}
- (id)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)d isHeapOrBufferBacked:(BOOL)backed {
 (void)backed;
 return [NBTextureLayout layoutForDescriptor:d];
}
- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)count {
 NmbQueue *handle=nmb_queue_new_with_max_count(gDevice,count);
 if(!handle){NBLOG(@"native-metal: bounded command queue refused: %@",lastError());return nil;}
 NBQueue *queue=[NBQueue new];queue.handle=handle;
 NBLOG(@"native-metal: command queue uses requested in-flight limit=%lu",(unsigned long)count);
 return (id<MTLCommandQueue>)queue;
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
 NmbBuffer *handle=nmb_buffer_new(gDevice,length,options,nullptr);
 if(!handle && trimOnDeviceMemoryFailure())handle=nmb_buffer_new(gDevice,length,options,nullptr);
 if(!handle){NBLOG(@"native-metal: buffer refused: %@",lastError());return nil;}
 NBBuffer *buffer=[NBBuffer new];buffer.handle=handle;buffer.options=options;return (id<MTLBuffer>)buffer;
}
- (id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options {
 if(!bytes)return nil;
 NmbBuffer *handle=nmb_buffer_new(gDevice,length,options,bytes);
 if(!handle && trimOnDeviceMemoryFailure())handle=nmb_buffer_new(gDevice,length,options,bytes);
 if(!handle){NBLOG(@"native-metal: buffer refused: %@",lastError());return nil;}
 NBBuffer *buffer=[NBBuffer new];buffer.handle=handle;buffer.options=options;return (id<MTLBuffer>)buffer;
}
- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)pointer length:(NSUInteger)length options:(MTLResourceOptions)options deallocator:(void (^)(void *,NSUInteger))deallocator {
 void *context=deallocator?(__bridge_retained void *)[deallocator copy]:nullptr;
 NmbBuffer *handle=nmb_buffer_new_no_copy_releasing(gDevice,pointer,length,options,context?releaseBlock:nullptr,context);
 if(!handle && trimOnDeviceMemoryFailure())
  handle=nmb_buffer_new_no_copy_releasing(gDevice,pointer,length,options,context?releaseBlock:nullptr,context);
 if(!handle) {
  if(context)(void)(__bridge_transfer id)context;
  NBLOG(@"native-metal: no-copy buffer refused: %@",lastError());return nil;
 }
 gNoCopyAll++;if(context)gNoCopyMade++;
 NBBuffer *buffer=[NBBuffer new];buffer.handle=handle;buffer.options=options;buffer.wrapped=pointer;
 return (id<MTLBuffer>)buffer;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d {
 NmbTextureDescriptor descriptor;if(!textureDescriptor(d,&descriptor))return nil;
 NmbTexture *handle=nmb_texture_new(gDevice,&descriptor);
 if(!handle && trimOnDeviceMemoryFailure())handle=nmb_texture_new(gDevice,&descriptor);
 noteFormat((uint32_t)descriptor.format,handle!=nullptr);
 if(!handle){NBLOG(@"native-metal: texture refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;
 frameEvent(49,(uintptr_t)handle,d.sampleCount,descriptor.samples,d.pixelFormat,descriptor.format,d.width,d.height,packedSwizzle(d.swizzle));return (id<MTLTexture>)texture;
}
// The surface's pages are the texels: its mapping in this process is imported without a copy (pinned by the kernel, mapped into the GPU's
// 4 KiB context by Turnip), so every process that imports the surface renders and samples the same memory.
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)surface plane:(NSUInteger)plane {
 // A planar surface (Apple's lossless-compressed BGRA spelling has one plane, video formats two) is imported whole; the texture starts at
 // the plane's own address and rows. Only a plain surface has no planes, and then only plane 0 exists.
 const size_t planes=surface?IOSurfaceGetPlaneCount(surface):0;
 if(!surface || !d || (planes?plane>=planes:plane!=0)){refuseSurface(1,surface,d,plane,0,0);return nil;}
 if(planes)gSurfacePlanar++;
 void *base=IOSurfaceGetBaseAddress(surface);const size_t page=(size_t)getpagesize(),allocation=IOSurfaceGetAllocSize(surface);
 if(!page || (page&(page-1)) || !base || ((uintptr_t)base&(page-1)) || !allocation || allocation>SIZE_MAX-(page-1)){refuseSurface(2,surface,d,plane,(uintptr_t)base,0);return nil;}
 const uintptr_t start=planes?(uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface,plane):(uintptr_t)base;
 const size_t row=planes?IOSurfaceGetBytesPerRowOfPlane(surface,plane):IOSurfaceGetBytesPerRow(surface);
 if(start<(uintptr_t)base || start-(uintptr_t)base>=allocation || !row){refuseSurface(3,surface,d,plane,start-(uintptr_t)base,row);return nil;}
 const size_t bytes=(allocation+page-1)&~(page-1);
 NBBuffer *buffer=surfaceBuffer(surface,base,bytes);if(!buffer){refuseSurface(4,surface,d,plane,start-(uintptr_t)base,row);return nil;}
 NBTexture *texture=planes>=2?textureOnSurface(buffer,d,surface,plane,start-(uintptr_t)base,row):textureOnBuffer(buffer,d,start-(uintptr_t)base,row,true);
 if(!texture){refuseSurface(5,surface,d,plane,start-(uintptr_t)base,row);return nil;}
 frameEvent(49,(uintptr_t)texture.handle,d.sampleCount,texture.descriptor.samples,d.pixelFormat,texture.descriptor.format,d.width,d.height,packedSwizzle(d.swizzle),plane);
 gSurfaceTextures++;texture.surface=(IOSurfaceRef)CFRetain(surface);return (id<MTLTexture>)texture;
}
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error {
 const void *bytes=nullptr;size_t size=0;dispatch_data_t mapped=dispatch_data_create_map(data,&bytes,&size);
 const uint64_t libraryBegan=nowUs();
 NmbLibrary *handle=nmb_library_new(gDevice,bytes,size,false);(void)mapped;gTimeLibrary.add(libraryBegan);
 if(!handle)return failWith<id<MTLLibrary>>(error,@"library",nil);
 NBLibrary *library=[NBLibrary new];library.handle=handle;if(error)*error=nil;return (id<MTLLibrary>)library;
}
- (id<MTLLibrary>)newLibraryWithFile:(NSString *)path error:(NSError **)error {
 NSData *bytes=path?[NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:error]:nil;if(!bytes)return nil;
 const uint64_t libraryBegan=nowUs();
 NmbLibrary *handle=nmb_library_new(gDevice,bytes.bytes,bytes.length,false);gTimeLibrary.add(libraryBegan);
 if(!handle)return failWith<id<MTLLibrary>>(error,@"library",nil);
 NBLibrary *library=[NBLibrary new];library.handle=handle;if(error)*error=nil;return (id<MTLLibrary>)library;
}
- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error {
 if(!url.isFileURL){if(error)*error=nbError(@"a file URL is required");return nil;}
 return [(id<MTLDevice>)self newLibraryWithFile:url.path error:error];
}
- (id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle error:(NSError **)error {
 NSURL *url=[bundle URLForResource:@"default" withExtension:@"metallib"];
 if(!url){if(error)*error=nbError(@"the bundle has no default.metallib");return nil;}
 return [(id<MTLDevice>)self newLibraryWithURL:url error:error];
}
- (id<MTLLibrary>)newDefaultLibrary {return [(id<MTLDevice>)self newDefaultLibraryWithBundle:[NSBundle mainBundle] error:nil];}
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function error:(NSError **)error {
 return [(id<MTLDevice>)self newComputePipelineStateWithFunction:function options:MTLPipelineOptionNone reflection:nil error:error];
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function options:(MTLPipelineOption)options
                                                         reflection:(MTLAutoreleasedComputePipelineReflection *)reflection error:(NSError **)error {
 if(reflection)*reflection=nil;
 if(![(id)function isKindOfClass:[NBFunction class]]){if(error)*error=nbError(@"foreign function");return nil;}
 const NmbPipelineOptions creation={(uint64_t)options,nullptr,0};
 const uint64_t pipelineBegan=nowUs();
 NmbComputePipeline *handle=nmb_compute_pipeline_new_with_options(gDevice,[(NBFunction *)function handle],&creation);gTimePipeline.add(pipelineBegan);
 if(!handle)return failWith<id<MTLComputePipelineState>>(error,@"compute pipeline",nil);
 NBComputePipeline *state=[NBComputePipeline new];state.handle=handle;gPipelines++;if(error)*error=nil;return (id<MTLComputePipelineState>)state;
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d options:(MTLPipelineOption)options
                                                       reflection:(MTLAutoreleasedComputePipelineReflection *)reflection error:(NSError **)error {
 if(reflection)*reflection=nil;
 NmbComputePipelineDescriptor p={};if(!computeDescriptor(d,&p,error))return nil;
 std::vector<NmbBinaryArchive *> archives;
 if(!pipelineArchives(d.binaryArchives,archives,error))return nil;
 const NmbPipelineOptions creation={(uint64_t)options,archives.data(),archives.size()};
 const uint64_t pipelineBegan=nowUs();
 NmbComputePipeline *handle=nmb_compute_pipeline_new_with_descriptor(gDevice,&p,&creation);gTimePipeline.add(pipelineBegan);
 if(!handle)return failWith<id<MTLComputePipelineState>>(error,@"compute pipeline",nil);
 NBComputePipeline *state=[NBComputePipeline new];state.handle=handle;state.label=d.label;gPipelines++;
 if(error)*error=nil;return (id<MTLComputePipelineState>)state;
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {
 return [(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:error];
}
// Route both selected-OS convenience forms to the real runtime descriptor
// implementation; do not fall into the uninitialized Apple GPU base table.
- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d error:(NSError **)error {
 return [(id<MTLDevice>)self newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:error];
}
- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d options:(MTLPipelineOption)options
                            completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler {
 if(!handler)return;
 MTLComputePipelineDescriptor *snapshot=[d copy];
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{@autoreleasepool {
  NSError *error=nil;MTLComputePipelineReflection *reflection=nil;
  id<MTLComputePipelineState> state=[(id<MTLDevice>)self newComputePipelineStateWithDescriptor:snapshot options:options reflection:&reflection error:&error];
  handler(state,reflection,error);
 }});
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d options:(MTLPipelineOption)options
                                                        reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection error:(NSError **)error {
 if(reflection)*reflection=nil;
 auto *p=new NmbRenderPipelineDescriptor();
 if(!renderDescriptor(d,p,error)){delete p;return nil;}
 std::vector<NmbBinaryArchive *> archives;
 if(!pipelineArchives(d.binaryArchives,archives,error)){delete p;return nil;}
 const NmbPipelineOptions creation={(uint64_t)options,archives.data(),archives.size()};
 const uint64_t pipelineBegan=nowUs();
 const NBCost costBegan=gJournalVerbose?NBCost::now():NBCost{};
 NmbRenderPipeline *handle=nmb_render_pipeline_new_with_options(gDevice,p,&creation);delete p;gTimePipeline.add(pipelineBegan);
 journalRenderPipeline(d,pipelineBegan,handle?nullptr:nmb_last_error(),gJournalVerbose?NBCost::now().since(costBegan):std::string());
 if(!handle)return failWith<id<MTLRenderPipelineState>>(error,@"render pipeline",nil);
 NBRenderPipeline *state=[NBRenderPipeline new];state.handle=handle;state.stateFormat=(uint32_t)d.colorAttachments[0].pixelFormat;gPipelines++;if(error)*error=nil;return (id<MTLRenderPipelineState>)state;
}
// Offload asynchronous creation with a descriptor snapshot. The runtime's
// bounded compiler pool remains responsible for Vulkan compilation; the UI
// caller must not spend a cold shader compile inside an asynchronous API.
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d completionHandler:(MTLNewRenderPipelineStateCompletionHandler)handler {
 if(!handler)return;
 MTLRenderPipelineDescriptor *snapshot=[d copy];
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{@autoreleasepool {
  NSError *error=nil;id<MTLRenderPipelineState> state=[(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:snapshot error:&error];
  handler(state,error);
 }});
}
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d options:(MTLPipelineOption)options
                           completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)handler {
 if(!handler)return;
 MTLRenderPipelineDescriptor *snapshot=[d copy];
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{@autoreleasepool {
  NSError *error=nil;MTLRenderPipelineReflection *reflection=nil;
  id<MTLRenderPipelineState> state=[(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:snapshot options:options reflection:&reflection error:&error];
  handler(state,reflection,error);
 }});
}
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function completionHandler:(MTLNewComputePipelineStateCompletionHandler)handler {
 if(!handler)return;
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{@autoreleasepool {
  NSError *error=nil;id<MTLComputePipelineState> state=[(id<MTLDevice>)self newComputePipelineStateWithFunction:function error:&error];
  handler(state,error);
 }});
}
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function options:(MTLPipelineOption)options
                          completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler {
 if(!handler)return;
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{@autoreleasepool {
  NSError *error=nil;MTLComputePipelineReflection *reflection=nil;
  id<MTLComputePipelineState> state=[(id<MTLDevice>)self newComputePipelineStateWithFunction:function options:options reflection:&reflection error:&error];
  handler(state,reflection,error);
 }});
}
// SPI: a precompiled pipeline library (RenderBox.framework/default.pipelinelib) holds Apple GPU binaries. None is opened; the caller logs the
// error and builds its pipelines from the shader library instead (RB::Device::render_pipeline_state).
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error {
 if(error)*error=nbError([NSString stringWithFormat:@"no pipeline library on this GPU: %@",path.lastPathComponent]);
 return nil;
}
- (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)d error:(NSError **)error {
 if(d.url && !d.url.isFileURL){if(error)*error=nbError(@"a file URL is required");return nil;}
 NmbBinaryArchive *handle=nmb_binary_archive_new(gDevice,d.url?d.url.fileSystemRepresentation:nullptr);
 if(!handle)return failWith<id<MTLBinaryArchive>>(error,@"binary archive",nil);
 NBBinaryArchive *archive=[NBBinaryArchive new];archive.handle=handle;if(error)*error=nil;return (id<MTLBinaryArchive>)archive;
}
- (NSUInteger)requiredLinearTextureBytesPerRowForDescriptor:(MTLTextureDescriptor *)d {
 const uint64_t row=nmb_device_linear_bytes_per_row(gDevice,d.pixelFormat,d.width);
 if(!row)NBLOG(@"native-metal: requiredLinearTextureBytesPerRow: %@",lastError());
 return row;
}
- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d {
 NmbSamplerDescriptor s={(uint64_t)d.minFilter,(uint64_t)d.magFilter,(uint64_t)d.mipFilter,(uint64_t)d.sAddressMode,(uint64_t)d.tAddressMode,(uint64_t)d.rAddressMode,
     (uint64_t)d.borderColor,(uint64_t)d.normalizedCoordinates,d.maxAnisotropy,(uint64_t)d.compareFunction,(uint64_t)d.lodAverage,d.lodMinClamp,d.lodMaxClamp};
 NmbSampler *handle=nmb_sampler_new(gDevice,&s);
 if(!handle){NBLOG(@"native-metal: sampler refused: %@",lastError());return nil;}
 NBSampler *sampler=[NBSampler new];sampler.handle=handle;return (id<MTLSamplerState>)sampler;
}
- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d {
 auto face=[](MTLStencilDescriptor *s)->NmbStencil {
  return {(uint64_t)s.stencilCompareFunction,(uint64_t)s.stencilFailureOperation,(uint64_t)s.depthFailureOperation,(uint64_t)s.depthStencilPassOperation,s.readMask,s.writeMask};
 };
 NmbDepthStencilDescriptor s={(uint64_t)d.depthCompareFunction,(uint64_t)d.depthWriteEnabled,face(d.frontFaceStencil),face(d.backFaceStencil)};
 NmbDepthStencil *handle=nmb_depth_stencil_new(gDevice,&s);
 if(!handle){NBLOG(@"native-metal: depth/stencil state refused: %@",lastError());return nil;}
 NBDepthStencil *state=[NBDepthStencil new];state.handle=handle;return (id<MTLDepthStencilState>)state;
}
@end

// The device queries the runtime answers (nmb_device_answer_selector), as methods of the device class. One without an argument is read once
// and installed only where the device has an answer of its own; one with an argument caches the runtime answer per selector/argument, and a refusal answers 0/NO
// with the reason logged.
static std::unordered_map<SEL,uint64_t> gAnswers;
static NSUInteger countAnswer(id,SEL cmd) {return (NSUInteger)gAnswers.at(cmd);}
static BOOL flagAnswer(id,SEL cmd) {return gAnswers.at(cmd)!=0;}
static uint64_t askAnswer(SEL cmd,NSUInteger arg) {
 static std::mutex lock;
 static std::unordered_map<SEL,std::unordered_map<NSUInteger,uint64_t>> kept;
 {
  std::lock_guard<std::mutex> hold(lock);
  auto answers=kept.find(cmd);
  if(answers!=kept.end()) {
   auto answer=answers->second.find(arg);
   if(answer!=answers->second.end())return answer->second;
  }
 }
 uint64_t value=0;
 if(!nmb_device_answer(gDevice,sel_getName(cmd),arg,&value)) {
  NBLOG(@"native-metal: %s(%lu): %@",sel_getName(cmd),(unsigned long)arg,lastError());value=0;
 }
 std::lock_guard<std::mutex> hold(lock);
 kept[cmd][arg]=value;
 return value;
}
static NSUInteger countArgAnswer(id,SEL cmd,NSUInteger arg) {return (NSUInteger)askAnswer(cmd,arg);}
static BOOL flagArgAnswer(id,SEL cmd,NSUInteger arg) {return askAnswer(cmd,arg)!=0;}
static void installAnswers(Class cls) {
 uint32_t kind=0;
 for(size_t i=0;const char *name=nmb_device_answer_selector(i,&kind);i++) {
  // Board methods were installed first. Do not probe the runtime's generic
  // absent answer for a selector the qualified frontend already implements.
  if(class_getInstanceMethod([NBDeviceMethods class],sel_registerName(name)))continue;
  SEL sel=sel_registerName(name);uint64_t value=0;
  switch(kind) {
  case NMB_ANSWER_COUNT: case NMB_ANSWER_FLAG:
   if(!nmb_device_answer(gDevice,name,0,&value)){NBLOG(@"native-metal: no device answer: %@",lastError());continue;}
   // Extended-range 10-bit (XR10) framebuffers are withheld: this display chain presents 8-bit BGRX scanout only. The render server picks its
   // wide-gamut 64-bit (and Apple-compressed) framebuffer when a device offers them, and the display driver could not show it (int-089: the
   // swap's surface was '&w4a', 8 bytes per pixel, and every swap was refused).
   if(!strcmp(name,"supportsExtendedXR10Formats"))value=0;
   gAnswers[sel]=value;
   class_addMethod(cls,sel,kind==NMB_ANSWER_COUNT?(IMP)countAnswer:(IMP)flagAnswer,kind==NMB_ANSWER_COUNT?"Q16@0:8":"B16@0:8");
   break;
  case NMB_ANSWER_COUNT_ARG: class_addMethod(cls,sel,(IMP)countArgAnswer,"Q24@0:8Q16");break;
  case NMB_ANSWER_FLAG_ARG: class_addMethod(cls,sel,(IMP)flagArgAnswer,"B24@0:8Q16");break;
  }
 }
}

extern "C" id native_bridge_metal_open(const char *loader,const char *cache,uint64_t registryID,void (*missing)(const char *,const char *)) {
 if(gDeviceObject)return gDeviceObject;
 Class base=objc_getClass("_MTLDevice");
#if TARGET_OS_IPHONE
 if(!PongMetalDeviceAbiAvailable(base)){NBLOG(@"Pong Metal: matching guest device ABI unavailable");return nil;}
#endif
 gMissing=missing;gInFlight=[NSMutableSet set];watchImportPressure();
 gLoaderPath=loader?loader:"";
 if(cache && *cache)configureShaderDiskCache(cache);
 gDevice=nmb_device_open(loader,cache);
 if(frameTraceOn() && loader) {
  void *library=dlopen(loader,RTLD_NOW|RTLD_NOLOAD);
  if(library)gFrameTransport=(bool (*)(uint32_t,const void *,uint32_t))dlsym(library,"tu_darwin_frame_diagnostic");
  nmb_gpu_diagnostic_hook(frameRuntime);
  frameText(46,getprogname());frameEvent(48,singleSample(),getenv("SHENG_GPU_PIXELS")!=nullptr);
  if(library)if(auto build=(const char *(*)())dlsym(library,"tu_darwin_frame_build_id"))frameText(58,build());
  frameText(59,getenv("TU_DEBUG")?getenv("TU_DEBUG"):"default");
#ifdef NATIVE_METAL_BUILD_ID
  frameText(47,NATIVE_METAL_BUILD_ID);
#endif
 }

 if(!gDevice){NBLOG(@"native-metal: device refused: %s",nmb_last_error());return nil;}
 if(cache){gJournalPath=std::string(cache)+"/pipelines-"+std::to_string(getpid())+".log";gJournalBegan=nowUs();}
 // Normal UI draws must not unwind QuartzCore stacks, open/close an APFS
 // log and serialize every successful shader/pipeline creation. Keep all
 // failures; full journals remain available for a targeted capture.
 const char *trace=getenv("PONG_METAL_PIPELINE_TRACE");
 gJournalVerbose=trace && !strcmp(trace,"1");
 char name[128]={};nmb_device_name(gDevice,name,sizeof name);gName=[NSString stringWithUTF8String:name];gRegistryID=registryID;
 // Report unmet reference-family rows; supportsFamily keeps the runtime's actual answer.
 {char waived[4096]={};nmb_device_family_waivers(gDevice,waived,sizeof waived);
  for(char *line=waived,*next;*line;line=next){next=strchr(line,'\n');if(next)*next++=0;else next=line+strlen(line);journal(std::string("F ")+line);}}
#if !TARGET_OS_IPHONE
 // Host instrument: on NSObject every selector a client sends that this file lacks reaches the missing hook, instead of the host's
 // own _MTLDevice implementation.
 if(getenv("NMB_DEVICE_BASE_NSOBJECT"))base=[NSObject class];
#endif
#if TARGET_OS_IPHONE
 // Actual20G81 layout checked before allocating the Vulkan runtime above.
#endif
 if(!base)base=[NSObject class];
 Class cls=objc_allocateClassPair(base,"NativeBridgeMetalDevice",0);if(!cls)return nil;
 unsigned count=0;Method *methods=class_copyMethodList([NBDeviceMethods class],&count);
 for(unsigned i=0;i<count;i++)class_addMethod(cls,method_getName(methods[i]),method_getImplementation(methods[i]),method_getTypeEncoding(methods[i]));
 free(methods);installAnswers(cls);class_addProtocol(cls,@protocol(MTLDevice));
 // The original registration (_MTLAddDevice) asserts that a plugin's device conforms to Metal's private device protocol as well; the target's own
 // drivers get it from their IOGPU base class. Conformance only answers conformsToProtocol:, the methods are still looked up one by one.
 if(Protocol *spi=objc_getProtocol("MTLDeviceSPI"))class_addProtocol(cls,spi);
 // Runtime classes implement these protocols; framework clients must see the
 // same conformance as the device rather than reject genuine bridge objects.
 static const char *const conformances[][2]={
  {"NBBuffer","MTLBuffer"},{"NBTexture","MTLTexture"},{"NBLibrary","MTLLibrary"},{"NBFunction","MTLFunction"},
  {"NBComputePipeline","MTLComputePipelineState"},{"NBRenderPipeline","MTLRenderPipelineState"},
  {"NBSampler","MTLSamplerState"},{"NBDepthStencil","MTLDepthStencilState"},{"NBEvent","MTLEvent"},
  {"NBSharedEvent","MTLSharedEvent"},{"NBFence","MTLFence"},{"NBQueue","MTLCommandQueue"},
  {"NBCommandBuffer","MTLCommandBuffer"},{"NBEncoder","MTLCommandEncoder"},
  {"NBComputeEncoder","MTLComputeCommandEncoder"},{"NBRenderEncoder","MTLRenderCommandEncoder"},
  {"NBBlitEncoder","MTLBlitCommandEncoder"},{"NBHeap","MTLHeap"},{"NBBinaryArchive","MTLBinaryArchive"}};
 for(const auto &c:conformances){Class k=objc_getClass(c[0]);Protocol *pr=objc_getProtocol(c[1]);if(k&&pr)class_addProtocol(k,pr);}
 objc_registerClassPair(cls);
 gDeviceObject=[cls new];
 return gDeviceObject;
}

// Explicit bare AIR entry for an owned caller, without a metallib wrapper.
extern "C" id native_bridge_metal_library_with_air(const void *bytes,size_t length) {
 NmbLibrary *handle=gDevice?nmb_library_new(gDevice,bytes,length,true):nullptr;if(!handle)return nil;
 NBLibrary *library=[NBLibrary new];library.handle=handle;return library;
}
extern "C" bool native_bridge_metal_shader_cache_stats(NmbShaderCacheStats *out) {
 return gDevice && nmb_device_shader_cache_stats(gDevice,out);
}
extern "C" bool native_bridge_metal_work_stats(uint64_t out[10]) {
 return gDevice && nmb_device_work_stats(gDevice,out);
}

extern "C" size_t native_bridge_metal_status(char *out,size_t size) {
 std::lock_guard<std::mutex> lock(gProblemLock);
 return (size_t)snprintf(out,size,"c%llu/%llu d%llu p%llu x%llu R%llu D%llu F%llu M%llu C%llu B%llu %s | %s",(unsigned long long)gCommits.load(),(unsigned long long)gCommitFailures.load(),(unsigned long long)gCompleted.load(),
  (unsigned long long)gPipelines.load(),(unsigned long long)gProblems.load(),(unsigned long long)gPasses.load(),(unsigned long long)gDraws.load(),(unsigned long long)gFullScreenPasses.load(),(unsigned long long)(gBiggestPass.load()>>10),(unsigned long long)gDispatches.load(),(unsigned long long)gBlits.load(),gLastProblems[0],gLastProblems[1]);
}

static size_t native_bridge_metal_formats_locked(char *out,size_t size) {
 int n=snprintf(out,size,"tex x%llu s%llu/%llu p%llu f",(unsigned long long)gTextureRefusals.load(),(unsigned long long)gSurfaceTextures.load(),
              (unsigned long long)gSurfaceRefusals.load(),(unsigned long long)gSurfacePlanar.load());
 for(unsigned i=0;i<gFormatCount && n>0 && (size_t)n<size;i++)n+=snprintf(out+n,size-(size_t)n,"%u ",gFormats[i]);
 return n>0?(size_t)n:0;
}
// The telemetry table the plugin streams to the kernel's log: newline-separated rows of at most 90 characters.
extern "C" size_t native_bridge_metal_telemetry(char *out,size_t size) {
 size_t at=0;out[0]=0;
 auto row=[&](const char *format,auto... args){
  char text[200];int n=snprintf(text,sizeof text,format,args...);if(n<=0)return;if(n>90)n=90;
  if(at+(size_t)n+2>=size)return;memcpy(out+at,text,(size_t)n);at+=(size_t)n;out[at++]='\n';out[at]=0;};
 auto ms=[](uint64_t us){return (unsigned long long)(us/1000);};
 row("S c%llu/%llu d%llu p%llu x%llu R%llu D%llu F%llu M%lluk C%llu B%llu",(unsigned long long)gCommits.load(),(unsigned long long)gCommitFailures.load(),
  (unsigned long long)gCompleted.load(),(unsigned long long)gPipelines.load(),(unsigned long long)gProblems.load(),(unsigned long long)gPasses.load(),
  (unsigned long long)gDraws.load(),(unsigned long long)gFullScreenPasses.load(),(unsigned long long)(gBiggestPass.load()>>10),
  (unsigned long long)gDispatches.load(),(unsigned long long)gBlits.load());
 {
  struct {uint64_t key,n;} top[5]={};
  {std::lock_guard<std::mutex> lock(gPassFormatLock);
   for(unsigned i=0;i<gPassFormatCount;i++)for(unsigned t=0;t<5;t++)if(gPassFormatRows[i].n>top[t].n){for(unsigned u=4;u>t;u--)top[u]=top[u-1];top[t]={gPassFormatRows[i].key,gPassFormatRows[i].n};break;}}
  for(unsigned t=0;t<5 && top[t].n;t++)row("DF p%u s%u l%u %s%s n%llu",(unsigned)(top[t].key&0xffff),(unsigned)(top[t].key>>32),
    (unsigned)(top[t].key>>16&3),top[t].key>>18&1?"S":"-",top[t].key>>19&1?"B":"-",(unsigned long long)top[t].n);
 }
 for(int k=0;k<3;k++) {
  const std::atomic<uint64_t> *bins=k==0?gHistFrameGpu:k==1?gHistOtherGpu:gHistFrameEncode;
  for(unsigned first=0;first<10;first+=5)row("H %s %u %llu %llu %llu %llu %llu",k==0?"frame-gpu":k==1?"other-gpu":"frame-enc",first,
   (unsigned long long)bins[first].load(),(unsigned long long)bins[first+1].load(),(unsigned long long)bins[first+2].load(),
   (unsigned long long)bins[first+3].load(),(unsigned long long)bins[first+4].load());
 }
 row("PR functions resumed=%u",native_prime_functions_resumed);
 row("T pipeline n=%llu total=%llums max=%llums",(unsigned long long)gTimePipeline.count.load(),ms(gTimePipeline.total.load()),ms(gTimePipeline.slowest.load()));
 row("T library n=%llu total=%llums max=%llums",(unsigned long long)gTimeLibrary.count.load(),ms(gTimeLibrary.total.load()),ms(gTimeLibrary.slowest.load()));
 row("T function n=%llu total=%llums max=%llums",
  (unsigned long long)gTimeFunction.count.load(),ms(gTimeFunction.total.load()),ms(gTimeFunction.slowest.load()));
 row("G commit-to-complete n=%llu total=%llums max=%llums mean=%lluus",(unsigned long long)gTimeEncodeToCommit.count.load(),ms(gTimeEncodeToCommit.total.load()),
  ms(gTimeEncodeToCommit.slowest.load()),(unsigned long long)(gTimeEncodeToCommit.count.load()?gTimeEncodeToCommit.total.load()/gTimeEncodeToCommit.count.load():0));
 row("J scheduled wait n=%llu total=%llums max=%llums",(unsigned long long)gTimeWaitScheduled.count.load(),ms(gTimeWaitScheduled.total.load()),ms(gTimeWaitScheduled.slowest.load()));
 row("J completed wait n=%llu total=%llums max=%llums",(unsigned long long)gTimeWaitCompleted.count.load(),ms(gTimeWaitCompleted.total.load()),ms(gTimeWaitCompleted.slowest.load()));
 row("J resource wait n=%llu total=%llums max=%llums",(unsigned long long)gTimeResourceWait.count.load(),ms(gTimeResourceWait.total.load()),ms(gTimeResourceWait.slowest.load()));
 row("J resource checks=%llu skipped=%llu",(unsigned long long)gResourceChecks.load(),(unsigned long long)gResourceSkipped.load());
 row("J surface cache n=%llu bytes=%llu dup=%llu us=%llu max=%llu",(unsigned long long)gTimeSurfaceFlush.count.load(),
  (unsigned long long)gSurfaceFlushBytes.load(),(unsigned long long)gSurfaceFlushDuplicates.load(),
  (unsigned long long)gTimeSurfaceFlush.total.load(),(unsigned long long)gTimeSurfaceFlush.slowest.load());
 row("N no-copy %llu dealloc %llu/%llu surface %llu/%llu",(unsigned long long)gNoCopyAll.load(),(unsigned long long)gNoCopyReleased.load(),
  (unsigned long long)gNoCopyMade.load(),
  (unsigned long long)gSurfaceImportsReleased.load(),(unsigned long long)gSurfaceImportsMade.load());
 // The runtime's VkPipeline makings (the creation log, METAL2VULKAN_LOG_PIPELINES; they happen at draws, not at the state's creation) and
 // its shader cache: where the driver's compile time goes.
 uint64_t work[10]={};
 if(gDevice && nmb_device_work_stats(gDevice,work)) {
  row("V vkpipe n=%llu total=%llums max=%llums waited=%llums failed=%llu",
   (unsigned long long)work[0],ms(work[1]),ms(work[2]),ms(work[3]),(unsigned long long)work[4]);
  row("V cache hit%llu miss%llu wr%llu | msg i%llu w%llu",(unsigned long long)work[7],(unsigned long long)work[8],(unsigned long long)work[9],(unsigned long long)work[5],
   (unsigned long long)work[6]);
  char warning[200];size_t n=nmb_last_warning(warning,sizeof warning);
  if(n){row("W %.86s",warning);if(n>86)row("Wb %.86s",warning+86);}
 }
 {
  std::lock_guard<std::mutex> lock(gProblemLock);
  char formats[160];native_bridge_metal_formats_locked(formats,sizeof formats);row("F %.86s",formats);
  const SurfaceRefusal &r=gSurfaceLastRefusal;
  if(r.step)row("SR %u %08x %c%c%c%c pl%u/%u o%llx rb%llu a%llx %llux%llu pf%u",r.step,r.format,(char)(r.format>>24),(char)(r.format>>16),(char)(r.format>>8),
   (char)r.format,r.plane,r.planes,(unsigned long long)r.offset,(unsigned long long)r.row,(unsigned long long)r.allocation,(unsigned long long)r.width,
   (unsigned long long)r.height,r.pixelFormat);
  row("P0 %.86s",gLastProblems[0]);row("P0b %.86s",strlen(gLastProblems[0])>86?gLastProblems[0]+86:"");
  row("P1 %.86s",gLastProblems[1]);row("P1b %.86s",strlen(gLastProblems[1])>86?gLastProblems[1]+86:"");
 }
 {
  std::lock_guard<std::mutex> lock(gCommitLock);
  const uint32_t newest=(uint32_t)gCommits.load();
  row("%s","Q timing v2: per-buffer API counts; ctc=commit-to-callback; age includes caller idle");
  for(uint32_t k=0;k<20 && k<newest;k++) {
   const CommitRecord &r=gCommitRing[(newest-k)%32];
   if(r.index!=newest-k)continue;
   row("K%u t=%llu.%03llus rp%u dr%u kpx%llu cs%u bl%u ctc=%lluus",r.index,(unsigned long long)(r.committedUs/1000000),(unsigned long long)((r.committedUs/1000)%1000),
    r.work.passes,r.work.draws,(unsigned long long)(r.work.pixels>>10),r.work.dispatches,r.work.blits,(unsigned long long)r.callbackUs);
   if(k<4)row("E%u age=%lluus scheduled=%lldus",r.index,(unsigned long long)r.ageUs,(long long)r.scheduledUs);
  }
 }
 return at;
}

extern "C" size_t native_bridge_metal_formats(char *out,size_t size) {
 std::lock_guard<std::mutex> lock(gProblemLock);
 return native_bridge_metal_formats_locked(out,size);
}
