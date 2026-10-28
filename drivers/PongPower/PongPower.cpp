/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <IOKit/IOService.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
extern kmod_info_t g_kernel_kmod_info;
extern "C" {
void pong_rpmh_init(void);
void sheng_adsp_probe(void);
void sheng_audio_probe(void);
void sheng_battery_start(void);
void PongBatteryStaticInit(void);
volatile int32_t native_battery_stats[16];
void pong_power_start_optional(void) {
 pong_rpmh_init();
 unsigned enabled=0;PE_parse_boot_argn("pong_adsp",&enabled,sizeof(enabled));
 if(enabled!=2)return;
 sheng_adsp_probe();
 // DSP channel registration is ready synchronously; remote opens follow
 // only after firmware and PAS prerequisites complete on its worker.
 void *h=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
 if(h){PongBatteryStaticInit();if(!OSMetaClass::postModLoad(h))sheng_battery_start();}
 sheng_audio_probe();
}
}
