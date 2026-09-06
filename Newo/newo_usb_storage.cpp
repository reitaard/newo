#include "newo_usb_storage.h"
#include "newo_usb_msc_lun.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <esp_err.h>
#include <esp_vfs_fat.h>
#include <freertos/task.h>

namespace {
constexpr UBaseType_t kMscTaskPriority = 2;
constexpr UBaseType_t kWorkerTaskPriority = 1;
constexpr uint32_t kMscTaskStack = 4096;
constexpr uint32_t kWorkerTaskStack = 6144;
constexpr uint32_t kMediaRetryBaseMs = 2000;
constexpr uint32_t kMediaRetryMaxMs = 8000;
constexpr uint32_t kMountedHealthMs = 1000;
constexpr uint32_t kReleaseRetryMs = 1000;

// Temporary physical-release gate for 0.5.9-dev. It creates one hidden 4 MiB
// file, verifies every byte, then runs continuous read-only I/O. The operator
// unplugs only after READ_STRESS_READY. Three unplug/reconnect/remount cycles
// must preserve the file before this gate declares PASS. This block is removed
// before the production 0.6 merge.
constexpr char kValidationPath[] = "/usb/.newo_usb_validation.tmp";
constexpr size_t kValidationBytes = 4u * 1024u * 1024u;
constexpr size_t kValidationChunk = 8192u;
constexpr uint8_t kValidationDisconnectCycles = 3;
constexpr uint64_t kValidationProgressBytes = 16ull * 1024ull * 1024ull;

enum class ValidationPhase : uint8_t {
  INITIAL_RW,
  READ_STRESS,
  COMPLETE,
  FAILED,
};

ValidationPhase gValidationPhase = ValidationPhase::INITIAL_RW;
uint8_t gValidationDisconnects = 0;
bool gValidationNeedsReconnectVerify = false;

void logError(const char* event, esp_err_t error) {
  Serial.printf("[usb-storage] %s — reason=%s (%d)\n", event,
                esp_err_to_name(error), static_cast<int>(error));
}

uint32_t nextBackoff(uint8_t failures) {
  uint32_t delay = kMediaRetryBaseMs;
  for (uint8_t i = 1; i < failures && delay < kMediaRetryMaxMs; ++i) {
    delay *= 2;
  }
  return delay > kMediaRetryMaxMs ? kMediaRetryMaxMs : delay;
}

uint8_t validationByte(size_t offset) {
  uint32_t x = static_cast<uint32_t>(offset);
  x ^= x >> 13;
  x *= 0x85ebca6bu;
  x ^= x >> 16;
  return static_cast<uint8_t>((x + 0x5au) & 0xffu);
}

void fillValidationPattern(uint8_t* buffer, size_t length, size_t offset) {
  for (size_t i = 0; i < length; ++i) buffer[i] = validationByte(offset + i);
}

bool validateBuffer(const uint8_t* buffer, size_t length, size_t offset,
                    size_t& mismatchOffset) {
  for (size_t i = 0; i < length; ++i) {
    if (buffer[i] != validationByte(offset + i)) {
      mismatchOffset = offset + i;
      return false;
    }
  }
  return true;
}

bool verifyValidationFile(msc_host_device_handle_t device, const char* stage) {
  uint8_t* buffer = static_cast<uint8_t*>(malloc(kValidationChunk));
  if (buffer == nullptr) {
    Serial.printf("[usb-validation] %s_FAIL — reason=no_memory\n", stage);
    return false;
  }

  FILE* file = fopen(kValidationPath, "rb");
  if (file == nullptr) {
    free(buffer);
    Serial.printf("[usb-validation] %s_FAIL — reason=open_read\n", stage);
    return false;
  }

  bool ok = true;
  size_t offset = 0;
  while (offset < kValidationBytes) {
    if (!msc_host_media_ready(device)) {
      Serial.printf("[usb-validation] %s_FAIL — reason=media_lost offset=%u\n",
                    stage, static_cast<unsigned>(offset));
      ok = false;
      break;
    }

    const size_t want = (kValidationBytes - offset) < kValidationChunk
        ? (kValidationBytes - offset) : kValidationChunk;
    const size_t got = fread(buffer, 1, want, file);
    if (got != want) {
      Serial.printf("[usb-validation] %s_FAIL — reason=short_read offset=%u got=%u want=%u\n",
                    stage, static_cast<unsigned>(offset), static_cast<unsigned>(got),
                    static_cast<unsigned>(want));
      ok = false;
      break;
    }

    size_t mismatchOffset = 0;
    if (!validateBuffer(buffer, got, offset, mismatchOffset)) {
      Serial.printf("[usb-validation] %s_FAIL — reason=data_mismatch offset=%u\n",
                    stage, static_cast<unsigned>(mismatchOffset));
      ok = false;
      break;
    }
    offset += got;
  }

  if (fclose(file) != 0 && ok) {
    Serial.printf("[usb-validation] %s_FAIL — reason=close_read\n", stage);
    ok = false;
  }
  free(buffer);
  return ok;
}

bool runValidationRw(msc_host_device_handle_t device) {
  uint8_t* buffer = static_cast<uint8_t*>(malloc(kValidationChunk));
  if (buffer == nullptr) {
    Serial.println("[usb-validation] RW_TEST_FAIL — reason=no_memory");
    return false;
  }

  // Remove only our own stale hidden validation file from an interrupted run.
  remove(kValidationPath);
  FILE* file = fopen(kValidationPath, "wb");
  if (file == nullptr) {
    free(buffer);
    Serial.println("[usb-validation] RW_TEST_FAIL — reason=open_write");
    return false;
  }

  Serial.printf("[usb-validation] RW_TEST_START — bytes=%u chunk=%u\n",
                static_cast<unsigned>(kValidationBytes),
                static_cast<unsigned>(kValidationChunk));

  bool ok = true;
  size_t offset = 0;
  while (offset < kValidationBytes) {
    if (!msc_host_media_ready(device)) {
      Serial.printf("[usb-validation] RW_TEST_FAIL — reason=media_lost_during_write offset=%u\n",
                    static_cast<unsigned>(offset));
      ok = false;
      break;
    }

    const size_t count = (kValidationBytes - offset) < kValidationChunk
        ? (kValidationBytes - offset) : kValidationChunk;
    fillValidationPattern(buffer, count, offset);
    const size_t wrote = fwrite(buffer, 1, count, file);
    if (wrote != count) {
      Serial.printf("[usb-validation] RW_TEST_FAIL — reason=short_write offset=%u wrote=%u want=%u\n",
                    static_cast<unsigned>(offset), static_cast<unsigned>(wrote),
                    static_cast<unsigned>(count));
      ok = false;
      break;
    }
    offset += wrote;
  }

  if (ok && fflush(file) != 0) {
    Serial.println("[usb-validation] RW_TEST_FAIL — reason=flush");
    ok = false;
  }
  if (fclose(file) != 0 && ok) {
    Serial.println("[usb-validation] RW_TEST_FAIL — reason=close_write");
    ok = false;
  }
  free(buffer);

  if (!ok) return false;
  if (!verifyValidationFile(device, "RW_VERIFY")) return false;

  Serial.printf("[usb-validation] RW_TEST_PASS — bytes=%u\n",
                static_cast<unsigned>(kValidationBytes));
  return true;
}

bool runReadStressUntilDisconnect(msc_host_device_handle_t device) {
  uint8_t* buffer = static_cast<uint8_t*>(malloc(kValidationChunk));
  if (buffer == nullptr) {
    Serial.println("[usb-validation] READ_STRESS_FAIL — reason=no_memory");
    return false;
  }

  FILE* file = fopen(kValidationPath, "rb");
  if (file == nullptr) {
    free(buffer);
    Serial.println("[usb-validation] READ_STRESS_FAIL — reason=open_read");
    return false;
  }

  const uint8_t cycle = static_cast<uint8_t>(gValidationDisconnects + 1);
  Serial.printf("[usb-validation] READ_STRESS_READY — cycle=%u/%u UNPLUG_STORAGE_NOW\n",
                static_cast<unsigned>(cycle),
                static_cast<unsigned>(kValidationDisconnectCycles));

  uint64_t bytesRead = 0;
  uint64_t nextProgress = kValidationProgressBytes;
  bool expectedDisconnect = false;
  bool failed = false;

  while (true) {
    if (!msc_host_media_ready(device)) {
      expectedDisconnect = true;
      break;
    }

    const size_t got = fread(buffer, 1, kValidationChunk, file);
    if (got == kValidationChunk) {
      bytesRead += got;
    } else if (got > 0) {
      bytesRead += got;
      if (feof(file)) {
        clearerr(file);
        rewind(file);
      } else if (!msc_host_media_ready(device)) {
        expectedDisconnect = true;
        break;
      } else {
        Serial.printf("[usb-validation] READ_STRESS_FAIL — reason=short_read got=%u\n",
                      static_cast<unsigned>(got));
        failed = true;
        break;
      }
    } else {
      if (!msc_host_media_ready(device)) {
        expectedDisconnect = true;
        break;
      }
      if (feof(file)) {
        clearerr(file);
        rewind(file);
        continue;
      }
      Serial.println("[usb-validation] READ_STRESS_FAIL — reason=read_error");
      failed = true;
      break;
    }

    if (bytesRead >= nextProgress) {
      Serial.printf("[usb-validation] READ_STRESS_ACTIVE — cycle=%u bytes=%llu\n",
                    static_cast<unsigned>(cycle),
                    static_cast<unsigned long long>(bytesRead));
      nextProgress += kValidationProgressBytes;
    }
  }

  // Read-only close should not create new filesystem writes. Do it before the
  // storage worker processes the queued disconnect and unregisters FatFS.
  fclose(file);
  free(buffer);

  if (failed || !expectedDisconnect) return false;

  ++gValidationDisconnects;
  gValidationNeedsReconnectVerify = true;
  Serial.printf("[usb-validation] READ_INTERRUPT_PASS — cycle=%u bytes_before_unplug=%llu\n",
                static_cast<unsigned>(gValidationDisconnects),
                static_cast<unsigned long long>(bytesRead));
  return true;
}

void runPhysicalValidation(msc_host_device_handle_t device) {
  if (device == nullptr || gValidationPhase == ValidationPhase::COMPLETE ||
      gValidationPhase == ValidationPhase::FAILED) {
    return;
  }

  if (gValidationPhase == ValidationPhase::INITIAL_RW) {
    if (!runValidationRw(device)) {
      gValidationPhase = ValidationPhase::FAILED;
      Serial.println("[usb-validation] VALIDATION_FAIL — stage=rw");
      return;
    }
    gValidationPhase = ValidationPhase::READ_STRESS;
  }

  if (gValidationNeedsReconnectVerify) {
    Serial.printf("[usb-validation] RECONNECT_VERIFY_START — cycle=%u/%u\n",
                  static_cast<unsigned>(gValidationDisconnects),
                  static_cast<unsigned>(kValidationDisconnectCycles));
    if (!verifyValidationFile(device, "RECONNECT_VERIFY")) {
      gValidationPhase = ValidationPhase::FAILED;
      Serial.printf("[usb-validation] VALIDATION_FAIL — stage=reconnect cycle=%u\n",
                    static_cast<unsigned>(gValidationDisconnects));
      return;
    }
    gValidationNeedsReconnectVerify = false;
    Serial.printf("[usb-validation] RECONNECT_VERIFY_PASS — cycle=%u/%u\n",
                  static_cast<unsigned>(gValidationDisconnects),
                  static_cast<unsigned>(kValidationDisconnectCycles));

    if (gValidationDisconnects >= kValidationDisconnectCycles) {
      if (remove(kValidationPath) != 0) {
        gValidationPhase = ValidationPhase::FAILED;
        Serial.println("[usb-validation] VALIDATION_FAIL — stage=cleanup");
        return;
      }
      gValidationPhase = ValidationPhase::COMPLETE;
      Serial.printf("[usb-validation] VALIDATION_PASS — rw=pass read_unplug_cycles=%u reconnect_verify=pass\n",
                    static_cast<unsigned>(gValidationDisconnects));
      return;
    }
  }

  if (gValidationPhase == ValidationPhase::READ_STRESS) {
    if (!runReadStressUntilDisconnect(device)) {
      gValidationPhase = ValidationPhase::FAILED;
      Serial.println("[usb-validation] VALIDATION_FAIL — stage=read_stress");
    }
  }
}

esp_err_t probeAnyLun(msc_host_device_handle_t device,
                      bool verbose,
                      uint8_t& selectedLun,
                      uint8_t& maxLun) {
  uint8_t activeLun = 0;
  esp_err_t stateError = newo_msc_get_lun_state(device, &activeLun, &maxLun);
  if (stateError != ESP_OK) return stateError;

  // Probe the previously selected LUN first so card reinsertion into the same
  // slot is one SCSI transaction path. Then scan the remaining reported LUNs.
  for (uint8_t pass = 0; pass <= maxLun; ++pass) {
    uint8_t lun = 0;
    if (pass == 0) {
      lun = activeLun;
    } else {
      const uint8_t candidate = static_cast<uint8_t>(pass - 1);
      lun = candidate >= activeLun ? static_cast<uint8_t>(candidate + 1) : candidate;
      if (lun > maxLun) continue;
    }

    esp_err_t selectError = newo_msc_select_lun(device, lun);
    if (selectError != ESP_OK) return selectError;

    const esp_err_t probeError = msc_host_probe_media(device);
    if (probeError == ESP_OK) {
      selectedLun = lun;
      return ESP_OK;
    }
    if (probeError == ESP_ERR_MSC_MOUNT_FAILED) {
      if (verbose) {
        Serial.printf("[usb-storage] LUN_EMPTY — lun=%u\n",
                      static_cast<unsigned>(lun));
      }
      continue;
    }

    // Transport errors are not equivalent to an empty slot. Preserve the
    // existing BOT reset/backoff path instead of hiding a real USB failure by
    // continuing to another LUN.
    return probeError;
  }

  return ESP_ERR_MSC_MOUNT_FAILED;
}
}  // namespace

