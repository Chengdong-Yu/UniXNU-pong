/* SPDX-License-Identifier: MIT */
#pragma once
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdlib.h>
#include <string.h>

/* Actual20G81 metadata, described by describe-pong-metal-abi.py: _MTLDevice
 * extent528, not Sheng21H461's536. No private superclass ivars are written.
 * New methods use the target's own Objective-C dispatch and public Metal ABI.
 */
static bool PongMetalDeviceAbiAvailable(Class base)
{
    if (!base || class_getInstanceSize(base)!=528) return false;
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
