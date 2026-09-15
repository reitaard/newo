#include "driver/gpio.h"
#include "esp_log.h"
#include "led_strip.h"
#include "led_strip_rmt.h"

namespace {
constexpr gpio_num_t kOnboardRgbPin = GPIO_NUM_48;
constexpr const char *kTag = "newo2_led";

void force_onboard_rgb_off() {
    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = kOnboardRgbPin;
    strip_config.max_leds = 1;
    strip_config.led_model = LED_MODEL_WS2812;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    led_strip_rmt_config_t rmt_config = {};
    rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_config.resolution_hz = 10 * 1000 * 1000;
    led_strip_handle_t strip = nullptr;
    const esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &strip);
    if (err == ESP_OK && strip) {
        led_strip_clear(strip);
        led_strip_del(strip);
    } else {
        ESP_LOGW(kTag, "WS2812 init failed: %s", esp_err_to_name(err));
    }
    gpio_reset_pin(kOnboardRgbPin);
    gpio_set_direction(kOnboardRgbPin, GPIO_MODE_OUTPUT);
    gpio_set_level(kOnboardRgbPin, 0);
    ESP_LOGI(kTag, "GPIO48 WS2812 forced OFF");
}

struct BootLedOff { BootLedOff() { force_onboard_rgb_off(); } };
BootLedOff g_boot_led_off;
}  // namespace
