/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Firmware allocations are owned by their named consumers. Keep the old
 * pong-audio-dma 64 KiB contract intact. USB needs an independent reservation
 * and a quiesced-controller handoff; a clock status bit is not ownership.
 */
#ifndef PONG_LOW_DMA_H
#define PONG_LOW_DMA_H
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
namespace PongLowDma {
constexpr unsigned AudioOffset=0,AudioBytes=0x10000,UsbBytes=0x8000;
static inline bool lease(const char *property,unsigned bytes,uint64_t &result) {
 auto chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
 auto data=chosen?OSDynamicCast(OSData,chosen->getProperty(property)):nullptr;
 uint64_t lease[2]={};
 if(data&&data->getLength()==sizeof lease)memcpy(lease,data->getBytesNoCopy(),sizeof lease);
 if(chosen)chosen->release();
 if(!lease[0] || lease[1]!=bytes || (lease[0]&4095) ||
    lease[0]>0x100000000ULL-bytes)return false;
 result=lease[0];return true;
}
static inline bool address(unsigned offset,unsigned bytes,uint64_t &result) {
 return !offset&&bytes==AudioBytes&&lease("pong-audio-dma",AudioBytes,result);
}
static inline bool usbAddress(uint64_t &result) {
 if(!lease("pong-usb-dma",UsbBytes,result))return false;
 uint64_t audio=0;
 if(lease("pong-audio-dma",AudioBytes,audio) &&
    result<audio+AudioBytes && audio<result+UsbBytes)return false;
 return true;
}
static inline bool usbHandoff(bool *phyRetained=nullptr) {
 // Version, core address, SID, flags (PHY/repeater retained + old DMA stopped).
 // Only a board loader that actually performed those operations may supply
 // this record; never synthesize it from static DT or a boot argument.
 auto chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
 auto data=chosen?OSDynamicCast(OSData,chosen->getProperty("pong-usb-handoff")):nullptr;
 uint64_t record[4]={};
 if(data&&data->getLength()==sizeof record)memcpy(record,data->getBytesNoCopy(),sizeof record);
 auto statusData=chosen?OSDynamicCast(OSData,chosen->getProperty("pong-usb-status")):nullptr;
 uint64_t status[2]={};
 if(statusData&&statusData->getLength()==sizeof status)memcpy(status,statusData->getBytesNoCopy(),sizeof status);
 if(chosen)chosen->release();
 if(record[3]!=3)IOLog("PongUSB: firmware ownership stage=%llu status=%llx flags=%llx; PHY retention not certified\n",status[0],status[1],record[3]);
 if(phyRetained)*phyRetained=(record[3]&1)!=0;
 return record[0]==1&&record[1]==0xa600000&&record[2]==0&&!(record[3]&~3ull)&&(record[3]&2);
}
}
#endif
