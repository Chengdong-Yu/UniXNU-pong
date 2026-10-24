/* SPDX-License-Identifier: MIT */
#pragma once
#include <IOKit/IOService.h>
/* Actual 20G81 IOAcceleratorES: 168 IOService slots, no operational
 * overrides, 136-byte instance. Bind only C2, D2 and its real metaclass.
 * Only the original empty parent's metaclass is initialized; Apple's
 * IOSurface/AGX hardware providers are not started. */
class IOAcceleratorES:public IOService {
    OSDeclareAbstractStructors(IOAcceleratorES)
};
static_assert(sizeof(IOService)==136 && sizeof(IOAcceleratorES)==136,"20G81 accelerator base extent");
