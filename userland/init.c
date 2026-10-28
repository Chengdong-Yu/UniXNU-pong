/*
 * Pong / UniXNU adaptation of HackiOS stage0 and init (MIT; see ../LICENSE).
 * Static init; the build selects the matching restore and runtime UI adapter.
 */
#include "mini.h"
#ifndef PONG_NATIVE_APFS18
#error This init belongs to the native Darwin24 APFS recipe
#endif
#ifndef PONG_SYSTEM_VERSION
#define PONG_SYSTEM_VERSION "18.7.10"
#endif
#ifndef PONG_SYSTEM_BUILD
#define PONG_SYSTEM_BUILD "22H374"
#endif
#ifndef PONG_UI_LIBRARY_PATH
#define PONG_UI_LIBRARY_PATH "/pong/libpong-ui18.dylib"
#endif
#ifndef PONG_UI_EXTRA_PROCESSES
#define PONG_UI_EXTRA_PROCESSES ""
#endif

/* Apple clang may lower zero-initialized aggregates to bzero even freestanding. */
void bzero(void *p, size_t n) { memset(p, 0, n); }

/* Reuse the ordinary console path; boot progress must not depend on a
 * custom sysctl or its name lookup/locking before launchd can start. */
static void
boot_stage(const char *message)
{
    printf("Pong init: %s\n", message);
}

static unsigned long
number(const char *s, int base)
{
    unsigned long v = 0;
    while (*s >= '0' && *s <= '9' && *s - '0' < base)
        v = v * (unsigned)base + (unsigned)(*s++ - '0');
    return v;
}

static int
read_text(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY, 0);
    size_t used = 0;
    ssize_t n;
    if (fd < 0) return -1;
    while (used + 1 < cap && (n = read(fd, buf + used, cap - used - 1)) > 0)
        used += (size_t)n;
    close(fd);
    buf[used] = 0;
    return (int)used;
}

static int
copy_new(const char *source, const char *dest, int mode, int uid, int gid)
{
    char buf[4096];
    int in = open(source, O_RDONLY, 0), out;
    ssize_t n;
    if (in < 0) return -1;
    out = open(dest, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (out < 0) { close(in); return errno == 17 ? 0 : -1; }
    int ok = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t done = 0;
        while (done < n) {
            ssize_t w = write(out, buf + done, (size_t)(n - done));
            if (w <= 0) { ok = -1; break; }
            done += w;
        }
        if (ok) break;
    }
    if (n < 0) ok = -1;
    close(in); close(out);
    if (ok) { unlink(dest); return -1; }
    return chmod(dest, mode) || chown(dest, uid, gid) ? -1 : 0;
}

/* Atomically publish a small port-owned preference before launchd starts.
 * Setup bypass is a one-time Data migration, guarded by a marker so later
 * user changes are preserved. */
static int
copy_replace(const char *source, const char *dest, const char *temporary,
             int mode, int uid, int gid)
{
    char buf[4096];
    int in = open(source, O_RDONLY, 0), out;
    ssize_t n;
    if (in < 0) return -1;
    (void)unlink(temporary);
    out = open(temporary, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (out < 0) { close(in); return -1; }
    int ok = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t done = 0;
        while (done < n) {
            ssize_t w = write(out, buf + done, (size_t)(n - done));
            if (w <= 0) { ok = -1; break; }
            done += w;
        }
        if (ok) break;
    }
    if (n < 0) ok = -1;
    if (!ok && fsync(out) != 0) ok = -1;
    close(in);
    if (close(out) != 0) ok = -1;
    if (!ok && (chmod(temporary, mode) != 0 || chown(temporary, uid, gid) != 0)) ok = -1;
    if (!ok && rename(temporary, dest) == 0) return 0;
    (void)unlink(temporary);
    return -1;
}

