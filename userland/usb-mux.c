/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * USB Mux wire bridge to real, launchd-owned iOS loopback services.
 * Protocol fields follow the public libimobiledevice/usbmuxd wire format.
 * This implements transport only: lockdownd owns pairing, trust and TLS.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <arpa/inet.h>
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { MaxConnections=16, Buffer=131072, WireMax=49152, OutCapacity=262144,
       Syn=2, Rst=4, Ack=16 };
struct connection {
 int fd,state;uint16_t host,port;uint32_t receive,send,acked,window;
 size_t pending;uint8_t input[Buffer];uint64_t deadline;
 unsigned trace;
};
static struct connection clients[MaxConnections];
static io_connect_t transport;
static uint64_t generation;
// One final USB chunk can contain the tail of a maximum frame followed by
// the next header. Retain it until consume() has split the two frames.
static uint8_t incoming[WireMax+4096],outgoing[OutCapacity];
static size_t incomingSize,outgoingSize,outgoingAt;
static unsigned version;static uint16_t txSequence,rxSequence;
static volatile sig_atomic_t quitting;
static char nativeIdentity[64];static unsigned nativeIdentityDone;
static bool identityApplied;
static uint64_t identityRetry;
static void *loadIdentity(void *unused){
 (void)unused;
 void *library=dlopen("/usr/lib/libMobileGestalt.dylib",RTLD_LAZY|RTLD_LOCAL);
 CFTypeRef (*answer)(CFStringRef)=library?dlsym(library,"MGCopyAnswer"):NULL;
 // The same scoped board adapter supplies lockdownd and the descriptor.
 // dlsym of an original MG symbol need not return dyld's interposed target.
 CFTypeRef (*boardAnswer)(CFStringRef)=dlsym(RTLD_DEFAULT,"pong_usb_copy_board_identity");
 CFTypeRef value=boardAnswer?boardAnswer(CFSTR("UniqueDeviceID")):NULL;
 if(!value)value=answer?answer(CFSTR("UniqueDeviceID")):NULL;
 char text[64]={0};bool valid=value&&CFGetTypeID(value)==CFStringGetTypeID()&&
  CFStringGetCString(value,text,sizeof text,kCFStringEncodingASCII)&&strlen(text)>=16;
 unsigned different=0;char first=0;
 if(valid)for(unsigned i=0;text[i];i++) {
  char c=text[i];if(!((c>='0'&&c<='9')||(c>='A'&&c<='F')||
    (c>='a'&&c<='f')||c=='-'))valid=false;
  if(c!='-'){if(!first)first=c;else if((c|32)!=(first|32))different++;}
 }
 if(!different)valid=false; // MG's all-f/all-zero unavailable sentinel is not a UDID.
 if(valid)memcpy(nativeIdentity,text,sizeof text);
 if(value)CFRelease(value);
 // Keep the image loaded: Gestalt can retain process-wide state/callbacks.
 __atomic_store_n(&nativeIdentityDone,1,__ATOMIC_RELEASE);return NULL;
}
static void registerIdentity(void){
 for(unsigned i=0;i<75&&!__atomic_load_n(&nativeIdentityDone,__ATOMIC_ACQUIRE);i++)usleep(10000);
 const bool ready=__atomic_load_n(&nativeIdentityDone,__ATOMIC_ACQUIRE)!=0;
 const char *serial=ready?nativeIdentity:"";
 kern_return_t r=IOConnectCallStructMethod(transport,3,serial,strlen(serial)+1,NULL,NULL);
 identityApplied=!r&&ready&&serial[0];
 if(!r)fprintf(stderr,"Pong USB Mux: %s identity supplied to USB descriptor\n",identityApplied?"native iOS UniqueDeviceID":"retained board");
}
static uint64_t milliseconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static uint16_t get16(const uint8_t *p){return (uint16_t)((p[0]<<8)|p[1]);}
static uint32_t get32(const uint8_t *p){return ((uint32_t)get16(p)<<16)|get16(p+2);}
static void put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static void put32(uint8_t *p,uint32_t v){put16(p,(uint16_t)(v>>16));put16(p+2,(uint16_t)v);}
static int nativeSocket(unsigned method,struct connection *c,unsigned port,const void *in,size_t inSize,void *out,size_t *outSize,size_t *done){
 uint64_t input[3]={generation,(uint64_t)(c-clients),port},answer[2]={0};uint32_t count=method==6?2:1;
 if(method==8)count=0;
 kern_return_t r=IOConnectCallMethod(transport,method,input,method==4?3:2,in,inSize,
     count?answer:NULL,&count,out,outSize);
 if(r&&outSize)*outSize=0;
 if(done)*done=r?0:(size_t)answer[1];
 return r?ENOTCONN:method==8?0:(int)answer[0];
}
static void closeClient(struct connection *c){
 if(c->fd>=0&&transport)(void)nativeSocket(8,c,0,NULL,0,NULL,NULL,NULL);
 memset(c,0,sizeof(*c));c->fd=-1;
}
static void reset(void){for(unsigned i=0;i<MaxConnections;i++)closeClient(&clients[i]);incomingSize=outgoingAt=outgoingSize=0;version=0;txSequence=0;rxSequence=0xffff;}
static bool wire(unsigned protocol,const void *payload,size_t length){
 unsigned header=protocol!=0&&version>=2?16:8;size_t n=header+length;
 if(n>4096||outgoingSize+n+4>OutCapacity)return false;
 // Kernel accepts exactly one full frame per call, preserving USB short
 // packet/ZLP boundaries. Internal queue prefixes never appear on USB.
 uint8_t *p=outgoing+outgoingSize;memcpy(p,&n,4);p+=4;
 put32(p,protocol);put32(p+4,(uint32_t)n);
 if(header==16){put32(p+8,0xfaceface);put16(p+12,txSequence++);put16(p+14,rxSequence);}
 if(length)memcpy(p+header,payload,length);outgoingSize+=4+n;return true;
}
static bool tcp(struct connection *c,uint8_t flags,const void *payload,size_t length){
 uint8_t p[2048]={0};if(length>sizeof(p)-20)return false;
 put16(p,c->port);put16(p+2,c->host);put32(p+4,c->send);put32(p+8,c->receive);
 p[12]=5<<4;p[13]=flags;put16(p+14,(uint16_t)((Buffer-c->pending)>>8));
 if(length)memcpy(p+20,payload,length);return wire(6,p,20+length);
}
static void refuse(uint16_t host,uint16_t port,uint32_t received){
 struct connection c={.fd=-1,.host=host,.port=port,.receive=received};(void)tcp(&c,Rst,NULL,0);
}
static bool tcpInput(const uint8_t *p,size_t n){
 if(n<20)return false;unsigned words=p[12]>>4;
 // Older native Mux writers omit the IP/TCP data-offset nibble for their
 // fixed 20-byte header. This is a USB tunnel, not an unvalidated IP frame.
 unsigned header=words?words*4:20;
 if(header<20||header>n){fprintf(stderr,"Pong USB Mux: invalid TCP header=%u bytes=%zu\n",header,n);return false;}
 uint16_t host=get16(p),port=get16(p+2);uint32_t seq=get32(p+4),ack=get32(p+8);
 uint8_t flags=p[13];const uint8_t *data=p+header;size_t size=n-header;
 struct connection *c=NULL;
 for(unsigned i=0;i<MaxConnections;i++)if(clients[i].state&&clients[i].host==host&&clients[i].port==port){c=&clients[i];break;}
 if(flags&Rst){if(c)closeClient(c);return true;}
 if(flags==Syn&&!size) {
  if(c){(void)tcp(c,Rst,NULL,0);closeClient(c);return true;}
  for(unsigned i=0;i<MaxConnections;i++)if(!clients[i].state){c=&clients[i];break;}
  if(!c||!port){refuse(host,port,seq+1);return true;}
  int result=nativeSocket(4,c,port,NULL,0,NULL,NULL,NULL);
  if(result&&result!=EINPROGRESS){refuse(host,port,seq+1);return true;}
  memset(c,0,sizeof(*c));c->fd=(int)(c-clients);c->state=1;c->host=host;c->port=port;
  c->receive=seq+1;c->window=(uint32_t)get16(p+14)<<8;c->deadline=milliseconds()+10000;
  fprintf(stderr,"Pong USB Mux: connecting real loopback service %u\n",port);
  return true;
 }
 if(!c){refuse(host,port,seq+(uint32_t)size);return true;}
 if(c->state==1)return true; // socket connection has not completed
 // Native hosts may mark data with PSH. It does not alter byte sequence
 // or acknowledgement semantics and is not a connection reset.
 if((flags&~8u)!=Ack){(void)tcp(c,Rst,NULL,0);closeClient(c);return true;}
 if((int32_t)(ack-c->acked)<0||(int32_t)(c->send-ack)<0)return false;
 c->acked=ack;c->window=(uint32_t)get16(p+14)<<8;
 if(c->state==2){if(ack!=1)return false;c->state=3;fprintf(stderr,"Pong USB Mux: native service %u connected\n",port);}
 if(seq!=c->receive){return tcp(c,Ack,NULL,0);}
 if(size>Buffer-c->pending)return false;
  if(size){memcpy(c->input+c->pending,data,size);c->pending+=size;c->receive+=(uint32_t)size;
   if(c->port==62078&&c->trace++<6)fprintf(stderr,"Pong USB Mux: native request bytes=%zu pending=%zu window=%u\n",size,c->pending,c->window);
   return tcp(c,Ack,NULL,0);}
 return true;
}
static bool consume(void){
 while(incomingSize>=8) {
  unsigned protocol=get32(incoming),length=get32(incoming+4);
  if(length<8||length>WireMax)return false;
  // Framing is identifiable per packet. Native transitions may retain a
  // v1 SETUP/TCP frame after VERSION2, while VERSION itself stays v1.
  unsigned header=8;
  if(protocol!=0&&length>=16) {
   if(incomingSize<16)return true;
   uint32_t magic=get32(incoming+8);
   if(magic==0xfeedface||magic==0xfaceface)header=16;
  }
  if(incomingSize<length)return true;
  if(header==16){
   uint32_t magic=get32(incoming+8);
   // Native host OUT is FEEDFACE; device IN is FACEFACE. Accept both
   // known v2 framing markers on input, but emit the device direction.
   if(magic!=0xfeedface&&magic!=0xfaceface){fprintf(stderr,"Pong USB Mux: bad frame magic=%08x protocol=%u length=%u\n",magic,protocol,length);return false;}
   rxSequence=get16(incoming+12);
  }
  const uint8_t *p=incoming+header;size_t size=length-header;
  if(protocol==0) {
   if(size!=12||get32(p)<1||get32(p)>2)return false;
   for(unsigned i=0;i<MaxConnections;i++)closeClient(&clients[i]);
   outgoingAt=outgoingSize=0;version=0;txSequence=0;rxSequence=0xffff;
   uint8_t answer[12]={0};unsigned negotiated=get32(p);put32(answer,negotiated);
   if(!wire(0,answer,12))return false;version=negotiated;
   fprintf(stderr,"Pong USB Mux: wire version %u negotiated; native trust remains required\n",version);
  } else if(protocol==2) {
   // macOS requested5 on the real Pong session; libimobiledevice uses7.
   // Both select the implemented basic v2 transport (bit1 is optional).
   if(version<2||size!=1||(p[0]&~2u)!=5){fprintf(stderr,"Pong USB Mux: unsupported SETUP size=%zu code=%u\n",size,size?p[0]:0);return false;}
   txSequence=0;rxSequence=header==16?get16(incoming+12):0xffff;
  } else if(protocol==6) {
   if(!version||!tcpInput(p,size))return false;
  } else if(protocol==1&&size&&(p[0]==5||p[0]==7)) {
   // Defined warning/information controls carry no command to execute.
   // An error control (3), or an unknown control, still closes the session.
   fprintf(stderr,"Pong USB Mux: native control=%u bytes=%zu\n",p[0],size);
  } else {fprintf(stderr,"Pong USB Mux: unsupported protocol=%u size=%zu\n",protocol,size);return false;}
  incomingSize-=length;if(incomingSize)memmove(incoming,incoming+length,incomingSize);
 }
 return true;
}
static bool serviceClients(void){
 for(unsigned i=0;i<MaxConnections;i++) {
  struct connection *c=&clients[i];if(!c->state)continue;
  if(outgoingSize>OutCapacity-8192)break;
  if(c->state==1) {
   if(milliseconds()>c->deadline){(void)tcp(c,Rst,NULL,0);closeClient(c);continue;}
   int error=nativeSocket(5,c,0,NULL,0,NULL,NULL,NULL);
   if(error==EINPROGRESS||error==EALREADY)continue;
   if(error){(void)tcp(c,Rst,NULL,0);closeClient(c);continue;}
   if(!tcp(c,Syn|Ack,NULL,0))return false;c->send=1;c->state=2;
  }
  if(c->state==2&&milliseconds()>c->deadline){(void)tcp(c,Rst,NULL,0);closeClient(c);continue;}
  if(c->state!=3)continue;
  if(c->pending) {
   size_t written=0,size=c->pending>4096?4096:c->pending;
   int error=nativeSocket(6,c,0,c->input,size,NULL,NULL,&written);
   if(written){c->pending-=written;if(c->pending)memmove(c->input,c->input+written,c->pending);
    if(c->port==62078&&c->trace++<6)fprintf(stderr,"Pong USB Mux: native service accepted bytes=%zu\n",written);
    if(!tcp(c,Ack,NULL,0))return false;}
   if(error&&error!=EAGAIN&&error!=EINTR){(void)tcp(c,Rst,NULL,0);closeClient(c);continue;}
  }
  uint32_t outstanding=c->send-c->acked;
  {
   uint8_t buffer[1536];size_t allowed=c->window>outstanding?c->window-outstanding:0;
   if(allowed>sizeof buffer)allowed=sizeof buffer;if(!allowed)continue;
   size_t read=allowed;int error=nativeSocket(7,c,0,NULL,0,buffer,&read,NULL);
   if(read){if(!tcp(c,Ack,buffer,read))return false;c->send+=(uint32_t)read;
    if(c->port==62078&&c->trace++<6)fprintf(stderr,"Pong USB Mux: native reply bytes=%zu\n",read);}
   if((!error&&!read)||(error&&error!=EAGAIN&&error!=EINTR)){if(!tcp(c,Rst,NULL,0))return false;closeClient(c);}
  }
 }
 return true;
}
static void stop(int value){(void)value;quitting=1;}
int main(void){
 if(geteuid())return 77;signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGPIPE,SIG_IGN);
 pthread_t identityThread;if(!pthread_create(&identityThread,NULL,loadIdentity,NULL))pthread_detach(identityThread);
 for(unsigned i=0;i<MaxConnections;i++)clients[i].fd=-1;
 setvbuf(stderr,NULL,_IOLBF,0);
 while(!quitting) {
  if(!transport) {
   io_service_t service=IOServiceGetMatchingService(MACH_PORT_NULL,IOServiceMatching("PongUsbMuxTransport"));
   if(!service){usleep(250000);continue;}
   kern_return_t r=IOServiceOpen(service,mach_task_self(),0,&transport);IOObjectRelease(service);
   if(r){transport=0;usleep(250000);continue;}
   fprintf(stderr,"Pong USB Mux: root bridge attached to physical DWC3 transport\n");
   registerIdentity();identityRetry=milliseconds()+1000;
  }
  uint64_t state[3]={0};uint32_t count=3;
  kern_return_t r=IOConnectCallScalarMethod(transport,0,NULL,0,state,&count);
  if(r||count!=3){IOServiceClose(transport);transport=0;reset();continue;}
  if(generation!=state[0]){reset();generation=state[0];}
  if(!identityApplied&&milliseconds()>=identityRetry&&__atomic_load_n(&nativeIdentityDone,__ATOMIC_ACQUIRE)&&nativeIdentity[0]) {
   registerIdentity();identityRetry=milliseconds()+1000;
  }
  if(!state[1]){usleep(20000);continue;}
  if(outgoingAt<outgoingSize) {
   uint32_t length=0;memcpy(&length,outgoing+outgoingAt,4);
   r=IOConnectCallMethod(transport,2,&generation,1,outgoing+outgoingAt+4,length,NULL,NULL,NULL,NULL);
   if(!r){outgoingAt+=4+length;if(outgoingAt==outgoingSize)outgoingAt=outgoingSize=0;}
   else if(r!=kIOReturnNoSpace){IOServiceClose(transport);transport=0;reset();continue;}
  }
  if(state[2]&&outgoingSize<OutCapacity-8192) {
   uint8_t block[4096];size_t size=sizeof block;
   r=IOConnectCallMethod(transport,1,&generation,1,NULL,0,NULL,NULL,block,&size);
   if(!r&&size&&incomingSize+size<=sizeof incoming){memcpy(incoming+incomingSize,block,size);incomingSize+=size;
    if(!consume()){fprintf(stderr,"Pong USB Mux: malformed/unsupported stream; reset bounded session\n");IOServiceClose(transport);transport=0;reset();continue;}}
   else if(r||incomingSize+size>sizeof incoming){IOServiceClose(transport);transport=0;reset();continue;}
  }
  if(!serviceClients()){IOServiceClose(transport);transport=0;reset();continue;}
  usleep(state[2]||outgoingSize?1000:5000);
 }
 reset();if(transport)IOServiceClose(transport);return 0;
}
