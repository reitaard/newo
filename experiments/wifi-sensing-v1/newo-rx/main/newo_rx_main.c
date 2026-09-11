/*
 * Standalone Newo ESP32-S3 CSI receiver.
 *
 * Measurement-plane patterns were adapted after review of RuView's
 * csi_collector.c (MIT, Copyright (c) 2024 rUv). See the experiment's
 * THIRD_PARTY_NOTICES.md for attribution and the deliberately excluded scope.
 */
#include <inttypes.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"
#include "sdkconfig.h"

#include "ncsi_protocol.h"
#include "probe_protocol.h"
#include "rate_gate.h"
#include "collector_discovery.h"
#include "track_protocol.h"
#include "track_control_state.h"
#include "sync_protocol.h"
#include "sync_estimator.h"

#ifndef CONFIG_ESP_WIFI_CSI_ENABLED
#error "CONFIG_ESP_WIFI_CSI_ENABLED must be enabled for the Newo CSI receiver"
#endif

#if CONFIG_NEWO_CSI_RATE_HZ > 50
#error "Newo CSI experiment raw rate must not exceed 50 Hz"
#endif

#if CONFIG_NEWO_GATEWAY_PING_ENABLE && CONFIG_NEWO_GATEWAY_PING_HZ > 50
#error "Newo CSI experiment gateway ping rate must not exceed 50 Hz"
#endif

#if CONFIG_NEWO_ESPNOW_TRANSMIT && CONFIG_NEWO_ESPNOW_PROBE_HZ > 50
#error "Newo ESP-NOW experiment probe rate must not exceed 50 Hz"
#endif

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_MAXIMUM_RETRY 10
#define LTF_LLTF BIT0
#define LTF_HT BIT1
#define LTF_STBC_HT BIT2

#if CONFIG_NEWO_ESPNOW_RECEIVE
#define ESPNOW_ROLE "rx"
#elif CONFIG_NEWO_ESPNOW_TRANSMIT
#define ESPNOW_ROLE "tx"
#else
#define ESPNOW_ROLE "off"
#endif

static const char *TAG = "newo_csi_rx";

typedef struct {
    uint8_t source_mac[6];
    uint8_t destination_mac[6];
    uint32_t sequence;
    uint64_t timestamp_us;
    uint32_t driver_rx_timestamp_us;
    uint8_t channel;
    uint8_t secondary_channel;
    uint8_t bandwidth;
    uint8_t phy_mode;
    int8_t rssi;
    int8_t noise_floor;
    uint8_t antenna;
    uint8_t ltf_mask;
    uint16_t flags;
    uint16_t path_id;
    uint16_t driver_length;
    uint16_t driver_rx_sequence;
    uint16_t rx_flags;
    uint16_t packet_length;
    uint8_t phy_rate;
    uint8_t mcs;
    uint8_t ampdu_count;
    uint8_t rx_state;
    uint8_t sanitized_prefix_bytes;
    uint8_t csi[NCSI_MAX_CSI_BYTES];
} csi_slot_t;

typedef struct {
    uint8_t source_mac[6];
    int8_t rssi;
    uint16_t csi_length;
    uint8_t channel;
} last_sample_t;

static EventGroupHandle_t s_wifi_events;
static int s_wifi_retry_count;
static uint8_t s_receiver_mac[6];
static uint8_t s_ap_bssid[6];
static uint8_t s_filter_mac[6];
static bool s_filter_enabled;
static bool s_peer_enabled;
static uint8_t s_peer_mac[6];
static bool s_csi_enabled;
static bool s_self_ping_enabled;
static volatile bool s_associated;
static volatile bool s_identity_ready;
static volatile uint32_t s_refresh_requested;
static volatile uint32_t s_refresh_handled;
static volatile uint32_t s_association_epoch;
static uint8_t s_ap_primary;
static uint8_t s_ap_secondary;
static uint32_t s_gateway_address;
static uint32_t s_boot_id;
static uint32_t s_sync_session_id;
static int s_udp_socket = -1;
static esp_ping_handle_t s_ping_handle;

static csi_slot_t s_ring[CONFIG_NEWO_QUEUE_DEPTH];
static volatile uint32_t s_ring_write;
static volatile uint32_t s_ring_read;
static newo_rate_gate_t s_rate_gate;

static volatile uint32_t s_callbacks_total;
static volatile uint32_t s_rate_gate_drops;
static volatile uint32_t s_source_filter_drops;
static volatile uint32_t s_accepted_total;
static volatile uint32_t s_ring_full_drops;
static volatile uint32_t s_transport_ok;
static volatile uint32_t s_transport_drops;
static volatile uint32_t s_invalid_frame_drops;
static volatile uint32_t s_next_sequence;
static volatile uint32_t s_status_sequence;
static volatile uint32_t s_diagnostic_sequence;
static volatile uint32_t s_status_transport_ok;
static volatile uint32_t s_status_transport_drops;
static volatile uint32_t s_path_gate_drops[NEWO_PATH_COUNT];
static volatile uint32_t s_probe_tx_attempted;
static volatile uint32_t s_probe_tx_queued;
static volatile uint32_t s_probe_tx_success;
static volatile uint32_t s_probe_tx_link_failure;
static volatile uint32_t s_probe_tx_submit_failure;
static volatile uint32_t s_probe_tx_skipped_busy;
static volatile uint32_t s_probe_tx_skipped_unassociated;
static volatile uint32_t s_probe_rx_valid;
static volatile uint32_t s_probe_rx_invalid;
static volatile bool s_probe_in_flight;
static volatile bool s_track_active;
static TaskHandle_t s_sender_task;
#if CONFIG_NEWO_ESPNOW_TRANSMIT
static newo_track_control_state_t s_track_control_state;
static newo_sync_estimator_t s_sync_estimator;
static portMUX_TYPE s_sync_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_sync_record_sequence;
#define NEWO_SYNC_BEACON_PERIOD_US 500000u
#endif

