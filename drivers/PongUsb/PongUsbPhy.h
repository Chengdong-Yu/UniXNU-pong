/* SPDX-License-Identifier: GPL-2.0-only
 * Pong eUSB2 board sequence adapted from NothingOSS SM8475
 * drivers/usb/phy/phy-msm-snps-eusb2.c and repeater-i2c-eusb2.c.
 * Copyright (c) 2021-2022, Qualcomm Innovation Center, Inc.
 * Rail names, QUP5, NXP parameters and addresses are Pong REC DT values.
 * Included inside PongUsb after Map; no dependence on a WLAN consumer.
 */
static Map usbDb,usbPhy,usbRepeaterBus,usbPins;
static GeniI2c repeater;
static uint64_t phyNow() {
 uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000000;
}
static bool phyFail(const char *reason) {IOLog("PongUSB: board PHY unavailable: %s\n",reason);return false;}
static bool phyWait(Map &m,unsigned off,uint32_t mask,uint32_t value,unsigned ms) {
 const uint64_t end=phyNow()+ms;
 do{if((m.r(off)&mask)==value)return true;IOSleep(1);}while(phyNow()<end);return false;
}
static bool phyMask(unsigned off,uint32_t mask,uint32_t value) {
 usbPhy.w(off,(usbPhy.r(off)&~mask)|(value&mask));return (usbPhy.r(off)&mask)==(value&mask);
}
static bool repeaterTransfer(bool read,uint8_t reg,uint8_t value=0,uint8_t *out=nullptr) {
 if(read?geni_i2c_start_read(&repeater,reg):geni_i2c_start_write(&repeater,reg,value))return false;
 for(unsigned phase=0;phase<3;phase++) {
  const uint64_t end=phyNow()+20;
  do{int result=geni_i2c_poll(&repeater);
   if(result==GeniDone){if(out)*out=repeater.data;return true;}
   if(result==GeniFailed)return false;IODelay(20);
  }while(phyNow()<end);
  if(geni_i2c_timeout(&repeater)!=GeniPending)break;
 }
 return false;
}
static bool prepareBoardPhy() {
 if(!gcc.open(0x100000,0xa0000)||!usbDb.open(0x80860000,0x20000))return phyFail("GCC/cmd-db mapping");
 auto *copy=(uint8_t *)IOMallocData(0x20000);if(!copy)return phyFail("cmd-db copy");
 for(unsigned off=0;off<0x20000;off+=4){uint32_t word=usbDb.r(off);memcpy(copy+off,&word,4);}
 // Retain actual USB supplies. Shared votes are never lowered on an optional
 // failure. VRM uses mV, enable +4, mode +8, as on Pong's other consumers.
 struct Rail{const char *name;unsigned mv;};
 const Rail rails[]={{"ldob5",880},{"ldoc10",1200},{"smpb10",1800},{"ldob2",3075}};
 bool ok=true;
 for(const auto &rail:rails) {
  QcomRpmhResource resource={};
  if(qcom_cmd_db_find(copy,0x20000,rail.name,&resource)||
     !pong_rpmh_vote(resource.address+8,7)||!pong_rpmh_vote(resource.address,rail.mv)||
     !pong_rpmh_vote(resource.address+4,1)){ok=false;break;}
 }
 QcomRpmhResource xo={};
 if(ok)ok=!qcom_cmd_db_find(copy,0x20000,"xo.lvl",&xo)&&pong_rpmh_vote(xo.address,3);
 IOFreeData(copy,0x20000);if(!ok)return phyFail("real USB regulator/XO request rejected");
 IOSleep(2);
 // No DWC3 read before its power domain and every required bus/UTMI clock.
 gcc.w(0x49004,gcc.r(0x49004)&~1u);
 if(!phyWait(gcc,0x49004,1u<<31,1u<<31,100))return phyFail("USB30 GDSC did not power on");
 const unsigned usbClocks[]={0x49018u,0x49020u,0x49024u,0x49084u,0x49088u,0x9c00cu};
 for(unsigned off:usbClocks) {
  gcc.w(off,gcc.r(off)|1u);
  if(!phyWait(gcc,off,0x80000001u,1u,100))return phyFail("USB bus/ref clock did not run");
 }
 // QUP0 SE5 is shared on Pong. Reuse its I2C microcode and leave all other
 // engines/pins alone: the Type-C switch is another device on this bus.
 gcc.w(0x62008,gcc.r(0x62008)|(1u<<6)|(1u<<7)|(1u<<15));
 const unsigned qupClocks[]={0x27004u,0x27008u,0x27610u};
 for(unsigned off:qupClocks) {
  // BRANCH_HALT_VOTED: HWCG may keep CLK_OFF set while idle. The branch
  // opens on a bus request, as on the working Pong amp QUP owner. Requiring
  // CLK_OFF=0 rejects a valid 0x80000002 AHB clock (seen in REC).
  bool available=false;
  const uint64_t end=phyNow()+100;
  do{unsigned value=gcc.r(off);if(!(value&(1u<<31))||(value&2u)){available=true;break;}IOSleep(1);}while(phyNow()<end);
  if(!available){IOLog("PongUSB: QUP clock off=%x value=%x vote=%x\n",off,gcc.r(off),gcc.r(0x62008));return phyFail("repeater QUP clock did not run");}
 }
 if((gcc.r(0x62008)&((1u<<6)|(1u<<7)|(1u<<15)))!=((1u<<6)|(1u<<7)|(1u<<15)))return phyFail("QUP5 vote did not latch");
 if(!usbRepeaterBus.open(0x994000,0x4000))return phyFail("QUP5 mapping");
 // Never steal a firmware transaction or replace the I2C microcode.
 if(usbRepeaterBus.r(0x40)&3u)return phyFail("QUP5 transaction still active");
 if(((usbRepeaterBus.r(GENI_FW_REVISION)>>8)&255)!=GENI_PROTO_I2C)return phyFail("QUP5 is not running the I2C firmware");
 // Pong DT/cape pinctrl: QUP5 on GPIO206/207, function1, pull-up/2mA.
 // The USB firmware stop can restore those pads to GPIO; retain all other
 // pins on this shared wrapper and claim EGPIO only when advertised.
 if(!usbPins.open(0xf100000,0xd0000))return phyFail("QUP5 pin mapping");
 const unsigned pins[]={206,207};
 for(unsigned pin:pins) {
  unsigned off=pin*0x1000,old=usbPins.r(off),cfg=(old&~0x3ffu)|3u|(1u<<2);
  if(old&(1u<<11))cfg|=1u<<12;
  usbPins.w(off,cfg);if((usbPins.r(off)&0x3ffu)!=(cfg&0x3ffu))return phyFail("QUP5 pad did not latch");
 }
 // Match the real DFS table's XO/19.2MHz level, never guess a perf index.
 unsigned level=0xff;
 for(unsigned i=0;i<8;i++){unsigned cfg=gcc.r(0x27618+0x1c+4*i);if(((cfg>>8)&7)==0&&((cfg>>12)&3)==0&&(cfg&31)<=1){level=i;break;}}
 if(level==0xff)return phyFail("QUP5 has no 19.2MHz DFS level");
 usbRepeaterBus.w(0x28,usbRepeaterBus.r(0x28)|0x7f);usbRepeaterBus.w(0xe30,usbRepeaterBus.r(0xe30)|15);
 usbRepeaterBus.w(0x24,0x7f);usbRepeaterBus.w(0x20,1);usbRepeaterBus.w(0xe18,0);
 usbRepeaterBus.w(0x7c,level);usbRepeaterBus.w(GENI_CLK_CFG,0x21);
 usbRepeaterBus.w(GENI_SCL_COUNTERS,(5u<<20)|(11u<<10)|22);
 GeniIo io={[](void *,uint32_t o){return usbRepeaterBus.r(o);},
            [](void *,uint32_t o,uint32_t v){usbRepeaterBus.w(o,v);},nullptr};
 unsigned configuration=0;
 if(geni_i2c_prepare(&repeater,&io,0x4f,&configuration)) {
  IOLog("PongUSB: QUP5 inherited protocol/timing=%x\n",configuration);return phyFail("repeater I2C firmware/timing unavailable");
 }
 // An actual NXP response proves its inherited PM8350c GPIO7 reset has
 // released. Do not use guessed SPMI writes to force somebody else's GPIO.
 uint8_t revision=0,id[3]={};
 if(!repeaterTransfer(true,0x13,0,&revision)||!repeaterTransfer(true,0x14,0,&id[0])||
    !repeaterTransfer(true,0x15,0,&id[1])||!repeaterTransfer(true,0x16,0,&id[2])||
    ((!id[0]&&!id[1]&&!id[2])||(id[0]==255&&id[1]==255&&id[2]==255)))
  return phyFail("NXP repeater not responding; reset remains unowned");
 const uint8_t params[][2]={{6,0x40},{7,0x22},{8,0x64}};
 for(const auto &param:params) {
  uint8_t got=0;
  if(!repeaterTransfer(false,param[0],param[1])||!repeaterTransfer(true,param[0],0,&got)||got!=param[1])
   return phyFail("NXP DT parameter rejected");
 }
 // The eUSB2 PHY is 0x088e3000, not the older FEMTO PHY at 0x088e2000.
 // Require released PHY/AHB resets before accessing this separate block.
 if((gcc.r(0x22000)&1u)||(gcc.r(0x7a000)&1u))return phyFail("eUSB2/AHB reset still asserted");
 if(!usbPhy.open(0x88e3000,0x1000))return phyFail("eUSB2 mapping");
 // Nothing's 19.2 MHz XO program. Every write is masked and read back;
 // leave the PHY enabled/retained until the USB owner can retire its DMA.
 const unsigned program[][3]={
  {0x94,2,2},{0x50,2,2},{0x54,9,9},{0x130,4,4},{0x98,0x40,0},{0xb8,1,1},
  {0x54,0x70,0},{0x5c,0xff,0x90},{0x60,0xf,1},{0x60,0xf0,0},
  {0x58,0xfe,2},{0x68,0xfc,0x20},{0x68,3,1},{0x6c,0x1f,0x10},
  {0x70,7,0},{0x6c,0xc0,0x40},{0x64,0x10,0x10},{0x64,0xc,0xc},
  {0x3c,1,1},{0x54,2,2},{0x54,4,0},{0x50,2,0},{0x64,8,0}
 };
 for(const auto &write:program)if(!phyMask(write[0],write[1],write[2]))return phyFail("eUSB2 program did not latch");
 IOSleep(2);
 IOLog("PongUSB: real Pong eUSB2/NXP board power ready revision=%x id=%x:%x:%x\n",revision,id[0],id[1],id[2]);
 return true;
}
