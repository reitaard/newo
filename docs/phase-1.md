# Phase 1 — networking foundation

## Required software

- Arduino IDE 2.x
- Arduino-ESP32 3.3.11
- ArduinoJson 7.4.3

No WiFiManager, AutoConnect, ESPAsyncWebServer, or other third-party networking layer is required for Phase 1.

## Sketch location

Open `Newo/Newo.ino` in Arduino IDE. All sketch-local `.h` and `.cpp` files live beside it in the `Newo/` folder so Arduino includes and compiles them correctly.

## Board configuration

Use these Arduino IDE settings for the current ESP32-S3 N16R8 board:

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

## First boot

1. Compile and upload `Newo/Newo.ino`.
2. Open Serial Monitor at `115200`.
3. Newo should print its ESP32-S3 hardware information.
4. With no saved networks, Newo creates an access point named `newo@ai.link`.
5. Serial Monitor prints a fresh 12-character setup password.
6. Connect a phone or PC to `newo@ai.link` using that password.
7. The captive portal may open automatically. If it does not, browse to `http://192.168.4.1`.
8. Select a nearby Wi-Fi network, enter its password, and press **Save & connect**.
9. Newo stores the network in NVS and reboots.
10. On the next boot it should connect automatically.

Expected first-boot output is similar to:

```text
================================
            NEWO
================================
Chip: ESP32-S3
CPU: 240 MHz
Flash: 16 MB
PSRAM: 8 MB
Free PSRAM: 7 MB
================================
[storage] Loaded 0 saved network(s)
[wifi] No saved networks
[wifi] Setup AP: newo@ai.link
[wifi] Setup password: Ab3Example9Z
[wifi] Setup URL: http://192.168.4.1
[portal] HTTP server started
[portal] Captive DNS started
[boot] Newo ready
```

The setup password changes whenever setup mode starts after a reboot.

After a network is saved, a later boot should show a connection and DHCP address:

```text
[wifi] Trying 1 saved network(s)...
[wifi] Connected: example-ssid
[wifi] IP: 192.168.1.123
[wifi] RSSI: -52 dBm
[wifi] Local name: http://newo.local
```

`newo.local` depends on the LAN allowing peer communication and mDNS. Client-isolated Wi-Fi may still permit Newo to reach the Internet while preventing another local device from reaching Newo.

## Local endpoints

### `/`

Setup/status web interface.

### `/api/status`

JSON device status, including connectivity, IP, signal level, free heap/PSRAM, uptime, and number of saved networks.

### `/api/wifi/scan`

JSON list of visible Wi-Fi networks with RSSI and whether the AP reports security enabled.

## Saved-network behavior

Newo supports up to eight saved networks in Phase 1. At boot, the firmware loads those credentials into the official Arduino-ESP32 `WiFiMulti` component. WiFiMulti scans and attempts an available entry from its configured list.

If no saved network is reachable, Newo falls back to `newo@ai.link`. While disconnected, it periodically retries the saved list.

Adding, updating, deleting, or clearing a network through the web interface saves the change and reboots Newo so the network state starts cleanly.

## Development security note

The setup WLAN uses a random per-boot WPA password generated from the ESP32-S3 hardware RNG, so the provisioning traffic is not sent over an open radio network. The setup page itself is still local HTTP, not HTTPS, and Phase 1 remains development firmware. Physical gating, credential reset controls, and production provisioning hardening are still planned.

## Upstream references

- Arduino-ESP32 Wi-Fi API: https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html
- Arduino-ESP32 Preferences API: https://docs.espressif.com/projects/arduino-esp32/en/latest/api/preferences.html
- Espressif captive portal example: https://github.com/espressif/arduino-esp32/tree/master/libraries/DNSServer/examples/CaptivePortal
- Espressif mDNS example: https://github.com/espressif/arduino-esp32/tree/master/libraries/ESPmDNS/examples/mDNS_Web_Server
- ESP32-S3 random-number API: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/random.html
- Arduino-ESP32 partition-table guide: https://docs.espressif.com/projects/arduino-esp32/en/latest/tutorials/partition_table.html
