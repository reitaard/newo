#include "newo_mww_engine.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <new>

#include <esp_heap_caps.h>

#include "newo_alfred_model.h"
#include "newo_config.h"
#include "newo_log.h"

#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/version.h"

extern "C" {
#include "src/tflm_microfrontend/frontend.h"
#include "src/tflm_microfrontend/frontend_util.h"
}

namespace {
constexpr size_t kTensorArenaBytes = 192 * 1024;
constexpr int kResourceVariables = 8;
constexpr int kFeatureSize = 40;
constexpr int kInputFeatureFrames = 2;
constexpr int kInputElements = kFeatureSize * kInputFeatureFrames;
constexpr int kSlidingWindow = 5;
constexpr float kProbabilityCutoff = 0.97f;
constexpr size_t kExpectedModelBytes = 137984;
constexpr uint32_t kDetectorReadTimeoutMs = 100;
constexpr uint32_t kDetectorTaskStackBytes = 12 * 1024;

constexpr int32_t kFrontendValueScale = 256;
constexpr int32_t kFrontendValueDiv = 666;

using AlfredOpResolver = tflite::MicroMutableOpResolver<14>;

bool registerAlfredOps(AlfredOpResolver& resolver) {
  return resolver.AddCallOnce() == kTfLiteOk &&
         resolver.AddVarHandle() == kTfLiteOk &&
         resolver.AddReadVariable() == kTfLiteOk &&
         resolver.AddAssignVariable() == kTfLiteOk &&
         resolver.AddReshape() == kTfLiteOk &&
         resolver.AddConcatenation() == kTfLiteOk &&
         resolver.AddStridedSlice() == kTfLiteOk &&
         resolver.AddConv2D() == kTfLiteOk &&
         resolver.AddDepthwiseConv2D() == kTfLiteOk &&
         resolver.AddSplitV() == kTfLiteOk &&
         resolver.AddFullyConnected() == kTfLiteOk &&
         resolver.AddLogistic() == kTfLiteOk &&
         resolver.AddQuantize() == kTfLiteOk;
}

void logDetail(NewoLog::Level level, const char* event, const char* format,
               unsigned long a = 0, unsigned long b = 0, unsigned long c = 0) {
  char detail[128];
  snprintf(detail, sizeof(detail), format, a, b, c);
  NewoLog::log(level, NewoLog::Subsystem::AUDIO, event, detail);
}
}  // namespace

struct NewoMicroWakeEngine::Runtime {
  FrontendConfig frontendConfig{};
  FrontendState frontendState{};
  bool frontendReady = false;

  AlfredOpResolver resolver;
  uint8_t* tensorArena = nullptr;
  bool tensorArenaPsram = false;
  const tflite::Model* model = nullptr;
  tflite::MicroAllocator* allocator = nullptr;
  tflite::MicroResourceVariables* resources = nullptr;
  alignas(tflite::MicroInterpreter)
      uint8_t interpreterStorage[sizeof(tflite::MicroInterpreter)];
  tflite::MicroInterpreter* interpreter = nullptr;
  bool interpreterConstructed = false;

  int8_t featureHistory[kInputElements]{};
  size_t featureFrames = 0;
  uint8_t probabilities[kSlidingWindow]{};
  size_t probabilityCount = 0;
  size_t probabilityIndex = 0;
  uint32_t inferenceCount = 0;
};