bool NewoUsbStorage::begin(NewoUsbHost& host) {
  if (host_ != nullptr) return host_ == &host;
  if (!host.ready()) {
    Serial.println("[usb-storage] CLIENT_FAILED — reason=host_not_ready");
    return false;
  }

  host_ = &host;
  retryDelayMs_ = kMediaRetryBaseMs;
  if (xTaskCreate(workerTaskEntry, "newo-usb-vfs", kWorkerTaskStack, this,
                  kWorkerTaskPriority, &workerTask_) != pdPASS) {
    Serial.println("[usb-storage] CLIENT_FAILED — reason=worker_task");
    host_ = nullptr;
    workerTask_ = nullptr;
    return false;
  }

  msc_host_driver_config_t mscConfig = {};
  mscConfig.create_backround_task = true;
  mscConfig.task_priority = kMscTaskPriority;
  mscConfig.stack_size = kMscTaskStack;
  mscConfig.core_id = tskNO_AFFINITY;
  mscConfig.callback = mscEvent;
  mscConfig.callback_arg = this;
  if (!host.installMscClient(mscConfig)) {
    Serial.println("[usb-storage] CLIENT_FAILED — reason=msc_install");
    vTaskDelete(workerTask_);
    workerTask_ = nullptr;
    host_ = nullptr;
    return false;
  }

  Serial.println("[usb-storage] CLIENT_READY — mount=/usb");
  return true;
}

