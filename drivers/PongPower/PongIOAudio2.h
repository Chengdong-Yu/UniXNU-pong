/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Native output-only IOAudio2 adapter. Architecture reference: data-a-live's
 * documented PandoraAudio contract; selectors, control dictionaries and
 * status mapping were checked against Pong's 22H374 CoreAudio. No Apple
 * IOAudio2Family is prelinked in this board profile. No capture endpoint.
 */
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <IOKit/IOMessage.h>
extern "C" bool pong_audio_start_ready(void);

namespace {
// Native HAL input is signed packed S16. Convert explicitly to the Q31
// physical ring; do not rely on an inferred 24-in-32 client alignment.
constexpr unsigned Audio2Frames=16384,Audio2Rate=48000,Audio2FrameBytes=4;
// A modest digital loudness curve: +6dB at the lowest nonzero step,
// tapering to unchanged unity at full scale. Publish the matching dB map;
// retain true zero mute and the factory analog gain/protection settings.
constexpr unsigned Audio2Gain[]={0,8192,13777,18674,23170,27392,31405,35254,38968,42567,46067,49481,52817,56085,59291,62439,65536};
constexpr int64_t Audio2Db[]={-412316860416LL,-77574839190LL,-58181129392LL,-46836536411LL,-38787419595LL,-32544039547LL,-27442826613LL,-23129812533LL,-19393709797LL,-16098233632LL,-13150329750LL,-10483626314LL,-8049116816LL,-5809585173LL,-3736102736LL,-1805736768LL,0LL};
struct Audio2Status {volatile uint64_t frames,host,seed;volatile uint64_t clientInputEnd,clientOutputEnd;};
struct Audio2Transition {
 IOLock *lock;
 explicit Audio2Transition(IOLock *value):lock(value){IOLockLock(lock);}
 ~Audio2Transition(){IOLockUnlock(lock);}
};
void audio2num(OSDictionary *d,const char *key,uint64_t value,unsigned bits=32) {
 auto n=OSNumber::withNumber(value,bits);if(n){d->setObject(key,n);n->release();}
}
OSDictionary *audio2format(bool range) {
 auto d=OSDictionary::withCapacity(11);if(!d)return nullptr;
 if(range){audio2num(d,"min sample rate",uint64_t(Audio2Rate)<<32,64);audio2num(d,"max sample rate",uint64_t(Audio2Rate)<<32,64);}
 else audio2num(d,"sample rate",uint64_t(Audio2Rate)<<32,64);
 audio2num(d,"format ID",'lpcm');audio2num(d,"format flags",12);
 audio2num(d,"bytes per packet",Audio2FrameBytes);audio2num(d,"frames per packet",1);
 audio2num(d,"bytes per frame",Audio2FrameBytes);audio2num(d,"channels per frame",2);audio2num(d,"bits per channel",16);return d;
}
}

class IOAudio2Device : public IOService {
 OSDeclareDefaultStructors(IOAudio2Device)
public:
 IOBufferMemoryDescriptor *status=nullptr,*ring=nullptr;
 IOLock *gate=nullptr,*controlGate=nullptr,*ioGate=nullptr;
 bool running=false,clockRunning=false,starting=false,streamEnabled=true,draining=false;unsigned users=0;
 uint64_t origin=0,seed=0,readAt=0,drainEnd=0;bool readValid=false,pcmReported=false;
 unsigned gain=Audio2Gain[13],volume=13,muted=0;
 bool start(IOService *) override;
 void free() override;
 IOReturn setProperties(OSObject *) override;
 IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
 IOReturn startOutput(bool retained=false);IOReturn resumeOutput();void stopOutput();void clockLoop();
 void controls();IOReturn control(unsigned,unsigned,uint64_t *);
 IOReturn setStreamActive(bool);
 bool fill(int32_t *,unsigned);
};
OSDefineMetaClassAndStructors(IOAudio2Device,IOService)
static IOAudio2Device *pongAudio2;

