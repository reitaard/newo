# Production References

## ESP-SR / WakeNet

Use Espressif's current ESP-SR documentation as the source of truth for Newo's local wake-word layer. Newo's written identity is **Newo**, while the spoken name and desired wake phrase are **Neo**. The Sherpa `NEO` hotword is contextual ASR bias only; the actual local wake phrase is determined by the WakeNet model stored in the ESP-SR `model` partition.

Primary references:

- ESP-SR repository and current WakeNet model list: https://github.com/espressif/esp-sr
- WakeNet model documentation: https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/README.html
- Wake-word customization process: https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/ESP_Wake_Words_Customization.html
- Model selection/loading and model partition: https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/flash_model/README.html
- TTS/community wake-word request process: https://github.com/espressif/esp-sr/issues/88
- Arduino-ESP32 ESP_SR build hook for `srmodels.bin`: https://github.com/espressif/arduino-esp32/blob/master/platform.txt

Newo-specific conclusions and the recommended low-complexity path for obtaining a real `Neo` WakeNet model are recorded in [`wake-word.md`](wake-word.md).

## ElatoAI

Repository: https://github.com/akdeb/ElatoAI

Use ElatoAI as a production reference when hardening or expanding Newo's ESP32 voice stack. Revisit its implementation for practical patterns around:

- realtime ESP32 speech-to-speech architecture
- secure persistent WebSocket audio transport
- Opus compression and buffering
- server-side VAD and conversation flow
- device authentication and management
- captive-portal Wi-Fi provisioning
- OTA firmware updates
- remote volume/factory-reset controls
- tool/function calling from voice agents
- Cloudflare/Durable Objects or edge deployment patterns
- multi-device scaling and long-running voice sessions

This is a reference for production insights, not a dependency or a requirement to copy its architecture.
