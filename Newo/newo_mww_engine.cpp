#include "newo_wake_engine.h"

#include <algorithm>
#include <cstring>
#include <new>

#include <esp_heap_caps.h>

#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

extern "C" {
#if __has_include("frontend.h")
#include "frontend.h"
#include "frontend_util.h"
#elif __has_include("tensorflow/lite/experimental/microfrontend/lib/frontend.h")
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"
#else
#error "Newo Alfred wake word requires the TFLM microfrontend headers"
#endif
}

#include "newo_config.h"
#include "newo_log.h"

extern "C" const uint8_t newo_alfred_tflite_start[];
extern "C" const uint8_t newo_alfred_tflite_end[];

namespace {
constexpr int kSampleRate = 16000;
constexpr int kFeatureDurationMs = 30;
constexpr int kFeatureStepMs = 10;
constexpr int kFeatureSize = 40;
constexpr int kInputFrames = 2;
constexpr int kInputElements = kFeatureSize * kInputFrames;
constexpr size_t kSlidingWindow = 5;
constexpr uint8_t kProbabilityCutoff = static_cast<uint8_t>(0.95f * 255.0f + 0.5f);
constexpr size_t kArenaBytes = 192 * 1024;
constexpr int kResourceVariables = 8;
constexpr int32_t kFrontendValueScale = 256;
constexpr int32_t kFrontendValueDiv = 666;

using WakeOpResolver = tflite::MicroMutableOpResolver<14>;

WakeOpResolver gResolver;
bool gResolverReady = false;
uint8_t* gArena = nullptr;
const tflite::Model* gModel = nullptr;
tflite::MicroAllocator* gAllocator = nullptr;
tflite::MicroResourceVariables* gResources = nullptr;
alignas(tflite::MicroInterpreter) uint8_t gInterpreterStorage[sizeof(tflite::MicroInterpreter)];
tflite::MicroInterpreter* gInterpreter = nullptr;
bool gInterpreterConstructed = false;
FrontendConfig gFrontendConfig;
FrontendState gFrontendState;
bool gFrontendReady = false;
int8_t gFeatureHistory[kInputElements] = {};
size_t gFeatureFrames = 0;
uint8_t gScores[kSlidingWindow] = {};
size_t gScoreCount = 0;
size_t gScoreIndex = 0;

bool registerOps() {
  if (gResolverReady) return true;
  bool ok = true;
  ok &= gResolver.AddCallOnce() == kTfLiteOk;
  ok &= gResolver.AddVarHandle() == kTfLiteOk;
  ok &= gResolver.AddReadVariable() == kTfLiteOk;
  ok &= gResolver.AddAssignVariable() == kTfLiteOk;
  ok &= gResolver.AddReshape() == kTfLiteOk;
  ok &= gResolver.AddConcatenation() == kTfLiteOk;
  ok &= gResolver.AddStridedSlice() == kTfLiteOk;
  ok &= gResolver.AddConv2D() == kTfLiteOk;
  ok &= gResolver.AddDepthwiseConv2D() == kTfLiteOk;
  ok &= gResolver.AddSplitV() == kTfLiteOk;
  ok &= gResolver.AddFullyConnected() == kTfLiteOk;
  ok &= gResolver.AddLogistic() == kTfLiteOk;
  ok &= gResolver.AddQuantize() == kTfLiteOk;
  gResolverReady = ok;
  return ok;
}

void resetDetectorState() {
  std::memset(gFeatureHistory, 0, sizeof(gFeatureHistory));
  std::memset(gScores, 0, sizeof(gScores));
  gFeatureFrames = 0;
  gScoreCount = 0;
  gScoreIndex = 0;
  if (gFrontendReady) FrontendReset(&gFrontendState);
}

void destroyRuntime() {
  if (gInterpreterConstructed && gInterpreter) gInterpreter->~MicroInterpreter();
  gInterpreter = nullptr;
  gInterpreterConstructed = false;
  gAllocator = nullptr;
  gResources = nullptr;
  gModel = nullptr;
  if (gFrontendReady) {
    FrontendFreeStateContents(&gFrontendState);
    gFrontendReady = false;
  }
  if (gArena) {
    heap_caps_free(gArena);
    gArena = nullptr;
  }
}

bool initRuntime() {
  destroyRuntime();
  if (!registerOps()) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "MWW_OPS_FAILED");
    return false;
  }

  FrontendFillConfigWithDefaults(&gFrontendConfig);
  gFrontendConfig.window.size_ms = kFeatureDurationMs;
  gFrontendConfig.window.step_size_ms = kFeatureStepMs;
  gFrontendConfig.filterbank.num_channels = kFeatureSize;
  gFrontendConfig.filterbank.lower_band_limit = 125.0f;
  gFrontendConfig.filterbank.upper_band_limit = 7500.0f;
  gFrontendConfig.noise_reduction.smoothing_bits = 10;
  gFrontendConfig.noise_reduction.even_smoothing = 0.025f;
  gFrontendConfig.noise_reduction.odd_smoothing = 0.06f;
  gFrontendConfig.noise_reduction.min_signal_remaining = 0.05f;
  gFrontendConfig.pcan_gain_control.enable_pcan = true;
  gFrontendConfig.pcan_gain_control.strength = 0.95f;
  gFrontendConfig.pcan_gain_control.offset = 80.0f;
  gFrontendConfig.pcan_gain_control.gain_bits = 21;
  gFrontendConfig.log_scale.enable_log = true;
  gFrontendConfig.log_scale.scale_shift = 6;
  if (!FrontendPopulateState(&gFrontendConfig, &gFrontendState, kSampleRate)) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "MWW_FRONTEND_FAILED");
    return false;
  }
  gFrontendReady = true;

  gArena = static_cast<uint8_t*>(heap_caps_aligned_alloc(
      16, kArenaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!gArena) {
    gArena = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, kArenaBytes, MALLOC_CAP_8BIT));
  }
  if (!gArena) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "MWW_ARENA_FAILED");
    destroyRuntime();
    return false;
  }
  std::memset(gArena, 0, kArenaBytes);

  const size_t modelBytes = static_cast<size_t>(newo_alfred_tflite_end - newo_alfred_tflite_start);
  gModel = tflite::GetModel(newo_alfred_tflite_start);
  if (!gModel || gModel->version() != TFLITE_SCHEMA_VERSION) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "MWW_SCHEMA_FAILED");
    destroyRuntime();
    return false;
  }
  gAllocator = tflite::MicroAllocator::Create(gArena, kArenaBytes);
  if (!gAllocator) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "MWW_ALLOCATOR_FAILED");
    destroyRuntime();
    return false;
  }
  gResources = tflite::MicroResourceVariables::Create(gAllocator, kResourceVariables);
  if (!gResources) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "MWW_RESOURCES_FAILED");
    destroyRuntime();
    return false;
  }
  gInterpreter = new (gInterpreterStorage)
      tflite::MicroInterpreter(gModel, gResolver, gAllocator, gResources);
  gInterpreterConstructed = true;
  if (gInterpreter->AllocateTensors() != kTfLiteOk) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "MWW_TENSORS_FAILED");
    destroyRuntime();
    return false;
  }

  TfLiteTensor* input = gInterpreter->input(0);
  TfLiteTensor* output = gInterpreter->output(0);
  if (!input || input->type != kTfLiteInt8 || input->dims->size != 3 ||
      input->dims->data[0] != 1 || input->dims->data[1] != kInputFrames ||
      input->dims->data[2] != kFeatureSize || !output || output->type != kTfLiteUInt8 ||
      output->dims->size != 2 || output->dims->data[0] != 1 || output->dims->data[1] != 1) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "MWW_TENSOR_SHAPE_FAILED");
    destroyRuntime();
    return false;
  }

  resetDetectorState();
  char detail[96];
  snprintf(detail, sizeof(detail), "model=alfred bytes=%u arena=%u input=1x2x40 threshold=0.95 window=5",
           static_cast<unsigned>(modelBytes), static_cast<unsigned>(kArenaBytes));
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "MWW_READY", detail);
  return true;
}

