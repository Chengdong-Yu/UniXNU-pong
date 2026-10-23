/* SPDX-License-Identifier: MIT */
/* NativeAdreno fixed-record ABI only. No AGX command ABI, mailbox,
 * fake Metal service or firmware-framebuffer export. */
#include "PongAdrenoCp.h"
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
namespace {
PongCpBackend *backend;
IOLock *client_gate;
bool slots[4];
struct Window {uint64_t start,bytes;};
const Window windows[4]={{0x20000000,0x40000000},{0x60000000,0xaaa9000},
    {0x6aaa9000,0xaaa9000},{0x75552000,0xaaa9000}};
}
class PongAdrenoUserClient final:public IOUserClient {
    OSDeclareDefaultStructors(PongAdrenoUserClient)
    NativeAdrenoUserClient *core=nullptr;
    task_t owner=TASK_NULL;
    bool live=false,closing=false;
    unsigned slot=4;
    struct View {uint32_t token;IOMemoryDescriptor *memory;};
    View *views=nullptr;
    IORecursiveLock *operations=nullptr;
    static IOReturn action(OSObject *,void *,IOExternalMethodArguments *);
    IOReturn run(unsigned,const void *,size_t,void *,size_t);
    void dropViews(uint32_t token);
public:
    bool mount(task_t);
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn clientMemoryForType(UInt32,IOOptionBits *,IOMemoryDescriptor **) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t,IOExternalMethodArguments *,IOExternalMethodDispatch *,OSObject *,void *) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(PongAdrenoUserClient,IOUserClient)
class NativeAdrenoGpuPublished final:public IOService {
    OSDeclareDefaultStructors(NativeAdrenoGpuPublished)
public:
    IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NativeAdrenoGpuPublished,IOService)

