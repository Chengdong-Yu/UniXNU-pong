/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Pong / SM8475 QCA6490 PCIe0 cold start. Board facts come from live DT.
 * Linux ath11k owns MHI/QMI/radio state. Optional failure leaves boot running;
 * endpoint DMA storage and translation tables stay retained until reset. */
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <libkern/c++/OSMetaClass.h>
#include <sys/random.h>
#include "nw_abi.h"
#include "../PongAdreno/ArmSmmu4K/ArmSmmu4KProgram.h"
#include "PongWifiInputs.h"
#include "QcomRpmh.h"
extern "C" {
int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *);
void pong11_lx_irq_raise_all(void);
void pong_wifi_net_start(const uint8_t mac[6]);
bool pong_wifi_net_prepare(void);
void PongWifiNetStaticInit(void);
bool pong_ufs_ready(void);
bool pong_rpmh_vote(uint32_t,uint32_t);
bool pong_smmu_lease_lock(void);
void pong_smmu_lease_unlock(void);
void pong_rpmh_init(void);
extern const uint8_t pong_wifi_firmware[], pong_wifi_firmware_end[];
}
extern kmod_info_t g_kernel_kmod_info;
namespace {
#include "../NativeUfs/SmmuTakeover.h"
constexpr uint64_t Iova=0xa0000000;
constexpr unsigned MsiAddressLo=0x820, MsiAddressHi=0x824, MsiEnable=0x828, MsiMask=0x82c, MsiStatus=0x830;
constexpr unsigned PcieGlobalIntid=32+140, ParfMsi0=1u<<23;
constexpr size_t ArenaBytes=128*1024*1024, TableBytes=512*1024;
struct Window {
 IOMemoryMap *map=nullptr; volatile uint8_t *v=nullptr; size_t n=0;
 bool open(uint64_t pa,size_t bytes) {
  auto m=IODeviceMemory::withRange(pa,bytes); if(!m)return false;
  map=m->map(kIOMapAnywhere|kIOMapInhibitCache);m->release();
  n=bytes;v=map?reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress()):nullptr;return v;
 }
 uint32_t r(size_t o) const {uint32_t x=*reinterpret_cast<volatile uint32_t *>(v+o);__asm__ volatile("dmb oshld":::"memory");return x;}
 void w(size_t o,uint32_t x) { *reinterpret_cast<volatile uint32_t *>(v+o)=x;__asm__ volatile("dsb oshst":::"memory");}
};
Window gcc,tlmm,parf,phy,dbi,atu,cfg,bar,smmu,gic,db,rsc;
Window pmicCore,pmicConfig,pmicObserver;
unsigned pmicChannels;
IOBufferMemoryDescriptor *arena,*tables,*msiPage;
uint8_t *arenaVa; uint64_t tablePa,msiPa; uint64_t cbOffset;
unsigned pcieCap,msiCap; volatile uint32_t stopped,wake; bool dmaOwned,hostMmioReady;
uint64_t initDeadline;
uint64_t now() {uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000000;}
void log(int level,const char *s) {if(level<7)IOLog("PongWifi[%d]: %s\n",level,s);}
bool fail(const char *s) {log(3,s);return false;}
void clean(void *p,size_t n) {uintptr_t a=reinterpret_cast<uintptr_t>(p)&~uintptr_t(63),e=reinterpret_cast<uintptr_t>(p)+n;for(;a<e;a+=64)__asm__ volatile("dc civac,%0"::"r"(a):"memory");__asm__ volatile("dsb sy":::"memory");}
bool wait(Window &w,size_t o,uint32_t mask,uint32_t value,unsigned ms) {uint64_t end=now()+ms;do {if((w.r(o)&mask)==value)return true;IOSleep(1);}while(now()<end);return false;}
void pin(unsigned p,uint32_t conf,int val) {
 const unsigned reg=0x1000*p,old=tlmm.r(reg);
 // Nothing's msm_pinmux_set_mux claims EGPIO-capable pins with bit12.
 // GPIO204 (QCA6490 XO latch) reports PRESENT bit11. Without claiming
 // APPS ownership, OE/output writes do not drive the LPASS/SSC-owned pad.
 conf=(old&~0x3ffu)|(conf&0x3ffu);
 if(old&(1u<<11))conf|=1u<<12;
 if(val>=0)tlmm.w(reg+4,val?2:0);
 tlmm.w(reg,conf);
 if(val>=0)tlmm.w(reg+4,val?2:0);
}
void enableWlan() {
 // Nothing CNSS2 QCA6490 PMU latch, also used for a retained-clock retry.
 if(!(tlmm.r(81*0x1000+4)&1))IOSleep(100);
 pin(204,(tlmm.r(204*0x1000)&~0x3cu)|(1u<<9),1);IODelay(200);
 pin(80,3u|7u<<6|1u<<9,1);IODelay(1000);
 IOSleep(3);pin(204,(tlmm.r(204*0x1000)&~0x3cu)|(1u<<9),0);IOSleep(60);
}
void reset(uint32_t off) {gcc.w(off,gcc.r(off)|1);IODelay(1000);gcc.w(off,gcc.r(off)&~1u);IODelay(1000);}
// Same read-only SPMI v7 observer as Pong's PMIC button path. Resource
// identity/PPID comes from this phone, not Sheng's different PMIC mapping.
bool pmicOpen() {
 if(!pmicCore.open(0xc400000,0x3000)||!pmicConfig.open(0xc42d000,0x4000))return false;
 const unsigned version=pmicCore.r(0);pmicChannels=pmicCore.r(4)&0x7ff;
 if(version<0x70000000||version>=0x80000000||!pmicChannels||pmicChannels>1024)return fail("incompatible Pong SPMI observer");
 return pmicObserver.open(0xc440000,(pmicChannels*0x20+4095)&~size_t(4095));
}
int pmicChannel(unsigned ppid) {
 int found=-1;
 for(unsigned i=0;i<pmicChannels;i++) {
  const unsigned map=pmicCore.r(0x2000+4*i);
  if(!map||((map>>8)&0xfff)!=ppid)continue;
  const unsigned owner=pmicConfig.r(4*i)&7;
  if(found<0||owner==0)found=int(i);
 }
 return found;
}
bool pmicRead(unsigned channel,unsigned reg,uint8_t *out,unsigned bytes) {
 if(!bytes||bytes>8||channel>=pmicChannels)return false;
 const unsigned base=channel*0x20;
 pmicObserver.w(base,(1u<<27)|(reg<<4)|(bytes-1));
 const uint64_t end=now()+2;unsigned status;
 do {status=pmicObserver.r(base+8);}while(!(status&1)&&now()<end);
 if((status&15)!=1)return false;
 const unsigned words[2]={pmicObserver.r(base+0x18),bytes>4?pmicObserver.r(base+0x1c):0};
 for(unsigned i=0;i<bytes;i++)out[i]=(words[i/4]>>(8*(i%4)))&255;
 return true;
}
bool railReady(const PongWifiRail &r) {
 if(!r.ppid)return true; // No guessed peripheral for shared analogue LDOs.
 const int channel=pmicChannel(r.ppid);uint8_t id[2]={};
 if(channel<0||!pmicRead(unsigned(channel),4,id,2)||id[0]!=r.type||id[1]!=r.subtype) {
  IOLog("PongWifi: rail %s PPID=%x unavailable/type mismatch %x/%x\n",r.name,r.ppid,id[0],id[1]);return false;
 }
 uint8_t status=0,enable=0,voltage[2]={};unsigned mv=0;const uint64_t end=now()+100;
 do {
  if(!pmicRead(unsigned(channel),8,&status,1)||!pmicRead(unsigned(channel),0x46,&enable,1)||!pmicRead(unsigned(channel),0x40,voltage,2))return false;
  mv=unsigned(voltage[0])|(unsigned(voltage[1])<<8);
  if((status&0x80)&&(enable&0x80)&&mv>=r.mv) {
   // Another PMIC consumer may legitimately request a higher setpoint.
   IOLog("PongWifi: rail %s physical ready PPID=%x %umV status=%x enable=%x\n",r.name,r.ppid,mv,status,enable);return true;
  }
  IOSleep(1);
 }while(now()<end);
 IOLog("PongWifi: rail %s not ready PPID=%x requested=%umV actual=%umV status=%x enable=%x\n",r.name,r.ppid,r.mv,mv,status,enable);
 return false;
}
/* APSS DRV2: SLEEP0..2, ACTIVE3..4, WAKE5..6; GPU leases 3.
 * A fresh command-status completion and idle state are required. IRQ remains
 * masked because this owner polls and has no RSC interrupt handler. */
