#pragma once

namespace NewoUsbHostLimits {
constexpr unsigned kRxFifoLines = 72;
constexpr unsigned kNptxFifoLines = 32;
constexpr unsigned kPtxFifoLines = 96;
constexpr unsigned kFifoLinesTotal = kRxFifoLines + kNptxFifoLines + kPtxFifoLines;
constexpr unsigned kMaxPeriodicOutBytes = kPtxFifoLines * 4;
constexpr unsigned kMaxNonPeriodicOutBytes = kNptxFifoLines * 4;
constexpr unsigned kMaxInPacketBytes = (kRxFifoLines - 2) * 4;
constexpr unsigned kHostChannels = 8;
constexpr unsigned kEnumerationChannelReserve = 1;
static_assert(kFifoLinesTotal == 200, "ESP32-S3 USB host FIFO budget");
static_assert(kHostChannels == 8, "ESP32-S3 USB host channel budget");
}  // namespace NewoUsbHostLimits