bool PongAdrenoUserClient::mount(task_t task)
{
    owner=task;core=new NativeAdrenoUserClient;
    operations=IORecursiveLockAlloc();
    views=IONewZero(View,NativeAdrenoClientMaxObjects);
    if(!core || !views || !operations)return false;
    IOLockLock(client_gate);
    for(unsigned i=0;i<4;i++)if(!slots[i]){slot=i;break;}
    IOReturn result=slot<4?core->open(NativeAdrenoAbiVersion,backend->vm,backend->submit,backend->ring,32,backend->output_bits):kIOReturnNoResources;
    if(!result)result=core->setScope(slot*NativeAdrenoAbiMaxQueues,windows[slot].start,windows[slot].bytes);
    if(!result)result=core->attachTimer(backend->timer);
    if(!result)result=core->attachStatus(backend->status);
    if(!result)result=core->attachDeviceInfo(&backend->info);
    if(!result){live=true;slots[slot]=true;backend->ring->requestContextReset();}
    else if(core)core->releaseAll(&backend->clock,0);
    IOLockUnlock(client_gate);return live;
}
void PongAdrenoUserClient::dropViews(uint32_t token)
{
    for(;;) {
        IOMemoryDescriptor *memory=nullptr;
        IOLockLock(client_gate);
        if(views)for(unsigned i=0;i<NativeAdrenoClientMaxObjects;i++)
            if(views[i].memory && (!token || views[i].token==token)) {memory=views[i].memory;views[i]={};break;}
        IOLockUnlock(client_gate);
        if(!memory)return;
        while(removeMappingForDescriptor(memory)){}
        memory->release();
    }
}
IOReturn PongAdrenoUserClient::clientClose()
{
    if(operations)IORecursiveLockLock(operations);
    IOLockLock(client_gate);closing=true;IOLockUnlock(client_gate);
    dropViews(0);
    IOLockLock(client_gate);
    IOReturn result=live?core->releaseAll(&backend->clock,500000):kIOReturnSuccess;
    if(!result){if(live)slots[slot]=false;live=false;}
    /* Failed DMA retirement retains this core and its slot; never recycle it. */
    IOLockUnlock(client_gate);
    if(operations)IORecursiveLockUnlock(operations);
    if(!result)terminate();return result;
}
void PongAdrenoUserClient::free()
{
    if(views){dropViews(0);IODelete(views,View,NativeAdrenoClientMaxObjects);views=nullptr;}
    if(core && !live){delete core;core=nullptr;}
    if(operations){IORecursiveLockFree(operations);operations=nullptr;}
    IOUserClient::free();
}
IOReturn PongAdrenoUserClient::clientMemoryForType(UInt32 token,IOOptionBits *options,IOMemoryDescriptor **memory)
{
    if(!memory || !options)return kIOReturnBadArgument;*memory=nullptr;
    if(!operations)return kIOReturnNotReady;
    IORecursiveLockLock(operations);
    IOLockLock(client_gate);IOReturn result=kIOReturnNotReady;
    if(live && !closing) {
        for(unsigned i=0;i<NativeAdrenoClientMaxObjects;i++)if(views[i].memory && views[i].token==token) {
            *memory=views[i].memory;(*memory)->retain();result=kIOReturnSuccess;break;
        }
        if(!*memory) {
            NativeAdrenoMemory *bo=core->memoryForToken(token);
            size_t count=bo?bo->spanCount():0;
            if(count && count<=65536) {
                auto ranges=IONewData(IOAddressRange,count);
                if(ranges) {
                    for(size_t i=0;i<count;i++)ranges[i]={bo->spans()[i].physical,bo->spans()[i].bytes};
                    IOMemoryDescriptor *view=IOMemoryDescriptor::withAddressRanges(ranges,UInt32(count),
                        IOOptionBits(kIODirectionInOut)|IOOptionBits(kIOMemoryTypePhysical64),nullptr);
                    IODeleteData(ranges,IOAddressRange,count);
                    if(view) {
                        for(unsigned i=0;i<NativeAdrenoClientMaxObjects;i++)if(!views[i].memory) {
                            views[i]={token,view};view->retain();*memory=view;result=kIOReturnSuccess;break;
                        }
                        if(!*memory)view->release();
                    }
                }
            }
        }
    }
    *options=kIOMapAnywhere|kIOMapInhibitCache;IOLockUnlock(client_gate);
    IORecursiveLockUnlock(operations);return result;
}
IOReturn PongAdrenoUserClient::run(unsigned method,const void *in,size_t inBytes,void *out,size_t outBytes)
{
    if(current_task()!=owner)return kIOReturnNotPermitted;
    if(method==NativeAdrenoMethodBoCpuUnmap && inBytes==sizeof(NativeAdrenoAbiBoCpuUnmap)) {
        const auto *request=static_cast<const NativeAdrenoAbiBoCpuUnmap *>(in);
        IOLockLock(client_gate);bool last=live && core->mappingLeases(request->token)<=1;IOLockUnlock(client_gate);
        if(last)dropViews(request->token);
    }
    IOLockLock(client_gate);IOReturn result=kIOReturnNotReady;
    if(live && !closing && backend->healthy()) {
        /* Each page-table update runs with ALL ring jobs genuinely retired. */
        result=kIOReturnSuccess;
        if(method==NativeAdrenoMethodBoMap || method==NativeAdrenoMethodBoFree)
            result=backend->submit->waitAll(&backend->clock,500000);
        if(!result && method==NativeAdrenoMethodBoImport) {
            NativeAdrenoAbiBoImport request{};NativeAdrenoAbiBoAllocReply reply{};
            memcpy(&request,in,sizeof(request));result=core->importBo(&request,owner,&reply);
            if(!result)memcpy(out,&reply,sizeof(reply));
        } else if(!result) {
            /* A caller cannot monopolize the client_gate with an unbounded fence wait. */
            NativeAdrenoAbiWaitFence wait{};
            if(method==NativeAdrenoMethodWaitFence) {
                memcpy(&wait,in,sizeof(wait));if(wait.timeout_ns>500000000)wait.timeout_ns=500000000;
                in=&wait;
            }
            result=core->dispatch(method,in,inBytes,out,outBytes,&backend->clock);
        }
    }
    IOLockUnlock(client_gate);return result;
}
IOReturn PongAdrenoUserClient::action(OSObject *target,void *,IOExternalMethodArguments *args)
{
    auto *client=OSDynamicCast(PongAdrenoUserClient,target);
    if(!client || !args)return kIOReturnBadArgument;
    return client->run(args->selector,args->structureInput,args->structureInputSize,args->structureOutput,args->structureOutputSize);
}
IOReturn PongAdrenoUserClient::externalMethod(uint32_t selector,IOExternalMethodArguments *args,IOExternalMethodDispatch *,OSObject *,void *)
{
    size_t input=NativeAdrenoUserClient::inputSize(selector),output=NativeAdrenoUserClient::outputSize(selector);
    if(input==SIZE_MAX || output==SIZE_MAX)return kIOReturnUnsupported;
    if(!operations)return kIOReturnNotReady;
    IOExternalMethodDispatch dispatch={action,0,uint32_t(input),0,uint32_t(output)};
    IORecursiveLockLock(operations);
    auto result=IOUserClient::externalMethod(selector,args,&dispatch,this,nullptr);
    IORecursiveLockUnlock(operations);return result;
}
IOReturn NativeAdrenoGpuPublished::newUserClient(task_t task,void *security,UInt32 type,OSDictionary *properties,IOUserClient **handler)
{
    if(!handler || (type!=0 && type!=0x4147))return kIOReturnUnsupported;*handler=nullptr;
    if(!backend || !backend->healthy())return kIOReturnNotReady;
    auto *client=new PongAdrenoUserClient;
    if(!client)return kIOReturnNoMemory;
    if(!client->initWithTask(task,security,type,properties) || !client->attach(this) || !client->start(this) || !client->mount(task)) {
        client->detach(this);client->release();return kIOReturnNoResources;
    }
    *handler=client;return kIOReturnSuccess;
}
bool PongAdrenoPublishClient(PongCpBackend *device)
{
    if(backend || !device || !device->healthy())return false;
    client_gate=IOLockAlloc();if(!client_gate)return false;backend=device;
    auto *service=new NativeAdrenoGpuPublished;
    if(!service || !service->init() || !service->attach(IOService::getServiceRoot()))return false;
    service->setName("PongAdreno730");
    service->setProperty("PongAdrenoCPReady",true);
    service->setProperty("PongChipID",device->info.chip_id,32);
    service->setProperty("PongNativePageBytes",4096,32);
    /* The service exists only after real CP fences. This opt-in exposes the
     * native ABI to the render server, not an AGX or IOAcceleratorES ABI.
     * The plugin separately checks the guest/runtime before registration. */
    unsigned metal=0;
    bool enabled=PE_parse_boot_argn("pong_metal",&metal,sizeof(metal)) && metal==1;
    service->setProperty("PongMetal20G81Enabled",enabled);
    if(enabled)service->setProperty("NativeSandboxGrantProcess","backboardd");
    service->registerService();return true;
}