static portMUX_TYPE s_sample_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_identity_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_gate_lock = portMUX_INITIALIZER_UNLOCKED;
static last_sample_t s_last_sample;

static inline uint32_t counter_get(volatile uint32_t *counter)
{
    return __atomic_load_n(counter, __ATOMIC_RELAXED);
}

static inline uint32_t counter_increment(volatile uint32_t *counter)
{
    return __atomic_fetch_add(counter, 1, __ATOMIC_RELAXED);
}

static inline bool track_active(void)
{
    return __atomic_load_n(&s_track_active, __ATOMIC_ACQUIRE);
}

static inline void set_track_active(bool active)
{
    __atomic_store_n(&s_track_active, active, __ATOMIC_RELEASE);
}

static void format_mac(const uint8_t mac[6], char output[18])
{
    snprintf(output, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

#if CONFIG_NEWO_SOURCE_FILTER_CUSTOM || !CONFIG_NEWO_ESPNOW_NONE
static bool parse_mac(const char *text, uint8_t output[6])
{
    unsigned values[6];
    char trailing;
    if (text == NULL || sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x%c",
                               &values[0], &values[1], &values[2],
                               &values[3], &values[4], &values[5], &trailing) != 6) {
        return false;
    }
    for (size_t i = 0; i < 6; ++i) {
        if (values[i] > UINT8_MAX) {
            return false;
        }
        output[i] = (uint8_t)values[i];
    }
    return true;
}
#endif

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_associated = false;
        newo_collector_discovery_set_associated(false);
        s_identity_ready = false;
        s_self_ping_enabled = false;
        if (s_wifi_retry_count++ < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_retry_count = 0;
        s_associated = true;
        newo_collector_discovery_set_associated(true);
        counter_increment(&s_refresh_requested);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_ssid_configured(void)
{
    /* Volatile read keeps the complete runtime pipeline in credential-free CI
     * binaries instead of letting whole-program optimization fold app_main at
     * the empty-default guard. No credential is logged or persisted here. */
    const volatile char *ssid = CONFIG_NEWO_WIFI_SSID;
    return ssid[0] != '\0';
}

static void initialise_wifi(void)
{
    if (!wifi_ssid_configured()) {
        ESP_LOGE(TAG, "Wi-Fi SSID is empty; configure it with idf.py menuconfig");
        abort();
    }

    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                wifi_event_handler, NULL));

    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, CONFIG_NEWO_WIFI_SSID, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, CONFIG_NEWO_WIFI_PASSWORD,
            sizeof(config.sta.password));
    config.sta.threshold.authmode = strlen(CONFIG_NEWO_WIFI_PASSWORD) == 0
                                        ? WIFI_AUTH_OPEN
                                        : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                            WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                            pdFALSE, pdFALSE, portMAX_DELAY);
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "failed to connect after %d retries", WIFI_MAXIMUM_RETRY);
        abort();
    }
}

static void configure_source_filter(void)
{
#if CONFIG_NEWO_SOURCE_FILTER_AP
    s_filter_enabled = true;
#elif CONFIG_NEWO_SOURCE_FILTER_CUSTOM
    if (!parse_mac(CONFIG_NEWO_SOURCE_MAC, s_filter_mac)) {
        ESP_LOGE(TAG, "invalid custom source MAC; expected aa:bb:cc:dd:ee:ff");
        abort();
    }
    s_filter_enabled = true;
#else
    s_filter_enabled = false;
#endif

#if !CONFIG_NEWO_SOURCE_FILTER_AP
    if (s_filter_enabled) {
        char mac[18];
        format_mac(s_filter_mac, mac);
        ESP_LOGI(TAG, "source filter enabled: %s", mac);
    } else
#endif
    {
#if CONFIG_NEWO_SOURCE_FILTER_AP
        ESP_LOGI(TAG, "source filter follows the associated AP BSSID");
#else
        ESP_LOGW(TAG, "source filter disabled; path identity must be validated by host");
#endif
    }
}

static bool source_is_peer(const uint8_t mac[6])
{
#if CONFIG_NEWO_ESPNOW_RECEIVE
    return s_peer_enabled && memcmp(mac, s_peer_mac, 6) == 0;
#else
    (void)mac;
    return false;
#endif
}

static uint8_t encode_secondary(wifi_second_chan_t secondary)
{
    switch (secondary) {
        case WIFI_SECOND_CHAN_NONE: return 0;
        case WIFI_SECOND_CHAN_ABOVE: return 1;
        case WIFI_SECOND_CHAN_BELOW: return 2;
        default: return UINT8_MAX;
    }
}

static uint8_t encode_phy(uint8_t sig_mode)
{
    if (sig_mode == 0) return 0;
    if (sig_mode == 1) return 1;
    return UINT8_MAX;
}

static uint8_t infer_ltf_mask(uint8_t sig_mode, bool stbc)
{
    uint8_t mask = LTF_LLTF;
    if (sig_mode == 1) mask |= LTF_HT;
    if (sig_mode == 1 && stbc) mask |= LTF_STBC_HT;
    return mask;
}

static uint16_t encode_rx_flags(const wifi_pkt_rx_ctrl_t *rx)
{
    uint16_t flags = 0;
#if !CONFIG_PM_ENABLE
    flags |= NCSI_RX_FLAG_TIMESTAMP_PRECISE;
#endif
    if (rx->sig_mode == 0) flags |= NCSI_RX_FLAG_RATE_VALID;
    if (rx->sig_mode == 1) flags |= NCSI_RX_FLAG_MCS_VALID;
    if (rx->sgi) flags |= NCSI_RX_FLAG_SHORT_GI;
    if (rx->aggregation) flags |= NCSI_RX_FLAG_AGGREGATED;
    if (rx->fec_coding) flags |= NCSI_RX_FLAG_LDPC;
    if (rx->smoothing) flags |= NCSI_RX_FLAG_SMOOTHING;
    if (rx->not_sounding) flags |= NCSI_RX_FLAG_NOT_SOUNDING;
    return flags;
}