bool NewoMicroWakeEngine::createRuntime() {
  destroyRuntime();
  runtime_ = new (std::nothrow) Runtime();
  if (!runtime_) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO,
                 "MWW_RUNTIME_ALLOC_FAILED");
    return false;
  }

  FrontendFillConfigWithDefaults(&runtime_->frontendConfig);
  runtime_->frontendConfig.window.size_ms = 30;
  runtime_->frontendConfig.window.step_size_ms = 10;
  runtime_->frontendConfig.filterbank.num_channels = kFeatureSize;
  runtime_->frontendConfig.filterbank.lower_band_limit = 125.0f;
  runtime_->frontendConfig.filterbank.upper_band_limit = 7500.0f;
  runtime_->frontendConfig.noise_reduction.smoothing_bits = 10;
  runtime_->frontendConfig.noise_reduction.even_smoothing = 0.025f;
  runtime_->frontendConfig.noise_reduction.odd_smoothing = 0.06f;
  runtime_->frontendConfig.noise_reduction.min_signal_remaining = 0.05f;
  runtime_->frontendConfig.pcan_gain_control.enable_pcan = true;
  runtime_->frontendConfig.pcan_gain_control.strength = 0.95f;
  runtime_->frontendConfig.pcan_gain_control.offset = 80.0f;
  runtime_->frontendConfig.pcan_gain_control.gain_bits = 21;
  runtime_->frontendConfig.log_scale.enable_log = true;
  runtime_->frontendConfig.log_scale.scale_shift = 6;
  if (!FrontendPopulateState(&runtime_->frontendConfig, &runtime_->frontendState,
                             NewoConfig::AUDIO_SAMPLE_RATE)) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO,
                 "MWW_FRONTEND_INIT_FAILED");
    destroyRuntime();
    return false;
  }
  runtime_->frontendReady = true;

  if (!registerAlfredOps(runtime_->resolver)) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO,
                 "MWW_OP_RESOLVER_FAILED");
    destroyRuntime();
    return false;
  }

  runtime_->tensorArena = static_cast<uint8_t*>(heap_caps_aligned_alloc(
      16, kTensorArenaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  runtime_->tensorArenaPsram = runtime_->tensorArena != nullptr;
  if (!runtime_->tensorArena) {
    runtime_->tensorArena = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        16, kTensorArenaBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (!runtime_->tensorArena) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO,
                 "MWW_ARENA_ALLOC_FAILED");
    destroyRuntime();
    return false;
  }
  memset(runtime_->tensorArena, 0, kTensorArenaBytes);

  const size_t modelBytes = newoAlfredModelSize();
  if (modelBytes != kExpectedModelBytes) {
    logDetail(NewoLog::Level::ERROR, "MWW_MODEL_SIZE_MISMATCH",
              "bytes=%lu expected=%lu", static_cast<unsigned long>(modelBytes),
              static_cast<unsigned long>(kExpectedModelBytes));
    destroyRuntime();
    return false;
  }

  runtime_->model = tflite::GetModel(newoAlfredModelData());
  if (!runtime_->model || runtime_->model->version() != TFLITE_SCHEMA_VERSION) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO,
                 "MWW_SCHEMA_FAILED");
    destroyRuntime();
    return false;
  }

  runtime_->allocator =
      tflite::MicroAllocator::Create(runtime_->tensorArena, kTensorArenaBytes);
  if (!runtime_->allocator) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO,
                 "MWW_ALLOCATOR_FAILED");
    destroyRuntime();
    return false;
  }
  runtime_->resources =
      tflite::MicroResourceVariables::Create(runtime_->allocator, kResourceVariables);
  if (!runtime_->resources) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO,
                 "MWW_RESOURCES_FAILED");
    destroyRuntime();
    return false;
  }

  runtime_->interpreter = new (runtime_->interpreterStorage) tflite::MicroInterpreter(
      runtime_->model, runtime_->resolver, runtime_->allocator, runtime_->resources);
  runtime_->interpreterConstructed = true;
  if (runtime_->interpreter->AllocateTensors() != kTfLiteOk) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO,
                 "MWW_ALLOCATE_TENSORS_FAILED");
    destroyRuntime();
    return false;
  }

  TfLiteTensor* input = runtime_->interpreter->input(0);
  TfLiteTensor* output = runtime_->interpreter->output(0);
  if (!input || !output || input->type != kTfLiteInt8 || output->type != kTfLiteUInt8 ||
      input->dims->size != 3 || input->dims->data[0] != 1 ||
      input->dims->data[1] != kInputFeatureFrames || input->dims->data[2] != kFeatureSize ||
      output->dims->size != 2 || output->dims->data[0] != 1 || output->dims->data[1] != 1) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO,
                 "MWW_TENSOR_SHAPE_FAILED");
    destroyRuntime();
    return false;
  }

  logDetail(NewoLog::Level::INFO, "MWW_MODEL_READY",
            "alfred bytes=%lu arena_used=%lu psram=%lu",
            static_cast<unsigned long>(modelBytes),
            static_cast<unsigned long>(runtime_->interpreter->arena_used_bytes()),
            runtime_->tensorArenaPsram ? 1UL : 0UL);
  return true;
}

void NewoMicroWakeEngine::destroyRuntime() {
  if (!runtime_) return;
  if (runtime_->interpreterConstructed && runtime_->interpreter) {
    runtime_->interpreter->~MicroInterpreter();
    runtime_->interpreter = nullptr;
    runtime_->interpreterConstructed = false;
  }
  if (runtime_->frontendReady) {
    FrontendFreeStateContents(&runtime_->frontendState);
    runtime_->frontendReady = false;
  }
  if (runtime_->tensorArena) {
    heap_caps_free(runtime_->tensorArena);
    runtime_->tensorArena = nullptr;
  }
  delete runtime_;
  runtime_ = nullptr;
}

bool NewoMicroWakeEngine::start(I2SClass& i2s, sr_cb callback) {
  if (running_) return true;
  if (!callback) return false;

  i2s_ = &i2s;
  callback_ = callback;
  stopRequested_ = false;
  taskFinished_ = false;
  wakeLatched_ = false;

  if (!createRuntime()) {
    taskFinished_ = true;
    i2s_ = nullptr;
    callback_ = nullptr;
    return false;
  }

  // NewoAudio previously restores a 1 s timeout for ESP-SR. A shorter detector
  // timeout lets stop() hand I2S to direct streaming promptly.
  i2s_->setTimeout(kDetectorReadTimeoutMs);
  if (xTaskCreatePinnedToCore(taskEntry, "newo-mww", kDetectorTaskStackBytes, this,
                              3, &task_, 1) != pdPASS) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO,
                 "MWW_TASK_CREATE_FAILED");
    taskFinished_ = true;
    task_ = nullptr;
    destroyRuntime();
    i2s_ = nullptr;
    callback_ = nullptr;
    return false;
  }

  running_ = true;
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO,
               "MWW_ARMED", "alfred cutoff=0.97 window=5");
  return true;
}