bool convertFeature(const FrontendOutput& output, int8_t feature[kFeatureSize]) {
  if (!output.values || output.size != kFeatureSize) return false;
  for (int i = 0; i < kFeatureSize; ++i) {
    int32_t value = ((output.values[i] * kFrontendValueScale) + (kFrontendValueDiv / 2)) /
                    kFrontendValueDiv;
    value += INT8_MIN;
    value = std::min<int32_t>(std::max<int32_t>(value, INT8_MIN), INT8_MAX);
    feature[i] = static_cast<int8_t>(value);
  }
  return true;
}

bool handleFeature(const int8_t feature[kFeatureSize]) {
  const size_t slot = gFeatureFrames % kInputFrames;
  std::memcpy(gFeatureHistory + slot * kFeatureSize, feature, kFeatureSize);
  ++gFeatureFrames;
  if ((gFeatureFrames % kInputFrames) != 0) return false;

  TfLiteTensor* input = gInterpreter->input(0);
  std::memcpy(input->data.int8, gFeatureHistory, kInputElements);
  if (gInterpreter->Invoke() != kTfLiteOk) return false;

  const uint8_t score = gInterpreter->output(0)->data.uint8[0];
  gScores[gScoreIndex] = score;
  gScoreIndex = (gScoreIndex + 1) % kSlidingWindow;
  if (gScoreCount < kSlidingWindow) ++gScoreCount;
  if (gScoreCount < kSlidingWindow) return false;

  uint32_t sum = 0;
  for (uint8_t value : gScores) sum += value;
  return sum > static_cast<uint32_t>(kProbabilityCutoff) * kSlidingWindow;
}