class IOAudio2DeviceUserClient : public IOUserClient {
 OSDeclareDefaultStructors(IOAudio2DeviceUserClient)
 IOAudio2Device *audio=nullptr;bool active=false;IOLock *requestGate=nullptr;unsigned clientId=0;
public:
 bool start(IOService *p) override {
  auto d=OSDynamicCast(IOAudio2Device,p);if(!d||!IOUserClient::start(p))return false;
  requestGate=IOLockAlloc();if(!requestGate)return false;
  static unsigned nextId=0;clientId=__atomic_add_fetch(&nextId,1,__ATOMIC_RELAXED);
  d->retain();audio=d;return true;
 }
 void stopClient() {
  if(!requestGate)return;
  Audio2Transition transition(requestGate);
  if(__atomic_exchange_n(&active,false,__ATOMIC_ACQ_REL)){audio->stopOutput();IOLog("PongAudio2: client=%u close/stop ownership released\n",clientId);}
 }
 void free() override {if(audio){stopClient();audio->release();audio=nullptr;}if(requestGate)IOLockFree(requestGate);IOUserClient::free();}
 IOReturn clientClose() override {stopClient();terminate();return kIOReturnSuccess;}
 IOReturn setProperties(OSObject *p) override {return audio?audio->setProperties(p):kIOReturnNotReady;}
 IOReturn clientMemoryForType(UInt32 type,IOOptionBits *options,IOMemoryDescriptor **memory) override {
  if(!audio||!options||!memory)return kIOReturnBadArgument;
  auto m=type==0?audio->status:(type==1||type==0x10000001)?audio->ring:nullptr;
  if(!m)return kIOReturnUnsupported;
  // 22H374 HALS_IOA2Engine2::_FirstIOThreadStarted writes the
  // client position/host at status+0x18/+0x20. This is a shared
  // bidirectional ABI, not a read-only zero-timestamp page. The
  // dedicated allocation contains no kernel pointers or adjacent data.
  m->retain();*options=0;*memory=m;return kIOReturnSuccess;
 }
 // IOAudio2 uses IOService interest messages for registry changes. Built-in
 // output has no asynchronous block-control buffer or DoIO command queue.
 IOReturn registerNotificationPort(mach_port_t,UInt32 type,io_user_reference_t) override {
  return type==0?kIOReturnSuccess:kIOReturnUnsupported;
 }
 IOReturn externalMethod(uint32_t s,IOExternalMethodArguments *a,IOExternalMethodDispatch *,OSObject *,void *) override;
 IOExternalTrap *getTargetAndTrapForIndex(IOService **target,UInt32 index) override {
  static IOExternalTrap trap={nullptr,static_cast<IOTrap>(&IOAudio2DeviceUserClient::doIO)};
  if(index||!target)return nullptr;*target=this;return &trap;
 }
 IOReturn doIO(void *,void *,void *,void *,void *,void *) {
  // Native built-in-style output exchanges samples through its mapped ring,
  // with no asynchronous block-control command. Publish CPU stores before
  // the physical worker reads them; never claim a completed DSP buffer here.
  __asm__ volatile("dmb ish":::"memory");return __atomic_load_n(&active,__ATOMIC_ACQUIRE)?kIOReturnSuccess:kIOReturnNotReady;
 }
};
OSDefineMetaClassAndStructors(IOAudio2DeviceUserClient,IOUserClient)

