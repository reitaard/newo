# Phase 1 — networking foundation

## Required software

- Arduino IDE 2.x or Arduino CLI
- Arduino-ESP32 3.3.11
- ArduinoJson 7.4.3
- WebSockets 2.7.2

The official Arduino-ESP32 `WiFi`, `WiFiProv`, and `Preferences` libraries provide networking, BLE provisioning, and NVS storage. No third-party Wi-Fi manager or web server is used.

## Board configuration

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| CPU Frequency | 240MHz (WiFi) |
| Flash Mode | QIO 80MHz |
| Flash Size | 16MB (128Mb) |
| PSRAM | OPI PSRAM |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| Upload Speed | 921600 |
| Core Debug Level | None |

Open the existing `Newo/Newo.ino` sketch directory; do not move the `.ino` away from its adjacent sources.

## First provisioning

1. Compile and upload the sketch.
2. Open Serial Monitor at 115200.
3. With no reachable saved network, Newo advertises BLE service `PROV_NEWO`.
4. Open Espressif **ESP BLE Provisioning** on Android/iOS.
5. Select the device, Security 1, and leave proof of possession empty.
6. Choose a 2.4 GHz Wi-Fi network and submit its credential.
7. Newo saves the credential only after the provisioning framework reports connection success.
8. BLE is stopped and Newo reboots; the next boot scans and connects automatically.

Representative output:

```text
[storage] Loaded 0 saved network(s)
[wifi] No saved networks; opening BLE provisioning
[prov] BLE provisioning started: PROV_NEWO (timeout 300 s)
[prov] Wi-Fi credentials received; waiting for connection success
[prov] Wi-Fi credentials accepted
[prov] Wi-Fi network saved; rebooting Newo
```

Provisioning stops after five minutes. Reboot to open another attempt. Hardware BLE execution remains pending while the board is disconnected. USB Serial continues to show important application events; firmware also keeps a fixed 64-entry volatile RAM event buffer for authenticated remote `/health`, `/logs`, and `/errors` requests. It is deliberately erased on reboot and never persisted to NVS or a filesystem.

## Saved-network behavior

Newo stores up to eight entries. Every recovery cycle scans first, matches only saved SSIDs, ranks matches by strongest RSSI, and attempts them in that order. Initial recovery lasts 18 seconds. After a previously connected station disconnects, Newo periodically runs bounded recovery windows.

Provisioning updates an existing SSID or appends a new one without deleting unrelated entries. Passwords are never printed or included in status/cloud messages.

## Security note

Security 1 provides X25519 key exchange and encrypted provisioning messages. Null PoP is an explicit prototype convenience, not production authentication. A later display/label flow should provide per-device PoP and QR data, while a physical action should gate provisioning/reset.

## Verified build

Arduino CLI compilation with Arduino-ESP32 3.3.11 and the settings above succeeds:

- program storage: 1,410,663 bytes (44% of 3,145,728)
- static RAM: 49,096 bytes (14% of 327,680)
- delta from supplied pre-BLE baseline: +217,756 flash, -1,044 RAM

## References

- [Arduino-ESP32 Wi-Fi API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html)
- [Arduino-ESP32 Wi-Fi provisioning API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi_provisioning.html)
- [Arduino-ESP32 Preferences API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/preferences.html)
