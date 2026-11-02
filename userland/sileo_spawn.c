/* SPDX-License-Identifier: MIT
 * Sileo's root command adapter for native Darwin24. Stock XNU rejects
 * non-root persona UID/GID zero overrides. Keep that kernel rule intact and
 * use the installed, caller-restricted setuid helper for these commands.
 */
#include <spawn.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>
extern int proc_pidpath(int,void *,uint32_t);
extern int posix_spawnattr_set_persona_np(const posix_spawnattr_t *,uid_t,uint32_t);
extern int posix_spawnattr_set_persona_uid_np(const posix_spawnattr_t *,uid_t);
extern int posix_spawnattr_set_persona_gid_np(const posix_spawnattr_t *,gid_t);
static pthread_once_t once=PTHREAD_ONCE_INIT;
static bool sileo;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static const posix_spawnattr_t *rootAttributes[32];
static void identify(void)
{
    char path[PATH_MAX],resolved[PATH_MAX];
    sileo=proc_pidpath(getpid(),path,sizeof path)>0 && realpath(path,resolved) &&
        (!strcmp(resolved,"/Applications/Sileo.app/Sileo") ||
         !strcmp(resolved,"/private/var/jb/Applications/Sileo.app/Sileo"));
}
static bool selected(const posix_spawnattr_t *attr)
{
    pthread_once(&once,identify);if(!sileo||!attr)return false;
    bool found=false;pthread_mutex_lock(&lock);
    for(unsigned i=0;i<32;i++)found|=rootAttributes[i]==attr;
    pthread_mutex_unlock(&lock);return found;
}
static int setPersona(const posix_spawnattr_t *attr,uid_t id,uint32_t flags)
{
    pthread_once(&once,identify);
    if(!sileo||!attr||id!=99||flags!=1)return posix_spawnattr_set_persona_np(attr,id,flags);
    pthread_mutex_lock(&lock);
    for(unsigned i=0;i<32;i++)if(!rootAttributes[i]||rootAttributes[i]==attr) {
        rootAttributes[i]=attr;pthread_mutex_unlock(&lock);return 0;
    }
    pthread_mutex_unlock(&lock);return ENOMEM;
}
static int setUid(const posix_spawnattr_t *attr,uid_t uid)
{ return selected(attr)?(uid==0?0:EINVAL):posix_spawnattr_set_persona_uid_np(attr,uid); }
static int setGid(const posix_spawnattr_t *attr,gid_t gid)
{ return selected(attr)?(gid==0?0:EINVAL):posix_spawnattr_set_persona_gid_np(attr,gid); }
static int destroy(posix_spawnattr_t *attr)
{
    pthread_mutex_lock(&lock);
    for(unsigned i=0;i<32;i++)if(rootAttributes[i]==attr)rootAttributes[i]=NULL;
    pthread_mutex_unlock(&lock);return posix_spawnattr_destroy(attr);
}
static int initialize(posix_spawnattr_t *attr)
{
    pthread_mutex_lock(&lock);
    for(unsigned i=0;i<32;i++)if(rootAttributes[i]==attr)rootAttributes[i]=NULL;
    pthread_mutex_unlock(&lock);return posix_spawnattr_init(attr);
}
static int spawn(pid_t *pid,const char *path,const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *attr,char *const argv[],char *const envp[])
{
    if(!selected(attr))return posix_spawn(pid,path,actions,attr,argv,envp);
    if(!path||path[0]!='/'||!argv||!argv[0])return EINVAL;
    size_t count=0;while(argv[count]){if(++count>4096)return E2BIG;}
    char **args=calloc(count+3,sizeof(*args));if(!args)return ENOMEM;
    const char *helper="/Applications/Sileo.app/giveMeRoot";
    args[0]=(char *)helper;args[1]="giveMeRoot";args[2]=(char *)path;
    for(size_t i=1;i<count;i++)args[i+2]=argv[i];
    int result=posix_spawn(pid,helper,actions,attr,args,envp);
    free(args);return result;
}
#define INTERPOSE(replacement,original) \
    __attribute__((used)) static struct {const void *replacement;const void *original;} \
    pair_##original __attribute__((section("__DATA,__interpose")))={(const void *)replacement,(const void *)original}
INTERPOSE(setPersona,posix_spawnattr_set_persona_np);
INTERPOSE(setUid,posix_spawnattr_set_persona_uid_np);
INTERPOSE(setGid,posix_spawnattr_set_persona_gid_np);
INTERPOSE(destroy,posix_spawnattr_destroy);
INTERPOSE(initialize,posix_spawnattr_init);
INTERPOSE(spawn,posix_spawn);
