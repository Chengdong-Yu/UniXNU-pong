/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Pong SM8475 stereo playback. Qualcomm wire codecs and the shared-ring ABI
 * come from UniXNU-sheng; board clocks, SID, TFA9873 and MI2S route are Pong.
 * No tone, calibration, OTP/MTP programming, or unimplemented input endpoint.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <libkern/c++/OSData.h>
extern "C" {
#include "ShengAdsp.h"
#include "ShengGpr.h"
#include "ShengAudioReach.h"
#include "ShengAudioAbi.h"
#include "GeniI2c.h"
#include "../PongAdreno/ArmSmmu4K/ArmSmmu4KContext.h"
void PongAudioServiceStaticInit(void);
void sheng_audio_service_start(void);
bool pong_smmu_lease_lock(void);
void pong_smmu_lease_unlock(void);
volatile int32_t native_audio_stats[16];
extern volatile int32_t native_audio_service[8];
}
extern kmod_info_t g_kernel_kmod_info;
namespace {
#include "../NativeUfs/SmmuTakeover.h"
#include "PongAmpProfiles.h"
constexpr unsigned PeriodFrames=480,PeriodBytes=480*2*4,Slots=8,Lead=30*480,Port=0x2001;
constexpr unsigned BufferBytes=65536,TableBytes=65536;
struct Window {
 IOMemoryMap *map=nullptr;volatile uint8_t *v=nullptr;
 bool open(uint64_t pa,size_t n,IOOptionBits cache=kIOMapInhibitCache){auto d=IODeviceMemory::withRange(pa,n);map=d?d->map(kIOMapAnywhere|cache):nullptr;if(d)d->release();v=map?(volatile uint8_t *)map->getVirtualAddress():nullptr;return v;}
 unsigned r(unsigned o){unsigned x=*(volatile uint32_t *)(v+o);__asm__ volatile("dmb oshld":::"memory");return x;}
 void w(unsigned o,unsigned x){*(volatile uint32_t *)(v+o)=x;__asm__ volatile("dsb oshst":::"memory");}
};
Window gcc,pins,se,dma,smmu;GeniI2c bus;
IOBufferMemoryDescriptor *control,*outRing,*tables;
ShengAudioControl *ctl;int16_t *out;
uint64_t dmaPa,dspAddress,tablePa,cbOffset;
ShengAdspChannel *channel;IOLock *lock;
struct Packet{unsigned length;uint8_t data[512];} queue[32];unsigned head,count;
volatile unsigned up,wanted,serving;
unsigned token=0x80000000,handle,writeToken,slotToken[Slots];bool running;
uint64_t sequence,completed;
int64_t baseLead;
bool active[Slots];unsigned inflight;
uint64_t nowMs(){uint64_t n;absolutetime_to_nanoseconds(mach_absolute_time(),&n);return n/1000000;}
void fail(int step,int error){native_audio_stats[0]=-step;native_audio_stats[1]=error;IOLog("PongAudio: failed stage=%d error=%d; endpoint not usable\n",step,error);}
bool wait(Window &w,unsigned off,unsigned mask,unsigned value,unsigned ms){uint64_t end=nowMs()+ms;do{if((w.r(off)&mask)==value)return true;IOSleep(1);}while(nowMs()<end);return false;}
void state(void *,int value){__atomic_store_n(&up,value?1u:0u,__ATOMIC_RELEASE);IOLockWakeup(lock,&count,false);}
void receive(void *,const uint8_t *data,uint32_t n){IOLockLock(lock);if(n<=512&&count<32){auto &p=queue[(head+count)%32];p.length=n;memcpy(p.data,data,n);count++;}else native_audio_stats[11]++;IOLockWakeup(lock,&count,false);IOLockUnlock(lock);}
bool next(ArAnswer &answer,unsigned ms){uint64_t end=nowMs()+ms;for(;;){Packet p={};IOLockLock(lock);if(count){p=queue[head];head=(head+1)%32;count--;}else if(nowMs()<end){uint64_t dl;clock_interval_to_deadline(10,kMillisecondScale,&dl);IOLockSleepDeadline(lock,&count,dl,THREAD_UNINT);}IOLockUnlock(lock);if(p.length&&ar_parse(p.data,p.length,&answer)>0)return true;if(nowMs()>=end||!up)return false;}}
bool send(const uint8_t *b,unsigned n){return n&&sheng_adsp_send(channel,b,n)==0;}
void completedWrite(const ArAnswer &a);
bool command(const uint8_t *b,unsigned n,unsigned opcode){
 if(n<GPR_HEADER_BYTES||!send(b,n)){native_audio_stats[1]=-35;return false;}
 unsigned expected=0;memcpy(&expected,b+16,sizeof expected);
 uint64_t end=nowMs()+2000;ArAnswer a={};
 while(nowMs()<end){
  if(!next(a,200))continue;
  // Buffer completions belong to the stream even while an APM command is
  // pending. Consuming and discarding them leaks the in-flight slots.
  if(a.opcode==AR_OP_WRITE_DONE){completedWrite(a);continue;}
  if(a.token!=expected)continue;
  if(a.opcode==AR_OP_MEM_MAP_DONE&&opcode==AR_OP_MEM_MAP){handle=a.handle;return handle!=0;}
  if(a.opcode==GPR_OP_BASIC_RESULT&&a.result_opcode==opcode){
   native_audio_stats[1]=a.status;
   if(a.status){IOLog("PongAudio: APM opcode=%x status=%u\n",opcode,a.status);return false;}
   // A successful map ACK is not its handle. Wait for MEM_MAP_DONE.
   if(opcode!=AR_OP_MEM_MAP)return true;
  }
 }
 native_audio_stats[1]=-60;
 IOLog("PongAudio: APM timeout opcode=%x token=%x\n",opcode,expected);return false;
}
bool framework(){uint8_t b[512];for(unsigned i=0;i<50&&up;i++){GprPacket p={};p.dst_domain=2;p.src_domain=3;p.src_port=Port;p.dst_port=1;p.token=++token;p.opcode=GPR_OP_APM_GET_SPF_STATE;if(send(b,gpr_encode(b,sizeof b,&p,nullptr,0))){ArAnswer a={};uint64_t end=nowMs()+1000;while(nowMs()<end){if(!next(a,100))continue;if(a.token==token&&a.opcode==GPR_OP_APM_SPF_STATE&&a.status==1)return true;}}IOSleep(100);}return false;}
bool transfer(unsigned address,const uint8_t *tx,unsigned n,uint16_t *value){bus.address=address;int r=value?geni_i2c_start_read_bytes(&bus,tx,n,2):geni_i2c_start_write_bytes(&bus,tx,n);if(r)return false;for(unsigned phase=0;phase<3;phase++){for(unsigned i=0;i<1000;i++){r=geni_i2c_poll(&bus);if(r==GeniDone){if(value)*value=(uint16_t(bus.rx&255)<<8)|uint16_t((bus.rx>>8)&255);return true;}if(r==GeniFailed)return false;IODelay(20);}if(geni_i2c_timeout(&bus)!=GeniPending)break;}return false;}
bool regRead(unsigned amp,unsigned reg,uint16_t &v){uint8_t b[]={uint8_t(reg)};return transfer(0x34+amp,b,1,&v);}
bool regWrite(unsigned amp,unsigned reg,unsigned v){uint8_t b[]={uint8_t(reg),uint8_t(v>>8),uint8_t(v)};return transfer(0x34+amp,b,3,nullptr);}
bool field(unsigned amp,unsigned id,unsigned value){uint16_t r;unsigned reg=id>>8,shift=(id>>4)&15,width=(id&15)+1;if(width+shift>16||!regRead(amp,reg,r))return false;unsigned mask=((1u<<width)-1)<<shift;if(reg>=0xf0)return (r&mask)==((value<<shift)&mask);return regWrite(amp,reg,(r&~mask)|((value<<shift)&mask));}
void pin(unsigned n,unsigned config,int high){
 const unsigned reg=n*0x1000,old=pins.r(reg);
 // Match Nothing's msm_pinmux_set_mux: preserve pad/ownership bits and
 // claim APPS control only when this pad advertises EGPIO presence.
 config=(old&~0x3ffu)|(config&0x3ffu);
 if(old&(1u<<11))config|=1u<<12;
 if(high>=0)pins.w(reg+4,high?2:0);
 pins.w(reg,config);
 if(high>=0)pins.w(reg+4,high?2:0);
}
bool setupBus(){if(!gcc.open(0x100000,0xa0000)||!pins.open(0xf100000,0xd0000))return false;gcc.w(0x62008,gcc.r(0x62008)|(15u<<18)|(1u<<27));const unsigned gates[]={0x3314cu,0x33140u,0x28004u,0x28008u,0x28610u};for(unsigned o:gates){uint64_t end=nowMs()+100;while((gcc.r(o)&(1u<<31))&&!(gcc.r(o)&2u)&&nowMs()<end)IOSleep(1);if((gcc.r(o)&(1u<<31))&&!(gcc.r(o)&2u))return false;}
 // Pong's i2c@a94000 pinctrl-0 is qupv3_se13_i2c_active: GPIO48/49,
 // qup13 mux1, pull-up / 2mA. GPIO52/53 mux2 selects QSPI, not this bus.
 const unsigned i2cPins[]={48,49};for(unsigned n:i2cPins)pin(n,3u|(1u<<2),-1);
 // Neither Pong TFA node supplies reset-polarity. Nothing's tfa98xx driver
 // therefore defaults to active HIGH: assert for 10ms, release LOW for 10ms.
 // Leaving these high holds both amps in reset, so their revision read NACKs.
 pin(1,1u|(1u<<9),1);pin(89,1u|(1u<<9),1);IOSleep(10);
 pin(1,1u|(1u<<9),0);pin(89,1u|(1u<<9),0);IOSleep(10);
 if(!se.open(0xa94000,0x4000))return false;unsigned proto=(se.r(GENI_FW_REVISION)>>8)&255;if(proto!=GENI_PROTO_I2C){IOLog("PongAudio: QUP1 SE5 firmware protocol=%u; I2C firmware required\n",proto);return false;}
 unsigned level=0xff;for(unsigned i=0;i<8;i++){unsigned cfg=gcc.r(0x28618+0x1c+4*i);if(((cfg>>8)&7)==0&&((cfg>>12)&3)==0&&(cfg&31)<=1){level=i;break;}}
 if(level==0xff)return false;
 se.w(0x28,se.r(0x28)|0x7f);se.w(0xe30,se.r(0xe30)|15);se.w(0x24,0x7f);se.w(0x20,1);se.w(0xe18,0);se.w(0x7c,level);se.w(GENI_CLK_CFG,0x21);se.w(GENI_SCL_COUNTERS,(5u<<20)|(11u<<10)|22);
 GeniIo io={[](void *,uint32_t o){return se.r(o);},[](void *,uint32_t o,uint32_t v){se.w(o,v);},nullptr};unsigned config=0;return geni_i2c_prepare(&bus,&io,0x34,&config)==0;
}
bool setupAmps(){for(unsigned a=0;a<2;a++){uint16_t rev;if(!regRead(a,3,rev)){IOLog("PongAudio: TFA%u I2C read failed reason=%u irq=%x\n",a,bus.error,bus.error_status);return false;}IOLog("PongAudio: TFA address=%x revision=%x\n",0x34+a,rev);if(rev!=0x0b73&&rev!=0x1a73)return false;
 if(!field(a,0x0000,1)||!field(a,0x0030,0))return false;
 // NXP tfa9873 B0 volatile optimal settings, not OTP programming.
 if(rev==0x0b73){if(!regWrite(a,0x0f,0x5a6b))return false;uint16_t key;if(!regRead(a,0xfb,key)||!regWrite(a,0xa0,key^0x5a))return false;
 const unsigned values[][2]={{2,0x0628},{0x61,0x0183},{0x63,0x005a},{0x6f,0x0082},{0x70,0xa3eb},{0x73,0x0187},{0x83,0x071c},{0x85,0x0380},{0xd5,0x004d}};for(auto &v:values)if(!regWrite(a,v[0],v[1]))return false;}
 const PongAmpField *v=a?kPongAmp1:kPongAmp0;unsigned n=a?sizeof(kPongAmp1)/sizeof(*v):sizeof(kPongAmp0)/sizeof(*v);for(unsigned i=0;i<n;i++)if(!field(a,v[i].field,v[i].value)){IOLog("PongAudio: TFA%u profile field=%x not applied\n",a,v[i].field);return false;}
 // Nothing's TFA9873-specific init disables FS synchronization. Apply
 // the 32-bit bus format while TDM is stopped, then enable its interface.
 if(!field(a,0x0480,0)||!field(a,0x2000,0)||!field(a,0x20c3,2)||
    !field(a,0x2144,31)||!field(a,0x2224,31)||!field(a,0x2000,1)||
    !field(a,0x0101,0)||!field(a,0x0000,1)||!field(a,0x0030,0))return false;
 }return true;}
bool amps(bool on){bool ok=true;for(unsigned a=0;a<2;a++)if(on)ok=field(a,0x0000,0)&&field(a,0x0120,1)&&field(a,0x0030,1)&&ok;else ok=field(a,0x0030,0)&&field(a,0x0000,1)&&ok;return ok;}
void clean(void *p,size_t n){uintptr_t a=(uintptr_t)p&~uintptr_t(63),end=(uintptr_t)p+n;for(;a<end;a+=64)__asm__ volatile("dc civac,%0"::"r"(a):"memory");__asm__ volatile("dsb sy":::"memory");}
bool setupDma(){auto chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);auto data=chosen?OSDynamicCast(OSData,chosen->getProperty("pong-audio-dma")):nullptr;uint64_t lease[2]={};if(data&&data->getLength()==16)memcpy(lease,data->getBytesNoCopy(),16);if(chosen)chosen->release();if(!lease[0]||lease[1]!=BufferBytes||(lease[0]&4095)||lease[0]+BufferBytes>0x100000000ull){IOLog("PongAudio: no UEFI-owned low DMA lease\n");return false;}dmaPa=lease[0];if(!dma.open(dmaPa,BufferBytes,kIOMapWriteCombineCache)||!smmu.open(0x15000000,0x100000))return false;
 tables=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,IOOptionBits(kIODirectionInOut)|IOOptionBits(kIOMemoryPhysicallyContiguous)|IOOptionBits(kIOMemoryMapperNone),TableBytes,4096);if(!tables)return false;IOByteCount span=0;tablePa=tables->getPhysicalSegment(0,&span,kIOMemoryMapperNone);if(span<TableBytes)return false;
 struct Lease{bool held;Lease():held(pong_smmu_lease_lock()){}~Lease(){if(held)pong_smmu_lease_unlock();}} leaseGuard;
 if(!leaseGuard.held)return false;
 auto plan=planUfsSmmu(0x1801,dmaPa,BufferBytes,[](unsigned o){return smmu.r(o);},78,120);if(plan.error){IOLog("PongAudio: SID1801 %s\n",plan.error);return false;}
 ArmSmmu4KConfig cfg={32,36,tablePa,BufferBytes/4096};ArmSmmu4KRange range={dmaPa,dmaPa,BufferBytes,3,0,2};ArmSmmu4KResult result={};auto tv=(uint64_t *)tables->getBytesNoCopy();if(ArmSmmu4KBuild(&cfg,&range,1,tv,TableBytes/4096,&result)!=ArmSmmu4KSuccess)return false;
 ArmSmmu4KContextConfig cc={};cc.lower=&cfg;cc.lower_tables=&result;cc.id0=smmu.r(0x20);cc.id2=smmu.r(0x28);cc.asid=0x1801;cc.coherent_walk=1;cc.polled_faults=1;ArmSmmu4KContextImage im={};if(ArmSmmu4KEncodeContext(&cc,&im)!=ArmSmmu4KSuccess)return false;clean(tv,result.table_pages*4096);cbOffset=(plan.pages+plan.bank)*4096;
 smmu.w(0x1000+4*plan.bank,im.cbar);smmu.w(0x1800+4*plan.bank,im.cba2r);smmu.w(cbOffset,0);*(volatile uint64_t *)(smmu.v+cbOffset+0x20)=im.ttbr0;*(volatile uint64_t *)(smmu.v+cbOffset+0x28)=im.ttbr1;smmu.w(cbOffset+0x30,im.tcr);smmu.w(cbOffset+0x10,im.tcr2);smmu.w(cbOffset+0x38,im.mair0);smmu.w(cbOffset+0x3c,0);smmu.w(cbOffset+0x58,smmu.r(cbOffset+0x58));smmu.w(cbOffset+0x610,cc.asid);smmu.w(cbOffset+0x7f0,0);if(!wait(smmu,cbOffset+0x7f4,1,0,20))return false;smmu.w(cbOffset,im.sctlr);smmu.w(0xc00+4*plan.slot,plan.bank);if(!plan.migrate&&!plan.reuse)smmu.w(0x800+4*plan.slot,0x80001801);
 dspAddress=dmaPa|(1ull<<32);memset((void *)dma.v,0,BufferBytes);IOLog("PongAudio: UEFI DMA=%llx SID1801 mapped\n",dmaPa);return true;
}
bool shared(){IOOptionBits flags=IOOptionBits(kIODirectionInOut)|IOOptionBits(kIOMemoryKernelUserShared);control=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,flags,4096,4096);outRing=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,flags,SHENG_AUDIO_RING_FRAMES*4,4096);if(!control||!outRing)return false;ctl=(ShengAudioControl *)control->getBytesNoCopy();out=(int16_t *)outRing->getBytesNoCopy();bzero(ctl,4096);bzero(out,SHENG_AUDIO_RING_FRAMES*4);ctl->magic=SHENG_AUDIO_MAGIC;ctl->version=1;ctl->rate=48000;ctl->out_channels=2;ctl->in_channels=0;ctl->period_frames=480;ctl->ring_frames=SHENG_AUDIO_RING_FRAMES;ctl->lead_frames=Lead;ctl->out_anchor_host=mach_absolute_time();mach_timebase_info_data_t tb;clock_timebase_info(&tb);ctl->ticks_per_frame_q32=((1000000000ull<<32)/48000)*tb.denom/tb.numer;return true;}
bool graph(unsigned opcode){uint8_t b[512];unsigned ids[]={opcode==AR_OP_GRAPH_STOP?AR_SG_PLAY:AR_SG_SPEAKER,opcode==AR_OP_GRAPH_STOP?AR_SG_SPEAKER:AR_SG_PLAY};bool ok=true;for(unsigned id:ids){unsigned n=ar_graph_command(b,sizeof b,Port,++token,opcode,id);ok=command(b,n,opcode)&&ok;}return ok;}
bool openGraphs(){
 uint8_t b[512];ArFormat hw={48000,32,2},f={48000,32,2};
 bool device=false,stream=false,mapped=false;unsigned n;
 n=ar_open_speaker_device(b,sizeof b,Port,++token);
 if(!command(b,n,AR_OP_GRAPH_OPEN))goto fail;
 device=true;
 n=ar_config_speaker_device(b,sizeof b,Port,++token,&hw);
 if(!command(b,n,AR_OP_SET_CFG))goto fail;
 n=ar_open_playback_stream(b,sizeof b,Port,++token);
 if(!command(b,n,AR_OP_GRAPH_OPEN))goto fail;
 stream=true;
 n=ar_config_playback_stream(b,sizeof b,Port,++token,&f);
 if(!command(b,n,AR_OP_SET_CFG))goto fail;
 n=ar_config_playback_shmem(b,sizeof b,Port,++token,&f);
 if(!command(b,n,AR_OP_SET_CFG))goto fail;
 n=ar_map(b,sizeof b,Port,++token,dspAddress,BufferBytes);
 mapped=command(b,n,AR_OP_MEM_MAP);
 if(mapped)return true;
fail:
 // A rejected endpoint configuration must release previously opened graphs.
 // Keep the original failure, including across the cleanup acknowledgements.
 int error=native_audio_stats[1];
 if(stream){n=ar_graph_command(b,sizeof b,Port,++token,AR_OP_GRAPH_CLOSE,AR_SG_PLAY);(void)command(b,n,AR_OP_GRAPH_CLOSE);}
 if(device){n=ar_graph_command(b,sizeof b,Port,++token,AR_OP_GRAPH_CLOSE,AR_SG_SPEAKER);(void)command(b,n,AR_OP_GRAPH_CLOSE);}
 native_audio_stats[1]=error;return false;
}
uint64_t framesNow(){
 const uint64_t now=mach_absolute_time(),anchor=ctl->out_anchor_host;
 if(now<=anchor)return 0;
 uint64_t ns=0;absolutetime_to_nanoseconds(now-anchor,&ns);
 return (ns/1000000000ull)*48000+(ns%1000000000ull)*48000/1000000000ull;
}
bool submit(unsigned slot){
 auto dst=(int32_t *)((uint8_t *)dma.v+slot*PeriodBytes);
 const uint64_t begin=sequence*PeriodFrames;
 for(unsigned i=0;i<PeriodFrames;i++)for(unsigned c=0;c<2;c++){
  auto &sample=out[2*((begin+i)%SHENG_AUDIO_RING_FRAMES)+c];
  dst[2*i+c]=(int32_t)sample*65536;sample=0;
 }
 __asm__ volatile("dsb sy":::"memory");
 uint8_t b[512];const unsigned request=0x10000000u|(++writeToken&0x0fffffffu);
 unsigned n=ar_write(b,sizeof b,Port,request,dspAddress+slot*PeriodBytes,handle,PeriodBytes);
 if(!send(b,n))return false;
 slotToken[slot]=request;sequence++;active[slot]=true;inflight++;return true;
}
void completedWrite(const ArAnswer &a){
 const uint64_t addr=a.address&0xffffffffull;
 if(addr<dmaPa||addr>=dmaPa+Slots*PeriodBytes||(addr-dmaPa)%PeriodBytes)return;
 const unsigned slot=unsigned((addr-dmaPa)/PeriodBytes);
 if(!active[slot]||slotToken[slot]!=a.token)return;
 active[slot]=false;if(inflight)inflight--;
 if(a.status){fail(11,a.status);__atomic_store_n(&wanted,0,__ATOMIC_RELEASE);return;}
 completed++;ctl->out_frames=completed*PeriodFrames;
 if(!running)return;
 // AudioReach returns buffers after copying them into its own queue, not
 // necessarily after DAC playback. Keep its steady lead relative to the
 // same host timeline used by CoreAudio (UniXNU's playback sync policy).
 const int64_t now=int64_t(framesNow());
 const int64_t lead=int64_t(sequence*PeriodFrames)-now;
 if(completed==8)baseLead=lead;
 else if(completed>8 && (lead-baseLead>3*int64_t(PeriodFrames)||
                         baseLead-lead>3*int64_t(PeriodFrames))){
  const int64_t next=now+baseLead+PeriodFrames/2;
  sequence=next>0?uint64_t(next)/PeriodFrames:0;
 }
 if(!submit(slot)){fail(12,-5);__atomic_store_n(&wanted,0,__ATOMIC_RELEASE);}
}
void start(){
 if(!graph(AR_OP_GRAPH_PREPARE)||!graph(AR_OP_GRAPH_START)||!amps(true)){
  amps(false);graph(AR_OP_GRAPH_STOP);ctl->state=0;
  __atomic_store_n(&serving,0,__ATOMIC_RELEASE);fail(8,native_audio_stats[1]);
  __atomic_store_n(&wanted,0,__ATOMIC_RELEASE);return;
 }
 bzero(out,SHENG_AUDIO_RING_FRAMES*4);
 // Do not label PCM playing now with a future anchor: that causes every
 // period to be consumed before HAL writes it. The shared timeline stays
 // stable from device discovery through repeated starts.
 sequence=(framesNow()+PeriodFrames-1)/PeriodFrames;
 completed=0;baseLead=0;inflight=0;memset(active,0,sizeof active);
 ctl->out_frames=0;running=true;
 for(unsigned i=0;i<4;i++)if(!submit(i)){running=false;break;}
 if(running){__atomic_or_fetch(&ctl->state,ShengAudioOutputRunning,__ATOMIC_RELEASE);
  IOLog("PongAudio: stereo PCM output started\n");
 }else{
  amps(false);graph(AR_OP_GRAPH_STOP);ctl->state=0;
  __atomic_store_n(&serving,0,__ATOMIC_RELEASE);
  __atomic_store_n(&wanted,0,__ATOMIC_RELEASE);fail(9,-5);
 }
}
void stop(){
 // A stop acknowledgement may be interleaved with WRITE_DONE. Retire the
 // slots, but never enqueue fresh DMA while stopping the graph.
 running=false;graph(AR_OP_GRAPH_STOP);amps(false);
 memset(active,0,sizeof active);inflight=0;
 __atomic_and_fetch(&ctl->state,~ShengAudioOutputRunning,__ATOMIC_RELEASE);
}
void worker(void *,wait_result_t){uint64_t end=nowMs()+90000;while(!up&&nowMs()<end)IOSleep(100);if(!up||!framework()){fail(1,-60);return;}IOLog("PongAudio: APM framework ready\n");if(!setupBus()){fail(2,-6);return;}if(!setupAmps()){fail(3,-6);return;}
 // Tertiary MI2S pins: sck121, data0 122, ws123, data1 124, function1 / 8mA.
 for(unsigned n=121;n<=124;n++)pin(n,(1u<<2)|(3u<<6),-1);
 if(!setupDma()){fail(4,-12);return;}if(!shared()||!openGraphs()){amps(false);fail(5,native_audio_stats[1]);return;}
 ctl->state=ShengAudioOutputReady;void *h=OSMetaClass::preModLoad(g_kernel_kmod_info.name);if(!h){fail(6,-1);return;}PongAudioServiceStaticInit();if(OSMetaClass::postModLoad(h)){fail(6,-2);return;}__atomic_store_n(&serving,1,__ATOMIC_RELEASE);sheng_audio_service_start();if(native_audio_service[0]!=1){__atomic_store_n(&serving,0,__ATOMIC_RELEASE);ctl->state=0;amps(false);fail(6,-3);return;}IOLog("PongAudio: TFA9873 stereo output service published\n");native_audio_stats[0]=7;
 for(;;){if(!up||(smmu.r(cbOffset+0x58)&0x4020001f)){if(running)stop();ctl->state=0;__atomic_store_n(&serving,0,__ATOMIC_RELEASE);fail(10,-5);return;}unsigned want=__atomic_load_n(&wanted,__ATOMIC_ACQUIRE);if(want&&!running)start();if(!want&&running)stop();ArAnswer a={};if(next(a,100)&&a.opcode==AR_OP_WRITE_DONE)completedWrite(a);}
}
}
extern "C" IOBufferMemoryDescriptor *sheng_audio_memory(uint32_t type){if(!serving)return nullptr;auto d=type==0?control:type==1?outRing:nullptr;if(d)d->retain();return d;}
extern "C" int sheng_audio_request(uint32_t selector){if(!serving)return -6;if(selector>1)return -45;__atomic_store_n(&wanted,selector==0?1u:0u,__ATOMIC_RELEASE);IOLockWakeup(lock,&count,false);return 0;}
extern "C" void sheng_audio_probe(void){unsigned mode=0;PE_parse_boot_argn("pong_audio",&mode,sizeof mode);if(!mode||lock)return;lock=IOLockAlloc();if(!lock)return;const ShengAdspClient client={state,receive};channel=sheng_adsp_channel("adsp_apps",512,20,1,&client,nullptr);thread_t t=nullptr;if(!channel||kernel_thread_start(worker,nullptr,&t)!=KERN_SUCCESS){fail(1,-12);return;}thread_deallocate(t);}