IOLock *powerLock,*smmuLeaseLock;
bool rawVote(uint32_t addr,uint32_t value) {
 constexpr unsigned b=0xd00+4*0x2a0,other=0xd00+3*0x2a0;
 unsigned caps=rsc.r(0xc),count=(caps>>12)&63,ncpt=(caps>>27)&31;
 if(((rsc.r(0)>>16)&255)>=3||count<8||ncpt<1||!rsc.r(b+0x18))return fail("active TCS4 unavailable");
 if(!rsc.r(other+0x18)){unsigned mask=rsc.r(other+0x1c);for(unsigned i=0;i<ncpt&&i<16;i++)if((mask&(1u<<i))&&rsc.r(other+0x34+20*i)==addr)return fail("conflicting RPMh transaction");}
 rsc.w(0xd00,rsc.r(0xd00)&~(1u<<4));
 rsc.w(b+0x14,rsc.r(b+0x14)&~((1u<<24)|(1u<<16)));rsc.w(b+0x1c,0);rsc.w(0xd08,1u<<4);
 if(!wait(rsc,0xd04,1u<<4,0,10))return false;
 rsc.w(b+0x30,0x10108);rsc.w(b+0x34,addr);rsc.w(b+0x38,value);
 rsc.w(b+0x10,1);rsc.w(b+0x1c,1);rsc.w(b+0x14,1u<<16);rsc.w(b+0x14,(1u<<16)|(1u<<24));
 bool progressed=false,done=false;uint64_t end=now()+100;
 do {unsigned st=rsc.r(b+0x3c),idle=rsc.r(b+0x18);if((st&0x10100)!=0x10100||!idle)progressed=true;if((st&0x10100)==0x10100&&idle&&(progressed||(rsc.r(0xd04)&(1u<<4)))){done=true;break;}IODelay(10);}while(now()<end);
 if(!done)return fail("RPMh response timeout; transaction retained");
 rsc.w(b+0x1c,0);rsc.w(0xd08,1u<<4);rsc.w(b+0x14,rsc.r(b+0x14)&~(1u<<24));return true;
}
// BCM auxiliary data is {u32 unit,u16 width,u8 vcd,...}, not ARC levels.
// Parse the actual bounded cmd-db entry; never invent a bandwidth unit.
bool bcmVote(const uint8_t *copy,const char *name) {
 auto u16=[](const uint8_t *v){return unsigned(v[0])|(unsigned(v[1])<<8);};
 auto u32=[](const uint8_t *v){return unsigned(v[0])|(unsigned(v[1])<<8)|(unsigned(v[2])<<16)|(unsigned(v[3])<<24);};
 for(unsigned slave=0;slave<8;slave++){auto h=copy+8+slave*16;if(!u16(h))break;size_t heads=144+u16(h+2),data=144+u16(h+4),count=u16(h+6);
  if(heads>0x20000||count>(0x20000-heads)/24)return false;
  for(size_t i=0;i<count;i++){auto e=copy+heads+24*i;if(strncmp(reinterpret_cast<const char *>(e),name,8))continue;
   size_t aux=data+u16(e+22),len=u16(e+20);if(len<8||aux>0x20000||len>0x20000-aux)return false;
   unsigned unit=u32(copy+aux),width=u16(copy+aux+4),addr=u32(e+16);if(!unit||!width||!addr)return false;
   // SM8450 pcie-anoc SN7 / qns_pcie_mem_noc has buswidth16, channels1.
   const uint64_t scaled=(2000000ull*width+15)/16;
   uint64_t x=(scaled*1000+unit-1)/unit;if(!x)x=1;if(x>0x3fff)x=0x3fff;
   return pong_rpmh_vote(addr,(1u<<30)|(1u<<29)|(unsigned(x)<<14)|unsigned(x));
  }
 }return fail("PCIe NoC SN7 cmd-db resource absent");
}
bool supplies() {
 if(!pmicOpen()||!db.open(0x80860000,0x20000))return false;
 uint8_t *copy=static_cast<uint8_t *>(IOMallocData(0x20000));if(!copy)return false;
 for(unsigned i=0;i<0x20000;i+=4){unsigned v=db.r(i);memcpy(copy+i,&v,4);}
 bool ok=true;
 for(const auto &r:kPongWifiRails) {QcomRpmhResource res={};if(qcom_cmd_db_find(copy,0x20000,r.name,&res)){ok=fail("required SM8475 rail missing in cmd-db");break;}
  // These are dedicated WLAN supplies, retained until endpoint reset. Their
  // minimum setpoints and maximum bounds are taken from this phone's DT.
  if(!pong_rpmh_vote(res.address+8,7)||!pong_rpmh_vote(res.address,r.mv)||!pong_rpmh_vote(res.address+4,1)){ok=false;break;}
  if(!railReady(r)){ok=fail("physical WLAN supply did not follow RPMh request");break;}
 }
 // Retain the actual highest allowed ARC parent votes, as the GPU owner
 // does; never lower a shared CX/MX vote inherited from another consumer.
 const char *parents[]={"mx.lvl","cx.lvl"};
 for(const char *name:parents)if(ok){QcomRpmhResource res={};if(qcom_cmd_db_find(copy,0x20000,name,&res)||!res.level_count){ok=false;break;}unsigned index=0;for(unsigned i=1;i<res.level_count;i++)if(res.levels[i]>res.levels[index])index=i;ok=pong_rpmh_vote(res.address,index);}
 // The actual PCIe ref_clk_src is RPMH_CXO_CLK, not just GCC CLKREF_EN.
 // clk-rpmh.c waipio_bi_tcxo votes xo.lvl=3 while this consumer is active.
 if(ok){QcomRpmhResource xo={};ok=!qcom_cmd_db_find(copy,0x20000,"xo.lvl",&xo)&&pong_rpmh_vote(xo.address,3);}
 if(ok)ok=bcmVote(copy,"SN7");
 IOFreeData(copy,0x20000);if(ok)log(6,"SM8475 WLAN rail requests acknowledged");return ok;
}
bool coldLink() {
 if(!gcc.open(0x100000,0xa0000)||!tlmm.open(0xf100000,0xd0000))return false;
 // REC leaves PCIe0 suspended. Only GCC/TLMM are accessible at this point:
 // even an LTSSM read from PARF can cause a NoC fault while GDSC/AHB is off.
 // pci-msm asserts endpoint reset first and accesses PARF only after its
 // regulator/clock/reset sequence has completed.
 log(6,"PCIe0 cold start; power and bus clocks precede host MMIO");
 pin(94,1u|1u<<9,0);pin(95,3u|1u<<2,-1);pin(96,3u,-1);
 pin(80,1u|1u<<9,0);IOSleep(150);
 if(!supplies())return false;
 // Sheng waits after the regulator transaction before releasing WLAN_EN.
 // The response completes the RPMh request, not the board's rail settling.
 IOSleep(20);
 // QCA6490 CNSS2: assert XO_CLK before WLAN_EN and keep the latch
 // asserted 2..5ms afterwards. GPIO204 is from this phone's DT.
 enableWlan();
 // A temporary boot can inherit the previous OS's suspended PIPE mux and
 // vote. Park on the real TCXO parent with the branch disabled before
 // resetting the core. Do not clock an uninitialized PHY from its output.
 gcc.w(0x62008,gcc.r(0x62008)&~(1u<<4));
 gcc.w(0x7b060,(gcc.r(0x7b060)&~3u)|2u);
 // This votable GDSC also has a separate APSS collapse vote. It is at
 // 0x162128 bit0 in Pong's live DT, not Sheng's 0x152020 register.
 gcc.w(kPongPcieCollapseVote,gcc.r(kPongPcieCollapseVote)&~kPongPcieCollapseMask);
 // SM8475 has a single PCIe0 GDSC at 7b004, no SM8550 PHY GDSC.
 gcc.w(0x7b004,gcc.r(0x7b004)&~1u);
 if(!wait(gcc,0x7b004,1u<<31,1u<<31,100))return fail("PCIe0 GDSC off");
 // Follow the complete Sheng QMP clock sequence: keep PIPE on TCXO during
 // reset/table setup, then select the PHY output before PCS reset release.
 // Mixing an early external parent with a stopped PIPE loses that clock
 // throughout core/PHY reset and the PCS/PIPE hand-off.
 gcc.w(0x7b068,1);gcc.w(0x7b064,gcc.r(0x7b064)|1);
 // pci-msm indexes max-clock-frequency-hz after TWO pipe-clock slots:
 // refgen is index 11, 100 MHz, during initial training. qcom,bw-scale
 // selects 19.2 MHz only after a real Gen1/Gen2 link is negotiated.
 // Retain the native initial clock and advertised speed from core reset.
 gcc.w(0x62018,gcc.r(0x62018)|1u); // GPLL0_OUT_MAIN, 600 MHz
 if(!wait(gcc,0,1u<<31,1u<<31,100))return fail("PCIe refgen GPLL0 did not lock");
 gcc.w(0x7b04c,(1u<<8)|11u);gcc.w(0x7b048,gcc.r(0x7b048)|1);
 if(!wait(gcc,0x7b064,1,0,20)||!wait(gcc,0x7b048,1,0,20))return fail("PCIe clock source update timeout");
 gcc.w(0x62008,gcc.r(0x62008)|0x3f); // PIPE is running from TCXO here.
 // Pong's clock-names lists BOTH aggregate NoC branches (IDs 8 and 9),
 // plus DDRSS SF TBU and PHY rate-change. gcc-waipio.c maps aggregate1
 // to vote bit11, not Sheng's SM8550 cnoc clock. Do not omit this path.
 gcc.w(0x62000,gcc.r(0x62000)|(1u<<11)|(1u<<12)|(1u<<19)|(1u<<22));
 gcc.w(0x9c004,gcc.r(0x9c004)|1);
 const unsigned branches[]={0x7b01cu,0x7b020u,0x7b028u,0x7b030u,0x7b034u};
 for(unsigned o:branches)if(!wait(gcc,o,1u<<31,0,100))return fail("PCIe clock remains halted");
 reset(0x7b000);
 // Pong DT names these core_reset and phy_reset. There is no
 // pcie_0_phy_pipe_reset entry: PHY_BCR resets the programmed PCS/SERDES.
 // Complete both resets before loading the vendor PHY sequence.
 reset(0x7c01c);
 if(!parf.open(0x1c00000,0x3000))return false;
 hostMmioReady=true;
 parf.w(0x1b0,parf.r(0x1b0)&~(1u<<8));
 if(!dbi.open(0x60000000,0x1000)||!phy.open(0x1c06000,0x2000)||!atu.open(0x60001000,0x1000))return false;
 // QMP PCS v5: assert SW_RESET, stop SERDES/PCS, then remove the analogue
 // power vote while PIPE uses TCXO and the EP is in reset.
 // The live DT table later powers up at 0x240, releases SW_RESET at 0x200,
 // and starts at 0x244. Give those writes real low-to-high transitions.
 phy.w(0x200,1);phy.w(0x244,0);phy.w(kPongPciePhyPowerDown,0);IODelay(1000);
 // PARF PHY_TEST_PWR_DOWN is bit0. The vendor's SW_PWRDN/REFCLK_DRV_DSBL
 // bits belong to PCS power-down at +0x240, not this PARF register.
 const unsigned inheritedPhyCtrl=parf.r(0x40);
 parf.w(0x1000,4);parf.w(0x40,inheritedPhyCtrl&~1u);parf.w(0x350,0);parf.w(0x358,0x04000000);
 IOLog("PongWifi: cold REFCLK PHY_CTRL=%x->%x mux=%x CLKREF=%x\n",inheritedPhyCtrl,parf.r(0x40),gcc.r(0x7b060),gcc.r(0x9c004));
 parf.w(0,0x365e);parf.w(0x174,parf.r(0x174)|(1u<<4));
 // Nothing pci-msm's active path and Sheng leave CLKREQ under the real
 // pin/controller, clearing a suspend override. A forced CLKREQ input is
 // not a replacement for ref_clk_src / CLKREF_EN votes. Match the native
 // PARF PM setup before PHY startup; endpoint ASPM remains disabled below.
 parf.w(0x2b0,0);parf.w(0x20,parf.r(0x20)&~(1u<<5));
 parf.w(0x1a8,parf.r(0x1a8)|(1u<<31));
 // Entire vendor PHY table from Pong DT, including power/reset/start entries.
 for(const auto &e:kPongWifiPhy){
  if(e.off==0x200&&!(e.value&1))gcc.w(0x7b060,gcc.r(0x7b060)&~3u);
  phy.w(e.off,e.value);if(e.delay)IODelay(e.delay);
 }
 IODelay(1000);
 // The vendor pipe-clock helper only pulses resets with a matching
 // pipe_reset DT name. Pong supplies none, so retain its freshly loaded
 // SERDES/PCS sequence and the running external PIPE.
 if(!wait(phy,kPongPciePhyStatus,kPongPciePhyStatusMask,0,200))return fail("Pong PCIe PHY not ready");
 // Dedicated PCIe0 SID table: BDF 100 maps to SID1c01, root SID1c00.
 for(unsigned i=0;i<256;i++)parf.w(0x2000+4*i,0);
 const unsigned bdfBytes[]={1u,0u};unsigned crc=0;for(unsigned byte:bdfBytes){crc^=byte;for(unsigned bit=0;bit<8;bit++)crc=(crc&0x80)?((crc<<1)^7)&255:(crc<<1)&255;}
 parf.w(0x2000+crc*4,0x01000100);parf.w(0x2c00,parf.r(0x2c00)&~1u);
 // SM8475's DesignWare core is reset above. Firmware's lane/clock setup
 // cannot be inherited after that reset: restore the actual x1 root port.
 dbi.w(0x8bc,dbi.r(0x8bc)|1); // DBI RO write enable, per pci-msm
 // DesignWare PORT_LINK_CONTROL: lane count alone does not enable the
 // data-link layer after a cold reset. FAST_LINK bypasses normal training.
 dbi.w(0x710,(dbi.r(0x710)&~((0x3fu<<16)|(1u<<7)))|(1u<<16)|(1u<<5));
 dbi.w(0x80c,(dbi.r(0x80c)&~0x1f00u)|0x100);
 dbi.w(0xb40,kPongPcieAuxMhz); // qcom,aux-clk-freq, not an OS timebase
 const unsigned maxSpeed=dbi.r(0x7c)&15;
 for(unsigned speed=3;speed<=maxSpeed && speed<=4;speed++) {
  dbi.w(0x890,(dbi.r(0x890)&~((3u<<24)|1u))|((speed-3)<<24));
  dbi.w(0x8a8,dbi.r(0x8a8)&~0x00ffff00u); // vendor default eq-pset vector=0
  dbi.w(0x8ac,dbi.r(0x8ac)&~31u);
 }
 if(maxSpeed>=3)dbi.w(0x154,0x55555555); // pci-msm default core preset
 if(maxSpeed>=4)dbi.w(0x178,0x55555555);
 dbi.w(0x8bc,dbi.r(0x8bc)&~1u);
 // pci-msm applies these before releasing PERST even for the x1 port.
 parf.w(0x2c4,parf.r(0x2c4)|(1u<<8));
 dbi.w(0x714,dbi.r(0x714)|(1u<<5));
 dbi.w(0x78,(dbi.r(0x78)&~0x7000u)|0x2000); // 512-byte MRRS
 dbi.w(0x80,dbi.r(0x80)&~0x13u); // no ASPM or inherited Link Disable
 dbi.w(0x18,0x10100);*reinterpret_cast<volatile uint16_t *>(dbi.v+4)=6;
 // The actual Pong REC pci-msm/CNSS log requests Gen2 and negotiates x1.
 // Keep the hardware maximum capability intact, but use that board target.
 dbi.w(0xa0,(dbi.r(0xa0)&~15u)|kPongPcieTargetSpeed);
 // Qualcomm ELBI XMLH_LINK_UP plus PCIe capability DLL active is the
 // vendor link criterion. PORT_DEBUG1 bit4 alone is not DLL readiness.
 bool linked=false;uint64_t states=0;
 for(unsigned attempt=0;attempt<2&&!linked;attempt++) {
  if(attempt) {
   // DetectQuiet/Active with no receiver needs a fresh PHY power/reset
   // transition as well as an endpoint reset. Replay Pong's pci-msm path:
   // stop PIPE, reset the PHY, load its complete DT table from the external
   // source, then enable PIPE. No BAR/DMA has been exposed at this point.
   parf.w(0x1b0,parf.r(0x1b0)&~(1u<<8));pin(94,1u|1u<<9,0);
   pin(80,1u|1u<<9,0);IOSleep(150);
   gcc.w(0x7b060,(gcc.r(0x7b060)&~3u)|2u);
   gcc.w(0x62008,gcc.r(0x62008)&~(1u<<4));
   reset(0x7c01c);
   phy.w(0x200,1);phy.w(0x244,0);phy.w(kPongPciePhyPowerDown,0);IODelay(1000);
   gcc.w(0x7b060,gcc.r(0x7b060)&~3u);
   for(const auto &e:kPongWifiPhy){phy.w(e.off,e.value);if(e.delay)IODelay(e.delay);}
   IODelay(1000);gcc.w(0x62008,gcc.r(0x62008)|(1u<<4));
   if(!wait(phy,kPongPciePhyStatus,kPongPciePhyStatusMask,0,200))
    return fail("PCIe native PHY restart not ready");
   enableWlan();
   IOLog("PongWifi: retry uses Pong pci-msm full PHY reset/table/PIPE sequence\n");
  }
  IOSleep(kPongPcieEpLatencyMs);
  // PCIe CEM requires at least 100ms stable power/refclk before PERST#.
  IOSleep(100);pin(94,1u|1u<<9,1);IODelay(kPongPciePerstDelayUs);
  parf.w(0x1b0,parf.r(0x1b0)|(1u<<8));
  if(!(parf.r(0x1b0)&(1u<<8)))return fail("PCIe0 LTSSM enable not retained");
  uint64_t linkEnd=now()+1000;
  do {unsigned elbi=dbi.r(0xf28),link=dbi.r(0x80);states|=1ull<<((elbi>>12)&63);
   if((elbi&0x400)&&(link&(1u<<29))){linked=true;break;}IOSleep(2);
  }while(now()<linkEnd);
  if(!linked)IOLog("PongWifi: training attempt=%u target=Gen%u states=%llx LTSSM_CTRL=%x CLKREQ=%x/%x WAKE=%x\n",attempt+1,kPongPcieTargetSpeed,states,parf.r(0x1b0),tlmm.r(95*0x1000),tlmm.r(95*0x1000+4),tlmm.r(96*0x1000+4));
 }
 if(!linked){IOLog("PongWifi: PCIe0 ELBI=%x link=%x PHY=%x LTSSM=%x/%x clock=%x/%x collapse=%x GPIO80=%x/%x PERST=%x/%x XO=%x/%x mux=%x\n",dbi.r(0xf28),dbi.r(0x80),phy.r(0x214),dbi.r(0x728),dbi.r(0x72c),gcc.r(0x7b03c),gcc.r(0x7b044),gcc.r(kPongPcieCollapseVote),tlmm.r(80*0x1000),tlmm.r(80*0x1000+4),tlmm.r(94*0x1000),tlmm.r(94*0x1000+4),tlmm.r(204*0x1000),tlmm.r(204*0x1000+4),gcc.r(0x7b060));pin(94,1u|1u<<9,0);return fail("PCIe0 link training timeout");}
 // Match msm_pcie_scale_link_bandwidth after the native DLL-up check.
 const unsigned negotiated=(dbi.r(0x80)>>16)&15u;
 if(negotiated==1 || negotiated==2) {
  gcc.w(0x7b04c,1);gcc.w(0x7b048,gcc.r(0x7b048)|1);
  if(!wait(gcc,0x7b048,1,0,20))return fail("PCIe negotiated refgen update timeout");
 }
 // Type-1 memory aperture, as in Sheng's endpoint setup, using Pong's
 // actual 0x60200000..0x63ffffff range rather than Sheng's 0x60300000.
 dbi.w(0x20,0x63f06020);
 // DesignWare unrolled outbound region0 CFG0 to endpoint BDF01:00.0.
 atu.w(0,4);atu.w(8,0x60100000);atu.w(12,0);atu.w(16,0x60100fff);atu.w(20,0x01000000);atu.w(24,0);atu.w(4,0x80000000);
 if(!wait(atu,4,0x80000000,0x80000000,20)||!cfg.open(0x60100000,0x1000))return false;
 if(cfg.r(0)!=0x110317cb||cfg.r(0x2c)!=0x010817cb)return fail("QCA6490 endpoint identity mismatch");
 // Disable bus mastering while host isolation and interrupts are established.
 *reinterpret_cast<volatile uint16_t *>(cfg.v+4)=2;
 cfg.w(0x10,0x60200004);cfg.w(0x14,0);
 // Memory outbound region1: full real 60200000..63ffffff host aperture.
 constexpr unsigned b=0x200;
 atu.w(b,0);atu.w(b+8,0x60200000);atu.w(b+12,0);atu.w(b+16,0x63ffffff);atu.w(b+20,0x60200000);atu.w(b+24,0);atu.w(b+4,0x80000000);
 if(!wait(atu,b+4,0x80000000,0x80000000,20))return false;
 uint64_t seen=0;unsigned cap=cfg.r(0x34)&255;
 for(unsigned n=0;cap&&n<48;n++){if(cap<0x40||cap>0xfc||(cap&3)||(seen&(1ull<<(cap/4))))return fail("PCI capability list invalid");seen|=1ull<<(cap/4);unsigned v=cfg.r(cap);if((v&255)==0x10)pcieCap=cap;if((v&255)==5)msiCap=cap;cap=(v>>8)&255;}
 if(cap||!pcieCap||!msiCap||!bar.open(0x60200000,0x200000))return false;
 log(6,"QCA6490 PCIe0 link and BAR established, bus master held off");return true;
}
IOBufferMemoryDescriptor *buffer(size_t n,uint64_t *pa,bool uncached=false) {
 auto d=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,IOOptionBits(kIODirectionInOut)|IOOptionBits(kIOMemoryPhysicallyContiguous)|IOOptionBits(kIOMemoryMapperNone)|IOOptionBits(uncached?kIOMapWriteCombineCache:0),n,4096);
 if(!d)return nullptr;IOByteCount span=0;*pa=d->getPhysicalSegment(0,&span,kIOMemoryMapperNone);
 if(!*pa||(*pa&4095)||span<n||*pa+n>(1ull<<36)){d->release();return nullptr;}
 bzero(d->getBytesNoCopy(),n);return d;
}
bool allocateArena() {
 // QCA6490 needs contiguous DMA addresses, not 128 MiB of adjacent RAM.
 // Wire ordinary pages and let this SID's native4K SMMU provide the IOVA
 // extent. A large physically contiguous allocation fails after UI startup.
 arena=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
  IOOptionBits(kIODirectionInOut)|IOOptionBits(kIOMemoryMapperNone)|IOOptionBits(kIOMapWriteCombineCache),ArenaBytes,4096);
 if(!arena)return fail("WiFi wired DMA arena allocation failed");
 if(arena->prepare()!=kIOReturnSuccess)return fail("WiFi DMA arena wiring failed");
 arenaVa=static_cast<uint8_t *>(arena->getBytesNoCopy());
 if(!arenaVa||(reinterpret_cast<uintptr_t>(arenaVa)&4095)||arena->getLength()!=ArenaBytes)return fail("WiFi DMA arena mapping invalid");
 bzero(arenaVa,ArenaBytes);return true;
}
int smmuRead32(void *,size_t off,uint32_t *out) {
 if(!out||(off&3)||off>smmu.n-4)return -1;
 *out=smmu.r(off);return 0;
}
int smmuRead64(void *,size_t off,uint64_t *out) {
 if(!out||(off&7)||off>smmu.n-8)return -1;
 *out=*reinterpret_cast<volatile uint64_t *>(smmu.v+off);
 __asm__ volatile("dmb oshld":::"memory");return 0;
}
int smmuWrite32(void *,size_t off,uint32_t value) {
 if((off&3)||off>smmu.n-4)return -1;
 smmu.w(off,value);return 0;
}
int smmuWrite64(void *,size_t off,uint64_t value) {
 if((off&7)||off>smmu.n-8)return -1;
 *reinterpret_cast<volatile uint64_t *>(smmu.v+off)=value;
 __asm__ volatile("dsb sy":::"memory");return 0;
}
int smmuBarrier(void *) {__asm__ volatile("dsb sy":::"memory");return 0;}
bool isolation() {
 if(!smmu.open(0x15000000,0x100000))return false;
 // Pong's root interrupt map routes int_global_int to SPI140. Use the
 // DesignWare MSI receiver already implemented in this core; GIC MBIS and
 // an unimplemented ITS command queue are not prerequisites for this path.
 if(!allocateArena())return false;
 tables=buffer(TableBytes,&tablePa);msiPage=buffer(4096,&msiPa);
 if(!tables||!msiPage)return fail("WiFi SMMU table/MSI allocation failed");
 struct Lease{bool held;Lease():held(pong_smmu_lease_lock()){}~Lease(){if(held)pong_smmu_lease_unlock();}} leaseGuard;
 if(!leaseGuard.held)return false;
 auto plan=planUfsSmmu(0x1c01,Iova,ArenaBytes+4096,[](unsigned o){return smmu.r(o);},78,120);
 if(plan.error)return fail(plan.error);
 ArmSmmu4KConfig bc={32,36,tablePa,(ArenaBytes/4096)+1};
 constexpr size_t capacity=ArenaBytes/4096+1;
 auto ranges=IONewData(ArmSmmu4KRange,capacity);
 if(!ranges)return fail("WiFi DMA span metadata allocation failed");
 size_t count=1;ranges[0]={Iova,msiPa,4096,3,0,2};
 for(size_t off=0;off<ArenaBytes;off+=4096) {
  IOByteCount span=0;const uint64_t pa=arena->getPhysicalSegment(off,&span,kIOMemoryMapperNone);
  if(!pa||(pa&4095)||span<4096||pa>=(1ull<<36)||4096>(1ull<<36)-pa) {
   IODeleteData(ranges,ArmSmmu4KRange,capacity);return fail("WiFi DMA page outside SM8475 output range");
  }
  auto &last=ranges[count-1];
  if(count>1&&last.physical+last.bytes==pa)last.bytes+=4096;
  else ranges[count++]={Iova+4096+off,pa,4096,3,0,2};
 }
 ArmSmmu4KResult br={};auto tv=static_cast<uint64_t *>(tables->getBytesNoCopy());
 const auto built=ArmSmmu4KBuild(&bc,ranges,count,tv,TableBytes/4096,&br);
 IODeleteData(ranges,ArmSmmu4KRange,capacity);
 if(built!=ArmSmmu4KSuccess)return fail("WiFi native4K page tables failed");
 IOLog("PongWifi: 128MiB wired DMA mapped from %lu spans, %lu native4K table pages\n",(unsigned long)(count-1),(unsigned long)br.table_pages);

 ArmSmmu4KContextConfig cc={};cc.lower=&bc;cc.lower_tables=&br;cc.id0=smmu.r(0x20);cc.id2=smmu.r(0x28);cc.asid=0x1c01;cc.coherent_walk=1;cc.polled_faults=1;
 clean(tv,br.table_pages*4096);__asm__ volatile("dsb sy":::"memory");
 // Nothing's arm_smmu_write_context_bank requires TCR2/TCR BEFORE TTBR:
 // TCR2.AS controls access to ASID[15:8]. The old adapter wrote SID1c01's
 // ASID while AS was still reset, losing its upper byte on readback.
 // Reuse the native4K programming primitive already used by the GPU, which
 // verifies the disabled image, synchronizes TLBs, then enables the bank.
 ArmSmmu4KIo io={nullptr,smmu.n,smmuRead32,smmuRead64,smmuWrite32,smmuWrite64,smmuBarrier,1};
 ArmSmmu4KProgramResult programmed={};
 const auto status=ArmSmmu4KProgramDisabledContext(&io,plan.bank,&cc,20000,&programmed);
 if(status!=ArmSmmu4KSuccess) {
  IOLog("PongWifi: SMMU program status=%u bank=%u phase=%u TTBR0=%llx TCR=%x TCR2=%x SCTLR=%x FSR=%x\n",
   unsigned(status),plan.bank,programmed.phase,programmed.readback.ttbr0,programmed.readback.tcr,
   programmed.readback.tcr2,programmed.readback.sctlr,programmed.fault_status);
  return fail("WiFi SMMU context programming failed; SID remains detached");
 }
 cbOffset=programmed.context_offset;
 smmu.w(0xc00+4*plan.slot,plan.bank);if(!plan.migrate&&!plan.reuse)smmu.w(0x800+4*plan.slot,0x80001c01);
 if(smmu.r(0xc00+4*plan.slot)!=plan.bank||smmu.r(0x800+4*plan.slot)!=0x80001c01)return fail("WiFi SID routing failed");
 dmaOwned=true;log(6,"exclusive SID1c01 native4K DMA and retained MSI landing page established");return true;
}
struct Block {uint32_t at,bytes;bool used;};Block blocks[16384];unsigned blockCount;
void *dmaAlloc(size_t n,uint64_t *bus) {
 if(!bus||!n||n>ArenaBytes)return nullptr;n=(n+63)&~size_t(63);if(n>=4096)n=(n+4095)&~size_t(4095);
 uint32_t align=64;if(n>=4096){align=4096;while(align<n)align<<=1;}
 for(unsigned i=0;i<blockCount;i++){auto b=blocks[i];if(b.used)continue;uint64_t addr=Iova+4096+b.at;uint32_t pad=uint32_t((-addr)&(align-1));if(n>b.bytes||pad>b.bytes-n)continue;
  unsigned extra=(pad?1:0)+(b.bytes>pad+n?1:0);if(blockCount+extra>16384)return nullptr;
  memmove(blocks+i+extra,blocks+i,(blockCount-i)*sizeof(Block));blockCount+=extra;unsigned at=i;
  if(pad)blocks[at++]={b.at,pad,false};blocks[at]={b.at+pad,uint32_t(n),true};if(b.bytes>pad+n)blocks[at+1]={uint32_t(b.at+pad+n),uint32_t(b.bytes-pad-n),false};
  *bus=Iova+4096+b.at+pad;void *p=arenaVa+b.at+pad;bzero(p,n);return p;
 }return nullptr;
}
void dmaFree(size_t,void *p,uint64_t) {uintptr_t off=reinterpret_cast<uintptr_t>(p)-reinterpret_cast<uintptr_t>(arenaVa);for(unsigned i=0;i<blockCount;i++)if(blocks[i].at==off&&blocks[i].used){blocks[i].used=false;if(i+1<blockCount&&!blocks[i+1].used){blocks[i].bytes+=blocks[i+1].bytes;memmove(blocks+i+1,blocks+i+2,(blockCount-i-2)*sizeof(Block));--blockCount;}if(i&&!blocks[i-1].used){blocks[i-1].bytes+=blocks[i].bytes;memmove(blocks+i,blocks+i+1,(blockCount-i-1)*sizeof(Block));--blockCount;}return;}}
uint64_t dmaMap(void *p,size_t n,int) {uintptr_t off=reinterpret_cast<uintptr_t>(p)-reinterpret_cast<uintptr_t>(arenaVa);return off<ArenaBytes&&n<=ArenaBytes-off?Iova+4096+off:0;}
void dmaSync(uint64_t bus,size_t n,int) {if(bus<Iova+4096||bus-(Iova+4096)>=ArenaBytes||n>ArenaBytes-(bus-(Iova+4096)))return;__asm__ volatile("dsb sy":::"memory");}

