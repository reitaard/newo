#pragma once
#include <Arduino.h>
#include <esp_wifi.h>
extern "C" {
#include "../experiments/wifi-sensing-v1/newo-rx/main/ncsi_protocol.h"
}

struct NewoCsiSlot {
  ncsi_csi_record_t record;
  uint8_t csi[NCSI_MAX_CSI_BYTES];
};

struct NewoCsiCounters {
  uint32_t callbacks, accepted, invalid, unrelated, peerGateDrops;
  uint32_t pathAccepted[3], pathGateDrops[3], ringDrops, ringHighWater, discarded;
};

bool newoCsiMeasurementBegin(const uint8_t receiver[6], const uint8_t ap[6],
                             const uint8_t peer[6], uint16_t peerRateHz);
void newoCsiMeasurementStopAccepting();
bool newoCsiMeasurementPop(NewoCsiSlot& slot);
uint32_t newoCsiMeasurementDiscardPending();
bool newoCsiMeasurementEmpty();
NewoCsiCounters newoCsiMeasurementCounters();
void newoCsiRxCallback(void*, wifi_csi_info_t* info);