static void csi_callback(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;
    /* TRACK_OFF must not accumulate callbacks or enqueue stale measurements. */
    if (!track_active()) return;
    counter_increment(&s_callbacks_total);
    if (info == NULL || info->buf == NULL || info->len == 0 ||
        info->len > NCSI_MAX_CSI_BYTES || (info->len & 1u) != 0) {
        counter_increment(&s_invalid_frame_drops);
        return;
    }

    if (!s_identity_ready) {
        counter_increment(&s_source_filter_drops);
        return;
    }

    bool filter_enabled;
    uint8_t filter_mac[6];
    portENTER_CRITICAL(&s_identity_lock);
    filter_enabled = s_filter_enabled;
    memcpy(filter_mac, s_filter_mac, sizeof(filter_mac));
    portEXIT_CRITICAL(&s_identity_lock);

    bool peer_source = source_is_peer(info->mac);
    bool filter_matched = peer_source;
    if (!peer_source && filter_enabled) {
        if (memcmp(info->mac, filter_mac, 6) != 0) {
            counter_increment(&s_source_filter_drops);
            return;
        }
        filter_matched = true;
    }

    int64_t now_us = esp_timer_get_time();
    uint16_t path_id = peer_source ? 3u : CONFIG_NEWO_PATH_ID;

    /*
     * Hardware validation: retain every qualifying Router->Newo CSI callback.
     * Peer/ESP-NOW paths keep the configured rate gate.
     */
    bool gate_accepted = true;
    if (peer_source) {
        const int64_t interval_us = 1000000LL / CONFIG_NEWO_CSI_RATE_HZ;
        portENTER_CRITICAL(&s_gate_lock);
        gate_accepted = newo_rate_gate_accept(&s_rate_gate, path_id, now_us, interval_us);
        portEXIT_CRITICAL(&s_gate_lock);
    }

    if (!gate_accepted) {
        counter_increment(&s_rate_gate_drops);
        if (path_id >= 1 && path_id <= NEWO_PATH_COUNT) {
            counter_increment(&s_path_gate_drops[path_id - 1u]);
        }
        return;
    }

    uint32_t sequence = counter_increment(&s_next_sequence);
    counter_increment(&s_accepted_total);

    uint32_t write = __atomic_load_n(&s_ring_write, __ATOMIC_RELAXED);
    uint32_t next = (write + 1u) % CONFIG_NEWO_QUEUE_DEPTH;
    if (next == __atomic_load_n(&s_ring_read, __ATOMIC_ACQUIRE)) {
        counter_increment(&s_ring_full_drops);
        return;
    }

    csi_slot_t *slot = &s_ring[write];
    memcpy(slot->source_mac, info->mac, 6);
    memcpy(slot->destination_mac, info->dmac, 6);
    slot->sequence = sequence;
    slot->timestamp_us = (uint64_t)now_us;
    slot->driver_rx_timestamp_us = info->rx_ctrl.timestamp;
    slot->channel = info->rx_ctrl.channel;
    slot->secondary_channel = encode_secondary(info->rx_ctrl.secondary_channel);
    slot->bandwidth = info->rx_ctrl.cwb ? 1 : 0;
    slot->phy_mode = encode_phy(info->rx_ctrl.sig_mode);
    slot->rssi = (int8_t)info->rx_ctrl.rssi;
    slot->noise_floor = (int8_t)info->rx_ctrl.noise_floor;
    slot->antenna = (uint8_t)info->rx_ctrl.ant;
    slot->ltf_mask = infer_ltf_mask(info->rx_ctrl.sig_mode, info->rx_ctrl.stbc != 0);
    slot->flags = 0;
    slot->flags |= NCSI_FLAG_RX_METADATA_VALID;
    if (filter_matched) slot->flags |= NCSI_FLAG_SOURCE_FILTER_MATCHED;
    if (info->rx_ctrl.stbc) slot->flags |= NCSI_FLAG_STBC;
    if (s_self_ping_enabled) slot->flags |= NCSI_FLAG_CONTROL_TRAFFIC_ACTIVE;
    if (slot->sequence == 0) slot->flags |= NCSI_FLAG_SEQUENCE_RESET;
    slot->driver_length = info->len;
    slot->driver_rx_sequence = info->rx_seq;
    slot->rx_flags = encode_rx_flags(&info->rx_ctrl);
    slot->packet_length = info->rx_ctrl.sig_len;
    slot->phy_rate = info->rx_ctrl.rate;
    slot->mcs = info->rx_ctrl.mcs;
    slot->ampdu_count = info->rx_ctrl.ampdu_cnt;
    slot->rx_state = info->rx_ctrl.rx_state;
    slot->path_id = path_id;
    slot->sanitized_prefix_bytes = 0;
    memcpy(slot->csi, info->buf, info->len);
    if (info->first_word_invalid) {
        slot->sanitized_prefix_bytes = ncsi_sanitize_invalid_prefix(
            slot->csi, info->len, true);
        slot->flags |= NCSI_FLAG_FIRST_WORD_INVALID_REPORTED |
                       NCSI_FLAG_INVALID_PREFIX_SANITIZED;
    }

    portENTER_CRITICAL(&s_sample_lock);
    memcpy(s_last_sample.source_mac, slot->source_mac, 6);
    s_last_sample.rssi = slot->rssi;
    s_last_sample.csi_length = slot->driver_length;
    s_last_sample.channel = slot->channel;
    portEXIT_CRITICAL(&s_sample_lock);

    __atomic_store_n(&s_ring_write, next, __ATOMIC_RELEASE);
}

