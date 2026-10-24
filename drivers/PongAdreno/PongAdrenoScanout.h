/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <IOKit/IOMemoryDescriptor.h>
#include "PongScanoutWire.h"
extern "C" {
void pong_scanout_initialize(uint64_t,size_t,uint32_t,uint32_t,uint32_t);
int pong_scanout_frame(IOMemoryDescriptor *,size_t,uint32_t,uint32_t,uint32_t);
bool pong_scanout_quarantined(void);
IOReturn pong_scanout_connect(task_t);
void pong_scanout_disconnect(task_t);
IOReturn pong_scanout_call(task_t,unsigned,const void *,size_t,void *,size_t);
IOMemoryDescriptor *pong_scanout_memory(task_t);
}
