/* SPDX-License-Identifier: MIT
 * Bounded root-only snapshots of the actual native GPU trace. No GPU work,
 * resets, reboot timers or synthetic completion; stored on writable Data.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
static int save(io_connect_t connection,uint32_t type,const char *path) {
 mach_vm_address_t address=0;mach_vm_size_t bytes=0;
 kern_return_t r=IOConnectMapMemory64(connection,type,mach_task_self(),&address,&bytes,kIOMapAnywhere|kIOMapReadOnly);
 if(r){fprintf(stderr,"Pong GPU capture: type=%x map=%x\n",type,r);return -1;}
 const size_t limit=type==0x4754?16u<<20:2u<<20;int result=-1;
 if(address && bytes>=16 && bytes<=limit) {
  char temporary[256];snprintf(temporary,sizeof temporary,"%s.new",path);
  int fd=open(temporary,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
  if(fd>=0) {
   size_t done=0;while(done<bytes){ssize_t n=write(fd,(const void *)(address+done),bytes-done);if(n<0&&errno==EINTR)continue;if(n<=0)break;done+=(size_t)n;}
   if(done==bytes && !fsync(fd))result=0;close(fd);
   if(!result && rename(temporary,path))result=-1;
   if(result)unlink(temporary);
   fprintf(stderr,"Pong GPU capture: %s bytes=%llu result=%d\n",path,(unsigned long long)bytes,result);
  }
 }
 IOConnectUnmapMemory64(connection,type,mach_task_self(),address);return result;
}
int main(void) {
 if(geteuid())return 77;
 io_service_t service=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("NativeAdrenoGpuPublished"));
 if(!service){fprintf(stderr,"Pong GPU capture: actual service unavailable\n");return 0;}
 const char *directory="/private/var/logs/pong";mkdir(directory,0755);
 unsigned passed=0;const unsigned captures[]={60,180,300};
 for(unsigned i=0;i<sizeof(captures)/sizeof(captures[0]);++i) {
  sleep(captures[i]-passed);passed=captures[i];char path[256];
  io_connect_t connection=0;kern_return_t r=IOServiceOpen(service,mach_task_self(),0x4754,&connection);
  if(r){fprintf(stderr,"Pong GPU capture: actual trace open=%x\n",r);continue;}
  snprintf(path,sizeof path,"%s/gpu-%u.trace.bin",directory,passed);save(connection,0x4754,path);
  snprintf(path,sizeof path,"%s/gpu-%u.pixels.bin",directory,passed);save(connection,0x4755,path);
  IOServiceClose(connection);
 }
 IOObjectRelease(service);return 0;
}
