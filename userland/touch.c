/* Pong touch-to-HID bridge adapted from HackiOS inject.c (MIT).
 * Runs in backboardd, reads only actual Goodix samples. No graphics hooks. */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <mach/mach_time.h>
#include "touch.h"
#define say(...) fprintf(stderr,__VA_ARGS__)
/* IOKit's HID event system, the part that has no public header on iOS (prototypes checked against 20F66) */
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;

/* IOHIDVirtualServiceClientCallbacksV2 */
struct virtual_service_callbacks {
	uint64_t version;               /* 2 */
	void (*notification)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, CFDictionaryRef property);
	Boolean (*setProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value);
	CFTypeRef (*copyProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key);
	IOHIDEventRef (*copyEvent)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, IOHIDEventRef matching, uint32_t options);
	int (*setOutputEvent)(void *target, void *refcon, IOHIDServiceClientRef service, IOHIDEventRef event);
	CFTypeRef (*copyMatchingEvent)(void *target, void *refcon, IOHIDServiceClientRef service, CFDictionaryRef matching);
};

static IOHIDEventSystemClientRef (*IOHIDEventSystemClientCreateWithType)(CFAllocatorRef allocator, int type, CFDictionaryRef attributes);
static void (*IOHIDEventSystemClientScheduleWithDispatchQueue)(IOHIDEventSystemClientRef client, dispatch_queue_t queue);
static IOHIDServiceClientRef (*IOHIDVirtualServiceClientCreateWithCallbacks)(IOHIDEventSystemClientRef client, CFDictionaryRef attributes,
    const struct virtual_service_callbacks *callbacks, void *target, void *refcon);
static Boolean (*IOHIDVirtualServiceClientRemove)(IOHIDServiceClientRef service);
static Boolean (*IOHIDVirtualServiceClientDispatchEvent)(IOHIDServiceClientRef service, IOHIDEventRef event);
static IOHIDEventRef (*IOHIDEventCreateDigitizerEvent)(CFAllocatorRef allocator, uint64_t time, uint32_t transducer, uint32_t index,
    uint32_t identity, uint32_t event_mask, uint32_t button_mask, double x, double y, double z, double tip_pressure,
    double barrel_pressure, double twist, Boolean range, Boolean touch, uint32_t options);
static IOHIDEventRef (*IOHIDEventCreateDigitizerFingerEvent)(CFAllocatorRef allocator, uint64_t time, uint32_t index, uint32_t identity,
    uint32_t event_mask, double x, double y, double z, double tip_pressure, double twist, Boolean range, Boolean touch,
    uint32_t options);
static IOHIDEventRef (*IOHIDEventCreateKeyboardEvent)(CFAllocatorRef allocator, uint64_t time,
    uint32_t usage_page,uint32_t usage,Boolean down,uint32_t options);
static void (*IOHIDEventAppendEvent)(IOHIDEventRef parent, IOHIDEventRef child, uint32_t options);
static void (*IOHIDEventSetIntegerValue)(IOHIDEventRef event, uint32_t field, CFIndex value);

#define HID_CLIENT_SIMPLE               4       /* a client that only sends */
#define HID_CLIENT_RATE_CONTROLLED      3       /* what IOHIDEventSystemClientCreate() makes */
#define HID_PAGE_DIGITIZER              0x0d
#define HID_USAGE_TOUCH_SCREEN          0x04
#define HID_PAGE_CONSUMER               0x0c
#define HID_USAGE_CONSUMER_CONTROL      0x01
#define HID_USAGE_POWER                 0x30    /* the lock button */
#define HID_USAGE_MENU                  0x40    /* the home button */
#define HID_USAGE_VOLUME_UP             0xe9
#define HID_USAGE_VOLUME_DOWN           0xea
#define DIGITIZER_HAND                  3       /* kIOHIDDigitizerTransducerTypeHand */
#define DIGITIZER_RANGE                 0x1     /* event mask: what has changed */
#define DIGITIZER_TOUCH                 0x2
#define DIGITIZER_POSITION              0x4
#define DIGITIZER_FROM_EDGE_TIP         0x800   /* not a change: this finger came in over an edge of the screen */
#define FIELD_IS_BUILT_IN               0x4
#define FIELD_DIGITIZER_DISPLAY_INTEGRATED 0xb0019

#define MAX_FINGERS     10

/*
 * A finger that lands this close to an edge of the screen (a fraction of the width or the height)
 * is said to have come in over that edge. Apple's touch controller tells that from the shape of
 * the contact and marks the finger's events; UIKit's screen edge recognizers (the swipe up that
 * leaves an application, the swipe back in a navigation stack, the pull down from the top) take
 * no touch that is not so marked, whatever its path. All we have is where the finger landed.
 */
