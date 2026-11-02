/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PONG_PLATFORM_RESOURCES_H
#define PONG_PLATFORM_RESOURCES_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void pong_rpmh_init(void);
bool pong_scm_call_lock(void);
void pong_scm_call_unlock(void);
bool pong_rpmh_vote(uint32_t address, uint32_t value);
bool pong_smmu_lease_lock(void);
void pong_smmu_lease_unlock(void);
#ifdef __cplusplus
}
#endif
#endif
