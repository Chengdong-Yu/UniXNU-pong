/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Pong SM8475 DWC3 firmware-handoff CDC console. Hardware inputs: live Pong
 * DT and upstream gcc-sm8450.c. Reuse a powered eUSB2 PHY/repeater only;
 * never run another SoC's FEMTO/QUSB2 program or borrow its SMMU SID.
 * This is a real USB transport, not an IOUSBDeviceFamily/Mux placeholder.
 * Included once in PongPower.cpp so the existing user build recipe stays intact.
 */
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/clock.h>
#include "QcomRpmh.h"
#include "../PongPower/GeniI2c.h"
#include "../PongPower/PongLowDma.h"
extern "C" bool pong_ufs_ready(void);
extern "C" int pong_battery_usb_online(void);
extern "C" bool pong_usb_mux_publish(void);
extern "C" void pong_usb_mux_session(bool);
extern "C" bool pong_usb_mux_receive(const void *,unsigned);
extern "C" bool pong_usb_mux_can_receive(unsigned);
extern "C" bool pong_usb_mux_bridge_ready(void);
extern "C" void pong_usb_mux_attached(bool);
extern "C" bool pong_usb_mux_serial(char *,unsigned);
extern "C" unsigned pong_usb_mux_transmit(void *,unsigned);
#include "Dwc3Registers.h"
namespace PongUsb {
#include "../NativeUfs/SmmuTakeover.h"
struct Map {
 IOMemoryMap *map=nullptr;volatile uint32_t *p=nullptr;
 bool open(uint64_t pa,size_t bytes,IOOptionBits cache=kIOMapInhibitCache) {
  auto d=IODeviceMemory::withRange(pa,bytes);map=d?d->map(kIOMapAnywhere|cache):nullptr;
  if(d)d->release();p=map?(volatile uint32_t *)map->getVirtualAddress():nullptr;return p;
 }
 uint32_t r(unsigned o){auto v=p[o/4];__asm__ volatile("dmb oshld":::"memory");return v;}
 void w(unsigned o,uint32_t v){p[o/4]=v;__asm__ volatile("dsb oshst":::"memory");}
};
static Map core,gcc,smmu,dma,log,qscratch;
#include "PongUsbPhy.h"
static uint64_t physical;
static unsigned eventOffset,stage,statusDep,controlBytes,controlDep,packet=64;
static bool failed,configured,dtr,txActive,rxActive,logReady;
static bool cableConnected;
static bool muxEnabled,muxRxActive,muxTxActive,muxZlp;
static bool iphoneIdentity;
static unsigned muxTxBytes;
static uint64_t bridgeStarted;
static char commandLine[64];static unsigned commandLength;static bool commandDiscard;
static uint32_t logCursor,txEnd;
static IOService *service;
static constexpr unsigned DmaBytes=PongLowDma::UsbBytes,EpTrb=0x1000,EpData=0x2000,TxTrb=0x3000,RxTrb=0x3020,TxData=0x4000,RxData=0x5000;
static uint8_t device[]={18,1,0,2,2,0,0,64,0xff,0xff,0x28,0x01,0,1,1,2,3,1};
static uint8_t config[]={9,2,67,0,2,1,0,0xc0,50,
 9,4,0,0,1,2,2,1,0,5,0x24,0,0x10,1,5,0x24,1,0,1,4,0x24,2,2,5,0x24,6,0,1,
 7,5,0x82,3,16,0,16,9,4,1,0,2,10,0,0,0,
 7,5,0x81,2,64,0,0,7,5,0x01,2,64,0,0};
// Mux first for native mobile-host discovery, plus a separate CDC IAD.
// Apple protocol VID/PID is used only by this opt-in compatibility function.
// Product/manufacturer continue to identify the actual Pong port.
static uint8_t composite[]={9,2,98,0,3,1,0,0xc0,50,
 9,4,0,0,2,0xff,0xfe,2,0,7,5,0x03,2,64,0,0,7,5,0x83,2,64,0,0,
 8,11,1,2,2,2,1,0,
 9,4,1,0,1,2,2,1,4,5,0x24,0,0x10,1,5,0x24,1,0,2,4,0x24,2,2,5,0x24,6,1,2,
 7,5,0x82,3,16,0,16,9,4,2,0,2,10,0,0,0,
 7,5,0x81,2,64,0,0,7,5,0x01,2,64,0,0};
static constexpr unsigned MuxTxTrb=0x3040,MuxRxTrb=0x3060,MuxTxData=0x6000,MuxRxData=0x7000;
static unsigned configuration(unsigned char *out,bool other=false) {
 unsigned length=muxEnabled?sizeof(composite):sizeof(config);
 memcpy(out,muxEnabled?composite:config,length);if(other)out[1]=7;
 // Walk real endpoint descriptors rather than relying on layout offsets.
 for(unsigned n=9;n+2<=length && out[n]>=2;n+=out[n])
  if(out[n+1]==5 && out[n]>=7 && (out[n+3]&3)==2) {
   unsigned p=other?(packet==512?64:512):packet;out[n+4]=uint8_t(p);out[n+5]=uint8_t(p>>8);
  }
 return length;
}
static void disconnect() {
 configured=dtr=txActive=rxActive=muxRxActive=muxTxActive=muxZlp=false;
 logReady=false;commandLength=0;commandDiscard=false;
 if(muxEnabled)pong_usb_mux_session(false);
}
static void input(const unsigned char *data,unsigned size) {
 for(unsigned n=0;n<size;n++) {
  unsigned c=data[n];
  if(c=='\r'||c=='\n') {
   if(commandLength&&!commandDiscard) {
    commandLine[commandLength]=0;
    unsigned destination=(!strcmp(commandLine,"reboot rec")||!strcmp(commandLine,"reboot recovery"))?1:
                         !strcmp(commandLine,"reboot fastboot")?2:0;
    if(destination)IOLog("PongUSB command: %s %s; syncing filesystems\n",commandLine,PongBootCapture::request(destination)?"accepted":"busy/rejected");
    else IOLog("PongUSB commands: reboot rec | reboot fastboot | help\n");
   }
   commandLength=0;commandDiscard=false;
  } else if(c<32||c>126||commandLength>=sizeof(commandLine)-1)commandDiscard=true;
  else if(!commandDiscard)commandLine[commandLength++]=char(c);
 }
}
static unsigned char *bytes(unsigned offset){return (unsigned char *)dma.p+offset;}
static bool wait(unsigned reg,uint32_t mask,uint32_t value,unsigned ms) {
 for(unsigned i=0;i<ms;i++){if((core.r(reg)&mask)==value)return true;IOSleep(1);}return false;
}
static bool cmd(unsigned ep,unsigned command,uint32_t p0=0,uint32_t p1=0,uint32_t p2=0) {
 core.w(DWC3_DEPCMDPAR0(ep),p0);core.w(DWC3_DEPCMDPAR1(ep),p1);core.w(DWC3_DEPCMDPAR2(ep),p2);
 core.w(DWC3_DEPCMD(ep),command|DWC3_DEPCMD_CMDACT);
 for(unsigned i=0;i<1000;i++) {
  auto v=core.r(DWC3_DEPCMD(ep));
  if(!(v&DWC3_DEPCMD_CMDACT)){if(!DWC3_DEPCMD_STATUS(v))return true;break;}
  IODelay(1);
 }
 IOLog("PongUSB: endpoint %u command %x failed status=%x\n",ep,command,core.r(DWC3_DEPCMD(ep)));failed=true;return false;
}
static bool transfer(unsigned ep,unsigned trb,unsigned buffer,unsigned size,unsigned type) {
 auto p=(volatile uint32_t *)bytes(trb);uint64_t address=physical+buffer;
 p[0]=uint32_t(address);p[1]=uint32_t(address>>32);p[2]=size;
 __asm__ volatile("dsb sy":::"memory");
 p[3]=type|DWC3_TRB_CTRL_LST|DWC3_TRB_CTRL_IOC|DWC3_TRB_CTRL_HWO;
 __asm__ volatile("dsb sy":::"memory");address=physical+trb;
 return cmd(ep,DWC3_DEPCMD_STARTTRANSFER,uint32_t(address>>32),uint32_t(address));
}
static bool endpoint(unsigned ep,unsigned type,unsigned maxPacket) {
 unsigned p0=DWC3_DEPCFG_EP_TYPE(type)|DWC3_DEPCFG_MAX_PACKET_SIZE(maxPacket);
 if(ep&1)p0|=DWC3_DEPCFG_FIFO_NUMBER(ep>>1);
 unsigned p1=DWC3_DEPCFG_EP_NUMBER(ep)|DWC3_DEPCFG_XFER_COMPLETE_EN|DWC3_DEPCFG_XFER_NOT_READY_EN;
 return cmd(ep,DWC3_DEPCMD_SETEPCONFIG,p0,p1) && cmd(ep,DWC3_DEPCMD_SETTRANSFRESOURCE,1);
}
static void armSetup() {
 stage=0;transfer(0,EpTrb,EpData,8,DWC3_TRBCTL_CONTROL_SETUP);
}
static void stallControl() {
 // DWC3 stalls the control pipe through physical EP0 regardless of the
 // request direction. Stalling EP1 can leave the next SETUP unanswered.
 cmd(0,DWC3_DEPCMD_SETSTALL);armSetup();
}
static void statusStage(unsigned ep) {
 stage=3;transfer(ep,EpTrb,EpData,0,controlBytes?DWC3_TRBCTL_CONTROL_STATUS3:DWC3_TRBCTL_CONTROL_STATUS2);
}
static void controlData(unsigned ep,unsigned length) {
 controlDep=ep;stage=1;
 transfer(ep,EpTrb,EpData+0x100,ep?length:64,DWC3_TRBCTL_CONTROL_DATA);
}
static void resetEndpoints() {
 disconnect();
 core.w(DWC3_DALEPENA,0);
 if(!cmd(0,DWC3_DEPCMD_DEPSTARTCFG) || !endpoint(0,0,64) || !endpoint(1,0,64))return;
 core.w(DWC3_DALEPENA,3);armSetup();
}
static unsigned stringDescriptor(unsigned index,unsigned char *out) {
 if(!index){out[0]=4;out[1]=3;out[2]=9;out[3]=4;return 4;}
 char serial[64];const bool native=muxEnabled&&pong_usb_mux_serial(serial,sizeof serial);
 const char *value=index==1?(iphoneIdentity?"Apple Inc.":"UniXNU Pong"):
  index==2?(iphoneIdentity?"iPhone":muxEnabled?"Pong iOS USB + console":"Pong XNU console"):
  index==3?(native?serial:"PONG-SM8475"):index==4?"Pong XNU console":"";
 unsigned length=(unsigned)strlen(value);if(length>63)length=63;
 out[0]=uint8_t(2+2*length);out[1]=3;for(unsigned i=0;i<length;i++){out[2+2*i]=uint8_t(value[i]);out[3+2*i]=0;}return out[0];
}
static void setup() {
 auto in=bytes(EpData);unsigned type=in[0],request=in[1],value=unsigned(in[2])|(unsigned(in[3])<<8);
 unsigned index=unsigned(in[4])|(unsigned(in[5])<<8);
 controlBytes=unsigned(in[6])|(unsigned(in[7])<<8);statusDep=(type&0x80)?0:1;unsigned length=0;
 auto out=bytes(EpData+0x100);
 if((type&0x60)==0 && request==6 && (type&0x80)) {
  switch(value>>8){
  case 1:memcpy(out,device,sizeof(device));length=sizeof(device);break;
  case 2:length=configuration(out);break;
  case 7:length=configuration(out,true);break;
  case 3:length=stringDescriptor(value&255,out);break;
  case 6:{const uint8_t q[]={10,6,0,2,device[4],device[5],device[6],64,1,0};memcpy(out,q,10);length=10;break;}
  default:stallControl();return;
  }
 } else if(type==0 && request==5 && !controlBytes && value<128) {
  core.w(DWC3_DCFG,(core.r(DWC3_DCFG)&~(127u<<3))|(value<<3));statusStage(1);return;
 } else if(type==0x02 && request==1 && !value&&!controlBytes) {
  unsigned ep=((index&15)<<1)|((index>>7)&1);
  if(index>0x8f || (index&0x70) || (ep!=2&&ep!=3&&ep!=5&&!(muxEnabled&&(ep==6||ep==7)))) {
   stallControl();return;
  }
  cmd(ep,DWC3_DEPCMD_CLEARSTALL);statusStage(1);return;
 } else if(type==0 && request==9 && !controlBytes && value<=1) {
  if(configured&&value==1){statusStage(1);return;}
  configured=value!=0;
  if(configured) {
   if(!endpoint(2,2,packet)||!endpoint(3,2,packet)||!endpoint(5,3,16))return;
   core.w(DWC3_DALEPENA,core.r(DWC3_DALEPENA)|0x2c);
   if(muxEnabled) {
    if(!endpoint(6,2,packet)||!endpoint(7,2,packet))return;
    core.w(DWC3_DALEPENA,core.r(DWC3_DALEPENA)|0xc0);pong_usb_mux_session(true);
   }
  } else disconnect();
  statusStage(1);return;
 } else if(type==0x80 && request==8){out[0]=configured;length=1;}
 else if(muxEnabled&&type==0xc0&&request==0x45&&!value&&!index&&controlBytes) {
  // AppleUSBHostiOSDevice requests host/device/default configuration and
  // extended function. All three refer to the one real composite config;
  // no unimplemented PTP, Ethernet, or video-capture mode is advertised.
  const uint8_t preferred[]={1,1,1,0};memcpy(out,preferred,sizeof preferred);length=sizeof preferred;
 }
 else if((type&0x60)==0 && request==0 && (type&0x80)){out[0]=type==0x80?1:0;out[1]=0;length=2;} // actual battery/self-powered board, no remote wake
 else if(type==0xa1 && request==0x21){const uint8_t coding[]={0,0xc2,1,0,0,0,8};memcpy(out,coding,7);length=7;}
 else if(type==0x21 && request==0x20 && controlBytes==7){controlData(0,7);return;}
 else if(type==0x21 && request==0x22 && !controlBytes){dtr=(value&1)!=0;commandLength=0;commandDiscard=false;statusStage(1);return;}
 else if(type==0x21 && request==0x23 && !controlBytes){statusStage(1);return;}
 else if(type==0x01 && request==11 && !controlBytes && !value){statusStage(1);return;}
 else if(type==0x81 && request==10){out[0]=0;length=1;}
 else {
  static unsigned unsupported;
  if(unsupported++<12)IOLog("PongUSB: unsupported control type=%x request=%x value=%x index=%x length=%u\n",type,request,value,index,controlBytes);
  stallControl();return;
 }
 if(length>controlBytes)length=controlBytes;
 if(controlBytes)controlData(1,length);else statusStage(statusDep);
}
static void publish() {
 if(service)return;
 service=new IOService;
 if(!service || !service->init() || !service->attach(IOService::getPlatform()) || !service->start(IOService::getPlatform()))return;
 service->setName("PongDwc3Console");service->setProperty("PongUsbConfigured",true);
 service->setProperty("Transport","DWC3 CDC-ACM; physical USB host configured");service->registerService();
 IOLog("PongUSB: host configured real CDC console and %s, SID0 DMA\n",muxEnabled?"native-service Mux endpoints":"no Mux function");
}
static void epEvent(uint32_t event) {
 unsigned ep=(event>>1)&31,kind=(event>>6)&15,status=(event>>12)&15;
 if(ep>=2) {
  if(kind==DWC3_DEPEVT_XFERCOMPLETE) {
   if(ep==3){txActive=false;logCursor=txEnd;}
   if(ep==2 && rxActive) {
    __asm__ volatile("dsb sy":::"memory");
    unsigned residual=dma.r(RxTrb+8)&0xffffff;
    // SHORT/IOC/LST are successful completion flags, not transfer errors.
    // A command line is normally a short OUT packet.
    if(!(dma.r(RxTrb+12)&DWC3_TRB_CTRL_HWO)&&residual<=packet && !(status&1))input(bytes(RxData),packet-residual);
    rxActive=false;
   }
   if(ep==7) {
    if(!(status&1)&&muxTxBytes && !(muxTxBytes%packet)){muxTxBytes=0;muxZlp=true;}
    else {muxTxActive=false;muxZlp=false;}
   }
   if(ep==6&&muxRxActive) {
    __asm__ volatile("dsb sy":::"memory");unsigned residual=dma.r(MuxRxTrb+8)&0xffffff;
    if(!(status&1)&&residual<=4096&&!(dma.r(MuxRxTrb+12)&DWC3_TRB_CTRL_HWO))
     if(!pong_usb_mux_receive(bytes(MuxRxData),4096-residual))IOLog("PongUSB Mux: retired receive dropped after bridge/session close\n");
    muxRxActive=false;
   }
  }
  return;
 }
 if(kind==DWC3_DEPEVT_XFERNOTREADY && stage==2 && status==DEPEVT_STATUS_CONTROL_STATUS){statusStage(ep);return;}
 if(kind!=DWC3_DEPEVT_XFERCOMPLETE)return;
 if(stage==0){setup();return;}
 if(stage==1){stage=2;return;}
 if(stage==3){if(configured)publish();armSetup();}
}
static void events() {
 unsigned count=core.r(DWC3_GEVNTCOUNT(0))&0xfffc;
 if(count>4096){failed=true;IOLog("PongUSB: event ring overrun\n");return;}
 for(unsigned done=0;done<count;done+=4) {
  auto event=dma.r(eventOffset);eventOffset=(eventOffset+4)&4095;
  if(!(event&1))epEvent(event);
  else switch((event>>8)&15) {
   case 0:disconnect();break;
   case 1:core.w(DWC3_DCFG,core.r(DWC3_DCFG)&~(127u<<3));resetEndpoints();break;
   case 2:packet=DWC3_DSTS_CONNECTSPD(core.r(DWC3_DSTS))==0?512:64;
    config[57]=config[64]=uint8_t(packet);config[58]=config[65]=uint8_t(packet>>8);break;
   default:break;
  }
 }
 if(count)core.w(DWC3_GEVNTCOUNT(0),count);
}
static void console() {
 if(!configured || !dtr)return;
 if(!rxActive)rxActive=transfer(2,RxTrb,RxData,packet,DWC3_TRBCTL_NORMAL);
 if(txActive || !log.p || log.r(0)!=0x43474244)return;
 constexpr uint32_t capacity=0x200000-12;
 uint32_t start=log.r(4),size=log.r(8);
 if(start>=capacity || size>capacity)return;
 if(!logReady){logCursor=(start+(size>65536?size-65536:0))%capacity;logReady=true;}
 uint32_t end=(start+size)%capacity;
 unsigned available=(end+capacity-logCursor)%capacity;
 if(!available)return;unsigned length=available>1024?1024:available;
 if(length>=packet && !(length%packet))--length; // terminate with a real short packet
 auto source=(volatile uint8_t *)log.p+12;auto target=bytes(TxData);
 for(unsigned i=0;i<length;i++)target[i]=source[(logCursor+i)%capacity];
 txEnd=(logCursor+length)%capacity;txActive=transfer(3,TxTrb,TxData,length,DWC3_TRBCTL_NORMAL);
}
static void mux() {
 if(!muxEnabled||!configured)return;
 if(!muxRxActive&&pong_usb_mux_can_receive(4096))muxRxActive=transfer(6,MuxRxTrb,MuxRxData,4096,DWC3_TRBCTL_NORMAL);
 if(muxZlp){muxZlp=false;transfer(7,MuxTxTrb,MuxTxData,0,DWC3_TRBCTL_NORMAL);return;}
 if(muxTxActive)return;
 unsigned n=pong_usb_mux_transmit(bytes(MuxTxData),4096);
 if(n){muxTxBytes=n;muxTxActive=transfer(7,MuxTxTrb,MuxTxData,n,DWC3_TRBCTL_NORMAL);}
}
static bool initialize() {
 // Firmware clocks surviving Fastboot do not prove the eUSB2/repeater/NOC
 // fabric or DMA ownership survived. An MMIO load on a gated interconnect
 // cannot be bounded by a software polling timeout. Require explicit board
 // handoff BEFORE the first controller read or shared-SMMU write.
 bool firmwarePhy=false;
 if(!PongLowDma::usbHandoff(&firmwarePhy) || !PongLowDma::usbAddress(physical)) {
  IOLog("PongUSB: independent UEFI USB DMA/controller handoff absent; no hardware takeover\n");return false;
 }
 // Firmware certifies DMA quiescence independently. Establish the actual
 // Pong board PHY here when the loader did not retain it; no fabricated bit.
 if(!firmwarePhy && !prepareBoardPhy())return false;
 if(firmwarePhy && (!gcc.open(0x100000,0xa0000) || !(gcc.r(0x49004)&0x80000000) ||
    (gcc.r(0x49018)&0x80000001)!=1 || (gcc.r(0x49024)&0x80000001)!=1))return false;
 if(!core.open(0xa600000,0xe000)||!qscratch.open(0xa6f8800,0x400))return false;
 auto id=core.r(DWC3_GSNPSID)&0xffff0000;
 if(id!=0x55330000 && id!=0x55340000 && id!=0x33310000)return false;
 if(((core.r(DWC3_GCTL)>>12)&3)!=2){IOLog("PongUSB: firmware did not hand off device role\n");return false;}
 core.w(DWC3_DCTL,core.r(DWC3_DCTL)&~DWC3_DCTL_RUN_STOP);
 if(!wait(DWC3_DSTS,DWC3_DSTS_DEVCTRLHLT,DWC3_DSTS_DEVCTRLHLT,250))return false;
 core.w(DWC3_DEVTEN,0);core.w(DWC3_DALEPENA,0);
 // DMA RAM needs a Normal non-cacheable mapping for memset/memcpy and
 // unaligned USB payloads. Device-MMIO attributes belong only to registers.
 if(!dma.open(physical,DmaBytes,kIOMapWriteCombineCache) || !smmu.open(0x15000000,0x100000))return false;
 if(!pong_smmu_lease_lock())return false;
 auto plan=planUfsSmmu(0,physical,DmaBytes,[](unsigned o){return smmu.r(o);},78,120);
 const char *error=plan.error?plan.error:configureUfsSmmu(plan,0,[](unsigned o){return smmu.r(o);},[](unsigned o,uint32_t v){smmu.w(o,v);});
 pong_smmu_lease_unlock();if(error){IOLog("PongUSB: SID0 lease failed: %s\n",error);return false;}
 memset(bytes(0),0,DmaBytes);__asm__ volatile("dsb sy":::"memory");
 core.w(DWC3_DCTL,core.r(DWC3_DCTL)|DWC3_DCTL_CSFTRST);
 if(!wait(DWC3_DCTL,DWC3_DCTL_CSFTRST,0,250))return false;
 core.w(DWC3_GUSB2PHYCFG(0),core.r(DWC3_GUSB2PHYCFG(0))&~(DWC3_GUSB2PHYCFG_SUSPHY|DWC3_GUSB2PHYCFG_ENBLSLPM));
 core.w(DWC3_DCFG,(core.r(DWC3_DCFG)&~0x3ffu)|0); // high-speed; address 0
 core.w(DWC3_GEVNTADRLO(0),uint32_t(physical));core.w(DWC3_GEVNTADRHI(0),uint32_t(physical>>32));
 core.w(DWC3_GEVNTSIZ(0),4096u|(1u<<31)); // polling backend owns no GIC IRQ
 core.w(DWC3_GEVNTCOUNT(0),0);eventOffset=0;
 unsigned enableMux=0;PE_parse_boot_argn("pong_usb_mux",&enableMux,sizeof enableMux);
 unsigned enablePhone=0;PE_parse_boot_argn("pong_usb_iphone",&enablePhone,sizeof enablePhone);
 muxEnabled=enableMux==1&&pong_usb_mux_publish();
 iphoneIdentity=muxEnabled&&enablePhone==1;
 if(muxEnabled){device[4]=0;device[8]=0xac;device[9]=5;device[10]=0xa8;device[11]=0x12;}
 bridgeStarted=mach_absolute_time();
 resetEndpoints();if(failed)return false;
 core.w(DWC3_DEVTEN,0xf);log.open(0x85200000,0x200000);
 // Pong uses an external PMIC/Type-C session detector. RUN_STOP without
 // the Qualcomm glue's VBUS override starts the core but never attaches.
 // Wait for a real PMIC cable report in the worker before asserting it.
 core.w(DWC3_GCTL,core.r(DWC3_GCTL)|DWC3_GCTL_DSBLCLKGTNG);
 IOLog("PongUSB: DWC3 prepared id=%x SID0 DMA=%llx; waiting for real PMIC USB session\n",core.r(DWC3_GSNPSID),physical);return true;
}
static void cableSession() {
 const int online=pong_battery_usb_online();if(online<0 || bool(online)==cableConnected)return;
 if(online&&muxEnabled&&!pong_usb_mux_bridge_ready()) {
  // Host usbmux has a finite VERSION deadline. Do not enumerate a Mux
  // function before its real service bridge can consume that request.
  uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-bridgeStarted,&ns);
  if(ns<60000000000ULL)return;
  muxEnabled=iphoneIdentity=false;device[4]=2;device[8]=device[9]=0xff;device[10]=0x28;device[11]=1;
  resetEndpoints();IOLog("PongUSB: native bridge unavailable after 60s; real CDC console retained\n");
 }
 const unsigned hs=(1u<<20)|(1u<<28),ss=1u<<24;
 if(online) {
  // dwc3-qcom.c vbus_override: external session valid from the board's
  // actual PMIC, not a fabricated charger reading or a permanent VBUS bit.
  qscratch.w(0x30,qscratch.r(0x30)|ss);qscratch.w(0x10,qscratch.r(0x10)|hs);
  if((qscratch.r(0x10)&hs)!=hs){failed=true;IOLog("PongUSB: QSCRATCH session vote did not latch\n");return;}
  if(muxEnabled)pong_usb_mux_attached(true);
  core.w(DWC3_DCTL,core.r(DWC3_DCTL)|DWC3_DCTL_RUN_STOP);cableConnected=true;
 } else {
  core.w(DWC3_DCTL,core.r(DWC3_DCTL)&~DWC3_DCTL_RUN_STOP);
  if(!wait(DWC3_DSTS,DWC3_DSTS_DEVCTRLHLT,DWC3_DSTS_DEVCTRLHLT,250)){failed=true;return;}
  qscratch.w(0x10,qscratch.r(0x10)&~hs);qscratch.w(0x30,qscratch.r(0x30)&~ss);
  cableConnected=false;disconnect();
  if(muxEnabled)pong_usb_mux_attached(false);
  // Retain DMA and endpoint resources while halted. The host's next real
  // USB reset reinitializes EP0 before any payload TRB is reused.
 }
 IOLog("PongUSB: real PMIC session=%u QSCRATCH=%x DSTS=%x\n",unsigned(cableConnected),qscratch.r(0x10),core.r(DWC3_DSTS));
}
static void worker(void *,wait_result_t) {
 // Root storage must have finished its exact SID/bank migration before
 // optional USB touches either the shared SMMU or the inherited controller.
 // Sleep only this worker; a failed/missing root never blocks boot here.
 IOLog("PongUSB: waiting for root transport before controller/SID0 takeover\n");
 for(unsigned i=0;i<180&&!pong_ufs_ready();i++)IOSleep(500);
 if(!pong_ufs_ready()) {
  IOLog("PongUSB: root transport unavailable; skip optional USB takeover\n");
  thread_terminate(current_thread());thread_block(THREAD_CONTINUE_NULL);return;
 }
 IOLog("PongUSB: root transport ready; starting independent USB DMA\n");
 if(initialize()) {
  while(!failed){cableSession();if(cableConnected){events();console();mux();}IOSleep(configured?2:10);}
  core.w(DWC3_DCTL,core.r(DWC3_DCTL)&~DWC3_DCTL_RUN_STOP);
  // Retain DMA/lease if retirement is unknown. Never reuse active pages.
  IOLog("PongUSB: controller failed, output stopped; DMA retained\n");
 } else IOLog("PongUSB: initialization unavailable; continue normal boot\n");
 thread_terminate(current_thread());thread_block(THREAD_CONTINUE_NULL);
}
static void start() {
 unsigned enabled=0;if(!PE_parse_boot_argn("pong_usb",&enabled,sizeof(enabled)) || enabled!=1)return;
 static uint32_t started;
 if(__atomic_exchange_n(&started,1,__ATOMIC_ACQ_REL))return;
 thread_t t=nullptr;if(kernel_thread_start(worker,nullptr,&t)==KERN_SUCCESS)thread_deallocate(t);
}
} // namespace PongUsb
