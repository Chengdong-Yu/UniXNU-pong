/* SPDX-License-Identifier: MIT */
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "PongTweakConfig.h"
int main(void)
{
    if(getuid()!=0)return 77;
    char base[1024],config[1024];size_t size=sizeof(base);
    if(sysctlbyname("kern.exec_inject",base,&size,NULL,0) || !size || size>sizeof(base) || base[size-1])return 71;
    char *old=strstr(base,PONG_TWEAK_RULE);if(old)*old=0;
    int result=pong_tweak_config(config,sizeof(config),base);
    if(result<0){fputs("Pong rootful: invalid tweak configuration; existing adapters preserved\n",stderr);return 65;}
    if(sysctlbyname("kern.exec_inject",NULL,NULL,config,strlen(config)+1))return 71;
    fprintf(stderr,"Pong rootful: ElleKit application injection %s; changes apply on next app launch\n",result?"configured":"disabled/absent");
    return 0;
}