void *alloc(size_t n) {if(n>SIZE_MAX-16)return nullptr;auto p=static_cast<uint8_t *>(IOMallocData(n+16));if(!p)return nullptr;*reinterpret_cast<size_t *>(p)=n+16;return p+16;}
void freeHeap(void *p) {if(p){auto b=static_cast<uint8_t *>(p)-16;IOFreeData(b,*reinterpret_cast<size_t *>(b));}}
void halt() {stopped=1;pin(94,1u|1u<<9,0);IOLog("PongWifi: worker stopped; DMA retained until reset\n");thread_terminate(current_thread());thread_block(THREAD_CONTINUE_NULL);__builtin_unreachable();}
void poll() {if(initDeadline&&now()>initDeadline){log(3,"firmware initialization exceeded deadline");halt();}if(dmaOwned){unsigned fs=smmu.r(cbOffset+0x58);if(fs&0xc00001fe){IOLog("PongWifi: owned SMMU fault=%x FAR=%x%08x FSYNR0=%x\n",fs,smmu.r(cbOffset+0x64),smmu.r(cbOffset+0x60),smmu.r(cbOffset+0x68));halt();}}}
void sleep(unsigned ms) {while(ms){poll();unsigned part=ms>100?100:ms;IOSleep(part);ms-=part;}poll();}
int fwGet(const char *path,const uint8_t **data,size_t *size) {
 const char *name=path;for(const char *p=path;*p;p++)if(*p=='/')name=p+1;
 // Linux WCN6855 hw2 firmware uses the vendor v2 image. Select from the
 // actual TCSR HW major (pci.c uses the same register), never PCI revision.
 if(!strcmp(name,"amss.bin")&&((bar.r(0x224)>>8)&15)==2)name="amss20.bin";
 const size_t total=size_t(pong_wifi_firmware_end-pong_wifi_firmware);unsigned count=0;if(total<8||memcmp(pong_wifi_firmware,"PWF2",4))return -1;memcpy(&count,pong_wifi_firmware+4,4);
 if(count>(total-8)/40)return -1;
 for(unsigned i=0;i<count;i++){const uint8_t *e=pong_wifi_firmware+8+i*40;if(strnlen(reinterpret_cast<const char *>(e),32)>=32||strcmp(reinterpret_cast<const char *>(e),name))continue;unsigned off,len;memcpy(&off,e+32,4);memcpy(&len,e+36,4);if(off>total||len>total-off)return -1;*data=pong_wifi_firmware+off;*size=len;IOLog("PongWifi: firmware %s bytes=%u\n",name,len);return 0;}return -1;
}
int cfgRead(int o,int n,uint32_t *v) {unsigned off=unsigned(o);if(off&0x80000000)off=(off&0x7fffffff)+pcieCap;if(!v||(n!=1&&n!=2&&n!=4)||off>4096-unsigned(n)||(off&(unsigned(n)-1)))return -1;*v=n==1?cfg.v[off]:n==2?*reinterpret_cast<volatile uint16_t *>(cfg.v+off):cfg.r(off);return 0;}
int cfgWrite(int o,int n,uint32_t v) {unsigned off=unsigned(o);if(off&0x80000000)off=(off&0x7fffffff)+pcieCap;if((n!=1&&n!=2&&n!=4)||off>4096-unsigned(n)||(off&(unsigned(n)-1)))return -1;if(off<0x40&&!(off==4&&n==2&&dmaOwned&&(v&~7u)==0))return -1;if(n==1)cfg.v[off]=uint8_t(v);else if(n==2)*reinterpret_cast<volatile uint16_t *>(cfg.v+off)=uint16_t(v);else cfg.w(off,v);__asm__ volatile("dsb sy":::"memory");return 0;}
int msiTarget(uint32_t *lo,uint32_t *hi,uint32_t *data) {if(!dmaOwned)return -1;*lo=uint32_t(Iova+0x40);*hi=0;*data=0;return 0;}
void *mapPhys(uint64_t pa,size_t n) {if(pa>=0x60200000&&pa<0x60400000&&n<=0x60400000-pa)return const_cast<uint8_t *>(bar.v)+pa-0x60200000;return nullptr;}
volatile uint32_t msiArrivals;
void signalWorker() {__atomic_store_n(&wake,1,__ATOMIC_RELEASE);thread_wakeup(reinterpret_cast<event_t>(const_cast<uint32_t *>(&wake)));}
void irq(void *) {
 unsigned pending=dbi.r(MsiStatus);
 if(pending){dbi.w(MsiStatus,pending);__atomic_fetch_add(&msiArrivals,1,__ATOMIC_RELAXED);}
 // Only this port's MSI group is enabled in PARF. Drop the aggregator's
 // level before EOI; no unrelated PCIe status is acknowledged here.
 parf.w(0x228,ParfMsi0);
 static unsigned empty;
 empty=pending?0:empty+1;
 if(empty>=64){dbi.w(MsiEnable,0);parf.w(0x22c,parf.r(0x22c)&~ParfMsi0);}
 signalWorker();
}
void worker(void *,wait_result_t) {
 // UFS owns another SID on the same APPSSMMU: serialize initial bank lease.
 unsigned n=0;for(;n<180&&!pong_ufs_ready();n++)IOSleep(500);
 if(n==180){fail("UFS prerequisites absent; WiFi skipped");goto done;}
 IOSleep(2000);
 if(!coldLink()||!isolation())goto done;
 // nw_abi.h requires events before start/step. Early station completion
 // must not be lost while the board is still waiting for its hardware MAC.
 if(!pong_wifi_net_prepare()){fail("network event setup failed");goto done;}
 dbi.w(MsiEnable,0);dbi.w(MsiMask,~0u);dbi.w(MsiStatus,~0u);
 dbi.w(MsiAddressLo,unsigned(Iova+0x40));dbi.w(MsiAddressHi,unsigned((Iova+0x40)>>32));
 parf.w(0x22c,0);parf.w(0x228,ParfMsi0);
 {uint32_t ctl=0;uint64_t route=0;
  if(armcpu_spi_attach(PcieGlobalIntid,false,irq,nullptr,&ctl,&route)){fail("PCIe0 SPI140 MSI receiver attach failed");goto done;}}
 dbi.w(MsiMask,0);dbi.w(MsiEnable,~0u);parf.w(0x22c,ParfMsi0);
 log(6,"DesignWare MSI receiver armed on Pong PCIe0 SPI140");
 blocks[0]={0,uint32_t(ArenaBytes),false};blockCount=1;
 {static const lx_platform platform={now,sleep,poll,log,halt,alloc,freeHeap,dmaAlloc,dmaFree,dmaMap,dmaSync,fwGet};
  static nw_board board;board={const_cast<uint8_t *>(bar.v),0x200000,0x1103,uint16_t(msiCap),800,cfgRead,cfgWrite,msiTarget,mapPhys};
  if(cfgWrite(4,2,6))goto done;
  initDeadline=now()+90000;int r=nw_wifi_start(&platform,&board);if(r){IOLog("PongWifi: ath11k start failed=%d\n",r);goto done;}
 }
 {bool net=false,irqSeen=false;for(;;){if(!irqSeen&&__atomic_load_n(&msiArrivals,__ATOMIC_RELAXED)){irqSeen=true;log(6,"QCA6490 MSI delivered by GIC");}poll();int busy=nw_wifi_step();nw_wifi_status st={};nw_wifi_status_get(&st);
   if(!net&&st.channels){bool valid=false;for(unsigned i=0;i<6;i++)valid|=st.mac[i]!=0;if(valid&&!(st.mac[0]&1)){initDeadline=0;net=true;
     auto chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);if(chosen){chosen->setProperty("mac-address-wifi0",st.mac,6);chosen->release();}
     void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);if(!handle)goto done;
     PongWifiNetStaticInit();if(OSMetaClass::postModLoad(handle)!=kIOReturnSuccess)goto done;
     pong_wifi_net_start(st.mac);log(6,"QCA6490 station ready; starting actual scan");nw_wifi_scan(nullptr,0);
   }}
   IOSleep(busy?1:5);
 }}
 done: stopped=1;
 // A mapped address does not prove a powered device. Failed GDSC/clock
 // prerequisites must not be followed by cleanup MMIO into a dead domain.
 if(hostMmioReady){if(dbi.v)dbi.w(MsiEnable,0);if(parf.v)parf.w(0x22c,0);}
 if(tlmm.v)pin(94,1u|1u<<9,0);
 log(3,"WiFi initialization stopped; normal iOS boot continues, DMA retained");thread_terminate(current_thread());thread_block(THREAD_CONTINUE_NULL);
}
}
extern "C" void PongWifiRandom(void *p,size_t n) {while(n){unsigned part=n>4096?4096:unsigned(n);read_random(p,part);p=static_cast<uint8_t *>(p)+part;n-=part;}}
extern "C" void pong_wifi_wake(void) {signalWorker();}
extern "C" void native_wifi_logf(int level,const char *fmt,...) {char line[256];va_list a;va_start(a,fmt);vsnprintf(line,sizeof line,fmt,a);va_end(a);log(level,line);}
extern "C" void pong_wifi_start_optional(void) {
 pong_rpmh_init();
 uint32_t enabled=0;if(!PE_parse_boot_argn("pong_wifi",&enabled,sizeof enabled)||enabled!=1)return;
 static uint32_t started;if(__atomic_exchange_n(&started,1,__ATOMIC_ACQ_REL))return;
 // These network classes are linked into the native kernel, not loaded as a
 // separate kext. Register their metaclasses before publishing any service.
 thread_t t=nullptr;if(kernel_thread_start(worker,nullptr,&t)==KERN_SUCCESS)thread_deallocate(t);else log(3,"WiFi worker allocation failed");
}

// All APSS software clients sharing active TCS4 use this sleeping lock.
// GPU has a separate active TCS3 lease and its retained rail minima.
extern "C" void pong_rpmh_init(void) {
 if(!powerLock)powerLock=IOLockAlloc();
 if(!smmuLeaseLock)smmuLeaseLock=IOLockAlloc();
}
extern "C" bool pong_rpmh_vote(uint32_t address,uint32_t value) {
 if(!powerLock)return false;
 IOLockLock(powerLock);
 bool ok=(rsc.v||rsc.open(0x17a20000,0x10000))&&rawVote(address,value);
 IOLockUnlock(powerLock);return ok;
}

extern "C" bool pong_smmu_lease_lock(void){if(!smmuLeaseLock)return false;IOLockLock(smmuLeaseLock);return true;}
extern "C" void pong_smmu_lease_unlock(void){if(smmuLeaseLock)IOLockUnlock(smmuLeaseLock);}
