#include "newo2_storage.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

namespace Newo2Storage {
namespace {
constexpr const char *TAG = "newo2_storage";
constexpr const char *kMount = "/sdcard";
constexpr const char *kSnapshotDir = "/sdcard/newo2/snapshots";
bool g_available = false;
}

bool begin() {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = GPIO_NUM_39;
    slot.cmd = GPIO_NUM_38;
    slot.d0 = GPIO_NUM_40;
    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = false;
    mount.max_files = 4;
    mount.allocation_unit_size = 16 * 1024;
    sdmmc_card_t *card = nullptr;
    const esp_err_t err = esp_vfs_fat_sdmmc_mount(kMount, &host, &slot, &mount, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD unavailable: %s; snapshots can still upload", esp_err_to_name(err));
        return false;
    }
    mkdir("/sdcard/newo2", 0775);
    mkdir(kSnapshotDir, 0775);
    g_available = true;
    ESP_LOGI(TAG, "SD mounted capacity=%lluMB", static_cast<unsigned long long>(card->csd.capacity) * card->csd.sector_size / (1024ULL * 1024ULL));
    return true;
}

bool available() { return g_available; }

bool save_snapshot(const uint8_t *jpeg, size_t len, uint32_t sequence, char *out_path, size_t out_path_len) {
    if (!g_available || !jpeg || !len || !out_path || out_path_len < 32) return false;
    const uint64_t uptime_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    snprintf(out_path, out_path_len, "%s/snap-%010llu-%06lu.jpg", kSnapshotDir,
             static_cast<unsigned long long>(uptime_ms), static_cast<unsigned long>(sequence));
    FILE *file = fopen(out_path, "wb");
    if (!file) return false;
    const size_t written = fwrite(jpeg, 1, len, file);
    fflush(file);
    fclose(file);
    if (written != len) {
        remove(out_path);
        out_path[0] = '\0';
        return false;
    }
    ESP_LOGI(TAG, "saved %s bytes=%u", out_path, static_cast<unsigned>(len));
    return true;
}
}  // namespace Newo2Storage
