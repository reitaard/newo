// Temporary compile probe for the Alfred microWakeWord integration.
// This file deliberately does not change runtime behaviour. It proves that the
// Arduino-ESP32 3.3.10 toolchain exposes the TFLite Micro APIs required by the
// streaming Alfred model before we touch the working ESP-SR wake path.

#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace {
using NewoMwwProbeResolver = tflite::MicroMutableOpResolver<14>;

void newoMwwCompileProbe() {
  NewoMwwProbeResolver resolver;
  (void)resolver.AddCallOnce();
  (void)resolver.AddVarHandle();
  (void)resolver.AddReadVariable();
  (void)resolver.AddAssignVariable();
  (void)resolver.AddReshape();
  (void)resolver.AddConcatenation();
  (void)resolver.AddStridedSlice();
  (void)resolver.AddConv2D();
  (void)resolver.AddDepthwiseConv2D();
  (void)resolver.AddSplitV();
  (void)resolver.AddFullyConnected();
  (void)resolver.AddLogistic();
  (void)resolver.AddQuantize();

  // Type references intentionally mirror the Tater streaming implementation.
  tflite::MicroAllocator* allocator = nullptr;
  tflite::MicroResourceVariables* resources = nullptr;
  tflite::MicroInterpreter* interpreter = nullptr;
  const tflite::Model* model = nullptr;
  (void)allocator;
  (void)resources;
  (void)interpreter;
  (void)model;
}
}  // namespace
