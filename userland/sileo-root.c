/* SPDX-License-Identifier: MIT
 * Sileo's setuid entry for the Pong development kernel. Use normal XNU
 * credentials; this port has no libjailbreak or jailbreakd dependency.
 */
#include <sys/stat.h>
#include <sys/param.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <spawn.h>
#include <errno.h>
#include <sys/wait.h>
extern char **environ;
extern int proc_pidpath(int,void *,uint32_t);
int main(int argc,char **argv)
{
    char app[PATH_MAX],parent[PATH_MAX],resolved[PATH_MAX];
    struct stat expected,actual;
    if(proc_pidpath(getppid(),parent,sizeof parent)<=0 || !realpath(parent,resolved))return 77;
    if(stat(resolved,&actual))return 77;
    const char *installed[]={"/Applications/Sileo.app/Sileo","/var/jb/Applications/Sileo.app/Sileo"};
    int trusted=0;
    for(unsigned i=0;i<sizeof installed/sizeof installed[0];i++)
        if(realpath(installed[i],app) && !stat(app,&expected) &&
           expected.st_dev==actual.st_dev && expected.st_ino==actual.st_ino &&
           expected.st_uid==0 && !(expected.st_mode&022))trusted=1;
    if(!trusted) {
        fputs("Pong Sileo: caller is not the installed Sileo executable\n",stderr);return 77;
    }
    if(geteuid()!=0 || setgid(0) || setuid(0) || getuid()!=0) {
        fputs("Pong Sileo: setuid credentials unavailable\n",stderr);return 77;
    }
    int command=1;
    if(argc>1 && !strcmp(argv[1],"giveMeRoot"))command++;
    if(argc<=command)return 0;
    if(!strcmp(argv[command],"whoami")){puts("root");return 0;}
    if(argv[command][0]!='/'){fputs("Pong Sileo: absolute command required\n",stderr);return 64;}
    // APT may start long after boot's initial network attempt. Synchronize
    // conventional resolver state from actual configd DNS for each package
    // transaction, without changing native DNS or requiring a fixed server.
    if(!strcmp(argv[command],"/usr/bin/apt") || !strcmp(argv[command],"/usr/bin/apt-get")) {
        char *network[]={"/pong/package-network",NULL};pid_t child;
        if(!posix_spawn(&child,network[0],NULL,NULL,network,environ)) {
            int status;while(waitpid(child,&status,0)<0 && errno==EINTR){}
        }
    }
    execv(argv[command],argv+command);
    perror("Pong Sileo execv");return 71;
}
