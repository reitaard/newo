#pragma once

#include <esp_now.h>

// Single ESP-NOW lifecycle boundary for current tracking and future peer/sync clients.
bool newoPeerRadioAcquire(const uint8_t peerMac[6], esp_now_recv_cb_t receiveCallback);
void newoPeerRadioRelease();
esp_err_t newoPeerRadioSend(const uint8_t* payload, size_t length);
bool newoPeerRadioOwned();
