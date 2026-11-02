/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Real DWC3 bulk-byte transport for the root iOS service bridge. No Apple
 * IOUSBDeviceController impersonation, user DMA mapping, or fake pairing.
 * Included in the existing late-initialized service translation unit.
 */
#pragma once
#include <IOKit/IOUserClient.h>
#include <sys/kpi_socket.h>
#include <sys/socket.h>
#include <sys/errno.h>
#include <netinet/in.h>
namespace PongMux {
constexpr unsigned Capacity=131072,Chunk=4096;
struct Queue {
 unsigned read=0,size=0;uint8_t data[Capacity];
 void clear(){read=size=0;}
 unsigned take(void *out,unsigned n){if(n>size)n=size;for(unsigned i=0;i<n;i++)((uint8_t *)out)[i]=data[(read+i)%Capacity];read=(read+n)%Capacity;size-=n;return n;}
 bool put(const void *in,unsigned n){if(n>Capacity-size)return false;for(unsigned i=0;i<n;i++)data[(read+size+i)%Capacity]=((const uint8_t *)in)[i];size+=n;return true;}
};
static Queue fromHost,toHost;
static IOLock *lock;
static bool connected,opened,identityReady,attached;
static char nativeSerial[64];
static uint64_t generation=1;
class PongUsbMuxTransport : public IOService {OSDeclareDefaultStructors(PongUsbMuxTransport)};
OSDefineMetaClassAndStructors(PongUsbMuxTransport,IOService)
static PongUsbMuxTransport *service;
class PongUsbMuxUserClient : public IOUserClient {
 OSDeclareDefaultStructors(PongUsbMuxUserClient)
 PongUsbMuxTransport *transport=nullptr;
 IOLock *netLock=nullptr;
 socket_t sockets[16]={};
 void closeSockets() {
  if(!netLock)return;
  IOLockLock(netLock);
  for(auto &s:sockets)if(s){sock_close(s);s=nullptr;}
  IOLockUnlock(netLock);
 }
 IOReturn socketMethod(uint32_t selector,IOExternalMethodArguments *a) {
  if(selector<4||selector>8||a->scalarInputCount!=(selector==4?3u:2u)||
     a->scalarInput[1]>=16||a->structureInputSize>Chunk||a->structureOutputSize>Chunk)
   return kIOReturnBadArgument;
  if((selector==4||selector==5||selector==8)&&(a->structureInputSize||a->structureOutputSize))return kIOReturnBadArgument;
  if((selector==4||selector==5||selector==7)&&a->scalarOutputCount!=1)return kIOReturnBadArgument;
  if(selector==6&&(a->scalarOutputCount!=2||!a->structureInputSize||a->structureOutputSize))return kIOReturnBadArgument;
  if(selector==7&&(a->structureInputSize||!a->structureOutputSize))return kIOReturnBadArgument;
  if(selector==8&&a->scalarOutputCount)return kIOReturnBadArgument;
  IOLockLock(lock);bool active=connected&&opened&&a->scalarInput[0]==generation;IOLockUnlock(lock);
  if(!active&&selector!=8)return kIOReturnNotReady;
  IOLockLock(netLock);auto &s=sockets[a->scalarInput[1]];int error=0;
  if(selector==8){if(s)sock_close(s);s=nullptr;}
  else if(selector==4) {
   if(s){sock_close(s);s=nullptr;}
   const unsigned port=unsigned(a->scalarInput[2]);
   if(!port||a->scalarInput[2]>65535)error=EINVAL;
   else error=sock_socket(PF_INET,SOCK_STREAM,IPPROTO_TCP,nullptr,nullptr,&s);
   if(!error) {
    // kpi_socket creates an actual in-kernel socket (peer PID0), as
    // required by Darwin24 lockdownd TCP_PEERPID. No credentials or native
    // service checks are patched; every connection stays on loopback.
    sockaddr_in address={};address.sin_len=sizeof address;address.sin_family=AF_INET;
    address.sin_port=uint16_t((port>>8)|(port<<8));address.sin_addr.s_addr=0x0100007f;
    int one=1;error=sock_setsockopt(s,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof one);
    if(!error)error=sock_connect(s,(sockaddr *)&address,MSG_DONTWAIT);
   }
   if(error&&error!=EINPROGRESS&&s){sock_close(s);s=nullptr;}
   a->scalarOutput[0]=unsigned(error);
  } else if(!s){a->scalarOutput[0]=ENOTCONN;if(selector==6)a->scalarOutput[1]=0;if(selector==7)a->structureOutputSize=0;}
  else if(selector==5) {
   int size=sizeof error;int r=sock_getsockopt(s,SOL_SOCKET,SO_ERROR,&error,&size);if(r)error=r;
   if(!error){sockaddr_in peer={};error=sock_getpeername(s,(sockaddr *)&peer,sizeof peer);if(error==ENOTCONN)error=EINPROGRESS;}
   a->scalarOutput[0]=unsigned(error);
  } else {
   iovec iov={};msghdr message={};size_t done=0;
   iov.iov_base=selector==6?const_cast<void *>(a->structureInput):a->structureOutput;
   iov.iov_len=selector==6?a->structureInputSize:a->structureOutputSize;
   message.msg_iov=&iov;message.msg_iovlen=1;
   error=selector==6?sock_send(s,&message,MSG_DONTWAIT,&done):sock_receive(s,&message,MSG_DONTWAIT,&done);
   a->scalarOutput[0]=unsigned(error);
   if(selector==6)a->scalarOutput[1]=done;else a->structureOutputSize=done;
  }
  IOLockUnlock(netLock);return kIOReturnSuccess;
 }
public:
 bool initWithTask(task_t task,void *security,UInt32 type,OSDictionary *properties) override {
  return type==0 && clientHasPrivilege(security,kIOClientPrivilegeAdministrator)==kIOReturnSuccess &&
   IOUserClient::initWithTask(task,security,type,properties);
 }
 bool start(IOService *provider) override {
  transport=OSDynamicCast(PongUsbMuxTransport,provider);
  if(!transport||!IOUserClient::start(provider)||!provider->open(this))return false;
  IOLockLock(lock);
  if(opened){IOLockUnlock(lock);provider->close(this);return false;}
  netLock=IOLockAlloc();if(!netLock){IOLockUnlock(lock);provider->close(this);return false;}
  opened=true;identityReady=false;IOLockUnlock(lock);return true;
 }
 IOReturn clientClose() override {
  IOLockLock(lock);opened=identityReady=false;generation++;fromHost.clear();toHost.clear();IOLockUnlock(lock);
  closeSockets();if(transport)transport->close(this);terminate();return kIOReturnSuccess;
 }
 void free() override {closeSockets();if(netLock){IOLockFree(netLock);netLock=nullptr;}IOUserClient::free();}
 IOReturn externalMethod(uint32_t selector,IOExternalMethodArguments *a,IOExternalMethodDispatch *,OSObject *,void *) override {
  if(!a || a->structureInputDescriptor || a->structureOutputDescriptor)return kIOReturnBadArgument;
  if(selector>=4)return socketMethod(selector,a);
  IOLockLock(lock);IOReturn r=kIOReturnBadArgument;
  if(selector==0 && !a->scalarInputCount&&!a->structureInputSize&&!a->structureOutputSize && a->scalarOutputCount==3) {
   a->scalarOutput[0]=generation;a->scalarOutput[1]=connected;a->scalarOutput[2]=fromHost.size;r=kIOReturnSuccess;
  } else if(selector==3&&!a->scalarInputCount&&!a->scalarOutputCount&&!a->structureOutputSize&&
            a->structureInputSize>=1&&a->structureInputSize<=sizeof nativeSerial) {
   const auto input=static_cast<const char *>(a->structureInput);
   unsigned length=unsigned(a->structureInputSize)-1;bool valid=input[length]==0;
   if(length&&length<16)valid=false;
   for(unsigned i=0;i<length;i++) {
    const char c=input[i];if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')||c=='-'))valid=false;
   }
   if(valid) {
    if(length&&attached&&strcmp(nativeSerial,input))r=kIOReturnBusy;
    else {if(length)strlcpy(nativeSerial,input,sizeof nativeSerial);identityReady=true;r=kIOReturnSuccess;}
   }
  } else if((selector==1||selector==2)&&a->scalarInputCount==1&&!a->scalarOutputCount) {
   if(a->scalarInput[0]!=generation||!connected)r=kIOReturnNotReady;
   else if(selector==1&&!a->structureInputSize&&a->structureOutputSize&&a->structureOutputSize<=Chunk) {
    a->structureOutputSize=fromHost.take(a->structureOutput,unsigned(a->structureOutputSize));r=kIOReturnSuccess;
   } else if(selector==2&&!a->structureOutputSize&&a->structureInputSize&&a->structureInputSize<=Chunk)
    {
    unsigned length=unsigned(a->structureInputSize);
    if(length+4>Capacity-toHost.size)r=kIOReturnNoSpace;
    else {toHost.put(&length,4);toHost.put(a->structureInput,length);r=kIOReturnSuccess;}
   }
  }
  IOLockUnlock(lock);return r;
 }
};
OSDefineMetaClassAndStructors(PongUsbMuxUserClient,IOUserClient)
}
extern "C" bool pong_usb_mux_publish(void) {
 using namespace PongMux;
 if(service)return true;
 lock=IOLockAlloc();if(!lock)return false;
 service=OSTypeAlloc(PongUsbMuxTransport);
 if(!service||!service->init()||!service->attach(IOService::getPlatform())||!service->start(IOService::getPlatform()))return false;
 service->setName("PongUsbMuxTransport");service->setProperty("IOUserClientClass","PongUsbMuxUserClient");
 service->setProperty("Transport","DWC3 physical USB Mux bulk endpoints");
 // Native lockdownd's connection detector consumes these interface
 // properties. They describe our actual implemented Mux endpoints; the
 // service keeps its Pong class and root-only transport user client.
 service->setProperty("USBDeviceFunction","AppleUSBMux");
 service->setProperty("IsActive",kOSBooleanFalse);
 service->registerService();return true;
}
extern "C" void pong_usb_mux_session(bool online) {
 using namespace PongMux;if(!lock)return;
 IOLockLock(lock);connected=online;generation++;fromHost.clear();toHost.clear();IOLockUnlock(lock);
 if(service) {
  service->setProperty("IsActive",online?kOSBooleanTrue:kOSBooleanFalse);
  // 22H374 mux_notification_handler: actual configured/unconfigured event.
  // Never call IOKit clients with the byte-queue mutex held.
  service->messageClients(0xe3ff8201u,reinterpret_cast<void *>(uintptr_t(online)));
 }
}
extern "C" bool pong_usb_mux_receive(const void *data,unsigned size) {
 using namespace PongMux;if(!lock)return false;
 IOLockLock(lock);bool ok=connected&&opened&&fromHost.put(data,size);IOLockUnlock(lock);return ok;
}
extern "C" bool pong_usb_mux_can_receive(unsigned size) {
 using namespace PongMux;if(!lock)return false;
 IOLockLock(lock);bool ok=connected&&opened&&size<=Capacity-fromHost.size;IOLockUnlock(lock);return ok;
}
extern "C" bool pong_usb_mux_bridge_ready(void) {
 using namespace PongMux;if(!lock)return false;
 IOLockLock(lock);bool ready=opened&&identityReady;IOLockUnlock(lock);return ready;
}
extern "C" void pong_usb_mux_attached(bool value) {
 using namespace PongMux;if(!lock)return;IOLockLock(lock);attached=value;IOLockUnlock(lock);
}
extern "C" bool pong_usb_mux_serial(char *out,unsigned capacity) {
 using namespace PongMux;if(!lock||!capacity)return false;
 IOLockLock(lock);bool ready=nativeSerial[0]!=0;if(ready)strlcpy(out,nativeSerial,capacity);IOLockUnlock(lock);return ready;
}
extern "C" unsigned pong_usb_mux_transmit(void *data,unsigned size) {
 using namespace PongMux;if(!lock)return 0;
 IOLockLock(lock);unsigned n=0;
 if(connected&&opened&&toHost.size>=4) {
  unsigned length=0;
  for(unsigned i=0;i<4;i++)length|=unsigned(toHost.data[(toHost.read+i)%Capacity])<<(8*i);
  if(length<=size&&length+4<=toHost.size){toHost.take(&length,4);n=toHost.take(data,length);}
 }IOLockUnlock(lock);return n;
}