void NewoMicroWakeEngine::stop() {
  if (!running_ && taskFinished_) {
    destroyRuntime();
    return;
  }
  stopRequested_ = true;
  const uint32_t started = millis();
  while (!taskFinished_ && millis() - started < 1500) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (!taskFinished_) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO,
                 "MWW_STOP_TIMEOUT");
    // Do not free a runtime that a stuck task may still be using.
    return;
  }
  running_ = false;
  task_ = nullptr;
  destroyRuntime();
  i2s_ = nullptr;
  callback_ = nullptr;
}

void NewoMicroWakeEngine::taskEntry(void* context) {
  static_cast<NewoMicroWakeEngine*>(context)->task();
}

void NewoMicroWakeEngine::task() {
  int16_t stereo[NewoConfig::AUDIO_SAMPLES_PER_FRAME * 2];
  int16_t mono[NewoConfig::AUDIO_SAMPLES_PER_FRAME];
  int8_t feature[kFeatureSize];

  while (!stopRequested_) {
    const size_t expectedBytes = sizeof(stereo);
    const size_t readBytes =
        i2s_ ? i2s_->readBytes(reinterpret_cast<char*>(stereo), expectedBytes) : 0;
    if (stopRequested_) break;
    if (readBytes != expectedBytes) continue;

    for (size_t i = 0; i < NewoConfig::AUDIO_SAMPLES_PER_FRAME; ++i) {
      mono[i] = stereo[i * 2 + (NewoConfig::AUDIO_I2S_MIC_IS_LEFT ? 0 : 1)];
    }

    size_t offset = 0;
    while (!stopRequested_ && offset < NewoConfig::AUDIO_SAMPLES_PER_FRAME) {
      size_t processed = 0;
      const FrontendOutput frontendOutput = FrontendProcessSamples(
          &runtime_->frontendState, mono + offset,
          NewoConfig::AUDIO_SAMPLES_PER_FRAME - offset, &processed);
      if (processed == 0) break;
      offset += processed;
      if (!frontendOutput.values || frontendOutput.size == 0) continue;
      if (frontendOutput.size != kFeatureSize) {
        NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO,
                     "MWW_FEATURE_SIZE_FAILED");
        stopRequested_ = true;
        break;
      }

      for (int i = 0; i < kFeatureSize; ++i) {
        int32_t value =
            ((static_cast<int32_t>(frontendOutput.values[i]) * kFrontendValueScale) +
             (kFrontendValueDiv / 2)) /
            kFrontendValueDiv;
        value += INT8_MIN;
        value = std::max<int32_t>(INT8_MIN, std::min<int32_t>(INT8_MAX, value));
        feature[i] = static_cast<int8_t>(value);
      }

      const size_t featureSlot = runtime_->featureFrames % kInputFeatureFrames;
      memcpy(runtime_->featureHistory + featureSlot * kFeatureSize, feature, kFeatureSize);
      ++runtime_->featureFrames;
      if ((runtime_->featureFrames % kInputFeatureFrames) != 0) continue;

      TfLiteTensor* input = runtime_->interpreter->input(0);
      TfLiteTensor* output = runtime_->interpreter->output(0);
      memcpy(input->data.int8, runtime_->featureHistory, kInputElements);
      if (runtime_->interpreter->Invoke() != kTfLiteOk) {
        NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO,
                     "MWW_INVOKE_FAILED");
        stopRequested_ = true;
        break;
      }
      ++runtime_->inferenceCount;

      const uint8_t raw = output->data.uint8[0];
      runtime_->probabilities[runtime_->probabilityIndex] = raw;
      runtime_->probabilityIndex =
          (runtime_->probabilityIndex + 1) % kSlidingWindow;
      if (runtime_->probabilityCount < kSlidingWindow) ++runtime_->probabilityCount;

      // Wait for a complete calibrated window before allowing the first wake.
      if (runtime_->probabilityCount < kSlidingWindow || wakeLatched_) continue;
      uint32_t sum = 0;
      for (uint8_t probability : runtime_->probabilities) sum += probability;
      const float average =
          static_cast<float>(sum) / (255.0f * static_cast<float>(kSlidingWindow));
      if (average >= kProbabilityCutoff) {
        wakeLatched_ = true;
        char detail[96];
        snprintf(detail, sizeof(detail), "alfred raw=%u avg=%.3f inference=%lu", raw,
                 static_cast<double>(average),
                 static_cast<unsigned long>(runtime_->inferenceCount));
        NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO,
                     "MWW_WAKE", detail);
        if (callback_) callback_(SR_EVENT_WAKEWORD, 0, 0);
      }
    }
  }

  taskFinished_ = true;
  task_ = nullptr;
  vTaskDelete(nullptr);
}
