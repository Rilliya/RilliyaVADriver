#include "RealtimeAudioRing.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace rilliya::audio_driver {
namespace {

constexpr std::size_t maximumChannelCount = 256;
constexpr std::size_t maximumCapacityFrames = 262144;
constexpr std::size_t maximumReaders = 256;

[[nodiscard]] bool multiplicationWouldOverflow(std::size_t left, std::size_t right) noexcept {
  return right != 0 && left > std::numeric_limits<std::size_t>::max() / right;
}

[[nodiscard]] std::uint64_t readyStamp(std::uint64_t absoluteFrame) noexcept {
  return (absoluteFrame << 1U) | 2U;
}

[[nodiscard]] std::uint64_t writingStamp(std::uint64_t absoluteFrame) noexcept {
  return (absoluteFrame << 1U) | 1U;
}

} // namespace

RealtimeAudioRing::RealtimeAudioRing(RealtimeAudioRingConfiguration configuration)
    : channelCount_(configuration.channelCount), capacityFrames_(configuration.capacityFrames),
      maximumReaderCount_(configuration.maximumReaderCount) {
  if (channelCount_ == 0 || channelCount_ > maximumChannelCount) {
    throw std::invalid_argument("channelCount must be between 1 and 256");
  }
  if (capacityFrames_ == 0 || capacityFrames_ > maximumCapacityFrames) {
    throw std::invalid_argument("capacityFrames must be between 1 and 262144");
  }
  if (maximumReaderCount_ == 0 || maximumReaderCount_ > maximumReaders) {
    throw std::invalid_argument("maximumReaderCount must be between 1 and 256");
  }
  if (multiplicationWouldOverflow(channelCount_, capacityFrames_)) {
    throw std::invalid_argument("sample capacity overflows size_t");
  }

  const std::size_t sampleCount = channelCount_ * capacityFrames_;
  sampleBits_ = std::make_unique<std::atomic<std::uint32_t>[]>(sampleCount);
  frameStamps_ = std::make_unique<std::atomic<std::uint64_t>[]>(capacityFrames_);
  readers_ = std::make_unique<ReaderState[]>(maximumReaderCount_);

  for (std::size_t index = 0; index < sampleCount; ++index) {
    sampleBits_[index].store(0, std::memory_order_relaxed);
  }
  for (std::size_t index = 0; index < capacityFrames_; ++index) {
    frameStamps_[index].store(0, std::memory_order_relaxed);
  }
}

std::size_t RealtimeAudioRing::channelCount() const noexcept { return channelCount_; }

std::size_t RealtimeAudioRing::capacityFrames() const noexcept { return capacityFrames_; }

std::size_t RealtimeAudioRing::maximumReaderCount() const noexcept { return maximumReaderCount_; }

std::uint64_t RealtimeAudioRing::publishedFrameCount() const noexcept {
  return publishedFrameCount_.load(std::memory_order_acquire);
}

std::optional<RealtimeAudioReaderToken> RealtimeAudioRing::registerReader() noexcept {
  for (std::size_t index = 0; index < maximumReaderCount_; ++index) {
    bool expected = false;
    if (!readers_[index].active.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                        std::memory_order_relaxed)) {
      continue;
    }

    const std::uint32_t generation = readers_[index].generation.load(std::memory_order_relaxed);
    readers_[index].cursor.store(publishedFrameCount(), std::memory_order_release);
    return RealtimeAudioReaderToken{static_cast<std::uint32_t>(index), generation};
  }

  return std::nullopt;
}

void RealtimeAudioRing::unregisterReader(RealtimeAudioReaderToken token) noexcept {
  if (!tokenIsValid(token)) {
    return;
  }

  ReaderState& reader = readers_[token.slot];
  reader.active.store(false, std::memory_order_release);
  reader.generation.fetch_add(1, std::memory_order_acq_rel);
}

