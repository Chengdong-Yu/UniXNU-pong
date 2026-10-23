/*
 * Pong / UniXNU adaptation of HackiOS stage0 and init (MIT; see ../LICENSE).
 * Static Darwin 22 executable; optional 20G81 UI library is configured separately.
 */
#include "mini.h"

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

static void
prepare_var(void)
{
    static char buf[65536];
    char args[4096] = {0}, device[96] = {0};
    size_t len = sizeof(args);
    struct stat root, var;
    int root_ok = stat("/", &root) == 0;
    int separate = root_ok && stat("/private/var", &var) == 0 &&
                   root.st_dev != var.st_dev;
    /* Explicit device only. Never guess disk numbers or format an existing disk. */
    if (!separate && sysctlbyname("kern.bootargs", args, &len, NULL, 0) == 0) {
        args[sizeof(args) - 1] = 0;
        const char *key = "pong_data_device=";
        for (char *p = args; *p;) {
            while (*p == ' ') p++;
            if (strncmp(p, key, strlen(key)) == 0) {
                p += strlen(key);
                size_t n = 0;
                while (*p && *p != ' ' && n + 1 < sizeof(device)) device[n++] = *p++;
                device[n] = 0;
                break;
            }
            while (*p && *p != ' ') p++;
        }
        if (root_ok && strncmp(device, "/dev/disk", 9) == 0) {
            /* Apple APFS 2142 mount ABI already used by the qualified Pong owner hook. */
            uint8_t payload[0x150] = {0};
            uint64_t fspec = (uint64_t)(uintptr_t)device, flags = 0;
            uint16_t mode = 1;
            uint32_t owner = 99;
            memcpy(payload, &fspec, 8); memcpy(payload + 8, &flags, 8);
            memcpy(payload + 16, &mode, 2);
            memcpy(payload + 0x144, &owner, 4); memcpy(payload + 0x148, &owner, 4);
            int result = mount("apfs", "/private/var", 0, payload);
            int error = result ? errno : 0;
            separate = result == 0 && stat("/private/var", &var) == 0 &&
                       root.st_dev != var.st_dev;
            printf("Pong init: Data mount %s result=%d errno=%d\n", device, result, error);
        }
    }
    if (!separate) {
        puts("Pong init: no separate writable Data volume; var seed skipped (required for full iOS startup)");
        return;
    }
    int fd = open("/private/var/.pong-write-check", O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) { printf("Pong init: Data not writable: errno=%d\n", errno); return; }
    close(fd); unlink("/private/var/.pong-write-check");
    if (read_text("/pong/etc/var.skel", buf, sizeof(buf)) < 0) {
        printf("Pong init: var.skel unavailable: errno=%d\n", errno); return;
    }
    int made = 0, errors = 0;
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
    printf("Pong init: writable Data ready, created=%d seed-errors=%d\n", made, errors);
}

/* Repair older Data seeds without replacing valid user timezone settings.
 * On 20G81 /usr/share/zoneinfo already links back into this Data directory. */