void NewoUsbStorage::workerTaskEntry(void* arg) {
  static_cast<NewoUsbStorage*>(arg)->workerTask();
}

void NewoUsbStorage::workerTask() {
  while (true) {
    TickType_t waitTicks = portMAX_DELAY;
    if (releaseRetryPending_) {
      waitTicks = pdMS_TO_TICKS(kReleaseRetryMs);
    } else if (mediaWaiting_) {
      waitTicks = pdMS_TO_TICKS(retryDelayMs_);
    } else if (mounted_) {
      // No USB traffic is generated by this timer. We only inspect the media
      // flag that diskio clears after a real I/O reports removal.
      waitTicks = pdMS_TO_TICKS(kMountedHealthMs);
    }

    const uint32_t notifications = ulTaskNotifyTake(pdTRUE, waitTicks);

    // Real connect/disconnect events always win over timer work.
    while (true) {
      bool disconnect = false;
      uint8_t address = 0;
      msc_host_device_handle_t disconnectedDevice = nullptr;

      portENTER_CRITICAL(&eventLock_);
      if (disconnectPending_) {
        disconnect = true;
        disconnectedDevice = pendingDisconnectDevice_;
        disconnectPending_ = false;
        pendingDisconnectDevice_ = nullptr;
      } else if (connectPending_) {
        address = pendingAddress_;
        connectPending_ = false;
      } else {
        portEXIT_CRITICAL(&eventLock_);
        break;
      }
      portEXIT_CRITICAL(&eventLock_);

      if (disconnect) handleDisconnected(disconnectedDevice);
      else handleConnected(address);
    }

    if (notifications != 0) continue;

    if (releaseRetryPending_) {
      releaseDevice();
      continue;
    }

    if (mounted_ && device_ != nullptr && !msc_host_media_ready(device_)) {
      Serial.println("[usb-storage] MEDIA_LOST — active I/O reported media unavailable");
      invalidateFilesystem("media_lost");
      mediaWaiting_ = true;
      retryDelayMs_ = kMediaRetryBaseMs;
      probeFailures_ = 0;
      continue;
    }

    if (mediaWaiting_ && device_ != nullptr) {
      probeMediaAndMount(false);
    }
  }
}

