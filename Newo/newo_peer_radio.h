#pragma once
#include <esp_now.h>

enum class NewoPeerRole : uint8_t { UNKNOWN = 0, LEADER = 1, FOLLOWER = 2 };
struct NewoPeerIdentity { uint32_t nodeId, capabilityFlags, bootSession; uint8_t mac[6]; NewoPeerRole role; };
struct NewoPeerRadioMetrics { uint32_t received, routed, unmatched, dropped, sendFailures; };

bool newoPeerRadioAcquire(const uint8_t peerMac[6]);
bool newoPeerRadioRegisterConsumer(uint32_t wireMagic, esp_now_recv_cb_t callback);
void newoPeerRadioUnregisterConsumer(uint32_t wireMagic);
void newoPeerRadioRelease();
esp_err_t newoPeerRadioSend(const uint8_t* payload, size_t length);
bool newoPeerRadioOwned();
NewoPeerRadioMetrics newoPeerRadioMetrics();
NewoPeerIdentity newoPeerRadioIdentity();
void newoPeerRadioSetIdentity(const NewoPeerIdentity& identity);
constexpr uint32_t newoPeerMagic(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 |
         uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}