static void initialise_udp(void)
{
    s_udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_udp_socket < 0) {
        ESP_LOGE(TAG, "failed to create UDP socket: errno=%d", errno);
        abort();
    }
    newo_collector_discovery_start(CONFIG_NEWO_COLLECTOR_IPV4,
                                   CONFIG_NEWO_COLLECTOR_PORT);
}

static bool udp_send_record(const uint8_t *record, size_t length)
{
    struct sockaddr_in collector;
    newo_collector_snapshot(&collector, NULL);
    int sent = sendto(s_udp_socket, record, length, 0,
                      (struct sockaddr *)&collector, sizeof(collector));
    return sent == (int)length;
}

static uint16_t current_status_flags(void)
{
    uint16_t flags = 0;
    if (s_associated) flags |= NCSI_STATUS_ASSOCIATED;
    if (s_csi_enabled) flags |= NCSI_STATUS_CSI_ENABLED;
    if (s_filter_enabled) flags |= NCSI_STATUS_FILTER_ENABLED;
    if (s_self_ping_enabled) flags |= NCSI_STATUS_SELF_PING_ENABLED;
    if (counter_get(&s_ring_full_drops) == 0) flags |= NCSI_STATUS_RING_HEALTHY;
    return flags;
}

static void send_status_and_log(uint32_t *previous_callbacks, uint32_t *previous_accepted)
{
    uint32_t accepted_total = counter_get(&s_accepted_total);
    uint32_t next_sequence = counter_get(&s_next_sequence);
    ncsi_status_record_t status = {
        .node_id = CONFIG_NEWO_NODE_ID,
        .status_flags = current_status_flags(),
        .boot_id = s_boot_id,
        .status_sequence = counter_increment(&s_status_sequence),
        .timestamp_us = (uint64_t)esp_timer_get_time(),
        .callbacks_total = counter_get(&s_callbacks_total),
        .rate_gate_drops = counter_get(&s_rate_gate_drops),
        .source_filter_drops = counter_get(&s_source_filter_drops),
        .accepted_total = accepted_total,
        .ring_full_drops = counter_get(&s_ring_full_drops),
        .transport_ok = counter_get(&s_transport_ok),
        .transport_drops = counter_get(&s_transport_drops),
        .last_csi_sequence = accepted_total == 0 ? 0 : next_sequence - 1u,
        .raw_target_hz = CONFIG_NEWO_CSI_RATE_HZ,
        .dsp_target_hz = 0,
    };
    portENTER_CRITICAL(&s_identity_lock);
    memcpy(status.receiver_mac, s_receiver_mac, 6);
    portEXIT_CRITICAL(&s_identity_lock);

    uint8_t wire[NCSI_STATUS_RECORD_SIZE];
    size_t length = ncsi_serialize_status(&status, wire, sizeof(wire));
    if (length == 0 || !udp_send_record(wire, length)) {
        counter_increment(&s_status_transport_drops);
    } else {
        counter_increment(&s_status_transport_ok);
    }

    ncsi_diagnostic_record_t diagnostic = {
        .node_id = CONFIG_NEWO_NODE_ID,
        .diagnostic_flags = status.status_flags,
        .boot_id = s_boot_id,
        .diagnostic_sequence = counter_increment(&s_diagnostic_sequence),
        .timestamp_us = (uint64_t)esp_timer_get_time(),
        .status_transport_ok = counter_get(&s_status_transport_ok),
        .status_transport_drops = counter_get(&s_status_transport_drops),
        .probe_tx_attempted = counter_get(&s_probe_tx_attempted),
        .probe_tx_queued = counter_get(&s_probe_tx_queued),
        .probe_tx_success = counter_get(&s_probe_tx_success),
        .probe_tx_link_failure = counter_get(&s_probe_tx_link_failure),
        .probe_tx_submit_failure = counter_get(&s_probe_tx_submit_failure),
        .probe_tx_skipped_busy = counter_get(&s_probe_tx_skipped_busy),
        .probe_tx_skipped_unassociated = counter_get(&s_probe_tx_skipped_unassociated),
        .probe_rx_valid = counter_get(&s_probe_rx_valid),
        .probe_rx_invalid = counter_get(&s_probe_rx_invalid),
        .association_epoch = counter_get(&s_association_epoch),
    };
    memcpy(diagnostic.receiver_mac, status.receiver_mac, 6);
    for (size_t i = 0; i < NEWO_PATH_COUNT; ++i) {
        diagnostic.path_gate_drops[i] = counter_get(&s_path_gate_drops[i]);
    }
    uint8_t diagnostic_wire[NCSI_DIAGNOSTIC_RECORD_SIZE];
    size_t diagnostic_length = ncsi_serialize_diagnostic(
        &diagnostic, diagnostic_wire, sizeof(diagnostic_wire));
    if (diagnostic_length == 0 || !udp_send_record(diagnostic_wire, diagnostic_length)) {
        ESP_LOGW(TAG, "diagnostic UDP record not delivered sequence=%" PRIu32,
                 diagnostic.diagnostic_sequence);
    }

#if CONFIG_NEWO_ESPNOW_TRANSMIT
    newo_sync_estimator_t sync;
    portENTER_CRITICAL(&s_sync_lock);
    sync = s_sync_estimator;
    portEXIT_CRITICAL(&s_sync_lock);
    uint64_t sync_now = (uint64_t)esp_timer_get_time();
    ncsi_sync_record_t sync_record = {
        .node_id = CONFIG_NEWO_NODE_ID, .sync_version = 1,
        .sync_state = newo_sync_estimator_quality(&sync, sync_now, NEWO_SYNC_BEACON_PERIOD_US),
        .boot_id = s_boot_id, .sync_sequence = counter_increment(&s_sync_record_sequence),
        .local_timestamp_us = sync.last_local_us, .leader_timestamp_us = sync.last_leader_us,
        .raw_offset_us = sync.raw_offset_us, .smoothed_offset_us = sync.smoothed_offset_us,
        .drift_milli_ppm = sync.drift_milli_ppm, .accepted_samples = sync.accepted,
        .rejected_samples = sync.rejected,
        .last_sync_age_us = newo_sync_estimator_age(&sync, sync_now),
        .leader_node_id = sync.leader_node_id, .leader_session_id = sync.leader_session_id,
        .follower_session_id = sync.follower_session_id,
        .beacon_sequence = sync.last_sequence, .jitter_us = newo_sync_estimator_jitter(&sync),
        .transport_rx = sync.transport_rx, .transport_drops = sync.transport_drops,
    };
    memcpy(sync_record.receiver_mac, status.receiver_mac, 6);
    uint8_t sync_wire[NCSI_SYNC_RECORD_SIZE];
    size_t sync_length = ncsi_serialize_sync(&sync_record, sync_wire, sizeof(sync_wire));
    if (!sync_length || !udp_send_record(sync_wire, sync_length)) {
        ESP_LOGW(TAG, "sync UDP record not delivered sequence=%" PRIu32,
                 sync_record.sync_sequence);
    }
#endif

    last_sample_t sample;
    portENTER_CRITICAL(&s_sample_lock);
    sample = s_last_sample;
    portEXIT_CRITICAL(&s_sample_lock);
    char source[18];
    format_mac(sample.source_mac, source);
    uint32_t callback_rate = status.callbacks_total - *previous_callbacks;
    uint32_t accepted_rate = status.accepted_total - *previous_accepted;
    *previous_callbacks = status.callbacks_total;
    *previous_accepted = status.accepted_total;
    ESP_LOGI(TAG,
             "diag cb=%" PRIu32 "/s accepted=%" PRIu32 "/s udp_ok=%" PRIu32
             " udp_fail=%" PRIu32 " queue_drop=%" PRIu32 " invalid=%" PRIu32
             " rssi=%d len=%u ch=%u src=%s sta=%s espnow=%s"
             " status_udp=%" PRIu32 "/%" PRIu32
             " gate_path=%" PRIu32 "/%" PRIu32 "/%" PRIu32
             " epoch=%" PRIu32
             " probe_tx=try:%" PRIu32 " queued:%" PRIu32 " ok:%" PRIu32
             " link_fail:%" PRIu32 " submit_fail:%" PRIu32
             " busy:%" PRIu32 " unassoc:%" PRIu32
             " probe_rx=valid:%" PRIu32 " invalid:%" PRIu32,
             callback_rate, accepted_rate, counter_get(&s_transport_ok),
             counter_get(&s_transport_drops), counter_get(&s_ring_full_drops),
             counter_get(&s_invalid_frame_drops), sample.rssi,
             (unsigned)sample.csi_length, (unsigned)sample.channel, source,
             s_associated ? "up" : "down", ESPNOW_ROLE,
             counter_get(&s_status_transport_ok), counter_get(&s_status_transport_drops),
             counter_get(&s_path_gate_drops[0]), counter_get(&s_path_gate_drops[1]),
             counter_get(&s_path_gate_drops[2]), counter_get(&s_association_epoch),
             counter_get(&s_probe_tx_attempted), counter_get(&s_probe_tx_queued),
             counter_get(&s_probe_tx_success), counter_get(&s_probe_tx_link_failure),
             counter_get(&s_probe_tx_submit_failure), counter_get(&s_probe_tx_skipped_busy),
             counter_get(&s_probe_tx_skipped_unassociated), counter_get(&s_probe_rx_valid),
             counter_get(&s_probe_rx_invalid));
}

