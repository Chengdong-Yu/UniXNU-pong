/* SPDX-License-Identifier: MIT
 * Register the installed Phone with the 22H374 single-user LS database.
 * A successful tool exit is insufficient: require its real application proxy.
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <grp.h>
#include <sys/stat.h>
#include <unistd.h>

@interface NSObject (PongLaunchServices)
+ (id)defaultWorkspace;
+ (id)applicationProxyForIdentifier:(NSString *)identifier;
- (BOOL)registerApplication:(NSURL *)url;
- (BOOL)registerApplicationDictionary:(NSDictionary *)info;
- (NSURL *)bundleURL;
- (id)appState;
- (BOOL)isValid;
@end

int main(void)
{
    /* Package commands use the caller-restricted setuid adapter. Icon
     * registration never allocates or overwrites launchd's personas. */
    /* LS uses the caller's user domain. Do not populate root's separate DB. */
    if(getuid()==0 && (setgroups(0,NULL) || setgid(501) || setuid(501))) {
        perror("Pong Phone: mobile credentials");return 77;
    }
    if(getuid()!=501)return 77;
    setenv("HOME","/var/mobile",1);
    setenv("CFFIXED_USER_HOME","/var/mobile",1);
    @autoreleasepool {
        dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices",RTLD_NOW);
        Class workspaceClass=NSClassFromString(@"LSApplicationWorkspace");
        Class proxyClass=NSClassFromString(@"LSApplicationProxy");
        if(![workspaceClass respondsToSelector:@selector(defaultWorkspace)] ||
           ![proxyClass respondsToSelector:@selector(applicationProxyForIdentifier:)])return 69;
        NSString *path=@"/Applications/MobilePhone.app";
        if(![[NSFileManager defaultManager] fileExistsAtPath:[path stringByAppendingPathComponent:@"Info.plist"]])
            path=@"/Applications/MobilePhone.app";
        NSDictionary *info=[NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Info.plist"]];
        NSString *identifier=info[@"CFBundleIdentifier"];
        if(![identifier isKindOfClass:NSString.class] || !identifier.length)return 66;
        id workspace=[workspaceClass defaultWorkspace];
        if(![workspace respondsToSelector:@selector(registerApplication:)])return 69;
        for(unsigned attempt=0;attempt<12;attempt++) {
            BOOL registered=[workspace registerApplication:[NSURL fileURLWithPath:path isDirectory:YES]];
            id proxy=[proxyClass applicationProxyForIdentifier:identifier];
            NSURL *actual=[proxy respondsToSelector:@selector(bundleURL)]?[proxy bundleURL]:nil;
            id state=[proxy respondsToSelector:@selector(appState)]?[proxy appState]:nil;
            BOOL valid=[state respondsToSelector:@selector(isValid)] && [state isValid];
            BOOL matches=[actual.path.stringByResolvingSymlinksInPath isEqualToString:path.stringByResolvingSymlinksInPath];
            fprintf(stderr,"Pong Phone: mobile LS attempt=%u registered=%d valid=%d path=%s\n",attempt+1,registered,valid,actual.path.UTF8String?:"(absent)");
            if(valid && matches)return 0; /* Also accept an already registered native system app. */
            sleep(5);
        }
    }
    return 75;
}
