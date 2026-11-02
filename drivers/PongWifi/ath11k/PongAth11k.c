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
#include <linux/mhi.h>

int pong_ath11k_pci_init(void);
void pong_ath11k_pci_exit(void);
int mhi_init_hook(void);
int nw_qrtr_mhi_register(void);
void nw_qrtr_mhi_unregister(void);
void lx_irq_raise_all(void);

struct nw_wifi_stats nw_wifi_stats;
static int running;
static struct ieee80211_hw *registered_hw;
static int firmware_error;
static u32 last_wmi[8], wmi_count;
/* Registration is called inside QMI's core bring-up work. Starting the MAC
 * there reenters a half-registered driver through the cooperative waits.
 * Consume this notification only after core bring-up has unwound. */
static void registered(struct ieee80211_hw *hw) { registered_hw = hw; }
static int station_identity(struct ieee80211_hw *hw)
{
    if (is_valid_ether_addr(hw->wiphy->perm_addr)) return 0;
    const u8 *data; size_t bytes;
    if (!lx_plat->fw_get || lx_plat->fw_get("wlan_mac.bin", &data, &bytes)) return -ENODEV;
    size_t start=0;
    for (size_t i=0;i<bytes;i++) if(data[i]=='=') start=i+1;
    u8 address[6]={0};unsigned count=0;
    for(size_t i=start;i<bytes && count<12;i++) {
        u8 c=data[i];int value=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
        if(value<0) {if(count)break;continue;}
        address[count/2]=(address[count/2]<<4)|value;count++;
    }
    if(count!=12 || !is_valid_ether_addr(address)) return -EINVAL;
    struct ath11k *ar=hw->priv;
    memcpy(hw->wiphy->perm_addr,address,6);memcpy(ar->mac_addr,address,6);
    lx_log(6,"Pong: installed board station identity before WMI vdev creation");
    return 0;
}
void pong_ath11k_wmi_submitted(u32 id)
{ last_wmi[wmi_count++ % ARRAY_SIZE(last_wmi)] = id; }
int nw_wifi_firmware_error(void) { return firmware_error; }
void pong_ath11k_mhi_failed(struct mhi_controller *controller, int reason)
{
    if (firmware_error) return;
    firmware_error = -EIO;
    struct ath11k_base *ab = dev_get_drvdata(controller->cntrl_dev);
    set_bit(ATH11K_FLAG_CRASH_FLUSH, &ab->dev_flags);
    nw_wifi_stats.last_err = firmware_error;
    lx_log(3, "Pong QCA6490 firmware stopped: MHI reason=%d, WMI submitted=%u", reason, wmi_count);
    for (u32 i = wmi_count > ARRAY_SIZE(last_wmi) ? wmi_count - ARRAY_SIZE(last_wmi) : 0; i < wmi_count; ++i)
        lx_log(3, "WMI before firmware stop: #%u command=0x%x", i + 1, last_wmi[i % ARRAY_SIZE(last_wmi)]);
}
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
    /* The shared station queues Ethernet MSDUs with HW_80211_ENCAP.
     * Configure WMI/HTT for that same mode before firmware resource setup;
     * Linux's default NATIVE_WIFI clears vif offload while our TX still
     * supplies Ethernet, so the host/firmware contracts would disagree. */
    ath11k_frame_mode = ATH11K_HW_TXRX_ETHERNET;
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
    if (firmware_error) return firmware_error;
    lx_irq_raise_all(); lx_pump();
    if (firmware_error) return firmware_error;
    if (registered_hw && registered_hw == nw_wifi_hw()) {
        struct ath11k *ar = registered_hw->priv;
        if (test_bit(ATH11K_FLAG_REGISTERED, &ar->ab->dev_flags)) {
            struct ieee80211_hw *hw = registered_hw;
            registered_hw = NULL;
            int result=station_identity(hw);
            if(result) {nw_wifi_stats.last_err=result;firmware_error=result;return result;}
            nw_sta_ready(hw);
        }
    }
    if (firmware_error) return firmware_error;
    nw_sta_run();
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
  // Match the shared station scan policy and the Apple80211 channel codec,
  // which currently publishes only 2.4/5 GHz channels on this board.
  if(b==NL80211_BAND_6GHZ)continue;
  struct ieee80211_supported_band *band=hw->wiphy->bands[b];if(!band)continue;
  for (unsigned i=0;i<band->n_channels && n<capacity;i++) {
   const struct ieee80211_channel *c=&band->channels[i];
   if (!(c->flags & IEEE80211_CHAN_DISABLED)) out[n++]=c->center_freq;
  }
 }
 return n;
}
