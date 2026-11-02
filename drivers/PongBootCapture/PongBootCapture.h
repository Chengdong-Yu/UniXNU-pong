/* SPDX-License-Identifier: MIT
 * One bounded diagnostic boot -> synced restart -> existing REC/pstore reader.
 * Pong REC DT: PMK8350 SID0 SDAM7100, restart@48 bits<1 7>, APPS EE0.
 * SPMI-v7 transaction structure follows UniXNU-sheng BootResetCore.h; all
 * physical windows below were checked against Pong's live reg/reg-names.
 * This is a software deadline, not a hardware watchdog or a panic bypass.
 */
#pragma once
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOPlatformExpert.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
extern "C" {
extern int (*PE_halt_restart)(unsigned int);
int reboot_kernel(int,char *);
void pong_ramoops_console_write(const char *,int);
void pong_ramoops_diagnostic_write(const char *,int);
}
namespace PongBootCapture {
struct Window {
 IOMemoryMap *map=nullptr;volatile uint32_t *p=nullptr;
 bool open(uint64_t address,size_t bytes) {
  auto d=IODeviceMemory::withRange(address,bytes);if(!d)return false;
  map=d->map(kIOMapAnywhere|kIOMapInhibitCache);d->release();
  p=map?reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()):nullptr;return p;
 }
 uint32_t read(unsigned o){auto v=p[o/4];__asm__ volatile("dmb oshld":::"memory");return v;}
 void write(unsigned o,uint32_t v){p[o/4]=v;__asm__ volatile("dsb sy":::"memory");}
};
static Window core,config,observer,channel,imem;
static unsigned seconds,target=1,apid,armed,resetStarted,shutdownRequested,forcedFallback;
static char id[64];static uint64_t origin;
static int (*previous)(unsigned int);
static uint64_t elapsed() {uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-origin,&ns);return ns/1000000000;}
static void record(const char *phase,int result=0) {
 char line[192];const int n=snprintf(line,sizeof line,"PONG CAPTURE: id=%s phase=%s result=%d\n",id,phase,result);
 if(n>0){const int size=n<int(sizeof line)?n:int(sizeof line)-1;pong_ramoops_console_write(line,size);pong_ramoops_diagnostic_write(line,size);}
}
static bool wait(Window &w,unsigned offset) {
 // Arbiter commands have a finite poll budget even with interrupts disabled.
 for(unsigned n=0;n<100000;n++){unsigned s=w.read(offset+8);if(s&1)return (s&15)==1;}
 return false;
}
static bool readCell(uint8_t &v) {
 observer.write(apid*0x20,(1u<<27)|(0x48u<<4));
 if(!wait(observer,apid*0x20))return false;v=observer.read(apid*0x20+0x18);return true;
}
static bool markTarget() {
 uint8_t before=0,after=0;if(!readCell(before))return false;
 const uint8_t value=(before&1)|(target<<1);
 channel.write(0x10,value);channel.write(0,0x48u<<4);
 if(!wait(channel,0)||!readCell(after)||after!=value)return false;
 imem.write(0,target==1?0x77665502u:0x77665500u);
 return imem.read(0)==(target==1?0x77665502u:0x77665500u);
}
static bool prepare() {
 if(!core.open(0xc400000,0x3000)||!config.open(0xc42d000,0x4000))return false;
 unsigned version=core.read(0),count=core.read(4)&0x7ff;
 if(version<0x70000000u||version>=0x80000000u||!count||count>1024)return false;
 bool found=false;
 for(unsigned n=0;n<count;n++)if(((core.read(0x2000+4*n)>>8)&0xfff)==0x71&&(config.read(4*n)&7)==0){apid=n;found=true;break;}
 if(!found)return false;
 if(!observer.open(0xc440000,(apid*0x20+0x20+4095)&~size_t(4095))||
    !channel.open(0xc500000+uint64_t(apid)*4096,4096)||!imem.open(0x146aa65c,4))return false;
 uint8_t seen;return readCell(seen);
}
static int reset(unsigned type) {
 if(!__atomic_load_n(&armed,__ATOMIC_ACQUIRE)||
    (type!=kPERestartCPU&&type!=kPEPanicRestartCPU&&type!=kPEPanicRestartCPUNoCallouts))
  return previous?previous(type):-1;
 if(__atomic_exchange_n(&resetStarted,1,__ATOMIC_ACQ_REL))return -1;
 record(shutdownRequested&&type==kPERestartCPU&&!forcedFallback?"logs-committed-reset":
        forcedFallback?"forced-reset-sync-unconfirmed":"panic-or-external-reset");
 __asm__ volatile("dsb sy":::"memory");
 register uint64_t x0 __asm__("x0")=0x84000009u,x1 __asm__("x1")=0,x2 __asm__("x2")=0,x3 __asm__("x3")=0;
 __asm__ volatile("smc #0":"+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3)::"memory","x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17");
 record("reset-returned",int(x0));__atomic_store_n(&resetStarted,0,__ATOMIC_RELEASE);return -1;
}
static void finish(){thread_terminate(current_thread());thread_block(THREAD_CONTINUE_NULL);}
static void timer(void *,wait_result_t) {
 while(!__atomic_load_n(&armed,__ATOMIC_ACQUIRE))IOSleep(10);
 while(elapsed()<seconds)IOSleep(250);
 __atomic_store_n(&shutdownRequested,1,__ATOMIC_RELEASE);record("timed-return-sync");
 // RB_QUICK: native file-system flush remains enabled; RB_NOSYNC is absent.
 const int result=reboot_kernel(0x400,nullptr);record("native-reboot-returned",result);finish();
}
static void fallback(void *,wait_result_t) {
 while(!__atomic_load_n(&armed,__ATOMIC_ACQUIRE))IOSleep(10);
 while(elapsed()<seconds+30)IOSleep(250);
 __atomic_store_n(&forcedFallback,1,__ATOMIC_RELEASE);
 record("shutdown-deadline-fallback");reset(kPERestartCPU);finish();
}
static void start() {
 if(armed||!PE_parse_boot_argn("pong_capture_seconds",&seconds,sizeof seconds)||!seconds)return;
 PE_parse_boot_argn("pong_capture_target",&target,sizeof target);
 if(seconds<30||seconds>3600||(target!=1&&target!=2))return;
 if(!PE_parse_boot_argn("pong_capture_id",id,sizeof id)){seconds=0;return;}
 id[sizeof id-1]=0;for(unsigned n=0;id[n];n++)if(!((id[n]>='a'&&id[n]<='z')||(id[n]>='A'&&id[n]<='Z')||(id[n]>='0'&&id[n]<='9')||id[n]=='-'||id[n]=='_'||id[n]=='.')){seconds=0;return;}
 if(!id[0]||!prepare()){record("prepare-failed");return;}
 origin=mach_absolute_time();thread_t a=nullptr,b=nullptr;
 if(kernel_thread_start(timer,nullptr,&a)!=KERN_SUCCESS){record("timer-create-failed");return;}
 if(kernel_thread_start(fallback,nullptr,&b)!=KERN_SUCCESS){thread_terminate(a);thread_deallocate(a);record("fallback-create-failed");return;}
 if(!markTarget()){thread_terminate(a);thread_terminate(b);thread_deallocate(a);thread_deallocate(b);record("reboot-target-failed");return;}
 previous=PE_halt_restart;PE_halt_restart=reset;
 record("armed");__atomic_store_n(&armed,1,__ATOMIC_RELEASE);
 thread_deallocate(a);thread_deallocate(b);
 IOLog("Pong capture: %u seconds then %s; boot id=%s\n",seconds,target==1?"REC":"bootloader",id);
}
// CDC requests are explicit, once per boot, and never start the timed capture.
// Do shutdown work on its own thread: USB must remain able to deliver the ACK.
static uint32_t manualPending;
static void manual(void *argument,wait_result_t) {
 target=unsigned(uintptr_t(argument));strlcpy(id,"usb-manual",sizeof id);
 IOSleep(500);
 uint8_t oldCell=0;uint32_t oldImem=0;
 if(!prepare() || !readCell(oldCell)) {record("manual-prepare-failed");goto done;}
 oldImem=imem.read(0);
 if(!markTarget()) {record("manual-target-failed");goto restore;}
 previous=PE_halt_restart;PE_halt_restart=reset;
 __atomic_store_n(&shutdownRequested,1,__ATOMIC_RELEASE);
 __atomic_store_n(&armed,1,__ATOMIC_RELEASE);
 record(target==1?"usb-recovery-sync":"usb-fastboot-sync");
 record("native-reboot-returned",reboot_kernel(0x400,nullptr));
 __atomic_store_n(&armed,0,__ATOMIC_RELEASE);PE_halt_restart=previous;
restore:
 channel.write(0x10,oldCell);channel.write(0,0x48u<<4);(void)wait(channel,0);
 imem.write(0,oldImem);
done:
 __atomic_store_n(&manualPending,0,__ATOMIC_RELEASE);finish();
}
static bool request(unsigned destination) {
 if((destination!=1&&destination!=2)||armed ||
    __atomic_exchange_n(&manualPending,1,__ATOMIC_ACQ_REL))return false;
 thread_t t=nullptr;
 if(kernel_thread_start(manual,reinterpret_cast<void *>(uintptr_t(destination)),&t)!=KERN_SUCCESS) {
  __atomic_store_n(&manualPending,0,__ATOMIC_RELEASE);return false;
 }
 thread_deallocate(t);return true;
}
}