void NewoUsbStorage::mscEvent(const msc_host_event_t* event, void* arg) {
  NewoUsbStorage* storage = static_cast<NewoUsbStorage*>(arg);
  if (event == nullptr || storage == nullptr || storage->workerTask_ == nullptr) return;

  portENTER_CRITICAL(&storage->eventLock_);
  if (event->event == msc_host_event_t::MSC_DEVICE_CONNECTED) {
    storage->pendingAddress_ = event->device.address;
    storage->connectPending_ = true;
  } else if (event->event == msc_host_event_t::MSC_DEVICE_DISCONNECTED) {
    storage->pendingDisconnectDevice_ = event->device.handle;
    storage->disconnectPending_ = true;
  } else {
    portEXIT_CRITICAL(&storage->eventLock_);
    return;
  }
  portEXIT_CRITICAL(&storage->eventLock_);
  xTaskNotifyGive(storage->workerTask_);
}

void NewoUsbStorage::handleConnected(uint8_t address) {
  if (device_ != nullptr) {
    Serial.printf("[usb-storage] MSC_IGNORED — address=%u reason=storage_slot_busy\n",
                  static_cast<unsigned>(address));
    return;
  }

  msc_host_device_handle_t device = nullptr;
  const esp_err_t error = msc_host_install_device_transport(address, &device);
  if (error != ESP_OK) {
    logError("TRANSPORT_FAILED", error);
    return;
  }

  device_ = device;
  releaseRetryPending_ = false;
  mediaWaiting_ = false;
  probeFailures_ = 0;
  retryDelayMs_ = kMediaRetryBaseMs;
  Serial.printf("[usb-storage] TRANSPORT_CONNECTED — address=%u\n",
                static_cast<unsigned>(address));

  uint8_t maxLun = 0;
  const esp_err_t lunError = newo_msc_get_max_lun(device_, &maxLun);
  msc_host_device_info_t usbInfo = {};
  const esp_err_t infoError = msc_host_get_device_info(device_, &usbInfo);
  if (lunError == ESP_OK && infoError == ESP_OK) {
    Serial.printf("[usb-storage] DEVICE — vid=%04x pid=%04x max_lun=%u\n",
                  static_cast<unsigned>(usbInfo.idVendor),
                  static_cast<unsigned>(usbInfo.idProduct),
                  static_cast<unsigned>(maxLun));
  } else {
    Serial.printf("[usb-storage] DEVICE_INFO_PARTIAL — info=%s lun=%s\n",
                  esp_err_to_name(infoError), esp_err_to_name(lunError));
  }
  if (maxLun > 0) {
    Serial.printf("[usb-storage] MULTI_LUN_ENABLED — scanning=0..%u\n",
                  static_cast<unsigned>(maxLun));
  }

  probeMediaAndMount(true);
}

