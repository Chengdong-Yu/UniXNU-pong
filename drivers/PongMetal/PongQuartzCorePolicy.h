/* SPDX-License-Identifier: MIT */
#ifndef PONG_QUARTZ_CORE_POLICY_H
#define PONG_QUARTZ_CORE_POLICY_H
#include <dlfcn.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <string.h>

/* Sheng's native Metal entry disables Apple-only compression in QuartzCore's
 * process-local state. These are the corresponding symbols from the supplied
 * 20G81 arm64 QuartzCore, not Sheng's iOS17 addresses. No code or dispatch_once
 * token is patched. Run each original once initializer first, then withdraw only the
 * image path that this frontend cannot consume.
 *
 * CA's ASTC source-image path additionally requires Apple's texture-layout
 * and raw-memory texture SPI. Native Vulkan ASTC sampling alone does not make
 * that path usable. With this capability withdrawn, 20G81's CompressedImage
 * factory explicitly falls back to image decoding; ordinary decoded textures,
 * Metal rendering, blur and GPU scanout remain on the native device. */
static int PongApplyQuartzCorePolicy20G81()
{
    static unsigned state;
    if(__atomic_load_n(&state,__ATOMIC_ACQUIRE)==2)return 0;
    unsigned expected=0;
    if(!__atomic_compare_exchange_n(&state,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return -1;
    auto finish=[](int status) {
        __atomic_store_n(&state,status==0?2u:0u,__ATOMIC_RELEASE);
        return status;
    };
    /* Do not load QuartzCore recursively from Metal catalog construction. */
    void *entry=dlsym(RTLD_DEFAULT,"CARenderServerStart");
    Dl_info image={};
    if(!entry || !dladdr(entry,&image) || !image.dli_fbase)return finish(-2);
    const auto *header=static_cast<const mach_header_64 *>(image.dli_fbase);
    if(header->magic!=MH_MAGIC_64 || header->ncmds>1024 || header->sizeofcmds>1024*1024)return finish(-3);
    static const uint8_t uuid[16]={0x11,0x38,0x7d,0x58,0xbc,0xb1,0x3d,0x0d,0xb2,0xfc,0x97,0x43,0x16,0xe9,0x33,0xb9};
    const uint8_t *commands=reinterpret_cast<const uint8_t *>(header+1);
    bool matching=false;
    for(uint32_t i=0,at=0;i<header->ncmds;++i) {
        if(at>header->sizeofcmds || header->sizeofcmds-at<sizeof(load_command))return finish(-3);
        const auto *command=reinterpret_cast<const load_command *>(commands+at);
        if(command->cmdsize<sizeof(load_command) || command->cmdsize>header->sizeofcmds-at)return finish(-3);
        if(command->cmd==LC_UUID && command->cmdsize>=sizeof(uuid_command))
            matching=memcmp(reinterpret_cast<const uuid_command *>(command)->uuid,uuid,sizeof uuid)==0;
        at+=command->cmdsize;
    }
    if(!matching)return finish(-4);
    const uintptr_t slide=reinterpret_cast<uintptr_t>(image.dli_fbase)-UINT64_C(0x188203000);
    /* These queries are inlined and are not exported in 20G81. Use their actual
     * global blocks with dispatch_once_f, preserving the original initializer,
     * context and once synchronization instead of resolving absent exports. */
    struct Flag {uintptr_t value,once,block,initializer;};
    const Flag flags[]={
        {0x1e1c266e8,0x1e1c26700,0x1d587d768,0x18831390c}, // ASTC source images
        {0x1e1c25588,0x1e1c259c0,0x1d587d268,0x1882f9da4}, // Universal compression
        {0x1dcca20b8,0x1e1c257b0,0x1d587d2a8,0x188310a34}, // Lossy universal
        {0x1e1c256a8,0x1e1c25fc0,0x1d587d1e8,0x1882fa954}, // IOSurface compression
        {0x1dcca1438,0x1dcca0670,0x1d587d228,0x188449f24}, // Framebuffer compression
    };
    unsigned changed=0;
    for(const auto &flag:flags) {
        uintptr_t invoke=0;vm_size_t blockCopied=0;
        if(vm_read_overwrite(mach_task_self(),flag.block+slide+16,sizeof invoke,
                reinterpret_cast<vm_address_t>(&invoke),&blockCopied)!=KERN_SUCCESS ||
                blockCopied!=sizeof invoke || invoke!=flag.initializer+slide)return finish(-5);
        dispatch_once_f(reinterpret_cast<dispatch_once_t *>(flag.once+slide),
            reinterpret_cast<void *>(flag.block+slide),reinterpret_cast<dispatch_function_t>(invoke));
        uint64_t once=0;uint8_t value=0;vm_size_t copied=0;
        if(vm_read_overwrite(mach_task_self(),flag.once+slide,sizeof once,
                reinterpret_cast<vm_address_t>(&once),&copied)!=KERN_SUCCESS || copied!=sizeof once || once!=UINT64_MAX)return finish(-6);
        if(vm_read_overwrite(mach_task_self(),flag.value+slide,sizeof value,
                reinterpret_cast<vm_address_t>(&value),&copied)!=KERN_SUCCESS || copied!=sizeof value || value>1)return finish(-6);
        if(!value)continue;
        vm_address_t address=flag.value+slide;vm_size_t length=0;
        vm_region_basic_info_data_64_t region={};mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object=MACH_PORT_NULL;
        kern_return_t status=vm_region_64(mach_task_self(),&address,&length,VM_REGION_BASIC_INFO_64,
            reinterpret_cast<vm_region_info_t>(&region),&count,&object);
        if(object!=MACH_PORT_NULL)mach_port_deallocate(mach_task_self(),object);
        if(status!=KERN_SUCCESS || !(region.protection&VM_PROT_WRITE) ||
           address>flag.value+slide || flag.value+slide-address>=length)return finish(-7);
        uint8_t zero=0;
        if(vm_write(mach_task_self(),flag.value+slide,reinterpret_cast<vm_offset_t>(&zero),sizeof zero)!=KERN_SUCCESS)return finish(-8);
        ++changed;
    }
    char line[160];snprintf(line,sizeof(line),"20G81 decoded-image policy active; Apple texture-layout path disabled (%u cached flags changed)",changed);
    report(line,0);
    return finish(0);
}
#endif
