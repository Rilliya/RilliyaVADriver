#include "RealtimeAudioRing.hpp"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using rilliya::audio_driver::RealtimeAudioReaderToken;
using rilliya::audio_driver::RealtimeAudioReadResult;
using rilliya::audio_driver::RealtimeAudioRing;
using rilliya::audio_driver::RealtimeAudioRingConfiguration;

class TestFailure final : public std::runtime_error {
public:
  explicit TestFailure(const char* message) : std::runtime_error(message) {}
};

void expect(bool condition, const char* message) {
  if (!condition) {
    throw TestFailure(message);
  }
}

void expectSamples(const std::vector<float>& actual, const std::vector<float>& expected,
                   const char* message) {
  expect(actual.size() == expected.size(), message);
  for (std::size_t index = 0; index < actual.size(); ++index) {
    if (std::abs(actual[index] - expected[index]) > 0.000001F) {
      throw TestFailure(message);
    }
  }
}

RealtimeAudioReaderToken requireReader(RealtimeAudioRing& ring) {
  auto reader = ring.registerReader();
  expect(reader.has_value(), "reader registration should succeed");
  return *reader;
}

void testRejectsInvalidConfiguration() {
  bool rejected = false;
  try {
    RealtimeAudioRing ring({.channelCount = 0});
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  expect(rejected, "zero channels should be rejected");

  rejected = false;
  try {
    RealtimeAudioRing ring({.maximumReaderCount = 0});
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  expect(rejected, "zero readers should be rejected");
}

void testReaderReceivesInterleavedFrames() {
  RealtimeAudioRing ring({.channelCount = 2, .capacityFrames = 8, .maximumReaderCount = 2});
  const RealtimeAudioReaderToken reader = requireReader(ring);
  const std::vector<float> source{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
  expect(ring.write(source.data(), 3), "write should succeed");

  std::vector<float> destination(6, -1.0F);
  const RealtimeAudioReadResult result = ring.read(reader, destination.data(), 3);
  expect(result.readerIsValid, "reader should remain valid");
  expect(result.audioFrames == 3, "three frames should contain audio");
  expect(result.silentFrames == 0, "no frames should be silent");
  expect(result.droppedFrames == 0, "no frames should be dropped");
  expectSamples(destination, source, "interleaved samples should be preserved");
}

void testReadersHaveIndependentCursors() {
  RealtimeAudioRing ring({.channelCount = 1, .capacityFrames = 8, .maximumReaderCount = 2});
  const RealtimeAudioReaderToken first = requireReader(ring);
  const RealtimeAudioReaderToken second = requireReader(ring);
  const std::vector<float> source{1.0F, 2.0F, 3.0F, 4.0F};
  expect(ring.write(source.data(), source.size()), "write should succeed");

  std::vector<float> firstHalf(2);
  expect(ring.read(first, firstHalf.data(), 2).audioFrames == 2,
         "first reader should consume two frames");
  expectSamples(firstHalf, {1.0F, 2.0F}, "first reader should start at the first frame");

  std::vector<float> entireBlock(4);
  expect(ring.read(second, entireBlock.data(), 4).audioFrames == 4,
         "second reader should retain its own cursor");
  expectSamples(entireBlock, source, "second reader should receive the entire block");

  std::vector<float> secondHalf(2);
  expect(ring.read(first, secondHalf.data(), 2).audioFrames == 2,
         "first reader should continue from its own cursor");
  expectSamples(secondHalf, {3.0F, 4.0F}, "first reader should receive its remaining frames");
}

void testLaggingReaderReportsOverrunAndNewestAudio() {
  RealtimeAudioRing ring({.channelCount = 1, .capacityFrames = 4, .maximumReaderCount = 1});
  const RealtimeAudioReaderToken reader = requireReader(ring);
  const std::vector<float> source{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
  expect(ring.write(source.data(), source.size()), "oversized write should succeed");

  std::vector<float> destination(4);
  const RealtimeAudioReadResult result = ring.read(reader, destination.data(), 4);
  expect(result.audioFrames == 4, "capacity-sized suffix should remain available");
  expect(result.droppedFrames == 2, "discarded prefix should be reported");
  expectSamples(destination, {3.0F, 4.0F, 5.0F, 6.0F},
                "reader should receive the newest retained suffix");
}

void testUnavailableFramesBecomeSilenceWithoutAdvancingPastPublishedAudio() {
  RealtimeAudioRing ring({.channelCount = 2, .capacityFrames = 4, .maximumReaderCount = 1});
  const RealtimeAudioReaderToken reader = requireReader(ring);
  std::vector<float> destination(4, 1.0F);

  const RealtimeAudioReadResult empty = ring.read(reader, destination.data(), 2);
  expect(empty.audioFrames == 0, "empty ring should not report audio");
  expect(empty.silentFrames == 2, "empty ring should fill silence");
  expectSamples(destination, {0.0F, 0.0F, 0.0F, 0.0F}, "empty ring should zero output");

  const std::vector<float> source{0.25F, 0.5F};
  expect(ring.write(source.data(), 1), "subsequent write should succeed");
  const RealtimeAudioReadResult later = ring.read(reader, destination.data(), 2);
  expect(later.audioFrames == 1, "reader should still receive newly published audio");
  expect(later.silentFrames == 1, "remaining frame should be silent");
  expectSamples(destination, {0.25F, 0.5F, 0.0F, 0.0F}, "audio should precede trailing silence");
}

void testUnregisteredTokenCannotReadReusedSlot() {
  RealtimeAudioRing ring({.channelCount = 1, .capacityFrames = 4, .maximumReaderCount = 1});
  const RealtimeAudioReaderToken stale = requireReader(ring);
  ring.unregisterReader(stale);
  const RealtimeAudioReaderToken replacement = requireReader(ring);
  expect(stale.generation != replacement.generation, "slot reuse should advance its generation");

  const float sample = 0.75F;
  expect(ring.write(&sample, 1), "write should succeed");
  float destination = 1.0F;
  const RealtimeAudioReadResult staleResult = ring.read(stale, &destination, 1);
  expect(!staleResult.readerIsValid, "stale token should be invalid");
  expect(destination == 0.0F, "invalid reader output should be safe silence");

  const RealtimeAudioReadResult replacementResult = ring.read(replacement, &destination, 1);
  expect(replacementResult.audioFrames == 1, "replacement reader should receive audio");
  expect(destination == sample, "replacement reader should receive the published sample");
}

void testConcurrentWriterAndReadersRemainBoundedAndDataRaceFree() {
  constexpr std::size_t framesPerWrite = 32;
  constexpr std::size_t writeCount = 10000;
  RealtimeAudioRing ring({.channelCount = 2, .capacityFrames = 512, .maximumReaderCount = 2});
  const RealtimeAudioReaderToken first = requireReader(ring);
  const RealtimeAudioReaderToken second = requireReader(ring);
  std::atomic<bool> writerFinished = false;

  std::thread writer([&] {
    std::vector<float> samples(framesPerWrite * 2);
    for (std::size_t writeIndex = 0; writeIndex < writeCount; ++writeIndex) {
      for (std::size_t frame = 0; frame < framesPerWrite; ++frame) {
        samples[frame * 2] = static_cast<float>(writeIndex);
        samples[frame * 2 + 1] = -static_cast<float>(writeIndex);
      }
      expect(ring.write(samples.data(), framesPerWrite), "concurrent write should succeed");
    }
    writerFinished.store(true, std::memory_order_release);
  });

  auto readUntilFinished = [&](RealtimeAudioReaderToken reader) {
    std::vector<float> samples(framesPerWrite * 2);
    std::uint64_t observedFrames = 0;
    while (!writerFinished.load(std::memory_order_acquire) ||
           observedFrames < ring.publishedFrameCount()) {
      const RealtimeAudioReadResult result = ring.read(reader, samples.data(), framesPerWrite);
      observedFrames += result.audioFrames + result.droppedFrames;
      expect(result.readerIsValid, "concurrent reader should remain valid");
      if (writerFinished.load(std::memory_order_acquire) && result.audioFrames == 0) {
        break;
      }
    }
  };

  std::thread firstReader(readUntilFinished, first);
  std::thread secondReader(readUntilFinished, second);
  writer.join();
  firstReader.join();
  secondReader.join();
  expect(ring.publishedFrameCount() == framesPerWrite * writeCount,
         "published timeline should include every written frame");
}

struct TestCase final {
  std::string_view name;
  void (*body)();
};

} // namespace

int main() {
  const TestCase tests[] = {
      {"rejects invalid configuration", testRejectsInvalidConfiguration},
      {"reads interleaved frames", testReaderReceivesInterleavedFrames},
      {"keeps independent reader cursors", testReadersHaveIndependentCursors},
      {"reports overruns", testLaggingReaderReportsOverrunAndNewestAudio},
      {"fills unavailable frames with silence",
       testUnavailableFramesBecomeSilenceWithoutAdvancingPastPublishedAudio},
      {"invalidates unregistered tokens", testUnregisteredTokenCannotReadReusedSlot},
      {"supports concurrent writer and readers",
       testConcurrentWriterAndReadersRemainBoundedAndDataRaceFree},
  };

  std::size_t failureCount = 0;
  for (const TestCase& test : tests) {
    try {
      test.body();
      std::cout << "PASS: " << test.name << '\n';
    } catch (const std::exception& error) {
      ++failureCount;
      std::cerr << "FAIL: " << test.name << ": " << error.what() << '\n';
    }
  }

  if (failureCount != 0) {
    std::cerr << failureCount << " test(s) failed\n";
    return EXIT_FAILURE;
  }

  std::cout << "All realtime audio ring tests passed\n";
  return EXIT_SUCCESS;
}