static void sender_task(void *arg)
{
    (void)arg;
    uint8_t wire[NCSI_MAX_RECORD_SIZE];
    int64_t next_status_us = esp_timer_get_time() + 1000000LL;
    uint32_t previous_callbacks = 0;
    uint32_t previous_accepted = 0;

    while (true) {
        if (!track_active()) {
            /* No measurement/status UDP while OFF. A control transition wakes us. */
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            next_status_us = esp_timer_get_time() + 1000000LL;
            previous_callbacks = counter_get(&s_callbacks_total);
            previous_accepted = counter_get(&s_accepted_total);
            continue;
        }
        uint32_t read = __atomic_load_n(&s_ring_read, __ATOMIC_RELAXED);
        uint32_t write = __atomic_load_n(&s_ring_write, __ATOMIC_ACQUIRE);
        if (read != write) {
            const csi_slot_t *slot = &s_ring[read];
            ncsi_csi_record_t record = {
                .node_id = CONFIG_NEWO_NODE_ID,
                .sequence = slot->sequence,
                .timestamp_us = slot->timestamp_us,
                .channel = slot->channel,
                .secondary_channel = slot->secondary_channel,
                .bandwidth = slot->bandwidth,
                .phy_mode = slot->phy_mode,
                .rssi_dbm = slot->rssi,
                .noise_floor_dbm = slot->noise_floor,
                .antenna = slot->antenna,
                .ltf_mask = slot->ltf_mask,
                .driver_csi_length = slot->driver_length,
                .csi_flags = slot->flags,
                .path_id = slot->path_id,
                .sanitized_prefix_bytes = slot->sanitized_prefix_bytes,
                .driver_rx_timestamp_us = slot->driver_rx_timestamp_us,
                .phy_rate = slot->phy_rate,
                .mcs = slot->mcs,
                .rx_flags = slot->rx_flags,
                .ampdu_count = slot->ampdu_count,
                .rx_state = slot->rx_state,
                .packet_length = slot->packet_length,
                .driver_rx_sequence = slot->driver_rx_sequence,
                .csi = slot->csi,
                .csi_length = slot->driver_length,
            };
            portENTER_CRITICAL(&s_identity_lock);
            memcpy(record.receiver_mac, s_receiver_mac, 6);
            portEXIT_CRITICAL(&s_identity_lock);
            memcpy(record.source_mac, slot->source_mac, 6);
            memcpy(record.destination_mac, slot->destination_mac, 6);
            size_t length = ncsi_serialize_csi(&record, wire, sizeof(wire));
            if (length != 0 && udp_send_record(wire, length)) {
                counter_increment(&s_transport_ok);
            } else {
                counter_increment(&s_transport_drops);
            }
            __atomic_store_n(&s_ring_read,
                             (read + 1u) % CONFIG_NEWO_QUEUE_DEPTH,
                             __ATOMIC_RELEASE);
        } else {
            /* pdMS_TO_TICKS(2) is zero at CONFIG_FREERTOS_HZ=100. */
            vTaskDelay(1);
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us >= next_status_us) {
            send_status_and_log(&previous_callbacks, &previous_accepted);
            next_status_us += ((now_us - next_status_us) / 1000000LL + 1) * 1000000LL;
        }
    }
}

