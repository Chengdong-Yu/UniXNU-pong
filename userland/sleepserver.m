// SPDX-License-Identifier: MIT
// Local Sleep.framework 20G81 configuration provider for the iPad restore.
// Apple sleepd requires SleepDaemon.framework, absent from that restore.
// The wire interfaces/models below are the shipped Sleep.framework objects.
// Own durable configuration and real client notifications; do not acknowledge
// alarms, Focus, Health recording or Watch synchronization we cannot perform.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <dlfcn.h>
#import <notify.h>
#import <sys/stat.h>

@interface NSObject (PongSleepABI)
- (id)initWithIdentifier:(NSString *)identifier;
- (id)objectWithSyncAnchor:(id)anchor;
- (id)initWithSleepMode:(NSInteger)mode syncAnchor:(id)anchor;
- (id)initWithScheduleState:(NSUInteger)state syncAnchor:(id)anchor;
- (id)valueForEntitlement:(NSString *)key;
- (void)clientShouldCheckInWithCompletion:(void (^)(id,NSError *))reply;
@end

typedef void (^ObjectReply)(id,NSError *);
typedef void (^BoolReply)(BOOL,NSError *);
typedef void (^ErrorReply)(NSError *);
static void *sleepImage;
static NSXPCInterface *(*serverInterface)(void),*(*clientInterface)(void);
static NSString *const stateDirectory=@"/private/var/mobile/Library/Application Support/PongSleep";
static NSString *const statePath=@"/private/var/mobile/Library/Application Support/PongSleep/state.plist";
static NSError *failure(NSInteger code,NSString *why) {
    return [NSError errorWithDomain:@"dev.unixnu.pong.Sleep" code:code
        userInfo:@{NSLocalizedDescriptionKey:why}];
}
static NSError *unavailable(void) {
    return failure(ENOTSUP,@"This port has no SleepDaemon alarm/Focus/Health/Watch implementation.");
}
static id model(NSString *name) {return [[NSClassFromString(name) alloc] init];}

@class PongSleepSession;
@interface PongSleepState : NSObject
@property(nonatomic,strong) NSMutableDictionary *disk;
@property(nonatomic,strong) NSHashTable<PongSleepSession *> *sessions;
@property(nonatomic,strong) NSSet<Class> *modelClasses;
- (id)anchors;
- (id)loadModel:(NSString *)key className:(NSString *)name;
- (NSError *)save:(id)value key:(NSString *)key className:(NSString *)name;
@end
@interface PongSleepSession : NSObject
@property(nonatomic,strong) PongSleepState *state;
@property(nonatomic,weak) NSXPCConnection *connection;
- (void)changed;
@end