static void
prepare_setup_bypass(void)
{
    static const char marker[] = "/private/var/.pong-setup-bypass-22H374";
    static const char buddy_source[] = "/pong/var-seed/mobile/Library/Preferences/com.apple.purplebuddy.plist";
    static const char mini_source[] = "/pong/var-seed/mobile/Library/Preferences/com.apple.purplebuddy.notbackedup.plist";
    static const char springboard_source[] = "/pong/var-seed/mobile/Library/Preferences/com.apple.springboard.plist";
    struct stat st;
    if (lstat(marker, &st) == 0) return;
    if (copy_replace(buddy_source,
            "/private/var/mobile/Library/Preferences/com.apple.purplebuddy.plist",
            "/private/var/mobile/Library/Preferences/.com.apple.purplebuddy.plist.pong-tmp",
            0600, 501, 501) != 0 ||
        copy_replace(mini_source,
            "/private/var/mobile/Library/Preferences/com.apple.purplebuddy.notbackedup.plist",
            "/private/var/mobile/Library/Preferences/.com.apple.purplebuddy.notbackedup.plist.pong-tmp",
            0600, 501, 501) != 0) {
        puts("Pong init: setup bypass preference update failed; retain current Data preferences");
        return;
    }
    /* Add development watchdog defaults only if this Data has no domain
     * yet. Never replace a populated SpringBoard preference file. */
    if (copy_new(springboard_source,
            "/private/var/mobile/Library/Preferences/com.apple.springboard.plist",
            0600, 501, 501) != 0)
        puts("Pong init: SpringBoard default preference seed skipped");
    int fd = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
        static const char version[] = "22H374 setup bypass applied\n";
        (void)write(fd, version, sizeof(version) - 1);
        (void)fsync(fd);
        (void)close(fd);
        (void)chown(marker, 0, 0);
        (void)chmod(marker, 0600);
        puts("Pong init: applied native 22H374 setup-complete preferences once");
    } else {
        printf("Pong init: setup bypass marker unavailable errno=%d\n", errno);
    }
}

static void
prepare_var(void)
{
    static char buf[65536];
    struct stat root, var;
    int root_ok = stat("/", &root) == 0;
    int separate = root_ok && stat("/private/var", &var) == 0 &&
                   root.st_dev != var.st_dev;
    /* Only the Darwin24 kernel mounts the UUID-selected APFS Data volume. */
    if (!separate) {
        puts("Pong init: no separate writable Data volume; var seed skipped (required for full iOS startup)");
        return;
    }
    int fd = open("/private/var/.pong-write-check", O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) { printf("Pong init: Data not writable: errno=%d\n", errno); return; }
    close(fd); unlink("/private/var/.pong-write-check");
    int made = 0, errors = 0;
    static const char *skeletons[] = {"/pong/etc/var.skel", "/pong/etc/preinstalled-assets.skel"};
    for (unsigned index = 0; index < sizeof(skeletons) / sizeof(skeletons[0]); index++) {
        if (read_text(skeletons[index], buf, sizeof(buf)) < 0) {
            if (index == 0) printf("Pong init: var.skel unavailable: errno=%d\n", errno);
            continue;
        }
        for (char *line = buf; *line;) {
            char *end = strchr(line, '\n'), *field[6] = {line};
            int count = 1;
            if (end) *end = 0;
            for (char *p = line; *p; p++) if (*p == '\t' && count < 6) {
                *p = 0; field[count++] = p + 1;
            }
            int result = 0;
            if (count == 5 && field[0][0] == 'd') {
                struct stat st;
                if (lstat(field[4], &st) != 0) {
                    result = mkdir(field[4], (int)number(field[1], 8));
                    if (!result) result = chmod(field[4], (int)number(field[1], 8)) ||
                        chown(field[4], (int)number(field[2], 10), (int)number(field[3], 10));
                    if (!result) made++;
                } else if ((st.st_mode & S_IFMT) != S_IFDIR) result = -1;
            } else if (count == 3 && field[0][0] == 'l') {
                struct stat st;
                if (lstat(field[2], &st) != 0) result = symlink(field[1], field[2]);
            } else if (count == 6 && field[0][0] == 'f') {
                result = copy_new(field[5], field[4], (int)number(field[1], 8),
                                  (int)number(field[2], 10), (int)number(field[3], 10));
            }
            errors += result != 0;
            if (!end) break;
            line = end + 1;
        }
    }
    printf("Pong init: writable Data ready, created=%d seed-errors=%d\n", made, errors);
}

