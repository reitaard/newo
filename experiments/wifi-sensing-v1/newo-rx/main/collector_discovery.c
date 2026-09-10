#include "collector_discovery.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/igmp.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define NCOL_PORT 47777
#define NCOL_GROUP "239.255.77.77"
#define NCOL_SIZE 18
#define NCOL_MIN_LEASE 5
#define NCOL_MAX_LEASE 300

static const char *TAG = "ncol";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static struct sockaddr_in s_fallback;
static struct sockaddr_in s_discovered;
static struct sockaddr_in s_override;
static bool s_has_discovered;
static bool s_has_override;
static volatile bool s_associated;
static int64_t s_lease_deadline_us;
static struct sockaddr_in s_candidate;
static uint32_t s_candidate_nonce;
static uint8_t s_candidate_count;
static newo_collector_source_t s_last_reported = NEWO_COLLECTOR_CONFIGURED;

static uint16_t le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

const char *newo_collector_source_name(newo_collector_source_t source)
{
    switch (source) {
        case NEWO_COLLECTOR_DISCOVERED: return "discovered";
        case NEWO_COLLECTOR_OVERRIDE: return "override";
        default: return "configured";
    }
}

static newo_collector_source_t select_locked(struct sockaddr_in *destination)
{
    if (s_has_override) {
        *destination = s_override;
        return NEWO_COLLECTOR_OVERRIDE;
    }
    if (s_has_discovered && esp_timer_get_time() < s_lease_deadline_us) {
        *destination = s_discovered;
        return NEWO_COLLECTOR_DISCOVERED;
    }
    *destination = s_fallback;
    return NEWO_COLLECTOR_CONFIGURED;
}

void newo_collector_snapshot(struct sockaddr_in *destination,
                             newo_collector_source_t *source)
{
    portENTER_CRITICAL(&s_lock);
    newo_collector_source_t selected = select_locked(destination);
    portEXIT_CRITICAL(&s_lock);
    if (source) *source = selected;
}

bool newo_collector_set_override(const char *ipv4, uint16_t port)
{
    struct sockaddr_in value = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (!ipv4 || port == 0 || inet_pton(AF_INET, ipv4, &value.sin_addr) != 1) return false;
    portENTER_CRITICAL(&s_lock);
    s_override = value;
    s_has_override = true;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "collector override=%s:%u", ipv4, (unsigned)port);
    return true;
}

void newo_collector_clear_override(void)
{
    portENTER_CRITICAL(&s_lock);
    s_has_override = false;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "collector override cleared");
}

void newo_collector_discovery_set_associated(bool associated)
{
    s_associated = associated;
    if (!associated) {
        portENTER_CRITICAL(&s_lock);
        s_has_discovered = false;
        s_candidate_count = 0;
        portEXIT_CRITICAL(&s_lock);
    }
}

static bool parse_announcement(const uint8_t *wire, size_t length,
                               const struct sockaddr_in *sender,
                               struct sockaddr_in *collector, uint16_t *lease,
                               uint32_t *nonce)
{
    if (length != NCOL_SIZE || memcmp(wire, "NCOL", 4) != 0 || wire[4] != 1 ||
        wire[5] != 0 || le32(wire + 14) != 0) return false;
    uint16_t port = le16(wire + 6);
    uint16_t seconds = le16(wire + 8);
    if (port == 0 || seconds < NCOL_MIN_LEASE || seconds > NCOL_MAX_LEASE ||
        IN_MULTICAST(ntohl(sender->sin_addr.s_addr)) ||
        sender->sin_addr.s_addr == INADDR_ANY) return false;
    *collector = *sender;
    collector->sin_port = htons(port);
    *lease = seconds;
    *nonce = le32(wire + 10);
    return true;
}

static void discovery_task(void *unused)
{
    (void)unused;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) { ESP_LOGE(TAG, "socket failed errno=%d", errno); vTaskDelete(NULL); return; }
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in bind_address = {.sin_family = AF_INET,
                                      .sin_port = htons(NCOL_PORT),
                                      .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(sock, (struct sockaddr *)&bind_address, sizeof(bind_address)) != 0) {
        ESP_LOGE(TAG, "bind failed errno=%d", errno); close(sock); vTaskDelete(NULL); return;
    }
    struct ip_mreq membership = {.imr_multiaddr.s_addr = inet_addr(NCOL_GROUP),
                                 .imr_interface.s_addr = htonl(INADDR_ANY)};
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership));
    struct timeval timeout = {.tv_sec = 1};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    uint8_t wire[NCOL_SIZE + 1];
    while (true) {
        struct sockaddr_in sender = {0};
        socklen_t sender_length = sizeof(sender);
        ssize_t received = recvfrom(sock, wire, sizeof(wire), 0,
                                    (struct sockaddr *)&sender, &sender_length);
        if (s_associated && received > 0) {
            struct sockaddr_in collector;
            uint16_t lease;
            uint32_t nonce;
            if (parse_announcement(wire, (size_t)received, &sender, &collector, &lease, &nonce)) {
                portENTER_CRITICAL(&s_lock);
                bool same = s_candidate.sin_addr.s_addr == collector.sin_addr.s_addr &&
                            s_candidate.sin_port == collector.sin_port && s_candidate_nonce == nonce;
                s_candidate_count = same ? (uint8_t)(s_candidate_count + 1) : 1;
                s_candidate = collector;
                s_candidate_nonce = nonce;
                if (s_candidate_count >= 2) {
                    s_discovered = collector;
                    s_has_discovered = true;
                    s_lease_deadline_us = esp_timer_get_time() + (int64_t)lease * 1000000LL;
                    s_candidate_count = 0;
                }
                portEXIT_CRITICAL(&s_lock);
            }
        }
        struct sockaddr_in selected;
        newo_collector_source_t source;
        newo_collector_snapshot(&selected, &source);
        if (source != s_last_reported) {
            ESP_LOGI(TAG, "collector source=%s address=" IPSTR ":%u",
                     newo_collector_source_name(source), IP2STR(&selected.sin_addr),
                     (unsigned)ntohs(selected.sin_port));
            s_last_reported = source;
        }
    }
}

void newo_collector_discovery_start(const char *fallback_ipv4, uint16_t fallback_port)
{
    memset(&s_fallback, 0, sizeof(s_fallback));
    s_fallback.sin_family = AF_INET;
    s_fallback.sin_port = htons(fallback_port);
    if (!fallback_ipv4 || inet_pton(AF_INET, fallback_ipv4, &s_fallback.sin_addr) != 1) {
        ESP_LOGE(TAG, "invalid configured collector");
        abort();
    }
    if (xTaskCreate(discovery_task, "ncol_discovery", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task creation failed");
        abort();
    }
}