bool processPcm(const int16_t* pcm, size_t sampleCount) {
  size_t offset = 0;
  while (offset < sampleCount) {
    size_t processed = 0;
    FrontendOutput output = FrontendProcessSamples(
        &gFrontendState, pcm + offset, sampleCount - offset, &processed);
    if (processed == 0 && output.size == 0) break;
    offset += processed;
    if (output.size == 0) continue;
    int8_t feature[kFeatureSize] = {};
    if (convertFeature(output, feature) && handleFeature(feature)) return true;
  }
  return false;
}
}  // namespace

bool NewoMicroWakeWordEngine::start(I2SClass& i2s, sr_cb callback) {
  if (running_) return true;
  if (!callback || NewoConfig::AUDIO_SAMPLE_RATE != kSampleRate) return false;
  if (!initRuntime()) return false;

  i2s_ = &i2s;
  callback_ = callback;
  stopRequested_ = false;
  running_ = true;
  if (xTaskCreatePinnedToCore(taskEntry, "newo-mww", 8192, this, 3, &task_, 0) != pdPASS) {
    running_ = false;
    task_ = nullptr;
    callback_ = nullptr;
    i2s_ = nullptr;
    destroyRuntime();
    return false;
  }
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "MWW_ARMED", "model=alfred");
  return true;
}

void NewoMicroWakeWordEngine::stop() {
  if (!running_ && !task_) return;
  stopRequested_ = true;
  const uint32_t deadline = millis() + 1500;
  while (task_ && static_cast<int32_t>(millis() - deadline) < 0) vTaskDelay(pdMS_TO_TICKS(1));
  if (task_) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "MWW_STOP_TIMEOUT");
    vTaskDelete(task_);
    task_ = nullptr;
  }
  running_ = false;
  callback_ = nullptr;
  i2s_ = nullptr;
  destroyRuntime();
}

void NewoMicroWakeWordEngine::taskEntry(void* context) {
  static_cast<NewoMicroWakeWordEngine*>(context)->task();
}

void NewoMicroWakeWordEngine::task() {
  int16_t stereo[NewoConfig::AUDIO_SAMPLES_PER_FRAME * 2];
  int16_t mono[NewoConfig::AUDIO_SAMPLES_PER_FRAME];

  while (!stopRequested_) {
    const size_t bytes = i2s_->readBytes(reinterpret_cast<char*>(stereo), sizeof(stereo));
    if (stopRequested_) break;
    if (bytes != sizeof(stereo)) continue;
    for (size_t i = 0; i < NewoConfig::AUDIO_SAMPLES_PER_FRAME; ++i) {
      mono[i] = stereo[i * 2 + (NewoConfig::AUDIO_I2S_MIC_IS_LEFT ? 0 : 1)];
    }
    if (processPcm(mono, NewoConfig::AUDIO_SAMPLES_PER_FRAME)) {
      NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "MWW_DETECTED", "model=alfred");
      if (callback_) callback_(SR_EVENT_WAKEWORD, 0, 0);
      resetDetectorState();
      vTaskDelay(pdMS_TO_TICKS(250));
    }
  }

  running_ = false;
  task_ = nullptr;
  vTaskDelete(nullptr);
}