#define EDGE_SIDE       (16.0 / 375)
#define EDGE_TOP        (16.0 / 812)
#define EDGE_BOTTOM     (34.0 / 812)    /* the strip below the applications' content, where the home bar is */

static struct {
	bool touching, edge;
	double x, y;
} fingers[MAX_FINGERS];

static CFNumberRef
number(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

/* A service of ours (the callbacks' refcon): what it says of itself, and whether the event server has taken it. */
struct service {
	CFMutableDictionaryRef properties;
	bool opened;
	bool display;
};

/* The properties of a service of ours; the event server asks for them one at a time. */
static CFTypeRef
service_copy_property(__attribute__((unused)) void *target, void *refcon, __attribute__((unused)) IOHIDServiceClientRef service, CFStringRef key)
{
    CFTypeRef value=key?CFDictionaryGetValue(((struct service *)refcon)->properties,key):NULL;
	return value ? CFRetain(value) : NULL;
}

/*
 * What the window server last asked of the touch screen ("TouchDetectionMode"), in the low
 * byte: 1 = report touches; TOUCH_MODE_WAKE while the display is off and a tap is to wake it
 * -- which Apple's multitouch controller then does on its own; TOUCH_MODE_OFF = nothing at
 * all. (Seen: 0xff and 0xfe during the boot, 0x1 with the display on, 0x1fe after the lock
 * button.)
 */
#define TOUCH_MODE_WAKE         0xfe
#define TOUCH_MODE_OFF          0xff
static int touch_mode;

/* What the event server tells a service of ours: said once per name, to learn what there is. */
static Boolean
service_set_property(__attribute__((unused)) void *target, void *refcon, __attribute__((unused)) IOHIDServiceClientRef service, CFStringRef key,
    CFTypeRef value)
{
	/* the last thing the event server does when it opens a service; see make_service() */
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) &&
	    CFGetTypeID(value) == CFBooleanGetTypeID()) {
		__atomic_store_n(&((struct service *)refcon)->opened,CFBooleanGetValue(value),__ATOMIC_RELEASE);
	}
	if (((struct service *)refcon)->display && key != NULL && value != NULL &&
        CFEqual(key, CFSTR("TouchDetectionMode")) && CFGetTypeID(value) == CFNumberGetTypeID()) {
		int mode = 0;

		CFNumberGetValue(value, kCFNumberIntType, &mode);
		if (mode != __atomic_load_n(&touch_mode,__ATOMIC_RELAXED)) {
			say("Pong touch HID: touch detection mode %#x\n", mode);
		}
		__atomic_store_n(&touch_mode,mode,__ATOMIC_RELAXED);
	}
	return true;
}

static void
service_notification(__attribute__((unused)) void *target, __attribute__((unused)) void *refcon, __attribute__((unused)) IOHIDServiceClientRef service,
    __attribute__((unused)) uint32_t type, __attribute__((unused)) CFDictionaryRef property)
{
}

static const struct virtual_service_callbacks service_callbacks = {
	.version = 2, .notification = service_notification, .setProperty = service_set_property,
	.copyProperty = service_copy_property,
};

/*
 * A HID service of ours inside the event server: built in, with one usage.
 *
 * The event server is this very program, and it is still starting. A client that reaches it at
 * the wrong moment of that (backboardd's "HID system open ... start": the connection is made
 * and then replaced, "IOHIDEventSystemServer died. Reestablishing connection.") is left with a
 * service the server cannot talk to and never opens -- and a touch screen that backboardd waits
 * for in vain ("still waiting for a main screen digitizer"). When that moment comes depends on
 * how fast the machine is and on how many processors it has, so no fixed delay avoids it:
 * the service is made again until the server has opened it.
 */
