#include "newo2_storage.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <dirent.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

namespace Newo2Storage {
namespace {
constexpr const char *TAG = "newo2_storage";
constexpr const char *kMount = "/sdcard";
constexpr const char *kSnapshotDir = "/sdcard/newo2/snapshots";
constexpr const char *kVideoDir = "/sdcard/newo2/videos";
// Use a fixed-size ring on the card. The sequence restarts after a reboot, so
// early slots are overwritten first; the directory can never grow beyond this
// many production snapshots.
constexpr uint32_t kSnapshotSlots = 500;
bool g_available = false;
FILE *g_video = nullptr;
size_t g_video_bytes = 0;
uint32_t g_next_video = 1;

void scan_video_sequence() {
    DIR *dir = opendir(kVideoDir);
    if (!dir) return;
    while (dirent *entry = readdir(dir)) {
        unsigned long value = 0;
        if (sscanf(entry->d_name, "video-%08lu.mjpeg", &value) == 1 && value >= g_next_video)
            g_next_video = static_cast<uint32_t>(value + 1);
    }
    closedir(dir);
}
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
    mkdir(kVideoDir, 0775);
    scan_video_sequence();
    g_available = true;
    ESP_LOGI(TAG, "SD mounted capacity=%lluMB", static_cast<unsigned long long>(card->csd.capacity) * card->csd.sector_size / (1024ULL * 1024ULL));
    return true;
}

bool available() { return g_available; }

bool save_snapshot(const uint8_t *jpeg, size_t len, uint32_t sequence, char *out_path, size_t out_path_len) {
    if (!g_available || !jpeg || !len || !out_path || out_path_len < 32) return false;
    const uint32_t slot = sequence ? (sequence - 1) % kSnapshotSlots : 0;
    snprintf(out_path, out_path_len, "%s/snapshot-%03lu.jpg", kSnapshotDir,
             static_cast<unsigned long>(slot));
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

bool begin_video(uint32_t sequence, char *out_path, size_t out_path_len) {
    if (!g_available || g_video || !out_path || out_path_len < 40) return false;
    const uint32_t id = std::max(g_next_video++, sequence);
    snprintf(out_path, out_path_len, "%s/video-%08lu.mjpeg", kVideoDir, static_cast<unsigned long>(id));
    g_video = fopen(out_path, "wb");
    g_video_bytes = 0;
    if (!g_video) { out_path[0] = '\0'; return false; }
    ESP_LOGI(TAG, "video opened %s", out_path);
    return true;
}

bool append_video_frame(const uint8_t *jpeg, size_t len) {
    if (!g_video || !jpeg || !len) return false;
    const size_t written = fwrite(jpeg, 1, len, g_video);
    if (written != len) return false;
    g_video_bytes += written;
    return true;
}

bool end_video(size_t *bytes_written) {
    if (!g_video) return false;
    fflush(g_video);
    const bool ok = ferror(g_video) == 0;
    fclose(g_video);
    g_video = nullptr;
    if (bytes_written) *bytes_written = g_video_bytes;
    ESP_LOGI(TAG, "video closed bytes=%u ok=%d", static_cast<unsigned>(g_video_bytes), ok);
    g_video_bytes = 0;
    return ok;
}

bool video_open() { return g_video != nullptr; }
}  // namespace Newo2Storage
