/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PONG_ATH11K_COMPAT_H
#define PONG_ATH11K_COMPAT_H
#include "lx_pci.h"
/* Linux permits side effects in wait conditions. ath11k's WMI condition
 * transfers skb ownership on success; the shared shim used to evaluate it
 * again when returning, sending the same skb twice. Check once per round
 * and honor deadlines even while other IRQ/work items remain busy. */
#undef wait_event_timeout
#define wait_event_timeout(w, cond, timeout) ({ \
    unsigned long pong_end = jiffies + (timeout); long pong_result; (void)(w); \
    for (;;) { \
        if (cond) { long pong_left = (long)(pong_end - jiffies); \
            pong_result = pong_left > 0 ? pong_left : 1; break; } \
        if (time_after(jiffies, pong_end) || jiffies == pong_end) { pong_result = 0; break; } \
        if (!lx_pump()) lx_idle(1); \
    } \
    pong_result; })
typedef unsigned long kernel_ulong_t;
#undef IS_ENABLED
#define PONG_DEFINED_1 0,
#define PONG_SECOND(_a, b, ...) b
#define PONG_DEFINED_II(x) PONG_SECOND(x 1, 0)
#define PONG_DEFINED_I(x) PONG_DEFINED_II(PONG_DEFINED_##x)
#define IS_ENABLED(x) PONG_DEFINED_I(x)
#define IS_REACHABLE(x) IS_ENABLED(x)
#define swab32(x) __builtin_bswap32((u32)(x))
#define prefetch(p) __builtin_prefetch(p)
#define from_tasklet(v, t, m) container_of(t, __typeof__(*(v)), m)
#define dma_alloc_noncoherent(d, n, h, dir, f) lx_dma_alloc(d, n, h)
#define dma_free_noncoherent(d, n, p, h, dir) lx_dma_free(d, n, p, h)
static inline u32 *memset32(u32 *dst, u32 value, size_t count)
{ u32 *result = dst; while (count--) *dst++ = value; return result; }
/* No RSS hash has been calculated for a newly received shim skb. Zero means
 * "not calculated", and is not a fabricated flow classification. */
static inline u32 skb_get_hash(struct sk_buff *skb) { (void)skb; return 0; }
static inline struct device_node *of_find_node_by_type(struct device_node *node, const char *type)
{ (void)node; (void)type; return NULL; }
static inline bool of_property_present(const struct device_node *node, const char *name)
{ (void)node; (void)name; return false; }
static inline bool of_machine_is_compatible(const char *name)
{ return name && (!strcmp(name, "qcom,sm8475") || !strcmp(name, "nothing,pong")); }
static inline int device_get_mac_address(struct device *device, u8 *address)
{ (void)device; memset(address, 0, 6); return -ENODEV; }
#define resource_size(r) ((r)->end - (r)->start + 1)
static inline int of_reserved_mem_region_to_resource(struct device_node *node, int i, struct resource *r)
{ (void)node; (void)i; (void)r; return -ENODEV; }
static inline void *devm_ioremap(struct device *device, phys_addr_t pa, size_t bytes)
{ (void)device; return lx_ioremap(pa, bytes); }
#endif