bool RealtimeAudioRing::write(const float* interleavedSamples, std::size_t frameCount) noexcept {
  if (frameCount == 0) {
    return true;
  }
  if (interleavedSamples == nullptr || multiplicationWouldOverflow(frameCount, channelCount_)) {
    return false;
  }

  const std::uint64_t previousPublished = publishedFrameCount_.load(std::memory_order_relaxed);
  const std::size_t retainedFrames = std::min(frameCount, capacityFrames_);
  const std::size_t skippedFrames = frameCount - retainedFrames;
  const float* retainedSamples = interleavedSamples + skippedFrames * channelCount_;
  const std::uint64_t firstAbsoluteFrame = previousPublished + skippedFrames;

  for (std::size_t frameOffset = 0; frameOffset < retainedFrames; ++frameOffset) {
    const std::uint64_t absoluteFrame = firstAbsoluteFrame + frameOffset;
    const std::size_t ringFrame = static_cast<std::size_t>(absoluteFrame % capacityFrames_);
    frameStamps_[ringFrame].store(writingStamp(absoluteFrame), std::memory_order_release);

    const std::size_t sourceOffset = frameOffset * channelCount_;
    const std::size_t destinationOffset = ringFrame * channelCount_;
    for (std::size_t channel = 0; channel < channelCount_; ++channel) {
      const std::uint32_t bits =
          std::bit_cast<std::uint32_t>(retainedSamples[sourceOffset + channel]);
      sampleBits_[destinationOffset + channel].store(bits, std::memory_order_relaxed);
    }

    frameStamps_[ringFrame].store(readyStamp(absoluteFrame), std::memory_order_release);
  }

  publishedFrameCount_.store(previousPublished + frameCount, std::memory_order_release);
  return true;
}

RealtimeAudioReadResult RealtimeAudioRing::read(RealtimeAudioReaderToken token,
                                                float* interleavedDestination,
                                                std::size_t frameCount) noexcept {
  RealtimeAudioReadResult result;
  if (frameCount == 0) {
    result.readerIsValid = tokenIsValid(token);
    return result;
  }
  if (interleavedDestination == nullptr || multiplicationWouldOverflow(frameCount, channelCount_)) {
    return result;
  }
  if (!tokenIsValid(token)) {
    zeroFrames(interleavedDestination, frameCount);
    result.silentFrames = frameCount;
    return result;
  }

  result.readerIsValid = true;
  ReaderState& reader = readers_[token.slot];
  const std::uint64_t published = publishedFrameCount();
  const std::uint64_t earliest = published > capacityFrames_ ? published - capacityFrames_ : 0;
  std::uint64_t cursor = reader.cursor.load(std::memory_order_acquire);

  if (cursor < earliest) {
    result.droppedFrames = earliest - cursor;
    cursor = earliest;
  }
  if (cursor > published) {
    cursor = published;
  }

  const std::uint64_t available = published - cursor;
  const std::size_t readableFrames = static_cast<std::size_t>(
      std::min<std::uint64_t>(available, static_cast<std::uint64_t>(frameCount)));

  for (std::size_t frameOffset = 0; frameOffset < readableFrames; ++frameOffset) {
    float* destination = interleavedDestination + frameOffset * channelCount_;
    if (copyFrame(cursor + frameOffset, destination)) {
      ++result.audioFrames;
    } else {
      zeroFrames(destination, 1);
      ++result.silentFrames;
      ++result.droppedFrames;
    }
  }

  const std::size_t trailingSilence = frameCount - readableFrames;
  zeroFrames(interleavedDestination + readableFrames * channelCount_, trailingSilence);
  result.silentFrames += trailingSilence;
  reader.cursor.store(cursor + readableFrames, std::memory_order_release);
  return result;
}

bool RealtimeAudioRing::tokenIsValid(RealtimeAudioReaderToken token) const noexcept {
  if (token.slot >= maximumReaderCount_) {
    return false;
  }

  const ReaderState& reader = readers_[token.slot];
  return reader.active.load(std::memory_order_acquire) &&
         reader.generation.load(std::memory_order_acquire) == token.generation;
}

bool RealtimeAudioRing::copyFrame(std::uint64_t absoluteFrame, float* destination) const noexcept {
  const std::size_t ringFrame = static_cast<std::size_t>(absoluteFrame % capacityFrames_);
  const std::uint64_t expectedStamp = readyStamp(absoluteFrame);
  if (frameStamps_[ringFrame].load(std::memory_order_acquire) != expectedStamp) {
    return false;
  }

  const std::size_t sourceOffset = ringFrame * channelCount_;
  for (std::size_t channel = 0; channel < channelCount_; ++channel) {
    const std::uint32_t bits = sampleBits_[sourceOffset + channel].load(std::memory_order_relaxed);
    destination[channel] = std::bit_cast<float>(bits);
  }

  return frameStamps_[ringFrame].load(std::memory_order_acquire) == expectedStamp;
}

void RealtimeAudioRing::zeroFrames(float* destination, std::size_t frameCount) const noexcept {
  if (frameCount == 0) {
    return;
  }
  std::fill_n(destination, frameCount * channelCount_, 0.0F);
}

} // namespace rilliya::audio_driver
