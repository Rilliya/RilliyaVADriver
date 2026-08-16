#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace rilliya::audio_driver {

struct RealtimeAudioRingConfiguration final {
  std::size_t channelCount = 2;
  std::size_t capacityFrames = 8192;
  std::size_t maximumReaderCount = 32;
};

struct RealtimeAudioReaderToken final {
  std::uint32_t slot = 0;
  std::uint32_t generation = 0;

  [[nodiscard]] bool operator==(const RealtimeAudioReaderToken&) const noexcept = default;
};

struct RealtimeAudioReadResult final {
  std::size_t audioFrames = 0;
  std::size_t silentFrames = 0;
  std::uint64_t droppedFrames = 0;
  bool readerIsValid = false;
};

/// A bounded, allocation-free realtime audio transport with one writer and independent readers.
///
/// Samples are interleaved Float32 values. Construction and reader registration are control-plane
/// operations. `write`, `read`, and `unregisterReader` do not allocate, block, or acquire locks.
/// The caller must serialize writes. Each reader owns an independent cursor, so one client cannot
/// consume another client's audio.
class RealtimeAudioRing final {
public:
  explicit RealtimeAudioRing(RealtimeAudioRingConfiguration configuration);
  ~RealtimeAudioRing() = default;

  RealtimeAudioRing(const RealtimeAudioRing&) = delete;
  RealtimeAudioRing& operator=(const RealtimeAudioRing&) = delete;
  RealtimeAudioRing(RealtimeAudioRing&&) = delete;
  RealtimeAudioRing& operator=(RealtimeAudioRing&&) = delete;

  [[nodiscard]] std::size_t channelCount() const noexcept;
  [[nodiscard]] std::size_t capacityFrames() const noexcept;
  [[nodiscard]] std::size_t maximumReaderCount() const noexcept;
  [[nodiscard]] std::uint64_t publishedFrameCount() const noexcept;

  [[nodiscard]] std::optional<RealtimeAudioReaderToken> registerReader() noexcept;
  void unregisterReader(RealtimeAudioReaderToken token) noexcept;

  /// Publishes the newest frames. If a write is larger than the capacity, only its newest suffix
  /// is retained while the absolute timeline still advances by the full frame count.
  [[nodiscard]] bool write(const float* interleavedSamples, std::size_t frameCount) noexcept;

  /// Reads up to `frameCount` frames and zero-fills any unavailable or concurrently overwritten
  /// frames. The result separates real audio, silence, and frames dropped because a reader lagged.
  [[nodiscard]] RealtimeAudioReadResult read(RealtimeAudioReaderToken token,
                                             float* interleavedDestination,
                                             std::size_t frameCount) noexcept;

private:
  struct ReaderState final {
    std::atomic<std::uint64_t> cursor{0};
    std::atomic<std::uint32_t> generation{1};
    std::atomic<bool> active{false};
  };

  [[nodiscard]] bool tokenIsValid(RealtimeAudioReaderToken token) const noexcept;
  [[nodiscard]] bool copyFrame(std::uint64_t absoluteFrame, float* destination) const noexcept;
  void zeroFrames(float* destination, std::size_t frameCount) const noexcept;

  std::size_t channelCount_;
  std::size_t capacityFrames_;
  std::size_t maximumReaderCount_;
  std::unique_ptr<std::atomic<std::uint32_t>[]> sampleBits_;
  std::unique_ptr<std::atomic<std::uint64_t>[]> frameStamps_;
  std::unique_ptr<ReaderState[]> readers_;
  alignas(64) std::atomic<std::uint64_t> publishedFrameCount_{0};
};

} // namespace rilliya::audio_driver
