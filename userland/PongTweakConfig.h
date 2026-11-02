/* SPDX-License-Identifier: MIT
 * Narrow rootful injection rule, shared by static init and package refresh.
 * The kernel's existing exec adapter composes this rule with the UI adapter.
 * No task-for-pid injection into PID 1, no replacement of its services.
 * Caller provides POSIX stat/open/read/close and string declarations.
 */
#pragma once
#define PONG_TWEAK_LIBRARY "/usr/lib/ellekit/libinjector.dylib"
#define PONG_TWEAK_RULE "; " PONG_TWEAK_LIBRARY
static int pong_tweak_config(char *out,size_t capacity,const char *base)
{
    size_t used=strlen(base);
    if(used>=capacity)return -1;
    memcpy(out,base,used+1);
    struct stat st;
    if(stat(PONG_TWEAK_LIBRARY,&st) || (st.st_mode&S_IFMT)!=S_IFREG || st.st_uid || (st.st_mode&022))return 0;
    // Recovery switch disables only third-party tweaks, preserving the
    // board's UI/runtime adapters. ElleKit also honors its native safe mode.
    if(!stat("/var/mobile/.eksafemode",&st) || !stat("/var/.pong-disable-tweaks",&st))return 0;
    int fd=open("/private/etc/pong-tweak-processes",O_RDONLY,0);
    if(fd<0)return 0;
    char text[256];
    if(fstat(fd,&st) || (st.st_mode&S_IFMT)!=S_IFREG || st.st_uid || (st.st_mode&022) || st.st_size<=0 || st.st_size>255){close(fd);return -1;}
    ssize_t n=read(fd,text,sizeof(text)-1);close(fd);
    if(n!=st.st_size)return -1;text[n]=0;
    const char rule[]=PONG_TWEAK_RULE;
    if(used+sizeof(rule)>=capacity)return -1;
    memcpy(out+used,rule,sizeof(rule));used+=sizeof(rule)-1;
    unsigned count=0;
    for(size_t i=0;i<(size_t)n;) {
        if(text[i]=='#'){while(i<(size_t)n && text[i]!='\n')i++;continue;}
        if(text[i]==' '||text[i]=='\n'||text[i]=='\t'||text[i]=='\r'){i++;continue;}
        size_t start=i;
        while(i<(size_t)n && text[i]!=' '&&text[i]!='\n'&&text[i]!='\t'&&text[i]!='\r') {
            char c=text[i++];
            if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return -1;
        }
        size_t length=i-start;
        if(!length||length>63||used+1+length>=capacity)return -1;
        // Injection targets are explicit application executables, never
        // PID 1 or its proxy. Those paths retain the native bootstrap ABI.
        if((length==7&&!memcmp(text+start,"launchd",7)) ||
           (length==10&&!memcmp(text+start,"launchd.ios",10)) ||
           (length==8&&!memcmp(text+start,"xpcproxy",8)))return -1;
        out[used++]=' ';memcpy(out+used,text+start,length);used+=length;out[used]=0;count++;
    }
    if(!count){memcpy(out,base,strlen(base)+1);return 0;}
    return 1;
}
