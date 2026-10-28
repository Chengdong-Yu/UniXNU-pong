/* SPDX-License-Identifier: MIT */
#pragma once
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdlib.h>
#include <string.h>

/* Guest metadata, described by describe-pong-metal-abi.py: _MTLDevice
 * is 528 bytes on 20G81 and 600 on 22H374. No private superclass ivars are written.
 * New methods use the target's own Objective-C dispatch and public Metal ABI.
 */
static bool PongMetalDeviceAbiAvailable(Class base)
{
#if PONG_GUEST_22H374
    constexpr size_t extent=600; // actual 22H374 _MTLDevice metadata
#else
    constexpr size_t extent=528; // actual 20G81 metadata
#endif
    if (!base || class_getInstanceSize(base)!=extent) return false;
    const struct {const char *name,*result,*argument;} expected[]={
        {"initWithAcceleratorPort:","@","I"},
        {"initLimits","v",nullptr},{"initFeatureQueries","v",nullptr},
        {"initGPUFamilySupport","v",nullptr}
    };
    for (const auto &row:expected) {
        Method method=class_getInstanceMethod(base,sel_registerName(row.name));
        if (!method || method_getNumberOfArguments(method)!=(row.argument?3u:2u)) return false;
        char *result=method_copyReturnType(method);
        char *argument=row.argument?method_copyArgumentType(method,2):nullptr;
        bool good=result && !strcmp(result,row.result) &&
            (!row.argument || (argument && !strcmp(argument,row.argument)));
        free(result);free(argument);
        if (!good) return false;
    }
    return true;
}
