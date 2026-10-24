/* SPDX-License-Identifier: MIT */
/* Sheng's native Metal accelerator publication, bound to Pong's 20G81
 * parent ABI. All user clients execute the same real NativeAdreno backend. */
#include "PongIOAcceleratorABI.h"
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <mach/kmod.h>
extern "C" void PongIOAcceleratorMetaConstruct(void *);
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void pong_ramoops_diagnostic_write(const char *,int);
extern "C" void PongAdrenoAcceleratorStaticInit(void);
extern IOReturn PongAdrenoNewUserClient(IOService *,task_t,void *,UInt32,OSDictionary *,IOUserClient **);
extern bool PongAdrenoConfigureService(IOService *);

extern "C" IOService *native_adreno_accelerator_allocate(void);
extern "C" unsigned native_adreno_accelerator_instance_size(void);

IOReturn PongAdrenoPublishMetalAccelerator()
{
    const OSSymbol *name=OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent=name?OSMetaClass::getMetaClassWithName(name):nullptr;
    if(name)name->release();
    /* Pong implements IOSurface in the native kernel. Loading all of Apple's
     * IOGPUFamily dependencies would initialize a second IOSurfaceRoot. Run
     * only the original20G81 empty IOAcceleratorES metaclass constructor;
     * its C2/D2 and vtable also remain the actual original implementations.
     * No Apple IOGPU/AGX hardware provider is started or impersonated. */
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!handle)return kIOReturnNoResources;
    if(!parent)PongIOAcceleratorMetaConstruct((void *)&IOAcceleratorES::gMetaClass);
    else if(parent->getClassSize()!=sizeof(IOAcceleratorES)) {
        (void)OSMetaClass::postModLoad(handle);return kIOReturnUnsupported;
    }
    PongAdrenoAcceleratorStaticInit();
    IOReturn result=OSMetaClass::postModLoad(handle);
    if(result)return result;
    auto *service=native_adreno_accelerator_allocate();
    if(!service)return kIOReturnNoMemory;
    if(!service->init() || !PongAdrenoConfigureService(service)) {
        service->release();return kIOReturnNotReady;
    }
    service->setName("PongAdreno730Metal");
    service->setProperty("MetalPluginName","PongAdrenoMetal");
    service->setProperty("MetalPluginClassName","PongAdrenoMetalDevice");
    if(!service->attach(IOService::getPlatform())) {service->release();return kIOReturnNotAttached;}
    service->registerService();
    const char message[]="PONG METAL: real IOAcceleratorES provider published for20G81 discovery\n";
    IOLog("%s",message);pong_ramoops_diagnostic_write(message,sizeof(message)-1);
    return kIOReturnSuccess;
}
