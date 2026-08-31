#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiProv.h>
#include <freertos/FreeRTOS.h>

#include <vector>

#include "newo_storage.h"

class NewoWiFi {
 public:
  explicit NewoWiFi(NewoStorage& storage);

  void begin();
  void loop();

  bool connected() const;
  String connectedSsid() const;
  int32_t rssi() const;
  uint32_t scanCount() const;
  uint32_t connectAttemptCount() const;
  uint32_t connectSuccessCount() const;
  uint32_t connectFailureCount() const;
  uint32_t disconnectCount() const;
  const char* lastDisconnectReason() const;
  bool provisioningActive() const { return provisioningActive_; }
  enum class LedEvent : uint8_t { NONE, ACCEPTED, REJECTED, SAVED, TIMEOUT };
  LedEvent consumeLedEvent();

 private:
  struct VisibleSavedNetwork {
    size_t savedIndex;
    int32_t rssi;
  };

  bool connectSavedNetworks(uint32_t windowMs);
  bool scanAndConnect(uint32_t deadlineMs);
  bool connectToSavedNetwork(const NewoWifiCredential& network, uint32_t timeoutMs);
  void startBleProvisioning();
  void stopBleProvisioning();
  void handleWiFiEvent(arduino_event_id_t eventId, const arduino_event_info_t& info);
  void processProvisioningHandoff();
  bool provisioningTimedOut() const;
  void scheduleReboot();
  static bool deadlineReached(uint32_t deadlineMs);
  static void copyProvisioningField(char* destination, size_t destinationSize,
                                    const uint8_t* source, size_t sourceSize);

  NewoStorage& storage_;
  bool provisioningAttempted_ = false;
  bool provisioningActive_ = false;
  bool hasConnected_ = false;
  uint32_t provisioningStartedAtMs_ = 0;
  uint32_t lastReconnectAttemptMs_ = 0;
  uint32_t rebootAtMs_ = 0;
  uint32_t scanCount_ = 0;
  uint32_t connectAttemptCount_ = 0;
  uint32_t connectSuccessCount_ = 0;
  uint32_t connectFailureCount_ = 0;
  uint32_t disconnectCount_ = 0;
  char lastDisconnectReason_[48] = "Unknown";

  portMUX_TYPE provisioningMux_ = portMUX_INITIALIZER_UNLOCKED;
  char pendingProvisioningSsid_[33] = {};
  char pendingProvisioningPassword_[65] = {};
  volatile bool provisioningCredentialsPending_ = false;
  volatile bool provisioningCredentialsSucceeded_ = false;
  volatile LedEvent pendingLedEvent_ = LedEvent::NONE;
};