@implementation PongSleepState
- (instancetype)init {
    if(!(self=[super init]))return nil;
    _sessions=[NSHashTable weakObjectsHashTable];
    NSMutableSet *classes=[NSMutableSet setWithArray:@[[NSArray class],[NSMutableArray class],
        [NSDictionary class],[NSMutableDictionary class],[NSSet class],[NSMutableSet class],
        [NSString class],[NSNumber class],[NSDate class],[NSData class],[NSUUID class],[NSNull class],
        [NSURL class],[NSCalendar class],[NSTimeZone class],[NSDateComponents class],[NSOrderedSet class]]];
    unsigned count=0;Class *list=objc_copyClassList(&count);
    for(unsigned i=0;i<count;i++) {
        const char *image=class_getImageName(list[i]);
        if(image && strstr(image,"/Sleep.framework/") &&
           [list[i] conformsToProtocol:@protocol(NSSecureCoding)]) [classes addObject:list[i]];
    }
    free(list);_modelClasses=classes;
    _disk=[[NSDictionary dictionaryWithContentsOfFile:statePath] mutableCopy];
    if(!_disk)_disk=[NSMutableDictionary dictionary];
    if(![_disk[@"identifier"] isKindOfClass:NSString.class])_disk[@"identifier"]=NSUUID.UUID.UUIDString;
    if(![_disk[@"revision"] isKindOfClass:NSNumber.class])_disk[@"revision"]=@1;
    return self;
}
- (id)anchors {
    @synchronized(self) {
        id anchors=[[NSClassFromString(@"HKSPSyncAnchorContainer") alloc] initWithIdentifier:self.disk[@"identifier"]];
        for(NSString *selector in @[@"updateSleepScheduleSyncAnchor:",@"updateSleepSettingsSyncAnchor:",@"updateSleepEventRecordSyncAnchor:",
            @"updateSleepScheduleStateSyncAnchor:",@"updateSleepModeSyncAnchor:",@"updateSleepEventSyncAnchor:"])
            ((void(*)(id,SEL,id))objc_msgSend)(anchors,NSSelectorFromString(selector),self.disk[@"revision"]);
        return anchors;
    }
}
- (id)loadModel:(NSString *)key className:(NSString *)name {
    @synchronized(self) {
        NSError *error=nil;id data=self.disk[key],value=nil;
        if([data isKindOfClass:NSData.class])value=[NSKeyedUnarchiver unarchivedObjectOfClasses:self.modelClasses fromData:data error:&error];
        if(![value isKindOfClass:NSClassFromString(name)])value=model(name);
        id anchor=[self anchors];
        NSString *anchorKey=[key isEqual:@"schedule"]?@"sleepScheduleSyncAnchor":
            ([key isEqual:@"settings"]?@"sleepSettingsSyncAnchor":@"sleepEventRecordSyncAnchor");
        return [value objectWithSyncAnchor:[anchor valueForKey:anchorKey]];
    }
}
- (NSError *)save:(id)value key:(NSString *)key className:(NSString *)name {
    if(![value isKindOfClass:NSClassFromString(name)])return failure(EINVAL,@"Invalid Sleep model class");
    // Persisting an enabled schedule/tracker would falsely promise physical
    // scheduling or Health collection. Preserve actual unsupported semantics.
    if([key isEqual:@"schedule"] && [[value valueForKey:@"enabled"] boolValue])return unavailable();
    if([key isEqual:@"settings"])for(NSString *flag in @[@"scheduledSleepMode",@"sleepTracking",@"timeInBedTracking",@"watchSleepFeaturesEnabled"])
        if([[value valueForKey:flag] boolValue])return unavailable();
    @synchronized(self) {
        NSError *error=nil;
        NSData *data=[NSKeyedArchiver archivedDataWithRootObject:value requiringSecureCoding:YES error:&error];
        if(!data)return error?:failure(EIO,@"Sleep configuration could not be encoded");
        NSMutableDictionary *next=[self.disk mutableCopy];next[key]=data;
        next[@"revision"]=@([self.disk[@"revision"] unsignedLongLongValue]+1);
        if(![NSFileManager.defaultManager createDirectoryAtPath:stateDirectory withIntermediateDirectories:YES
            attributes:@{NSFilePosixPermissions:@0700} error:&error])return error;
        NSData *plist=[NSPropertyListSerialization dataWithPropertyList:next format:NSPropertyListBinaryFormat_v1_0 options:0 error:&error];
        if(!plist || ![plist writeToFile:statePath options:NSDataWritingAtomic error:&error])return error?:failure(EIO,@"Sleep configuration was not committed");
        chmod(statePath.fileSystemRepresentation,0600);self.disk=next;
        for(PongSleepSession *session in self.sessions.allObjects)[session changed];
        return nil;
    }
}
@end