static IOHIDServiceClientRef
make_service(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display)
{
	struct service *made = calloc(1, sizeof(*made));
	IOHIDServiceClientRef service = NULL;
	CFMutableDictionaryRef properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFStringRef name = CFStringCreateWithCString(NULL, product, kCFStringEncodingUTF8);
	CFNumberRef page_number = number(page), usage_number = number(usage), zero = number(0);
	CFArrayRef pairs;
	if (!made || !properties || !pair || !name || !page_number || !usage_number || !zero) return NULL;

	CFDictionarySetValue(pair, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(pair, CFSTR("DeviceUsage"), usage_number);
	pairs = CFArrayCreate(NULL, (const void **)&pair, 1, &kCFTypeArrayCallBacks);
	if (!pairs) return NULL;
	CFDictionarySetValue(properties, CFSTR("PrimaryUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePairs"), pairs);
	CFDictionarySetValue(properties, CFSTR("Built-In"), kCFBooleanTrue);
	CFDictionarySetValue(properties, CFSTR("Transport"), display?CFSTR("SPI"):CFSTR("Virtual"));
	CFDictionarySetValue(properties, CFSTR("Manufacturer"), CFSTR("Pong"));
	CFDictionarySetValue(properties, CFSTR("Product"), name);
	CFDictionarySetValue(properties, CFSTR("VendorID"), zero);
	CFDictionarySetValue(properties, CFSTR("ProductID"), zero);
	if (display) {
		CFDictionarySetValue(properties, CFSTR("DisplayIntegrated"), kCFBooleanTrue);
	}
	/* the dictionary stays: it is what service_copy_property() answers from */
	made->properties = properties;
	made->display = display;
	CFRelease(pair);CFRelease(pairs);CFRelease(name);
	CFRelease(page_number);CFRelease(usage_number);CFRelease(zero);
	for (int attempt = 1; attempt <= 20; attempt++) {
		__atomic_store_n(&made->opened,false,__ATOMIC_RELEASE);
		service = IOHIDVirtualServiceClientCreateWithCallbacks(client, properties, &service_callbacks, NULL, made);
		if (service == NULL) {
            sleep(1);continue;
		}
		for (int tenths = 0; tenths < 20 && !__atomic_load_n(&made->opened,__ATOMIC_ACQUIRE); tenths++) {
			usleep(100000);
		}
		if (__atomic_load_n(&made->opened,__ATOMIC_ACQUIRE)) {
			break;
		}
		say("Pong touch HID: the event server has not opened \"%s\" (attempt %d): making it again\n", product, attempt);
		IOHIDVirtualServiceClientRemove(service);
		CFRelease(service);
		service = NULL;
	}
	return service;
}

/* One frame of the touch screen: the hand, and under it every finger that is down or has just lifted. */
static void
send_touch(IOHIDServiceClientRef screen, const struct pong_touch *t)
{
	uint32_t change = t->type == PONG_TOUCH_MOVE ? DIGITIZER_POSITION : DIGITIZER_RANGE | DIGITIZER_TOUCH;
	IOHIDEventRef hand, finger;
	bool any = false;
	unsigned f = t->finger;

	if (f >= MAX_FINGERS || (t->type != PONG_TOUCH_DOWN && !fingers[f].touching)) {
		return;
	}
	fingers[f].touching = t->type != PONG_TOUCH_UP;
	fingers[f].x = t->x / 65535.0;
	fingers[f].y = t->y / 65535.0;
	if (t->type == PONG_TOUCH_DOWN) {
		fingers[f].edge = fingers[f].x < EDGE_SIDE || fingers[f].x > 1 - EDGE_SIDE || fingers[f].y < EDGE_TOP ||
		    fingers[f].y > 1 - EDGE_BOTTOM;
	}
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		any |= fingers[i].touching;
	}

	hand = IOHIDEventCreateDigitizerEvent(NULL, t->time, DIGITIZER_HAND, 0, 0, change & ~DIGITIZER_RANGE, 0, fingers[f].x,
	    fingers[f].y, 0, 0, 0, 0, any, any, 0);
    if (!hand) return;
	IOHIDEventSetIntegerValue(hand, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
	IOHIDEventSetIntegerValue(hand, FIELD_IS_BUILT_IN, 1);
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		if (!fingers[i].touching && i != f) {
			continue;
		}
		finger = IOHIDEventCreateDigitizerFingerEvent(NULL, t->time, i + 1, i + 2,
		    (i == f ? change : 0) | (fingers[i].edge ? DIGITIZER_FROM_EDGE_TIP : 0), fingers[i].x, fingers[i].y, 0,
		    fingers[i].touching ? 1.0 : 0.0, 0, fingers[i].touching, fingers[i].touching, 0);
        if (!finger) {CFRelease(hand);return;}
		IOHIDEventSetIntegerValue(finger, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
		IOHIDEventAppendEvent(hand, finger, 0);
		CFRelease(finger);
	}
    if (!IOHIDVirtualServiceClientDispatchEvent(screen,hand)) {
        static unsigned failures;
        if (failures++<3) fprintf(stderr,"Pong touch HID: event dispatch refused\n");
    }
	CFRelease(hand);
}

/* A real tap can wake the display in the requested wake-detection mode.
 * This consumer service is the tap-to-wake adapter, not a physical button
 * driver. Do not send the same contact into a locked/asleep application. */
static void
tap_wake(IOHIDServiceClientRef control,uint64_t time)
{
    if (!control || !IOHIDEventCreateKeyboardEvent) return;
    for (unsigned down=1;;down=0) {
        IOHIDEventRef event=IOHIDEventCreateKeyboardEvent(NULL,time,HID_PAGE_CONSUMER,HID_USAGE_POWER,down,0);
        if (event) {
            IOHIDEventSetIntegerValue(event,FIELD_IS_BUILT_IN,1);
            IOHIDVirtualServiceClientDispatchEvent(control,event);CFRelease(event);
        }
        if (!down) break;
    }
}

static void
release_contacts(IOHIDServiceClientRef screen,uint64_t time)
{
    for (unsigned i=0;i<MAX_FINGERS;i++) if (fingers[i].touching) {
        struct pong_touch up={time,PONG_TOUCH_UP,i,
            (uint32_t)(fingers[i].x*65535),(uint32_t)(fingers[i].y*65535)};
        send_touch(screen,&up);
    }
}

static void *
input_thread(void *unused)
{
    (void)unused;pthread_setname_np("Pong touch HID");
    int device=-1;
    for (unsigned i=0;i<30 && device<0;i++) {
        device=open("/dev/pong-touch",O_RDONLY);
        if (device<0) sleep(1);
    }
    if (device<0) {say("Pong touch HID: real touch device unavailable (%s); SKIP\n",strerror(errno));return NULL;}
    IOHIDEventSystemClientRef client=NULL;IOHIDServiceClientRef screen=NULL,wake_control=NULL;
    const int types[]={HID_CLIENT_SIMPLE,HID_CLIENT_RATE_CONTROLLED};
    /* Register during BackBoard's main-screen digitizer initialization.
     * make_service already retries a server that is not ready yet. */
    for (unsigned i=0;i<2 && !screen;i++) {
        client=IOHIDEventSystemClientCreateWithType(NULL,types[i],NULL);
        if (!client) continue;
        IOHIDEventSystemClientScheduleWithDispatchQueue(client,dispatch_queue_create("pong.touch.hid",NULL));
        screen=make_service(client,"Pong Goodix Berlin-D",HID_PAGE_DIGITIZER,HID_USAGE_TOUCH_SCREEN,true);
        if (!screen) CFRelease(client);
    }
    if (!screen) {say("Pong touch HID: server did not open digitizer; SKIP\n");close(device);return NULL;}
    if (IOHIDEventCreateKeyboardEvent)
        wake_control=make_service(client,"Pong touch wake control",HID_PAGE_CONSUMER,HID_USAGE_CONSUMER_CONTROL,false);
    say("Pong touch HID: display-integrated digitizer opened\n");
    if (wake_control) say("Pong touch HID: tap-to-wake consumer opened\n");
    struct pong_touch sample;unsigned count=0,waking=0;ssize_t length;
    while ((length=read(device,&sample,sizeof(sample)))>0 || (length<0 && errno==EINTR)) {
        if (length<0) continue;
        if (length!=(ssize_t)sizeof(sample) || sample.type<PONG_TOUCH_DOWN || sample.type>PONG_TOUCH_UP) continue;
        if (sample.finger>=MAX_FINGERS) continue;
        if (count++<4) say("Pong touch HID: type=%u finger=%u xy=%u,%u\n",sample.type,sample.finger,sample.x,sample.y);
        unsigned mode=(unsigned)__atomic_load_n(&touch_mode,__ATOMIC_RELAXED)&0xff;
        unsigned bit=1u<<sample.finger;
        if (mode==TOUCH_MODE_OFF || mode==TOUCH_MODE_WAKE) {
            release_contacts(screen,sample.time);
            if (mode==TOUCH_MODE_WAKE && sample.type==PONG_TOUCH_DOWN && !waking)
                tap_wake(wake_control,sample.time);
            if (sample.type==PONG_TOUCH_UP) waking&=~bit;else waking|=bit;
            continue;
        }
        if (waking&bit) {if (sample.type==PONG_TOUCH_UP) waking&=~bit;continue;}
        send_touch(screen,&sample);
    }
    /* Release outstanding contacts if the physical source goes away. */
    release_contacts(screen,mach_absolute_time());
    if (wake_control) {IOHIDVirtualServiceClientRemove(wake_control);CFRelease(wake_control);}
    close(device);IOHIDVirtualServiceClientRemove(screen);CFRelease(screen);CFRelease(client);
    return NULL;
}

static void *
buttons_thread(void *unused)
{
    (void)unused;pthread_setname_np("Pong physical keys");
    int fd=-1;
    for (unsigned i=0;i<30 && fd<0;i++) {fd=open("/dev/pong-buttons",O_RDONLY);if (fd<0) sleep(1);}
    if (fd<0) {say("Pong keys HID: verified PMIC input unavailable; SKIP\n");return NULL;}
    IOHIDEventSystemClientRef client=NULL;IOHIDServiceClientRef keys=NULL;
    const int types[]={HID_CLIENT_SIMPLE,HID_CLIENT_RATE_CONTROLLED};
    for (unsigned i=0;i<2 && !keys;i++) {
        client=IOHIDEventSystemClientCreateWithType(NULL,types[i],NULL);
        if (!client) continue;
        IOHIDEventSystemClientScheduleWithDispatchQueue(client,dispatch_queue_create("pong.keys.hid",NULL));
        keys=make_service(client,"Pong physical side and volume keys",HID_PAGE_CONSUMER,HID_USAGE_CONSUMER_CONTROL,false);
        if (!keys) CFRelease(client);
    }
    if (!keys) {say("Pong keys HID: event server unavailable; SKIP\n");close(fd);return NULL;}
    say("Pong keys HID: physical side/volume consumer opened\n");
    struct pong_touch sample;ssize_t n;unsigned held=0;
    const uint32_t usages[]={HID_USAGE_POWER,HID_USAGE_VOLUME_UP,HID_USAGE_VOLUME_DOWN};
    while ((n=read(fd,&sample,sizeof(sample)))>0 || (n<0 && errno==EINTR)) {
        if (n<0) continue;
        if (n!=(ssize_t)sizeof(sample) || sample.finger>=3 ||
            (sample.type!=PONG_BUTTON_DOWN && sample.type!=PONG_BUTTON_UP)) continue;
        bool down=sample.type==PONG_BUTTON_DOWN;
        IOHIDEventRef event=IOHIDEventCreateKeyboardEvent(NULL,sample.time,HID_PAGE_CONSUMER,
            usages[sample.finger],down,0);
        if (!event) continue;
        IOHIDEventSetIntegerValue(event,FIELD_IS_BUILT_IN,1);
        Boolean sent=IOHIDVirtualServiceClientDispatchEvent(keys,event);CFRelease(event);
        if (down) held|=1u<<sample.finger;else held&=~(1u<<sample.finger);
        say("Pong keys HID: key=%u down=%u sent=%u\n",sample.finger,down,sent);
    }
    for (unsigned i=0;i<3;i++) if (held&(1u<<i)) {
        IOHIDEventRef up=IOHIDEventCreateKeyboardEvent(NULL,mach_absolute_time(),HID_PAGE_CONSUMER,usages[i],false,0);
        if (up) {IOHIDVirtualServiceClientDispatchEvent(keys,up);CFRelease(up);}
    }
    IOHIDVirtualServiceClientRemove(keys);CFRelease(keys);CFRelease(client);close(fd);return NULL;
}

void
pong_touch_start(void)
{
#define RESOLVE(name) do { name=(__typeof__(name))dlsym(RTLD_DEFAULT,#name); if (!name) {say("Pong touch HID: unavailable " #name "; SKIP\n");return;} } while (0)
    RESOLVE(IOHIDEventSystemClientCreateWithType);
    RESOLVE(IOHIDEventSystemClientScheduleWithDispatchQueue);
    RESOLVE(IOHIDVirtualServiceClientCreateWithCallbacks);
    RESOLVE(IOHIDVirtualServiceClientRemove);
    RESOLVE(IOHIDVirtualServiceClientDispatchEvent);
    RESOLVE(IOHIDEventCreateDigitizerEvent);
    RESOLVE(IOHIDEventCreateDigitizerFingerEvent);
    /* Physical touches remain available if this optional wake API is absent. */
    IOHIDEventCreateKeyboardEvent=(__typeof__(IOHIDEventCreateKeyboardEvent))dlsym(RTLD_DEFAULT,"IOHIDEventCreateKeyboardEvent");
    RESOLVE(IOHIDEventAppendEvent);
    RESOLVE(IOHIDEventSetIntegerValue);
#undef RESOLVE
    pthread_t thread;
    if (!pthread_create(&thread,NULL,input_thread,NULL)) pthread_detach(thread);
    if (IOHIDEventCreateKeyboardEvent && !pthread_create(&thread,NULL,buttons_thread,NULL)) pthread_detach(thread);
}
