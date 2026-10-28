/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Linux ath11k PCI/MHI + UniXNU's shared dispatcher/station ABI. */
#include "lx_pci.h"
#include "lx_plat.h"
#include "nw_board.h"
#include "nw_wifi.h"
#include "nw_mac80211.h"
#include "nw_sta.h"
#include "nw_qrtr.h"
#include <net/mac80211.h>
#include "core.h"
#include "debug.h"

int pong_ath11k_pci_init(void);
void pong_ath11k_pci_exit(void);
int mhi_init_hook(void);
int nw_qrtr_mhi_register(void);
void nw_qrtr_mhi_unregister(void);
void lx_irq_raise_all(void);

struct nw_wifi_stats nw_wifi_stats;
static int running;
static void registered(struct ieee80211_hw *hw) { nw_sta_ready(hw); }
static void receive(struct ieee80211_hw *hw, struct sk_buff *skb) { (void)hw; nw_sta_rx(skb); }
static void scanned(struct ieee80211_hw *hw, bool aborted) { (void)hw; nw_sta_scan_done(aborted); }
static const struct nw_wifi_hooks hooks = {
    .registered = registered, .rx = receive, .scan_done = scanned,
    .beacon_loss = nw_sta_link_lost, .eapol_status = nw_sta_eapol_status,
};

int nw_wifi_start(const struct lx_platform *p, const struct nw_board *b)
{
    if (running || !p || !b || b->device_id != 0x1103 || !b->bar || b->bar_len < 0x200000 ||
        !p->now_ms || !p->sleep_ms || !p->poll || !p->log || !p->halt || !p->alloc || !p->free ||
        !p->dma_alloc || !p->dma_free || !p->dma_map || !p->dma_sync || !p->fw_get ||
        !b->cfg_read || !b->cfg_write || !b->msi_target || !b->map_phys) return -EINVAL;
    lx_plat = p; nw_board = b;
    lx_root_init(); nw_qrtr_init(); nw_wifi_set_hooks(&hooks);
    int r = mhi_init_hook();
    if (r) return r;
    r = nw_qrtr_mhi_register();
    if (r) return r;
    nw_wifi_stats.stage = 1;
    r = pong_ath11k_pci_init();
    if (r) {
        nw_wifi_stats.last_err = r;
        nw_qrtr_mhi_unregister();
        lx_log(3, "Pong ath11k PCI/MHI initialization failed: %d", r);
        return r;
    }
    running = 1; nw_wifi_stats.stage = 2;
    return 0;
}
int nw_wifi_step(void)
{
    if (!running) return 0;
    const unsigned frames = nw_wifi_stats.frames;
    lx_irq_raise_all(); lx_pump(); nw_sta_run();
    return frames != nw_wifi_stats.frames || nw_sta_tx_pending();
}
void nw_wifi_stop(void)
{
    if (!running) return;
    running = 0; pong_ath11k_pci_exit(); nw_qrtr_mhi_unregister();
}

/* Optional extended diagnostic counters are not part of the station path.
 * Their absence is recorded rather than reporting firmware statistics that
 * were never requested or received. */
void nw_wifi_htt_stats(int type) { lx_log(7, "ath11k: optional HTT counter type %d not requested", type); }
void nw_wifi_rx_state(char *out, int n) { if (n > 0) snprintf(out, n, "RX frames=%u", nw_wifi_stats.frames); }
void nw_wifi_ring_state(char *out, int n) { if (n > 0) snprintf(out, n, "ath11k ring snapshot unavailable"); }
void nw_wifi_tx_reasons(char *out, int n) { if (n > 0) snprintf(out, n, "ath11k tx reason snapshot unavailable"); }

static void says(int level, const char *format, va_list args)
{
    char line[256]; vsnprintf(line, sizeof line, format, args);
    lx_log(level, "ath11k: %s", line);
}
void ath11k_info(struct ath11k_base *ab, const char *format, ...)
{ (void)ab; va_list a; va_start(a, format); says(6, format, a); va_end(a); }
void ath11k_warn(struct ath11k_base *ab, const char *format, ...)
{ (void)ab; va_list a; va_start(a, format); says(4, format, a); va_end(a); }
void ath11k_err(struct ath11k_base *ab, const char *format, ...)
{ (void)ab; va_list a; va_start(a, format); says(3, format, a); va_end(a); }

/* The framework's supported channels come from firmware/regulatory state. */
unsigned nw_wifi_channel_list(unsigned *out, unsigned capacity)
{
 struct ieee80211_hw *hw=nw_wifi_hw();unsigned n=0;
 if (!hw || !out) return 0;
 for (unsigned b=0;b<NUM_NL80211_BANDS;b++) {
  struct ieee80211_supported_band *band=hw->wiphy->bands[b];if(!band)continue;
  for (unsigned i=0;i<band->n_channels && n<capacity;i++) {
   const struct ieee80211_channel *c=&band->channels[i];
   if (!(c->flags & IEEE80211_CHAN_DISABLED)) out[n++]=c->center_freq;
  }
 }
 return n;
}