@implementation PongSleepSession
- (void)connectWithCompletion:(ObjectReply)reply {reply([self.state anchors],nil);}
- (void)checkInWithSyncAnchorContainer:(id)remote completion:(BoolReply)reply {
    if(![remote isKindOfClass:NSClassFromString(@"HKSPSyncAnchorContainer")]) {
        reply(NO,failure(EINVAL,@"Invalid Sleep sync anchor container"));return;
    }
    id current=[self.state anchors];BOOL different=NO;
    for(NSString *key in @[@"identifier",@"sleepScheduleSyncAnchor",@"sleepSettingsSyncAnchor",@"sleepEventRecordSyncAnchor",
        @"sleepScheduleStateSyncAnchor",@"sleepModeSyncAnchor",@"sleepEventSyncAnchor"])
        if(![[current valueForKey:key] isEqual:[remote valueForKey:key]])different=YES;
    reply(different,nil);
}
- (void)changed {
    id proxy=[self.connection remoteObjectProxyWithErrorHandler:^(NSError *error){(void)error;}];
    if(!proxy)return;
    // Framework-owned check-in path refreshes each object by its actual anchor.
    [proxy clientShouldCheckInWithCompletion:^(id anchors,NSError *error){(void)anchors;(void)error;}];
}
- (void)getSleepScheduleWithCompletion:(ObjectReply)reply {reply([self.state loadModel:@"schedule" className:@"HKSPSleepSchedule"],nil);}
- (void)getSleepSettingsWithCompletion:(ObjectReply)reply {reply([self.state loadModel:@"settings" className:@"HKSPSleepSettings"],nil);}
- (void)getSleepEventRecordWithCompletion:(ObjectReply)reply {reply([self.state loadModel:@"events" className:@"HKSPSleepEventRecord"],nil);}
- (void)getSleepModeWithCompletion:(void (^)(NSInteger,NSError *))reply {reply(0,nil);}
- (void)getSleepScheduleStateWithCompletion:(void (^)(NSUInteger,NSError *))reply {
    const NSUInteger *none=dlsym(sleepImage,"HKSPSleepScheduleStateNone");reply(*none,nil);
}
- (void)getSleepWidgetStateWithCompletion:(void (^)(NSInteger,NSError *))reply {reply(0,nil);}
- (void)saveSleepEventRecord:(id)value completion:(ErrorReply)reply {reply([self.state save:value key:@"events" className:@"HKSPSleepEventRecord"]);}
- (void)saveSleepSchedule:(id)value completion:(ErrorReply)reply {reply([self.state save:value key:@"schedule" className:@"HKSPSleepSchedule"]);}
- (void)saveSleepSettings:(id)value completion:(ErrorReply)reply {reply([self.state save:value key:@"settings" className:@"HKSPSleepSettings"]);}
- (void)setSleepMode:(NSInteger)mode reason:(NSUInteger)reason completion:(ErrorReply)reply {(void)mode;(void)reason;reply(unavailable());}
- (void)sleepAlarmWasDismissedOnDate:(NSDate *)date source:(NSUInteger)source completion:(BoolReply)reply {(void)date;(void)source;reply(NO,unavailable());}
- (void)sleepAlarmWasModifiedFromSource:(NSUInteger)source completion:(BoolReply)reply {(void)source;reply(NO,unavailable());}
- (void)sleepAlarmWasSnoozedUntilDate:(NSDate *)date source:(NSUInteger)source completion:(BoolReply)reply {(void)date;(void)source;reply(NO,unavailable());}
- (void)dismissSleepLockWithCompletion:(BoolReply)reply {reply(NO,unavailable());}
- (void)clearWidgetOverrideWithCompletion:(BoolReply)reply {reply(NO,unavailable());}
- (void)confirmAwakeWithCompletion:(BoolReply)reply {reply(NO,unavailable());}
- (void)createSleepFocusModeWithCompletion:(BoolReply)reply {reply(NO,unavailable());}
- (void)deleteSleepFocusModeWithCompletion:(BoolReply)reply {reply(NO,unavailable());}
- (void)dismissGoodMorningWithCompletion:(BoolReply)reply {reply(NO,unavailable());}
- (void)publishNotificationWithIdentifier:(NSString *)identifier userInfo:(NSDictionary *)info completion:(BoolReply)reply {(void)identifier;(void)info;reply(NO,unavailable());}
- (void)publishWakeUpResultsNotificationWithCompletion:(ObjectReply)reply {reply(nil,unavailable());}
- (void)setLockScreenOverrideState:(NSInteger)value userInfo:(NSDictionary *)info completion:(BoolReply)reply {(void)value;(void)info;reply(NO,unavailable());}
- (void)setWidgetOverrideState:(NSInteger)value completion:(BoolReply)reply {(void)value;reply(NO,unavailable());}
@end

@interface PongSleepListener : NSObject <NSXPCListenerDelegate>
@property(nonatomic,strong) PongSleepState *state;
@end
@implementation PongSleepListener
- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)connection {
    (void)listener;
    NSString *__unsafe_unretained *entitlement=(NSString *__unsafe_unretained *)dlsym(sleepImage,"HKSPSleepEntitlementName");
    if(!entitlement || ![[connection valueForEntitlement:*entitlement] boolValue])return NO;
    PongSleepSession *session=[PongSleepSession new];session.state=self.state;session.connection=connection;
    connection.exportedInterface=serverInterface();connection.remoteObjectInterface=clientInterface();
    connection.exportedObject=session;
    @synchronized(self.state){[self.state.sessions addObject:session];}
    [connection resume];return YES;
}
@end

int main(void) {
    @autoreleasepool {
        sleepImage=dlopen("/System/Library/PrivateFrameworks/Sleep.framework/Sleep",RTLD_NOW|RTLD_LOCAL);
        serverInterface=dlsym(sleepImage,"HKSPSleepServerInterface");clientInterface=dlsym(sleepImage,"HKSPSleepClientInterface");
        Protocol *wire=objc_getProtocol("HKSPSleepServer");
        if(!sleepImage || !serverInterface || !clientInterface || !wire ||
           !dlsym(sleepImage,"HKSPSleepScheduleStateNone") || !NSClassFromString(@"HKSPSyncAnchorContainer")) {
            NSLog(@"Pong Sleep: shipped20G81 interface/models unavailable");return 78;
        }
        class_addProtocol(PongSleepSession.class,wire);
        PongSleepListener *delegate=[PongSleepListener new];delegate.state=[PongSleepState new];
        NSXPCListener *listener=((id(*)(id,SEL,id))objc_msgSend)([NSXPCListener alloc],
            NSSelectorFromString(@"initWithMachServiceName:"),@"com.apple.sleepd.sleepserver");
        listener.delegate=delegate;[listener resume];
        NSString *__unsafe_unretained *lifecycle=(NSString *__unsafe_unretained *)dlsym(sleepImage,"HKSPSleepLifecycleName");
        if(lifecycle)notify_post([*lifecycle UTF8String]);
        NSLog(@"Pong Sleep: 20G81 configuration listener started; no alarm/Focus/Health/Watch backend");
        [NSRunLoop.currentRunLoop run];
    }
    return 0;
}
