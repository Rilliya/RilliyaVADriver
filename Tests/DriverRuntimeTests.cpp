#include "DriverRuntime.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using rilliya::audio_driver::DriverRuntimeError;
using rilliya::audio_driver::DriverRuntimeRegistry;
using rilliya::audio_driver::EndpointDefinition;
using rilliya::audio_driver::EndpointDirection;
using rilliya::audio_driver::EndpointIdentifier;

class TestFailure final : public std::runtime_error {
public:
  explicit TestFailure(const char* message) : std::runtime_error(message) {}
};

void expect(bool condition, const char* message) {
  if (!condition) {
    throw TestFailure(message);
  }
}

EndpointDefinition endpoint(std::uint8_t suffix, EndpointDirection direction) {
  EndpointIdentifier identifier{};
  identifier[15] = suffix;
  return EndpointDefinition{
      .identifier = identifier,
      .name = direction == EndpointDirection::input ? "Virtual Input" : "Virtual Output",
      .direction = direction,
      .sampleRate = 48000,
      .channelCount = 2,
  };
}

void expectSamples(const std::vector<float>& actual, const std::vector<float>& expected,
                   const char* message) {
  expect(actual == expected, message);
}

void testVisibleInputReadsIndependentCopiesWrittenToHiddenFeeder() {
  DriverRuntimeRegistry registry;
  const EndpointDefinition definition = endpoint(1, EndpointDirection::input);
  expect(static_cast<bool>(registry.replace(std::span(&definition, 1))), "catalog should prepare");
  const auto& published = registry.registry().endpoints()[0];
  auto* runtime = registry.runtimeForObject(published.objectIDs.visibleDevice);
  expect(runtime != nullptr, "visible device should resolve to runtime");
  expect(static_cast<bool>(runtime->startIO(published.inputDeviceObjectID(), 10)),
         "first input client should start");
  expect(static_cast<bool>(runtime->startIO(published.inputDeviceObjectID(), 20)),
         "second input client should start");
  expect(static_cast<bool>(runtime->startIO(published.outputDeviceObjectID(), 30)),
         "feeder output should start");

  const std::vector<float> source{1.0F, -1.0F, 0.5F, -0.5F};
  expect(static_cast<bool>(runtime->writeMix(published.outputDeviceObjectID(), source.data(), 2)),
         "hidden feeder should accept the mix");
  std::vector<float> first(4);
  std::vector<float> second(4);
  expect(
      runtime->readInput(published.inputDeviceObjectID(), 10, first.data(), 2).audio.audioFrames ==
          2,
      "first client should read both frames");
  expect(
      runtime->readInput(published.inputDeviceObjectID(), 20, second.data(), 2).audio.audioFrames ==
          2,
      "second client should independently read both frames");
  expectSamples(first, source, "first client should receive feeder samples");
  expectSamples(second, source, "second client should receive feeder samples");
}

void testVisibleOutputFeedsHiddenReader() {
  DriverRuntimeRegistry registry;
  const EndpointDefinition definition = endpoint(1, EndpointDirection::output);
  expect(static_cast<bool>(registry.replace(std::span(&definition, 1))), "catalog should prepare");
  const auto& published = registry.registry().endpoints()[0];
  auto* runtime = registry.runtimeForObject(published.objectIDs.visibleDevice);
  expect(static_cast<bool>(runtime->startIO(published.inputDeviceObjectID(), 10)),
         "hidden reader should start");
  expect(static_cast<bool>(runtime->startIO(published.outputDeviceObjectID(), 20)),
         "visible output should start");

  const std::vector<float> source{0.25F, 0.5F};
  expect(static_cast<bool>(runtime->writeMix(published.outputDeviceObjectID(), source.data(), 1)),
         "visible output should accept a client mix");
  std::vector<float> destination(2);
  expect(runtime->readInput(published.inputDeviceObjectID(), 10, destination.data(), 1)
                 .audio.audioFrames == 1,
         "hidden reader should receive the mix");
  expectSamples(destination, source, "hidden reader should preserve samples");
}

void testDirectionAndLifecycleErrorsFailSafely() {
  DriverRuntimeRegistry registry;
  const EndpointDefinition definition = endpoint(1, EndpointDirection::input);
  expect(static_cast<bool>(registry.replace(std::span(&definition, 1))), "catalog should prepare");
  const auto& published = registry.registry().endpoints()[0];
  auto* runtime = registry.runtimeForObject(published.objectIDs.visibleDevice);
  std::vector<float> samples(2, 1.0F);
  expect(runtime->writeMix(published.inputDeviceObjectID(), samples.data(), 1).error ==
             DriverRuntimeError::wrongDeviceDirection,
         "input device should reject output writes");
  const auto inactive = runtime->readInput(published.inputDeviceObjectID(), 10, samples.data(), 1);
  expect(inactive.error == DriverRuntimeError::clientNotRunning,
         "inactive client should be diagnosed");
  expectSamples(samples, {0.0F, 0.0F}, "inactive client should receive safe silence");
  expect(runtime->stopIO(published.inputDeviceObjectID(), 10).error ==
             DriverRuntimeError::clientNotRunning,
         "unbalanced stop should be diagnosed");
}

void testNestedStartRequiresBalancedStopAndBlocksCatalogChange() {
  DriverRuntimeRegistry registry;
  const EndpointDefinition definition = endpoint(1, EndpointDirection::input);
  expect(static_cast<bool>(registry.replace(std::span(&definition, 1))), "catalog should prepare");
  const auto& published = registry.registry().endpoints()[0];
  auto* runtime = registry.runtimeForObject(published.objectIDs.visibleDevice);
  expect(static_cast<bool>(runtime->startIO(published.inputDeviceObjectID(), 10)),
         "first start should succeed");
  expect(static_cast<bool>(runtime->startIO(published.inputDeviceObjectID(), 10)),
         "nested start should succeed");
  expect(runtime->runningClientCount(published.inputDeviceObjectID()) == 1,
         "nested start should retain one client slot");
  expect(registry.replace({}).error == DriverRuntimeError::deviceIsRunning,
         "catalog replacement should be rejected while running");
  expect(static_cast<bool>(runtime->stopIO(published.inputDeviceObjectID(), 10)),
         "first stop should decrement the reference");
  expect(runtime->isRunning(), "one remaining start should keep the device running");
  expect(static_cast<bool>(runtime->stopIO(published.inputDeviceObjectID(), 10)),
         "second stop should release the client");
  expect(!runtime->isRunning(), "balanced stop should make the device idle");
  expect(static_cast<bool>(registry.replace({})), "idle catalog should be replaceable");
}

struct TestCase final {
  std::string_view name;
  void (*body)();
};

} // namespace

int main() {
  const TestCase tests[] = {
      {"feeds visible input", testVisibleInputReadsIndependentCopiesWrittenToHiddenFeeder},
      {"captures visible output", testVisibleOutputFeedsHiddenReader},
      {"fails lifecycle errors safely", testDirectionAndLifecycleErrorsFailSafely},
      {"balances nested starts", testNestedStartRequiresBalancedStopAndBlocksCatalogChange},
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
  std::cout << "All driver runtime tests passed\n";
  return EXIT_SUCCESS;
}
