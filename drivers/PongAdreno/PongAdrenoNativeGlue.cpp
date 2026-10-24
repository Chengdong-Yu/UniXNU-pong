/* SPDX-License-Identifier: MIT */
/* A730 board -> complete upstream NativeAdreno publication/runtime. */
#include "PongAdrenoCp.h"
#include "PongAdrenoScanout.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <pexpert/pexpert.h>
#include <kern/task.h>
extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *,NativeAdrenoSubmit *,NativeAdrenoRing *,
    const NativeAdrenoRingClock *,const NativeAdrenoTimer *,const NativeAdrenoAbiDeviceInfo *,uint32_t,uint32_t);
extern "C" IOService *native_adreno_published_service(void);
extern "C" void pong_ramoops_diagnostic_write(const char *,int);
extern IOReturn PongAdrenoPublishMetalAccelerator();
static PongCpBackend *backend;
extern "C" bool pong_adreno_native_healthy(void)
{return backend && backend->healthy();}
extern "C" const NativeAdrenoStatus *pong_adreno_native_status(void)
{return backend?backend->status:nullptr;}
extern "C" IOReturn native_gop_handoff_call(uint32_t op,task_t task,const void *in,size_t inSize,void *out,size_t outSize)
{return pong_scanout_call(task,op+PongScanoutMode,in,inSize,out,outSize);}
extern "C" IOMemoryDescriptor *native_gop_handoff_memory(void)
{return pong_scanout_memory(current_task());}
bool PongAdrenoConfigureService(IOService *service)
{
    if(!service || !pong_adreno_native_healthy())return false;
    service->setProperty("PongAdrenoCPReady",true);
    service->setProperty("PongChipID",backend->info.chip_id,32);
    service->setProperty("PongNativePageBytes",4096,32);
    unsigned metal=0;
    const bool enabled=PE_parse_boot_argn("pong_metal",&metal,sizeof(metal)) && metal==1;
    service->setProperty("PongMetal20G81Enabled",enabled);
    if(enabled)service->setProperty("NativeSandboxGrantProcess","backboardd,SpringBoard,CollectionsPoster,WidgetRenderer_Default,PosterBoard,Weather,WeatherPoster,GradientPosterExtension");
    return true;
}
bool PongAdrenoPublishClient(PongCpBackend *device)
{
    if(backend || !device || !device->healthy())return false;
    backend=device;
    const IOReturn result=native_adreno_publish(device->vm,device->submit,device->ring,
        &device->clock,device->timer,&device->info,32,device->output_bits);
    if(result)return false;
    if(!PongAdrenoConfigureService(native_adreno_published_service()))return false;
    unsigned metal=0;
    if(PE_parse_boot_argn("pong_metal",&metal,sizeof(metal)) && metal==1) {
        const IOReturn status=PongAdrenoPublishMetalAccelerator();
        char line[160];const int n=snprintf(line,sizeof(line),
            "PONG METAL: NativeAdreno complete runtime publication status=0x%x\n",status);
        IOLog("%s",line);pong_ramoops_diagnostic_write(line,n);
        if(status)return false;
    }
    return true;
}
