/* SPDX-License-Identifier: MIT */
/* Match the Pong IOSurface extension in the Darwin24 patch. Send rights
 * authenticate storage ownership; real command completion releases it. */
#pragma once
#include <mach/mach.h>
#include <pthread.h>
#include <vector>
#include <dlfcn.h>
#include <unistd.h>
namespace pongsurface {
using Call=kern_return_t(*)(unsigned,unsigned,const uint64_t *,unsigned,uint64_t *,unsigned *);
static unsigned connection;
static Call call;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static void open() {
    void *io=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);
    if(!io)return;
    auto matching=(void *(*)(const char *))dlsym(io,"IOServiceMatching");
    auto service=(unsigned(*)(unsigned,void *))dlsym(io,"IOServiceGetMatchingService");
    auto release=(kern_return_t(*)(unsigned))dlsym(io,"IOObjectRelease");
    auto connect=(kern_return_t(*)(unsigned,mach_port_t,unsigned,unsigned *))dlsym(io,"IOServiceOpen");
    call=(Call)dlsym(io,"IOConnectCallScalarMethod");
    if(!matching||!service||!release||!connect||!call)return;
    unsigned entry=service(0,matching("IOCoreSurfaceRoot"));
    if(entry){(void)connect(entry,mach_task_self(),0,&connection);release(entry);}
}
static kern_return_t handoff(mach_port_t right,bool end,bool failed) {
    pthread_once(&once,open);
    if(!connection||!call)return 0xe00002d8; // native not-ready, never success
    const uint64_t input[]={right,end?1u:0u,failed?1u:0u};
    return call(connection,0x504f4701u,input,3,nullptr,nullptr);
}
static void complete(std::vector<mach_port_t> &rights,bool failed) {
    for(auto right:rights){(void)handoff(right,true,failed);mach_port_deallocate(mach_task_self(),right);}
    rights.clear();
}
static bool begin(NSMutableSet *textures,std::vector<mach_port_t> &rights) {
    std::vector<std::pair<uint32_t,IOSurfaceRef>> unique;
    for(id texture in textures) {
        IOSurfaceRef surface=[texture iosurface];
        if(surface)unique.emplace_back(IOSurfaceGetID(surface),surface);
    }
    std::sort(unique.begin(),unique.end(),[](const auto &a,const auto &b){return a.first<b.first;});
    unique.erase(std::unique(unique.begin(),unique.end(),[](const auto &a,const auto &b){return a.first==b.first;}),unique.end());
    for(unsigned attempt=0;attempt<100;attempt++) {
        kern_return_t result=0;
        for(auto &entry:unique) {
            mach_port_t right=IOSurfaceCreateMachPort(entry.second);
            if(!right){result=0xe00002bd;break;}
            result=handoff(right,false,false);
            if(result){mach_port_deallocate(mach_task_self(),right);break;}
            rights.push_back(right);
        }
        if(!result)return true;
        // No partial acquisition waits on another surface. This avoids
        // a CPU display reader/GPU multi-surface acquisition deadlock.
        complete(rights,false);
        if(unsigned(result)!=0xe00002d5)return false; // only retry busy
        usleep(1000);
    }
    return false;
}
}