void NewoUsbStorage::probeMediaAndMount(bool firstProbe) {
  if (device_ == nullptr || releaseRetryPending_) return;

  uint8_t selectedLun = 0;
  uint8_t maxLun = 0;
  esp_err_t error = probeAnyLun(device_, firstProbe, selectedLun, maxLun);
  if (error == ESP_OK) {
    const bool wasWaiting = mediaWaiting_;
    mediaWaiting_ = false;
    probeFailures_ = 0;
    retryDelayMs_ = kMediaRetryBaseMs;
    if (firstProbe || wasWaiting) {
      Serial.printf("[usb-storage] MEDIA_READY — lun=%u\n",
                    static_cast<unsigned>(selectedLun));
    }
    if (!mounted_) mountFilesystem();
    return;
  }

  if (error == ESP_ERR_MSC_MOUNT_FAILED) {
    if (firstProbe || !mediaWaiting_) {
      Serial.printf("[usb-storage] MEDIA_ABSENT — luns=0..%u reprobe=%lums\n",
                    static_cast<unsigned>(maxLun),
                    static_cast<unsigned long>(kMediaRetryBaseMs));
    }
    mediaWaiting_ = true;
    probeFailures_ = 0;
    retryDelayMs_ = kMediaRetryBaseMs;
    return;
  }

  if (error == ESP_ERR_INVALID_STATE) {
    // DEV_GONE is delivered asynchronously. Do not reuse or reopen this handle;
    // the disconnect callback will release it after in-flight BOT I/O unwinds.
    Serial.println("[usb-storage] MEDIA_PROBE_DEFERRED — transport_not_active");
    return;
  }

  // A timeout/stall is a transport fault, not proof that the media disappeared.
  // Recover BOT in-place; never tear down the shared USB host or D07/VCP clients.
  esp_err_t recovery = msc_host_reset_recovery(device_);
  if (recovery == ESP_ERR_MSC_MOUNT_FAILED) {
    mediaWaiting_ = true;
    probeFailures_ = 0;
    retryDelayMs_ = kMediaRetryBaseMs;
    Serial.println("[usb-storage] MEDIA_ABSENT — recovered transport, selected LUN still absent");
    return;
  }

  ++probeFailures_;
  retryDelayMs_ = nextBackoff(probeFailures_);
  mediaWaiting_ = true;
  Serial.printf("[usb-storage] MEDIA_RETRY — probe=%s recovery=%s backoff=%lums\n",
                esp_err_to_name(error), esp_err_to_name(recovery),
                static_cast<unsigned long>(retryDelayMs_));
}