static void stop_gateway_ping(void);
static void start_gateway_ping(const esp_ip4_addr_t *gateway_address);

static void discard_pending_measurements(void)
{
    uint32_t write = __atomic_load_n(&s_ring_write, __ATOMIC_ACQUIRE);
    __atomic_store_n(&s_ring_read, write, __ATOMIC_RELEASE);
}

static esp_err_t apply_measurement_state(bool active)
{
    if (active == track_active()) return ESP_OK;

    if (active) {
        esp_err_t error = esp_wifi_set_ps(WIFI_PS_NONE);
        if (error != ESP_OK) return error;
        error = esp_wifi_set_promiscuous(true);
        if (error != ESP_OK) {
            (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
            return error;
        }
        error = esp_wifi_set_csi(true);
        if (error != ESP_OK) {
            (void)esp_wifi_set_promiscuous(false);
            (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
            return error;
        }
        s_csi_enabled = true;
        set_track_active(true);
        esp_ip4_addr_t gateway = {.addr = s_gateway_address};
        start_gateway_ping(&gateway);
    } else {
        /* Close the callback gate before disabling the driver resource. */
        set_track_active(false);
        esp_err_t error = esp_wifi_set_csi(false);
        if (error != ESP_OK) {
            set_track_active(true);
            return error;
        }
        s_csi_enabled = false;
        stop_gateway_ping();
        discard_pending_measurements();
        (void)esp_wifi_set_promiscuous(false);
        (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }

    if (s_sender_task != NULL) xTaskNotifyGive(s_sender_task);
    return ESP_OK;
}

static void espnow_receive_callback(const esp_now_recv_info_t *info,
                                    const uint8_t *data, int length)
{
#if CONFIG_NEWO_ESPNOW_TRANSMIT
    if (info != NULL && data != NULL && s_peer_enabled &&
        memcmp(info->src_addr, s_peer_mac, 6) == 0 && length >= 4 &&
        memcmp(data, "NSYN", 4) == 0) {
        newo_sync_beacon_t beacon;
        uint64_t received_us = (uint64_t)esp_timer_get_time();
        portENTER_CRITICAL(&s_sync_lock);
        if (newo_sync_beacon_decode(data, (size_t)length, &beacon)) {
            (void)newo_sync_estimator_accept(&s_sync_estimator, &beacon, received_us);
        } else {
            s_sync_estimator.transport_rx++;
            s_sync_estimator.transport_drops++;
            s_sync_estimator.rejected++;
        }
        portEXIT_CRITICAL(&s_sync_lock);
        return;
    }
    newo_track_control_t control;
    if (info != NULL && s_peer_enabled && memcmp(info->src_addr, s_peer_mac, 6) == 0 &&
        newo_track_control_decode(data, (size_t)length, &control) && !control.ack) {
        const newo_track_control_state_t previous_state = s_track_control_state;
        newo_track_apply_result_t result = newo_track_control_apply(&s_track_control_state, &control);
        if (result == NEWO_TRACK_STALE || result == NEWO_TRACK_RETIRED_SESSION) return;
        bool applied = true;
        if (result == NEWO_TRACK_APPLIED) {
            if (apply_measurement_state(control.active) != ESP_OK) {
                s_track_control_state = previous_state;
                control.active = track_active();
                applied = false;
            }
        } else {
            control.active = track_active();
        }
        control.ack = true;
        control.applied = applied;
        uint8_t response[NEWO_TRACK_CONTROL_SIZE];
        size_t response_length = newo_track_control_encode(&control, response, sizeof(response));
        if (response_length) (void)esp_now_send(s_peer_mac, response, response_length);
        ESP_LOGI(TAG, "TRACK_%s sequence=%" PRIu32, control.active ? "ACTIVE" : "OFF", control.sequence);
        return;
    }
#endif
#if CONFIG_NEWO_ESPNOW_RECEIVE
    if (info == NULL || data == NULL || length != NEWO_PROBE_SIZE ||
        !source_is_peer(info->src_addr)) {
        counter_increment(&s_probe_rx_invalid);
        return;
    }
    newo_probe_t probe;
    if (newo_probe_decode(data, (size_t)length, &probe)) {
        counter_increment(&s_probe_rx_valid);
    } else {
        counter_increment(&s_probe_rx_invalid);
    }
#endif
}

#if CONFIG_NEWO_ESPNOW_TRANSMIT
static void espnow_send_callback(const wifi_tx_info_t *info,
                                 esp_now_send_status_t status)
{
    (void)info;
    counter_increment(status == ESP_NOW_SEND_SUCCESS ? &s_probe_tx_success
                                                      : &s_probe_tx_link_failure);
    __atomic_store_n(&s_probe_in_flight, false, __ATOMIC_RELEASE);
}

static void probe_sender_task(void *arg)
{
    (void)arg;
    TickType_t next = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000u / CONFIG_NEWO_ESPNOW_PROBE_HZ);
    uint32_t sequence = 0;
    while (true) {
        if (!track_active()) {
            /* Reset cadence so a long OFF interval cannot burst queued probes. */
            vTaskDelay(pdMS_TO_TICKS(100));
            next = xTaskGetTickCount();
            continue;
        }
        if (!s_associated) {
            counter_increment(&s_probe_tx_skipped_unassociated);
            vTaskDelayUntil(&next, period);
            continue;
        }
        if (__atomic_exchange_n(&s_probe_in_flight, true, __ATOMIC_ACQ_REL)) {
            counter_increment(&s_probe_tx_skipped_busy);
            vTaskDelayUntil(&next, period);
            continue;
        }
        newo_probe_t probe = {
            .sender_node_id = CONFIG_NEWO_NODE_ID,
            .sequence = sequence++,
            .sender_timestamp_us = (uint64_t)esp_timer_get_time(),
        };
        uint8_t wire[NEWO_PROBE_SIZE];
        size_t length = newo_probe_encode(&probe, wire, sizeof(wire));
        if (length == 0) {
            counter_increment(&s_probe_tx_submit_failure);
            __atomic_store_n(&s_probe_in_flight, false, __ATOMIC_RELEASE);
        } else {
            counter_increment(&s_probe_tx_attempted);
            if (esp_now_send(s_peer_mac, wire, length) != ESP_OK) {
                counter_increment(&s_probe_tx_submit_failure);
                __atomic_store_n(&s_probe_in_flight, false, __ATOMIC_RELEASE);
            } else {
                counter_increment(&s_probe_tx_queued);
            }
        }
        vTaskDelayUntil(&next, period);
    }
}
#endif

static void initialise_espnow(void)
{
#if !CONFIG_NEWO_ESPNOW_NONE
    if (!parse_mac(CONFIG_NEWO_ESPNOW_PEER_MAC, s_peer_mac)) {
        ESP_LOGE(TAG, "invalid ESP-NOW peer MAC; configure station MAC as aa:bb:cc:dd:ee:ff");
        abort();
    }
    s_peer_enabled = true;
    ESP_ERROR_CHECK(esp_now_init());

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_peer_mac, sizeof(peer.peer_addr));
    peer.ifidx = WIFI_IF_STA;
    peer.channel = 0; /* Follow the station interface's associated AP channel. */
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_receive_callback));
#if CONFIG_NEWO_ESPNOW_RECEIVE
    ESP_LOGI(TAG, "ESP-NOW receive enabled on STA current-channel peer semantics");
