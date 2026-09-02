#include "driver/gpio.h"
#include "esp_log.h"
#include "led_strip.h"
#include "led_strip_rmt.h"

namespace {

constexpr gpio_num_t kOnboardRgbPin = GPIO_NUM_48;
constexpr const char *kTag = "goouuu_led";

void force_onboard_rgb_off() {
    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = kOnboardRgbPin;
    strip_config.max_leds = 1;
    strip_config.led_model = LED_MODEL_WS2812;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_config.flags.invert_out = false;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_config.resolution_hz = 10 * 1000 * 1000;
    rmt_config.mem_block_symbols = 0;
    rmt_config.flags.with_dma = false;

    led_strip_handle_t strip = nullptr;
    const esp_err_t create_err = led_strip_new_rmt_device(&strip_config, &rmt_config, &strip);
    if (create_err == ESP_OK && strip) {
        // A WS2812 remembers its last latched color. Send a real black pixel
        // instead of merely pulling GPIO48 low.
        const esp_err_t clear_err = led_strip_clear(strip);
        if (clear_err != ESP_OK) {
            ESP_LOGW(kTag, "WS2812 clear failed: %s", esp_err_to_name(clear_err));
        }
        led_strip_del(strip);
    } else {
        ESP_LOGW(kTag, "WS2812 RMT init failed: %s", esp_err_to_name(create_err));
    }

    // Keep the data line quiet after the one-shot clear frame.
    gpio_reset_pin(kOnboardRgbPin);
    gpio_set_direction(kOnboardRgbPin, GPIO_MODE_OUTPUT);
    gpio_set_level(kOnboardRgbPin, 0);
    ESP_LOGI(kTag, "Onboard WS2812 forced OFF on GPIO48");
}

// ESP-IDF runs C++ global constructors before app_main(). This makes the LED
// dark before camera/model/Wi-Fi startup without touching the vision pipeline.
struct BootLedOff {
    BootLedOff() { force_onboard_rgb_off(); }
};

BootLedOff g_boot_led_off;

}  // namespace
