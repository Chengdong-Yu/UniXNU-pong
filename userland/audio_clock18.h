/* SPDX-License-Identifier: MIT
 * The native IOAudio2 path hides the old ASPL driver, including its host
 * clock adapter. Keep that clock adaptation independent of PCM ownership.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <syslog.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/sysctl.h>
static void pong_audio_clock_report(const char *format,...) {
 char line[320];va_list args;va_start(args,format);vsnprintf(line,sizeof line,format,args);va_end(args);
 fprintf(stderr,"%s\n",line);size_t count=CTL_MAXNAME;int mib[CTL_MAXNAME];
 if(!sysctlnametomib("kern.pong_diagnostic",mib,&count))sysctl(mib,(u_int)count,NULL,NULL,line,strlen(line));
}


static int pong_native_audio_clock(void) {
    double (*frequency)(void)=dlsym(RTLD_DEFAULT,"AudioGetHostClockFrequency");
    if(!frequency)return 0; // CoreAudio can finish loading after this adapter constructor.
    mach_timebase_info_data_t tb={0};
    if(mach_timebase_info(&tb) || !tb.numer || !tb.denom)return -1;
    const double actual=1000000000.0*tb.denom/tb.numer,before=frequency();
    if(before>=actual-1.0 && before<=actual+1.0) {
        pong_audio_clock_report("Pong CoreAudio clock: native=%.0f actual=%.0f already aligned",before,actual);return 1;
    }
    // Only the known 22H374 Apple24MHz constant may be adapted. The kernel
    // independently checks the owner, shared-cache address and old bits.
    if(before!=24000000.0) {
        pong_audio_clock_report("Pong CoreAudio clock: unknown native=%.0f actual=%.0f; unchanged",before,actual);return 1;
    }
    uintptr_t symbol=(uintptr_t)frequency&((((uintptr_t)1)<<47)-1);
    const uint32_t *code=(const uint32_t *)symbol;
    if(code[0]==0xd503245fu){code++;symbol+=4;} // real optional BTI c
    const uint32_t adrp=code[0],ldr=code[1];
    if((adrp&0x9f000000u)!=0x90000000u || (ldr&0xffc00000u)!=0xfd400000u ||
       ((ldr>>5)&31)!=(adrp&31) || code[2]!=0xd65f03c0u){pong_audio_clock_report("Pong CoreAudio clock: unknown getter instructions %08x %08x %08x; unchanged",code[0],code[1],code[2]);return -1;}
    int64_t pages=(int64_t)(((adrp>>5)&0x7ffffu)<<2|((adrp>>29)&3));
    if(pages&(1<<20))pages-=1<<21;
    const uint64_t address=(symbol&~(uintptr_t)4095)+(uint64_t)(pages*4096)+((ldr>>10)&4095)*8;
    CFMutableDictionaryRef (*matching)(const char *)=dlsym(RTLD_DEFAULT,"IOServiceMatching");
    uint32_t (*service)(mach_port_t,CFDictionaryRef)=dlsym(RTLD_DEFAULT,"IOServiceGetMatchingService");
    kern_return_t (*openService)(uint32_t,mach_port_t,uint32_t,uint32_t *)=dlsym(RTLD_DEFAULT,"IOServiceOpen");
    kern_return_t (*call)(uint32_t,uint32_t,const uint64_t *,uint32_t,uint64_t *,uint32_t *)=dlsym(RTLD_DEFAULT,"IOConnectCallScalarMethod");
    kern_return_t (*closeService)(uint32_t)=dlsym(RTLD_DEFAULT,"IOServiceClose");
    kern_return_t (*release)(uint32_t)=dlsym(RTLD_DEFAULT,"IOObjectRelease");
    if(!matching || !service || !openService || !call || !closeService || !release)return 0;
    const uint32_t provider=service(MACH_PORT_NULL,matching("PongAudio"));
    if(!provider)return 0;
    uint32_t connection=0;const kern_return_t opened=openService(provider,mach_task_self(),0,&connection);
    release(provider);if(opened)return 0;
    const kern_return_t result=call(connection,4,&address,1,NULL,NULL);
    closeService(connection);
    pong_audio_clock_report("Pong CoreAudio clock: native=%.0f actual=%.0f result=0x%x observed=%.0f",before,actual,result,frequency());
    return result==KERN_SUCCESS?1:-1;
}
static void *pong_audio_clock_wait(void *unused) {
    (void)unused;
    for(unsigned i=0;i<900;i++){
        usleep(100000);
        if(pong_native_audio_clock())return NULL;
    }
    pong_audio_clock_report("Pong CoreAudio clock: real provider unavailable; native clock retained");return NULL;
}
__attribute__((constructor)) static void pong_audio_clock_init(void) {
    if(strcmp(getprogname(),"audiomxd"))return;
    pong_audio_clock_report("Pong CoreAudio clock: native audiomxd adapter loaded");
    const int result=pong_native_audio_clock();
    if(result)return;
    pthread_t thread;
    if(!pthread_create(&thread,NULL,pong_audio_clock_wait,NULL))pthread_detach(thread);
}

// The adjacent 22H374 conversion exports are fixed 125/3 and3/125
// (24MHz), independently of the frequency getter above. Native HAL sample
// timestamps and DSP playback use Pong's actual mach counter; retain that
// same conversion in audiomxd across stop/start, not just its frequency.
extern uint64_t AudioConvertHostTimeToNanos(uint64_t);
extern uint64_t AudioConvertNanosToHostTime(uint64_t);
static mach_timebase_info_data_t pong_clock_timebase;
static pthread_once_t pong_clock_once=PTHREAD_ONCE_INIT;
static void pong_clock_get_timebase(void){(void)mach_timebase_info(&pong_clock_timebase);}
static uint64_t pong_clock_convert(uint64_t value,bool to_ns) {
 pthread_once(&pong_clock_once,pong_clock_get_timebase);
 if(!strcmp(getprogname(),"audiomxd") && pong_clock_timebase.numer && pong_clock_timebase.denom) {
  const uint32_t n=to_ns?pong_clock_timebase.numer:pong_clock_timebase.denom;
  const uint32_t d=to_ns?pong_clock_timebase.denom:pong_clock_timebase.numer;
  return (uint64_t)(((__uint128_t)value*n)/d);
 }
 uint64_t (*native)(uint64_t)=dlsym(RTLD_NEXT,to_ns?"AudioConvertHostTimeToNanos":"AudioConvertNanosToHostTime");
 return native?native(value):value;
}
static uint64_t pong_host_to_ns(uint64_t value){return pong_clock_convert(value,true);}
static uint64_t pong_ns_to_host(uint64_t value){return pong_clock_convert(value,false);}
__attribute__((used,section("__DATA,__interpose")))
static const struct {const void *replacement,*original;} pong_audio_clock_interpose[]={
 {(const void *)pong_host_to_ns,(const void *)AudioConvertHostTimeToNanos},
 {(const void *)pong_ns_to_host,(const void *)AudioConvertNanosToHostTime}
};