#elif CONFIG_NEWO_ESPNOW_TRANSMIT
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_callback));
    BaseType_t created = xTaskCreatePinnedToCore(probe_sender_task, "newo_probe", 3072,
                                                 NULL, 4, NULL, 1);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "failed to create ESP-NOW probe task");
        abort();
    }
    ESP_LOGI(TAG, "ESP-NOW transmit enabled rate=%dHz channel=0 STA peer",
             CONFIG_NEWO_ESPNOW_PROBE_HZ);
#endif
#endif
}

static void stop_gateway_ping(void)
{
#if CONFIG_NEWO_GATEWAY_PING_ENABLE
    if (s_ping_handle != NULL) {
        (void)esp_ping_stop(s_ping_handle);
        (void)esp_ping_delete_session(s_ping_handle);
        s_ping_handle = NULL;
    }
#endif
    s_self_ping_enabled = false;
}

static void start_gateway_ping(const esp_ip4_addr_t *gateway_address)
{
#if CONFIG_NEWO_GATEWAY_PING_ENABLE
    stop_gateway_ping();
    if (gateway_address == NULL || gateway_address->addr == 0) {
        ESP_LOGW(TAG, "gateway unavailable; controlled self-ping disabled");
        return;
    }

    char gateway[16];
    esp_ip4addr_ntoa(gateway_address, gateway, sizeof(gateway));
    ip_addr_t target = {0};
    if (!ipaddr_aton(gateway, &target)) {
        ESP_LOGW(TAG, "could not parse gateway address; self-ping disabled");
        return;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr = target;
    config.count = ESP_PING_COUNT_INFINITE;
    config.interval_ms = 1000u / CONFIG_NEWO_GATEWAY_PING_HZ;
    config.data_size = 1;
    config.task_stack_size = 3072;
    esp_ping_callbacks_t callbacks = {0};
    esp_err_t error = esp_ping_new_session(&config, &callbacks, &s_ping_handle);
    if (error == ESP_OK) error = esp_ping_start(s_ping_handle);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "gateway self-ping failed: %s", esp_err_to_name(error));
        if (s_ping_handle != NULL) {
            (void)esp_ping_delete_session(s_ping_handle);
        }
        s_ping_handle = NULL;
        return;
    }
    s_self_ping_enabled = true;
    ESP_LOGI(TAG, "gateway self-ping target=%s rate=%dHz payload=1 byte",
             gateway, CONFIG_NEWO_GATEWAY_PING_HZ);
#endif
}