/* Repair missing Data parents/timezone links without replacing valid settings. */
static void
prepare_runtime_paths(void)
{
    struct stat st;
    /* Native 22H374 usermanagerd persists its own manifest below keybags.
     * A sparse restore does not include these parents. Never replace files
     * or change ownership of an existing directory. */
    static const struct { const char *path; int mode; } root_dirs[] = {
        {"/private/var/keybags", 0700}, {"/private/var/logs", 0755},
        /* The reduced 22H374 Data seed has the MCM database but omits the
         * standard roots where the native manager creates per-system and
         * system-group containers. Create only those roots; MCM still owns
         * UUID allocation, metadata and each container's contents. */
        {"/private/var/containers/Data/System", 0755},
        {"/private/var/containers/Shared/SystemGroup", 0755}
    };
    for (unsigned i=0;i<sizeof(root_dirs)/sizeof(root_dirs[0]);i++) {
        if (lstat(root_dirs[i].path,&st)==0) continue;
        if (mkdir(root_dirs[i].path,root_dirs[i].mode)!=0 ||
            chown(root_dirs[i].path,0,0)!=0 || chmod(root_dirs[i].path,root_dirs[i].mode)!=0)
            printf("Pong init: runtime parent unavailable %s errno=%d\n",root_dirs[i].path,errno);
    }
    /* The sparse restore skeleton omits these database parents. Consumers
     * run as mobile and cannot create a multi-level path in one mkdir. */
    static const char *mobile_dirs[]={
        "/private/var/mobile/Library/CoreDuet",
        "/private/var/mobile/Library/CoreDuet/Knowledge",
        "/private/var/mobile/Library/IdentityServices",
        "/private/var/mobile/Library/IdentityServices/PDS",
        "/private/var/mobile/Library/Logs",
        "/private/var/mobile/Library/Logs/CrashReporter",
        "/private/var/mobile/Library/Pong",
        "/private/var/mobile/Library/Pong/LaunchServices",
        "/private/var/mobile/Library/Pong/LaunchServices/Library",
        "/private/var/mobile/Library/Pong/LaunchServices/Library/Caches"
    };
    for (unsigned i=0;i<sizeof(mobile_dirs)/sizeof(mobile_dirs[0]);i++) {
        const char *p=mobile_dirs[i];
        if (lstat(p,&st)==0) continue;
        if (mkdir(p,0700)!=0 || chown(p,501,501)!=0 || chmod(p,0700)!=0)
            printf("Pong init: mobile database parent unavailable %s errno=%d\n",p,errno);
    }
    /* 22H374 passwd: _installd uid/gid 33, home /var/installd. Root
     * creates missing parents before the original daemon drops privileges.
     * Existing directories and their ownership remain untouched. */
    static const char *installation_dirs[]={
        "/private/var/installd", "/private/var/installd/Library",
        "/private/var/installd/Library/MobileInstallation",
        "/private/var/installd/Library/Caches"
    };
    for (unsigned i=0;i<sizeof(installation_dirs)/sizeof(installation_dirs[0]);i++) {
        const char *p=installation_dirs[i];
        if (lstat(p,&st)==0) continue;
        if (mkdir(p,0700)!=0 || chown(p,33,33)!=0 || chmod(p,0700)!=0)
            printf("Pong init: installation directory unavailable %s errno=%d\n",p,errno);
    }
    const char *zone = "/private/var/db/timezone/zoneinfo";
    const char *local = "/private/var/db/timezone/localtime";
    if (stat("/usr/share/zoneinfo.default/UTC", &st) == 0 && stat(zone, &st) != 0) {
        int present = lstat(zone, &st) == 0;
        if ((!present || (st.st_mode & S_IFMT) == S_IFLNK) &&
            (!present || unlink(zone) == 0)) {
            if (symlink("/usr/share/zoneinfo.default", zone) == 0)
                puts("Pong init: timezone path repaired -> /usr/share/zoneinfo.default");
            else printf("Pong init: timezone path repair failed: errno=%d\n", errno);
        }
    }
    if (stat(local, &st) != 0 && stat("/usr/share/zoneinfo.default/UTC", &st) == 0) {
        int present = lstat(local, &st) == 0;
        if ((!present || (st.st_mode & S_IFMT) == S_IFLNK) &&
            (!present || unlink(local) == 0) &&
            symlink("/var/db/timezone/zoneinfo/UTC", local) != 0)
            printf("Pong init: localtime fallback failed: errno=%d\n", errno);
    }
    if (lstat("/private/var/cores", &st) != 0) {
        if (mkdir("/private/var/cores", 01775) == 0)
            (void)chmod("/private/var/cores", 01775);
        else printf("Pong init: cores directory unavailable: errno=%d\n", errno);
    }
}