bool NewoUsbStorage::mountFilesystem() {
  if (device_ == nullptr || !msc_host_media_ready(device_)) return false;

  uint8_t activeLun = 0;
  uint8_t maxLun = 0;
  const esp_err_t lunState = newo_msc_get_lun_state(device_, &activeLun, &maxLun);

  msc_host_device_info_t info = {};
  if (msc_host_get_device_info(device_, &info) == ESP_OK) {
    const uint64_t capacity = static_cast<uint64_t>(info.sector_count) * info.sector_size;
    if (lunState == ESP_OK) {
      Serial.printf("[usb-storage] capacity=%" PRIu64 " sector=%" PRIu32 " lun=%u\n",
                    capacity, info.sector_size, static_cast<unsigned>(activeLun));
    } else {
      Serial.printf("[usb-storage] capacity=%" PRIu64 " sector=%" PRIu32 "\n",
                    capacity, info.sector_size);
    }
  }

  esp_vfs_fat_mount_config_t mountConfig = {};
  mountConfig.format_if_mount_failed = false;
  mountConfig.max_files = 8;
  mountConfig.allocation_unit_size = 8192;

  msc_host_vfs_handle_t vfs = nullptr;
  const esp_err_t error = msc_host_vfs_register(device_, "/usb", &mountConfig, &vfs);
  if (error != ESP_OK) {
    logError("MOUNT_FAILED", error);
    // Keep the USB transport claimed. A transient FatFS failure must not force
    // re-enumeration or disturb other USB classes.
    mediaWaiting_ = true;
    ++probeFailures_;
    retryDelayMs_ = nextBackoff(probeFailures_);
    return false;
  }

  vfs_ = vfs;
  mounted_ = true;
  ++mountGeneration_;
  Serial.printf("[usb-storage] MOUNTED — path=/usb generation=%lu\n",
                static_cast<unsigned long>(mountGeneration_));

  // The validation intentionally runs in this same worker. Disconnect callbacks
  // can mark the transport gone immediately, but VFS teardown cannot race a
  // still-executing stdio call. Once the read returns, the queued disconnect is
  // processed and normal invalidation/release continues.
  runPhysicalValidation(device_);
  return true;
}

