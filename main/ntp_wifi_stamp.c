#include "ntp_internal.h"

#define EARLY_RING_SIZE 8

typedef struct {
    uint32_t      ts_sec, ts_frac;
    int64_t       wall_us;
    bool          valid;
} early_entry_t;

struct early_ring {
    early_entry_t ring[EARLY_RING_SIZE];
    portMUX_TYPE  lock;
    int           head;
};

static early_ring_t s_t1 = { .lock = portMUX_INITIALIZER_UNLOCKED };
static early_ring_t s_t4 = { .lock = portMUX_INITIALIZER_UNLOCKED };
static esp_netif_t *s_sta_netif;

static void stash_early(early_ring_t *r, uint32_t sec, uint32_t frac, int64_t us) {
    portENTER_CRITICAL(&r->lock);
    r->ring[r->head] = (early_entry_t){
        .ts_sec = sec, .ts_frac = frac, .wall_us = us, .valid = true,
    };
    r->head = (r->head + 1) % EARLY_RING_SIZE;
    portEXIT_CRITICAL(&r->lock);
}


bool consume_early(early_ring_t *r, uint32_t sec, uint32_t frac, int64_t *us) {
    bool found = false;
    portENTER_CRITICAL(&r->lock);
    for (int i = 0; i < EARLY_RING_SIZE; i++) {
        if (r->ring[i].valid && r->ring[i].ts_sec == sec && r->ring[i].ts_frac == frac) {
            *us = r->ring[i].wall_us;
            r->ring[i].valid = false;
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&r->lock);
    return found;
}

void shift_early(early_ring_t *r, int64_t delta_us) {
    portENTER_CRITICAL(&r->lock);
    for (int i = 0; i < EARLY_RING_SIZE; i++) {
        if (r->ring[i].valid) r->ring[i].wall_us += delta_us;
    }
    portEXIT_CRITICAL(&r->lock);
}


static inline uint16_t rd_be16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

static bool parse_ip_udp_ntp(const uint8_t *ip, size_t rem, uint16_t et,
                             int port_off, int ntp_field_off,
                             uint32_t *sec, uint32_t *frac) {
    const uint8_t *udp;
    if (et == 0x0800) {
        if (rem < 20u + 8u + 48u) return false;
        uint8_t ihl = (ip[0] & 0x0f) * 4;
        if (ihl < 20u || rem < (size_t)ihl + 8u + 48u || ip[9] != 17) return false;
        udp = ip + ihl;
    } else if (et == 0x86DD) {
        if (rem < 40u + 8u + 48u || ip[6] != 17) return false;
        udp = ip + 40;
    } else {
        return false;
    }
    if (rd_be16(udp + port_off) != NTP_PORT) return false;
    const uint8_t *ntp = udp + 8;
    memcpy(sec,  ntp + ntp_field_off,     4);
    memcpy(frac, ntp + ntp_field_off + 4, 4);
    return true;
}


static esp_err_t ntp_wifi_rxcb(void *buffer, uint16_t len, void *eb) {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    const uint8_t *buf = buffer;
    uint32_t sec, frac;
    if (len >= 14u + 20u + 8u + 48u &&
        parse_ip_udp_ntp(buf + 14, len - 14u, rd_be16(buf + 12),
                         /*src port*/ 0, /*orig_ts*/ 24, &sec, &frac)) {
        stash_early(&s_t4, sec, frac,
                    (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec);
    }
    return esp_netif_receive(s_sta_netif, buffer, len, eb);
}


static void ntp_wifi_tx_done_cb(uint8_t ifidx, uint8_t *data,
                                 uint16_t *data_len, bool txStatus) {
    (void)ifidx;
    if (!txStatus || !data || !data_len || *data_len < 4) return;
    uint16_t len = *data_len;

    struct timeval tv;
    gettimeofday(&tv, NULL);

    uint8_t fc0 = data[0], fc1 = data[1];
    if ((fc0 & 0x0c) != 0x08) return;
    uint8_t subtype = (fc0 & 0xf0) >> 4;
    if (subtype != 0 && subtype != 8) return;
    size_t hdr_len    = 24
                      + (subtype == 8           ? 2 : 0)
                      + ((fc1 & 0x03) == 0x03   ? 6 : 0);
    size_t cipher_hdr = (fc1 & 0x40) ? 8 : 0;
    if ((size_t)len < hdr_len + cipher_hdr + 8u + 20u + 8u + 48u) return;

    static const uint8_t llc_snap[6] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00};
    const uint8_t *p = data + hdr_len + cipher_hdr;
    if (memcmp(p, llc_snap, 6) != 0) return;

    uint32_t sec, frac;
    if (parse_ip_udp_ntp(p + 8, len - hdr_len - cipher_hdr - 8u, rd_be16(p + 6),
                         /*dst port*/ 2, /*xmt_ts*/ 40, &sec, &frac)) {
        stash_early(&s_t1, sec, frac,
                    (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec);
    }
}


void ntp_install_wifi_rx_hook(void) {
    s_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!s_sta_netif) { ESP_LOGW(TAG, "STA netif not found"); return; }
    esp_err_t err = esp_wifi_internal_reg_rxcb(WIFI_IF_STA, ntp_wifi_rxcb);
    if (err != ESP_OK) { ESP_LOGW(TAG, "rxcb reg failed: %d", err); return; }
    err = esp_wifi_set_tx_done_cb(ntp_wifi_tx_done_cb);
    if (err != ESP_OK) ESP_LOGW(TAG, "tx_done_cb reg failed: %d", err);
}

early_ring_t *early_t1_ring(void) { return &s_t1; }
early_ring_t *early_t4_ring(void) { return &s_t4; }