/* IOKit's fallback calendar is initialized before ArmDTPlatform is matched.
 * The packager supplies a per-build UTC epoch; PID 1 applies it before any
 * stock daemon sees 1970 timestamps. */
static void
prepare_calendar(void)
{
    char args[4096] = {0};
    size_t len=sizeof(args);
    if (sysctlbyname("kern.bootargs",args,&len,NULL,0)!=0) return;
    args[sizeof(args)-1]=0;
    for (char *p=args;*p;) {
        while (*p==' ') p++;
        if (strncmp(p,"epoch=",6)==0) {
            unsigned long epoch=number(p+6,10);
            if (epoch>=978307200UL) {
                struct timeval tv={.tv_sec=(long)epoch,.tv_usec=0};
                int result=settimeofday(&tv,NULL);
                printf("Pong init: calendar epoch=%lu result=%d errno=%d\n",
                       epoch,result,result?errno:0);
            }
            return;
        }
        while (*p && *p!=' ') p++;
    }
}

static void
prepare_boot_services(void)
{
    /* Darwin24 imports the canonical Version 7 job cache from OS and uses
     * the matching Cryptex namespace. */
    puts("Pong init: native 22H374 OS jobs and Cryptex namespace retained");
    /* Keep the native audiomxd session/CoreMedia service but hide physical HAL plug-ins;
     * retain all OS files and the separate VirtualAudio.plugin directory. */
    struct stat source, target;
    const char *empty="/pong/audio-hal18";
    const char *hal="/System/Library/Audio/Plug-Ins/HAL";
    if (lstat(empty,&source)==0 && (source.st_mode&S_IFMT)==S_IFDIR &&
        source.st_uid==0 && !(source.st_mode&022) &&
        lstat(hal,&target)==0 && (target.st_mode&S_IFMT)==S_IFDIR) {
        int result=mount("bindfs",hal,MNT_RDONLY,(void *)empty);
        printf("Pong init: Darwin24 board HAL view result=%d errno=%d\n",
               result,result?errno:0);
    }
}

static void
prepare_audio_policy(void)
{
    /* 22H374 can host CoreMedia endpoint discovery in audiomxd or airplayd.
     * Pong has no external AirPlay endpoint provider. Selecting airplayd
     * leaves audiomxd waiting on its DiscoveryMode RPC until it self-kills.
     * Use the real integrated provider selected by the native feature flag,
     * before launchd and CoreMedia cache that decision. Preserve any existing
     * operator override instead of overwriting unrelated feature choices. */
    static const char *dirs[]={"/private/var/preferences/FeatureFlags",
        "/private/var/preferences/FeatureFlags/Domain"};
    struct stat st;
    for(unsigned i=0;i<sizeof(dirs)/sizeof(dirs[0]);i++) {
        if(lstat(dirs[i],&st)!=0) {
            if(errno!=2 || mkdir(dirs[i],0755)!=0)goto failed;
            if(chown(dirs[i],0,0)!=0)goto failed;
        } else if((st.st_mode&S_IFMT)!=S_IFDIR || st.st_uid!=0 || (st.st_mode&022))goto failed;
    }
    if(copy_new("/pong/etc/AirPlay-local-audio.plist",
        "/private/var/preferences/FeatureFlags/Domain/AirPlay.plist",0644,0,0)!=0)goto failed;
    puts("Pong init: native integrated CoreMedia audio policy prepared");return;
failed:
    printf("Pong init: audio provider policy unavailable errno=%d; existing policy retained\n",errno);
}

/* An identity change must not reuse the restore's cached iPad answers.
 * Invalidate only the two real MobileGestalt cache files, before launchd
 * starts. Its original helper rebuilds them from the new device tree.
 * Keep user preferences, containers and shader caches intact. */