void NewoUsbStorage::invalidateFilesystem(const char* reason) {
  if (!mounted_ && vfs_ == nullptr) return;

  // Publish invalidation before touching FatFS so new script/file work refuses
  // this mount immediately. Long-running scripts should execute from a RAM/PSRAM
  // snapshot rather than hold the USB file open for their whole lifetime.
  mounted_ = false;
  ++mountGeneration_;
  Serial.printf("[usb-storage] INVALIDATED — reason=%s generation=%lu\n",
                reason ? reason : "unknown",
                static_cast<unsigned long>(mountGeneration_));

  if (vfs_ != nullptr) {
    const esp_err_t error = msc_host_vfs_unregister(vfs_);
    if (error != ESP_OK) logError("UNMOUNT_FAILED", error);
    vfs_ = nullptr;
    Serial.println("[usb-storage] UNMOUNTED");
  }
}

void NewoUsbStorage::handleDisconnected(msc_host_device_handle_t device) {
  if (device == nullptr || device != device_) return;
  Serial.println("[usb-storage] MSC_DISCONNECTED");
  mediaWaiting_ = false;
  releaseRetryPending_ = false;
  invalidateFilesystem("usb_disconnect");
  releaseDevice();
}

void NewoUsbStorage::releaseDevice() {
  mediaWaiting_ = false;
  invalidateFilesystem("device_release");
  if (device_ == nullptr) {
    releaseRetryPending_ = false;
    return;
  }

  const esp_err_t error = msc_host_uninstall_device(device_);
  if (error == ESP_OK || error == ESP_ERR_INVALID_STATE) {
    device_ = nullptr;
    releaseRetryPending_ = false;
    probeFailures_ = 0;
    retryDelayMs_ = kMediaRetryBaseMs;
    Serial.println("[usb-storage] TRANSPORT_RELEASED");
    return;
  }

  // Most importantly, never free a transfer object under an in-flight script or
  // FAT operation. The driver returns a bounded timeout; retry teardown later.
  releaseRetryPending_ = true;
  logError("MSC_RELEASE_DEFERRED", error);
}