void IOAudio2Device::controls() {
 auto list=OSArray::withCapacity(2);if(!list)return;
 for(unsigned id=1;id<=2;id++) {
  auto d=OSDictionary::withCapacity(10);if(!d)continue;
  audio2num(d,"control ID",id);audio2num(d,"class",id==1?'vlme':'mute');
  audio2num(d,"base class",id==1?'levl':'togl');audio2num(d,"scope",'outp');
  audio2num(d,"element",0);audio2num(d,"value",id==1?volume:muted);
  d->setObject("read only",kOSBooleanFalse);
  if(id==1) {
   auto ranges=OSArray::withCapacity(16);
   if(ranges) {
    for(unsigned i=0;i<16;i++) {
     auto range=OSDictionary::withCapacity(4);if(!range)continue;
     audio2num(range,"start int value",i);audio2num(range,"start db value",uint64_t(Audio2Db[i]),64);
     audio2num(range,"integer steps",1);audio2num(range,"db per step",uint64_t(Audio2Db[i+1]-Audio2Db[i]),64);
     ranges->setObject(range);range->release();
    }
    d->setObject("range map",ranges);audio2num(d,"transfer function",0);ranges->release();
   }
  }
  list->setObject(d);d->release();
 }
 setProperty("controls",list);list->release();
}
bool IOAudio2Device::start(IOService *p) {
 if(!IOService::start(p))return false;
 gate=IOLockAlloc();controlGate=IOLockAlloc();ioGate=IOLockAlloc();
 const auto opts=IOOptionBits(kIODirectionInOut)|IOOptionBits(kIOMemoryKernelUserShared);
 status=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,opts,4096,4096);
 ring=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,opts,Audio2Frames*Audio2FrameBytes,4096);
 auto stream=OSDictionary::withCapacity(6);auto current=audio2format(false);auto available=audio2format(true);
 auto formats=OSArray::withCapacity(1);auto outputs=OSArray::withCapacity(1);auto inputs=OSArray::withCapacity(1);
 bool ok=gate&&controlGate&&ioGate&&status&&ring&&stream&&current&&available&&formats&&outputs&&inputs;
 if(ok){
  bzero(status->getBytesNoCopy(),4096);bzero(ring->getBytesNoCopy(),Audio2Frames*Audio2FrameBytes);
  audio2num(stream,"stream ID",1);audio2num(stream,"starting channel",1);audio2num(stream,"terminal type",'spkr');
  formats->setObject(available);stream->setObject("current format",current);stream->setObject("available formats",formats);outputs->setObject(stream);
  setProperty("device UID","Speaker");setProperty("device name","Speaker");setProperty("device manufacturer","Nothing");
  // The endpoint is physically built into Pong, not a Thunderbolt device.
  // 22H374's transducer lookup needs the native Speaker UID. Keep output
  // only: publishing an unimplemented Hawking microphone is not a route fix.
  setProperty("transport type",uint64_t('bltn'),32);setProperty("sample rate",uint64_t(Audio2Rate)<<32,64);
  setProperty("io buffer frame size",uint64_t(Audio2Frames),32);setProperty("clock domain",uint64_t('pong'),32);
  setProperty("output latency",uint64_t(480),32);setProperty("output safety offset",uint64_t(960),32);
  setProperty("input latency",uint64_t(0),32);setProperty("input safety offset",uint64_t(0),32);
  setProperty("output streams",outputs);setProperty("input streams",inputs);
  setProperty("NativeSandboxGrantProcess","audiomxd,mediaserverd");controls();
 }
 OSSafeReleaseNULL(stream);OSSafeReleaseNULL(current);OSSafeReleaseNULL(available);
 OSSafeReleaseNULL(formats);OSSafeReleaseNULL(outputs);OSSafeReleaseNULL(inputs);return ok;
}
void IOAudio2Device::free() {
 OSSafeReleaseNULL(status);OSSafeReleaseNULL(ring);if(gate)IOLockFree(gate);if(controlGate)IOLockFree(controlGate);if(ioGate)IOLockFree(ioGate);IOService::free();
}
IOReturn IOAudio2Device::setProperties(OSObject *p) {
 // CoreAudio takes hog mode before activating the device. Do not allow a
 // user client to replace format, mappings, streams or arbitrary properties.
 auto d=OSDynamicCast(OSDictionary,p);if(!d)return kIOReturnBadArgument;
 auto owner=OSDynamicCast(OSNumber,d->getObject("exclusive access owner"));
 if(!owner||d->getCount()!=1)return kIOReturnUnsupported;
 setProperty("exclusive access owner",owner);return kIOReturnSuccess;
}
IOReturn IOAudio2Device::newUserClient(task_t t,void *s,UInt32 type,OSDictionary *props,IOUserClient **out) {
 if(type||!out)return kIOReturnBadArgument;
 auto c=OSTypeAlloc(IOAudio2DeviceUserClient);if(!c)return kIOReturnNoMemory;
 if(!c->initWithTask(t,s,type,props)||!c->attach(this)||!c->start(this)){c->detach(this);c->release();return kIOReturnError;}
 *out=c;IOLog("PongAudio2: native CoreAudio client opened\n");return kIOReturnSuccess;
}
static void audio2clock(void *p,wait_result_t) {
 static_cast<IOAudio2Device *>(p)->clockLoop();thread_terminate(current_thread());thread_block(THREAD_CONTINUE_NULL);
}
IOReturn IOAudio2Device::startOutput(bool retained) {
 // Serialize start/stop requests without blocking PCM or volume controls.
 // A rapid stop/start (lock/wake or ringtone preview) must wait for the old
 // clock to retire, rather than leave CoreAudio with a transient Busy error.
 Audio2Transition transition(ioGate);
 IOLockLock(gate);
 if(running){
  // A fresh HAL client can join the engine retained by another client
  // after display sleep. It must activate the shared output stream too.
  if(!streamEnabled){streamEnabled=true;readValid=false;pcmReported=false;}
  if(!retained)users++;IOLockUnlock(gate);return kIOReturnSuccess;
 }
 uint64_t until=0;clock_interval_to_deadline(100,kMillisecondScale,&until);
 while(clockRunning) {
  if(IOLockSleepDeadline(gate,&clockRunning,until,THREAD_UNINT)!=THREAD_AWAKENED && clockRunning) {
   IOLockUnlock(gate);return kIOReturnTimeout;
  }
 }
 if(starting){IOLockUnlock(gate);return kIOReturnBusy;}
 starting=true;IOLockUnlock(gate);
 // Starting the physical DSP graph may take seconds. Neither PCM fill nor
 // a system volume/mute request may wait behind that hardware transition.
 if(sheng_audio_request(0)){IOLockLock(gate);starting=false;IOLockUnlock(gate);return kIOReturnNotReady;}
 auto shared=sheng_audio_memory(0);auto c=shared?static_cast<ShengAudioControl *>(shared->getBytesNoCopy()):nullptr;
 // The hardware worker owns APM and amp transitions. Start the native clock
 // only after that real transition, not immediately on a queued request.
 for(unsigned i=0;c&&i<500&&!pong_audio_start_ready()&&!(c->state&ShengAudioOutputStartFailed)&&(c->state&ShengAudioOutputReady);i++)IOSleep(10);
 bool ready=c&&pong_audio_start_ready();OSSafeReleaseNULL(shared);
 if(!ready){sheng_audio_request(1);IOLockLock(gate);starting=false;IOLockUnlock(gate);return kIOReturnNotReady;}
 IOLockLock(gate);starting=false;
 bzero(ring->getBytesNoCopy(),Audio2Frames*Audio2FrameBytes);origin=mach_absolute_time();seed++;readAt=0;readValid=true;pcmReported=false;streamEnabled=true;
 auto st=static_cast<Audio2Status *>(status->getBytesNoCopy());st->host=origin;st->frames=0;
 // Start returns before CoreAudio's IO thread writes these positions. A
 // previous session's output end must not be interpreted as fresh samples.
 __atomic_store_n(&st->clientInputEnd,0,__ATOMIC_RELEASE);
 __atomic_store_n(&st->clientOutputEnd,0,__ATOMIC_RELEASE);
 __atomic_store_n(&st->seed,seed,__ATOMIC_RELEASE);
 running=clockRunning=true;users=1;retain();thread_t t=nullptr;
 auto r=kernel_thread_start(audio2clock,this,&t);
 if(r!=KERN_SUCCESS){running=clockRunning=false;users=0;release();sheng_audio_request(1);}
 else thread_deallocate(t);
 IOLockUnlock(gate);IOLog("PongAudio2: native output start result=%x\n",r);return r==KERN_SUCCESS?kIOReturnSuccess:kIOReturnNoResources;
}
void IOAudio2Device::stopOutput() {
 Audio2Transition transition(ioGate);
 IOLockLock(gate);
 if(!users || --users){const unsigned remaining=users;IOLockUnlock(gate);IOLog("PongAudio2: native StopIO remaining_users=%u\n",remaining);return;}
 auto st=static_cast<Audio2Status *>(status->getBytesNoCopy());
 drainEnd=__atomic_load_n(&st->clientOutputEnd,__ATOMIC_ACQUIRE);
 draining=running && readValid && drainEnd>readAt;
 const uint64_t pending=draining?(drainEnd-readAt<Audio2Frames?drainEnd-readAt:Audio2Frames):0;
 IOLockUnlock(gate);
 // A short lock sound can stop while its last HAL period is still ahead of
 // the physical reader. The old code stopped that reader immediately; the
 // later DSP drain could only drain silence. Hold ownership of the mapped
 // ring and consume only the frozen committed tail before StopIO returns.
 // No PCM gate is held while the real DSP worker progresses; a new start is
 // ordered by ioGate. At most one 341ms ring plus a bounded scheduling margin.
 if(pending){
  uint64_t deadline=0;clock_interval_to_deadline(400,kMillisecondScale,&deadline);
  while(mach_absolute_time()<deadline){
   IOLockLock(gate);const bool done=!running || readAt>=drainEnd;IOLockUnlock(gate);
   if(done)break;IOSleep(5);
  }
 }
 IOLockLock(gate);const uint64_t left=draining && drainEnd>readAt?drainEnd-readAt:0;
 draining=false;running=false;sheng_audio_request(1);IOLockUnlock(gate);
 IOLog("PongAudio2: native StopIO remaining_users=0 committed_tail=%llu unconsumed=%llu\n",pending,left);
}
IOReturn IOAudio2Device::resumeOutput() {
 // Lock-sound StopIO and a retained client's wake StartIO must use the same
 // transition lock. Checking running outside it can report success just
 // before another client stops the last physical engine. A retained client
 // reactivates its stream without adding another ownership reference.
 return startOutput(true);
}
void IOAudio2Device::clockLoop() {
 for(;;){IOSleep(10);IOLockLock(gate);if(!running){clockRunning=false;IOLockWakeup(gate,&clockRunning,false);IOLockUnlock(gate);break;}
  uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-origin,&ns);
  uint64_t frames=(ns/1000000000*48000+(ns%1000000000)*48000/1000000000)/Audio2Frames*Audio2Frames,ticks=0;
  nanoseconds_to_absolutetime(frames*62500/3,&ticks);
  auto st=static_cast<Audio2Status *>(status->getBytesNoCopy());st->frames=frames;st->host=origin+ticks;
  __atomic_store_n(&st->seed,seed,__ATOMIC_RELEASE);IOLockUnlock(gate);
 }release();
}
IOReturn IOAudio2Device::control(unsigned id,unsigned value,uint64_t *result) {
 if((id==1&&value>16)||(id==2&&value>1)||(id!=1&&id!=2))return kIOReturnBadArgument;
 // Registry publication has its own serialization. Do not hold the PCM
 // gate across allocations, IOKit property updates or client notifications.
 IOLockLock(controlGate);
 IOLockLock(gate);if(id==1){volume=value;gain=Audio2Gain[value];}else muted=value;
 if(result)*result=value;IOLockUnlock(gate);
 controls();IOLockUnlock(controlGate);
 IOLog("PongAudio2: native control id=%u value=%u\n",id,value);
 messageClients(kIOMessageServicePropertyChange);return kIOReturnSuccess;
}
bool IOAudio2Device::fill(int32_t *dst,unsigned frames) {
 if(!gate||frames>Audio2Frames||!IOLockTryLock(gate))return false;
 if(!running){IOLockUnlock(gate);return false;}
 uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-origin,&ns);
 const uint64_t now=(ns/1000000000)*48000+(ns%1000000000)*48000/1000000000;
 auto st=static_cast<Audio2Status *>(status->getBytesNoCopy());
 // 22H374 write_data_to_stream publishes the exclusive committed output
 // sample position at +0x20 after copying. Never replay a previous wrap or
 // read unwritten future samples, and never fall through to another owner.
 const uint64_t end=draining?drainEnd:__atomic_load_n(&st->clientOutputEnd,__ATOMIC_ACQUIRE);
 bzero(dst,frames*2*sizeof(*dst));
 if(!streamEnabled&&!draining){IOLockUnlock(gate);return true;}
 if(!end){IOLockUnlock(gate);return true;}
 // The producer can retain its sample epoch across display sleep while a
 // new native engine clock starts at zero. Consume only committed samples;
 // never reject every future buffer because its epoch differs from ours.
 if(!readValid || end<readAt){
  // HAL normally commits several milliseconds ahead. Picking end-960
  // drops the beginning of every new short sound after a stream resume.
  // Follow the live native timeline when epochs agree; on an actual
  // producer rewind, use its earliest still-resident committed position.
  const uint64_t floor=end>Audio2Frames?end-Audio2Frames:0;
  readAt=end>=now && end-now<=Audio2Frames?(now>floor?now:floor):floor;readValid=true;
  IOLog("PongAudio2: committed producer epoch end=%llu native_now=%llu read=%llu seed=%llu\n",end,now,readAt,seed);
 }
 if(end>Audio2Frames && readAt<end-Audio2Frames)readAt=end-Audio2Frames;
 const unsigned count=readAt<end?unsigned((end-readAt)<frames?(end-readAt):frames):0;
 auto src=static_cast<int16_t *>(ring->getBytesNoCopy());bool audible=false;uint64_t peak=0;
 for(unsigned i=0;i<count;i++)for(unsigned ch=0;ch<2;ch++) {
  const int32_t sample=src[2*((readAt+i)%Audio2Frames)+ch];
  // S16 -> Q31 followed by the declared software gain; multiplication
  // avoids undefined left shifts of negative signed samples.
  dst[2*i+ch]=muted?0:int32_t(int64_t(sample)*gain);
  const int64_t scaled=dst[2*i+ch];const uint64_t magnitude=scaled<0?uint64_t(-scaled):uint64_t(scaled);
  if(magnitude>peak)peak=magnitude;
  audible=audible || dst[2*i+ch]!=0;
 }
 if(audible && !pcmReported){pcmReported=true;IOLog("PongAudio2: committed S16 native PCM converted to Q31 end=%llu read=%llu peak=%llu gain_q16=%u muted=%u\n",end,readAt,peak,gain,muted);}
 readAt+=count;
 IOLockUnlock(gate);return true;
}
IOReturn IOAudio2Device::setStreamActive(bool enabled) {
 IOLockLock(gate);const bool changed=streamEnabled!=enabled;
 streamEnabled=enabled;if(changed)readValid=false;IOLockUnlock(gate);
 if(changed)IOLog("PongAudio2: native output stream active=%u\n",enabled);
 return kIOReturnSuccess;
}
IOReturn IOAudio2DeviceUserClient::externalMethod(uint32_t s,IOExternalMethodArguments *a,IOExternalMethodDispatch *,OSObject *,void *) {
 if(!audio||!a)return kIOReturnBadArgument;
 if(a->structureInputDescriptor||a->structureOutputDescriptor)return kIOReturnUnsupported;
 if(s==0||s==10){if(a->scalarInputCount!=(s==0?0u:1u)||a->structureInputSize||a->structureOutputSize)return kIOReturnBadArgument;
  Audio2Transition transition(requestGate);
  auto r=__atomic_load_n(&active,__ATOMIC_ACQUIRE)?audio->resumeOutput():audio->startOutput();if(!r)__atomic_store_n(&active,true,__ATOMIC_RELEASE);
  IOLog("PongAudio2: client=%u selector=%u StartIO result=%x active=%u\n",clientId,s,r,__atomic_load_n(&active,__ATOMIC_ACQUIRE));
  if(a->scalarOutputCount)a->scalarOutput[0]=0;return r;}
 if(s==1||s==11){if(a->scalarInputCount!=(s==1?0u:1u)||a->structureInputSize||a->structureOutputSize)return kIOReturnBadArgument;
  stopClient();if(a->scalarOutputCount)a->scalarOutput[0]=0;return kIOReturnSuccess;}
 if(s==2){if(a->scalarInputCount!=2||a->structureInputSize||a->structureOutputSize||a->scalarOutputCount!=1)return kIOReturnBadArgument;
  return audio->control(unsigned(a->scalarInput[0]),unsigned(a->scalarInput[1]),a->scalarOutput);}
 if(s==6){if(a->scalarInputCount!=2||a->scalarInput[0]!=1||a->scalarInput[1]>1||a->structureInputSize||a->scalarOutputCount||a->structureOutputSize)return kIOReturnBadArgument;return audio->setStreamActive(a->scalarInput[1]!=0);}
 if(s==5){if(a->scalarInputCount!=1||a->scalarInput[0]!=1||a->structureInputSize!=40||!a->structureInput)return kIOReturnBadArgument;
  const uint64_t rate=0x40e7700000000000ULL;const uint32_t format[]={uint32_t('lpcm'),12,4,1,4,2,16,0};
  return !memcmp(a->structureInput,&rate,8)&&!memcmp(static_cast<const uint8_t *>(a->structureInput)+8,format,32)?kIOReturnSuccess:kIOReturnUnsupported;}
 return kIOReturnUnsupported;
}
static bool pong_audio2_publish(IOService *provider) {
 // A duplicate class is a configuration error; this code is compiled into
 // the existing power archive, with its normal late metaclass registration.
 auto d=OSTypeAlloc(IOAudio2Device);if(!d)return false;
 if(!d->init()||!d->attach(provider)||!d->start(provider)){d->detach(provider);d->release();return false;}
 pongAudio2=d;d->registerService();IOLog("PongAudio2: real TFA9873 native stereo output published, no capture\n");return true;
}
extern "C" bool pong_audio2_fill(int32_t *dst,unsigned frames) {return pongAudio2&&pongAudio2->fill(dst,frames);}