static void
prepare_device_identity(void)
{
    static const char marker[]="/private/var/.pong-identity-iPhone17,1-22H374-island";
    static const char *paths[]={
        "/private/var/mobile/Library/Caches/com.apple.MobileGestalt.plist",
        "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist"
    };
    char product[64]={0};size_t length=sizeof(product);struct stat st;
    if(sysctlbyname("hw.product",product,&length,NULL,0)!=0 ||
       strcmp(product,"iPhone17,1") || lstat(marker,&st)==0)return;
    int ok=1,removed=0;
    for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++) {
        if(lstat(paths[i],&st)!=0) {if(errno!=2)ok=0;continue;}
        if((st.st_mode&S_IFMT)!=S_IFREG || unlink(paths[i])!=0) {ok=0;continue;}
        removed++;
    }
    if(ok) {
        int fd=open(marker,O_CREAT|O_EXCL|O_WRONLY,0600);
        if(fd>=0) {const char value[]="iPhone17,1 / iPhone 16 Pro / phone\n";
            if(write(fd,value,sizeof(value)-1)!=(ssize_t)sizeof(value)-1 || fsync(fd)!=0) {
                close(fd);unlink(marker);ok=0;
            } else close(fd);
        } else ok=0;
    }
    printf("Pong init: iPhone 16 Pro identity cache refresh %s removed=%d\n",ok?"PASS":"FAIL",removed);
}

static void
configure_ui_library(void)
{
    const char *path=PONG_UI_LIBRARY_PATH;
    /* 22H374 has audiomxd and no mediaserverd job. Its audio daemon needs no
     * UI hooks; inject only processes with an actual Darwin24 adapter. */
    const char *base=PONG_UI_LIBRARY_PATH " SpringBoard backboardd containermanagerd containermanagerd_system Preferences" PONG_UI_EXTRA_PROCESSES;
    struct stat st;
    if (stat(path,&st)!=0 || (st.st_mode&S_IFMT)!=S_IFREG || st.st_uid!=0) {
        puts("Pong init: UI compatibility library absent; SKIP");return;
    }
    /* Only the matching 22H374 runtime adapter is injected. */
    int result=sysctlbyname("kern.exec_inject",NULL,NULL,(void *)base,strlen(base)+1);
    printf("Pong init: UI library configuration result=%d errno=%d\n",result,result?errno:0);
}

int
main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    int fd = open("/dev/console", O_RDWR, 0);
    if (fd >= 0) {
        dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
        if (fd > 2) close(fd);
    }
    boot_stage("static " PONG_SYSTEM_BUILD " helper entered; console open returned");
    /* Upstream init does this before handing the console to non-root jobs. */
    (void)chmod("/dev/console", 0666);
    (void)chdir("/");
    puts("Pong init: userland adapter / " PONG_SYSTEM_VERSION " " PONG_SYSTEM_BUILD " / static arm64");
    /* maxvnodes tuning is optional; the owner mount hook already publishes
     * skipIgnition in the commpage and exec apple strings before this helper. */
    boot_stage("preparing writable Data seed");
    prepare_var();
    boot_stage("Data preparation returned");
    prepare_calendar();
    prepare_runtime_paths();
    prepare_setup_bypass();
    prepare_device_identity();
    prepare_boot_services();
    prepare_audio_policy();
    configure_ui_library();
    char *next[] = {"/sbin/launchd", NULL};
    char *env[] = {
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin",
        "HOME=/var/root", "TMPDIR=/var/tmp/",
        "DYLD_SHARED_CACHE_DIR=/private/preboot/System/Library/Caches/com.apple.dyld/",
        "DYLD_FALLBACK_LIBRARY_PATH=/private/preboot/usr/lib",
        NULL
    };
    puts("Pong init: exec original " PONG_SYSTEM_BUILD " launchd; stdin/stdout -> /dev/null, stderr -> /dev/console");
    boot_stage("exec original /sbin/launchd.ios");
    /* launchd requires a non-interactive stdout before its PID 1 setup.
     * Keep diagnostics on fd 2; fd 1 must not inherit this helper's console. */
    int nullfd = open("/dev/null", O_RDWR, 0);
    if (nullfd < 0 || dup2(nullfd, 0) < 0 || dup2(nullfd, 1) < 0) {
        if (nullfd > 2) close(nullfd);
        (void)dup2(2, 1);
        printf("Pong init: non-interactive launchd descriptors failed: errno=%d\n", errno);
        return 127;
    }
    if (nullfd > 2) close(nullfd);
    execve("/sbin/launchd.ios", next, env);
    int exec_error = errno;
    (void)dup2(2, 1);
    errno = exec_error;
    boot_stage("original launchd exec returned an error");
    printf("Pong init: launchd exec failed: errno=%d (%s)\n", errno, strerror(errno));
    /* A genuine PID 1 exec failure must remain visible to normal kernel handling. */
    return 127;
}
