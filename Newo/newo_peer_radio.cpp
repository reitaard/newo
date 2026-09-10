#include "newo_peer_radio.h"

#include <cstring>

namespace {
bool owned;
uint8_t peer[6];
}

bool newoPeerRadioAcquire(const uint8_t peerMac[6], esp_now_recv_cb_t receiveCallback) {
  if (owned || !peerMac || !receiveCallback) return false;
  if (esp_now_init() != ESP_OK) return false;
  owned = true;
  esp_now_peer_info_t info = {};
  memcpy(info.peer_addr, peerMac, sizeof(peer));
  info.ifidx = WIFI_IF_STA;
  info.channel = 0;
  if (esp_now_add_peer(&info) != ESP_OK || esp_now_register_recv_cb(receiveCallback) != ESP_OK) {
    newoPeerRadioRelease();
    return false;
  }
  memcpy(peer, peerMac, sizeof(peer));
  return true;
}

void newoPeerRadioRelease() {
  if (!owned) return;
  esp_now_unregister_recv_cb();
  esp_now_del_peer(peer);
  esp_now_deinit();
  memset(peer, 0, sizeof(peer));
  owned = false;
}

esp_err_t newoPeerRadioSend(const uint8_t* payload, size_t length) {
  return owned ? esp_now_send(peer, payload, length) : ESP_ERR_INVALID_STATE;
}

bool newoPeerRadioOwned() { return owned; }