static void
prepare_runtime_paths(void)
{
    struct stat st;
    /* The sparse restore skeleton omits these database parents. Consumers
     * run as mobile and cannot create a multi-level path in one mkdir. */
    static const char *mobile_dirs[]={
        "/private/var/mobile/Library/CoreDuet",
        "/private/var/mobile/Library/CoreDuet/Knowledge",
        "/private/var/mobile/Library/IdentityServices",
        "/private/var/mobile/Library/IdentityServices/PDS",
        "/private/var/mobile/Library/Logs",
        "/private/var/mobile/Library/Logs/CrashReporter"
    };
    for (unsigned i=0;i<sizeof(mobile_dirs)/sizeof(mobile_dirs[0]);i++) {
        const char *p=mobile_dirs[i];
        if (lstat(p,&st)==0) continue;
        if (mkdir(p,0700)!=0 || chown(p,501,501)!=0 || chmod(p,0700)!=0)
            printf("Pong init: mobile database parent unavailable %s errno=%d\n",p,errno);
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

/* Same composition as HackiOS init: hide physical audio plug-ins before
 * mediaserverd scans them. Keep mediaserverd for VideoToolbox/HEIC decoding.
 * The optional job set is a real 20G81 directory supplied in writable Data. */
static void
prepare_boot_services(void)
{
    static const struct { const char *source, *target; } views[] = {
        {"/private/var/pong/boot-jobs", "/pong/LaunchDaemons"},
        {"/private/var/pong/empty-hal", "/System/Library/Audio/Plug-Ins/HAL"},
        {"/private/var/pong/empty-hal", "/Library/Audio/Plug-Ins/HAL"},
        /* Keep a read-only path to stock assets before overlaying their
         * directory. The RAM view contains aliases, not copied pictures. */
        {"/Library/Wallpaper", "/private/var/pong/stock-wallpaper"},
        {"/private/var/pong/Wallpaper", "/Library/Wallpaper"}
    };
    for (unsigned i = 0; i < sizeof(views)/sizeof(views[0]); i++) {
        struct stat source, target;
        if (strcmp(views[i].target,"/Library/Wallpaper")==0 &&
            stat("/private/var/pong/stock-wallpaper/Collections/Collections~ipad.plist",&source)!=0) {
            puts("Pong init: wallpaper aliases SKIP (stock view unavailable)");
            continue;
        }
        if (lstat(views[i].source,&source)!=0 || (source.st_mode&S_IFMT)!=S_IFDIR ||
            source.st_uid!=0 || (source.st_mode&022)!=0 ||
            stat(views[i].target,&target)!=0 || (target.st_mode&S_IFMT)!=S_IFDIR) {
            printf("Pong init: optional boot view SKIP %s (source/target unavailable)\n",views[i].target);
            continue;
        }
        int result=mount("bindfs",views[i].target,MNT_RDONLY,(void *)views[i].source);
        printf("Pong init: boot view %s -> %s result=%d errno=%d\n",
               views[i].source,views[i].target,result,result?errno:0);
    }
}

static void
configure_ui_library(void)
{
    const char *path="/private/var/pong/libpong-ui.dylib";
    const char *base="/private/var/pong/libpong-ui.dylib SpringBoard backboardd containermanagerd containermanagerd_system mediaserverd Preferences";
    const char *metal="; /private/var/pong/PongAdrenoMetal.bundle/PongAdrenoMetal backboardd";
    char conf[512],args[4096]={0};size_t length=sizeof(args);
    memcpy(conf,base,strlen(base)+1);
    struct stat st;
    if (stat(path,&st)!=0 || (st.st_mode&S_IFMT)!=S_IFREG || st.st_uid!=0) {
        puts("Pong init: UI compatibility library absent; SKIP");return;
    }
    if(sysctlbyname("kern.bootargs",args,&length,NULL,0)==0) {
        args[sizeof(args)-1]=0;
        for(char *p=args;*p;) {
            while(*p==' ')p++;
            if(strncmp(p,"pong_metal=1",12)==0 && (!p[12] || p[12]==' ')) {
                if(stat("/private/var/pong/PongAdrenoMetal.bundle/PongAdrenoMetal",&st)==0 &&
                   (st.st_mode&S_IFMT)==S_IFREG && st.st_uid==0 && !(st.st_mode&022)) {
                    memcpy(conf+strlen(base),metal,strlen(metal)+1);
                    puts("Pong init: native Metal entry enabled for backboardd");
                }
                break;
            }
            while(*p && *p!=' ')p++;
        }
    }
    int result=sysctlbyname("kern.exec_inject",NULL,NULL,conf,strlen(conf)+1);
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
    boot_stage("static 20G81 helper entered; console open returned");
    /* Upstream init does this before handing the console to non-root jobs. */
    (void)chmod("/dev/console", 0666);
    (void)chdir("/");
    puts("Pong init: HackiOS userland adapter / iOS 16.6.1 20G81 / static arm64");
    /* maxvnodes tuning is optional; the owner mount hook already publishes
     * skipIgnition in the commpage and exec apple strings before this helper. */
    boot_stage("preparing writable Data seed");
    prepare_var();
    boot_stage("Data preparation returned");
    prepare_calendar();
    prepare_runtime_paths();
    prepare_boot_services();
    configure_ui_library();
    char *next[] = {"/sbin/launchd", NULL};
    char *env[] = {
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin",
        "HOME=/var/root", "TMPDIR=/var/tmp/",
        "DYLD_SHARED_CACHE_DIR=/private/preboot/System/Library/Caches/com.apple.dyld/",
        "DYLD_FALLBACK_LIBRARY_PATH=/private/preboot/usr/lib",
        NULL
    };
    puts("Pong init: exec original 20G81 launchd; stdin/stdout -> /dev/null, stderr -> /dev/console");
    boot_stage("exec original /sbin/launchd.ios");
    /* 20G81 launchd rejects an interactive stdout before its PID 1 setup.
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