static bool refresh_association_state(void)
{
    wifi_ap_record_t ap = {0};
    uint8_t receiver_mac[6];
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    s_identity_ready = false;
    stop_gateway_ping();
    if (!s_associated || sta == NULL || esp_wifi_sta_get_ap_info(&ap) != ESP_OK ||
        esp_wifi_get_mac(WIFI_IF_STA, receiver_mac) != ESP_OK ||
        esp_netif_get_ip_info(sta, &ip_info) != ESP_OK || ip_info.gw.addr == 0) {
        ESP_LOGW(TAG, "association refresh deferred: AP identity or gateway unavailable");
        return false;
    }

    bool changed = counter_get(&s_association_epoch) == 0 ||
                   memcmp(s_ap_bssid, ap.bssid, 6) != 0 ||
                   s_ap_primary != ap.primary ||
                   s_ap_secondary != encode_secondary(ap.second) ||
                   s_gateway_address != ip_info.gw.addr;

    portENTER_CRITICAL(&s_identity_lock);
    memcpy(s_ap_bssid, ap.bssid, 6);
    memcpy(s_receiver_mac, receiver_mac, 6);
#if CONFIG_NEWO_SOURCE_FILTER_AP
    memcpy(s_filter_mac, ap.bssid, 6);
#endif
    s_ap_primary = ap.primary;
    s_ap_secondary = encode_secondary(ap.second);
    s_gateway_address = ip_info.gw.addr;
    portEXIT_CRITICAL(&s_identity_lock);

    portENTER_CRITICAL(&s_gate_lock);
    memset(&s_rate_gate, 0, sizeof(s_rate_gate));
    portEXIT_CRITICAL(&s_gate_lock);

    if (track_active()) start_gateway_ping(&ip_info.gw);
    uint32_t epoch = counter_increment(&s_association_epoch) + 1u;
    s_identity_ready = true;

    char ap_mac[18];
    char station_mac[18];
    char gateway[16];
    format_mac(ap.bssid, ap_mac);
    format_mac(receiver_mac, station_mac);
    esp_ip4addr_ntoa(&ip_info.gw, gateway, sizeof(gateway));
    ESP_LOGI(TAG,
             "association epoch=%" PRIu32 " changed=%s station=%s bssid=%s"
             " primary=%u secondary=%u gateway=%s filter_refreshed=%s",
             epoch, changed ? "yes" : "no", station_mac, ap_mac,
             (unsigned)ap.primary, (unsigned)encode_secondary(ap.second), gateway,
#if CONFIG_NEWO_SOURCE_FILTER_AP
             "yes"
#else
             "not-ap-mode"
#endif
    );
    return true;
}

static void association_refresh_task(void *arg)
{
    (void)arg;
    int64_t next_off_health_us = esp_timer_get_time() + 5000000LL;
    while (true) {
        uint32_t requested = counter_get(&s_refresh_requested);
        if (!s_associated) {
            stop_gateway_ping();
        } else if (requested != counter_get(&s_refresh_handled) &&
                   refresh_association_state()) {
            __atomic_store_n(&s_refresh_handled, requested, __ATOMIC_RELAXED);
        }
        int64_t now_us = esp_timer_get_time();
        if (!track_active() && now_us >= next_off_health_us) {
            ESP_LOGI(TAG,
                     "TRACK_OFF health csi=%s ping=%s callbacks=%" PRIu32
                     " accepted=%" PRIu32 " probe_tx=%" PRIu32
                     " udp=%" PRIu32 "/%" PRIu32,
                     s_csi_enabled ? "on" : "off", s_self_ping_enabled ? "on" : "off",
                     counter_get(&s_callbacks_total), counter_get(&s_accepted_total),
                     counter_get(&s_probe_tx_attempted), counter_get(&s_transport_ok),
                     counter_get(&s_transport_drops));
            next_off_health_us = now_us + 5000000LL;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void configure_csi(void)
{
    wifi_csi_config_t config = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = true,
        .ltf_merge_en = true,
        .channel_filter_en = false,
        .manu_scale = false,
        .shift = 0,
    };
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&config));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(csi_callback, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi(false));
    s_csi_enabled = false;
    ESP_LOGI(TAG, "CSI configured: LLTF+HT-LTF+STBC-HT-LTF2 raw=%dHz max=50Hz",
             CONFIG_NEWO_CSI_RATE_HZ);
}

void app_main(void)
{
    /*
     * Hardware-validation build: deliberately do not initialize or erase
     * persistent NVS. Wi-Fi/PHY persistence is disabled in sdkconfig so this
     * temporary CSI firmware leaves production Newo state untouched.
     */
    s_boot_id = esp_random();
    s_sync_session_id = esp_random();
    if (!s_sync_session_id) s_sync_session_id = 1;
    newo_sync_estimator_init(&s_sync_estimator, s_sync_session_id);
    set_track_active(false);

    ESP_LOGI(TAG, "standalone measurement plane; no inference or production integration");
    initialise_wifi();
    configure_source_filter();
    initialise_udp();
    if (!refresh_association_state()) {
        ESP_LOGE(TAG, "initial association identity refresh failed");
        abort();
    }
    __atomic_store_n(&s_refresh_handled, counter_get(&s_refresh_requested),
                     __ATOMIC_RELAXED);
    configure_csi();
    initialise_espnow();
#if CONFIG_NEWO_ESPNOW_TRANSMIT
    ESP_LOGI(TAG, "Newo2 boot default TRACK_OFF resources=control_only"
                  " csi=off ping=off probe=off sender=blocked; awaiting leader control");
#else
    ESP_ERROR_CHECK(apply_measurement_state(true));
#endif

    BaseType_t refresh_created = xTaskCreatePinnedToCore(
        association_refresh_task, "association_refresh", 4096, NULL, 5, NULL, 1);
    if (refresh_created != pdPASS) {
        ESP_LOGE(TAG, "failed to create association refresh task");
        abort();
    }

    BaseType_t created = xTaskCreatePinnedToCore(sender_task, "ncsi_sender", 6144,
                                                 NULL, 5, &s_sender_task, 1);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "failed to create UDP sender task");
        abort();
    }
}
