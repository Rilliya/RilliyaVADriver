#include "EndpointRegistry.hpp"
#include "ProductConfiguration.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using rilliya::audio_driver::DriverObjectAddress;
using rilliya::audio_driver::DriverObjectKind;
using rilliya::audio_driver::EndpointDefinition;
using rilliya::audio_driver::EndpointDirection;
using rilliya::audio_driver::EndpointIdentifier;
using rilliya::audio_driver::EndpointRegistry;
using rilliya::audio_driver::EndpointRegistryError;
namespace product_configuration = rilliya::audio_driver::product_configuration;

class TestFailure final : public std::runtime_error {
public:
  explicit TestFailure(const char* message) : std::runtime_error(message) {}
};

void expect(bool condition, const char* message) {
  if (!condition) {
    throw TestFailure(message);
  }
}

EndpointIdentifier identifier(std::uint8_t suffix) {
  EndpointIdentifier value{};
  value[15] = suffix;
  return value;
}

EndpointDefinition endpoint(std::uint8_t suffix, std::string name, EndpointDirection direction) {
  return EndpointDefinition{
      .identifier = identifier(suffix),
      .name = std::move(name),
      .direction = direction,
      .sampleRate = 48000,
      .channelCount = 2,
  };
}

void testPublishesVisibleAndOppositeDirectionCompanion() {
  EndpointRegistry registry;
  const std::vector definitions{endpoint(1, "Remote Microphone", EndpointDirection::input),
                                endpoint(2, "Broadcast Mix", EndpointDirection::output)};
  expect(static_cast<bool>(registry.replace(definitions)), "valid catalog should be accepted");
  expect(registry.endpoints().size() == 2, "both endpoints should be published");
  expect(registry.endpoints()[0].objectIDs.visibleDevice == EndpointRegistry::firstEndpointObjectID,
         "first endpoint should receive the first object allocation");
  expect(registry.endpoints()[1].objectIDs.visibleDevice ==
             EndpointRegistry::firstEndpointObjectID + 4,
         "object allocations should be contiguous without hidden gaps");

  const auto& input = registry.endpoints()[0];
  expect(input.companionDirection() == EndpointDirection::output,
         "input endpoint should have an output feeder");
  expect(input.inputDeviceObjectID() == input.objectIDs.visibleDevice,
         "visible input should be the pair's input device");
  expect(input.outputDeviceObjectID() == input.objectIDs.companionDevice,
         "hidden feeder should be the pair's output device");
  expect(input.companionDeviceUID.ends_with(".internal.feeder"),
         "input companion UID should identify a feeder");

  const auto& output = registry.endpoints()[1];
  expect(output.companionDirection() == EndpointDirection::input,
         "output endpoint should have an input reader");
  expect(output.outputDeviceObjectID() == output.objectIDs.visibleDevice,
         "visible output should be the pair's output device");
  expect(output.inputDeviceObjectID() == output.objectIDs.companionDevice,
         "hidden reader should be the pair's input device");
  expect(output.companionDeviceUID.ends_with(".internal.reader"),
         "output companion UID should identify a reader");
}

void testStableIdentityRetainsObjectIDsAcrossRenameAndReorder() {
  EndpointRegistry registry;
  const std::vector initial{endpoint(1, "First", EndpointDirection::input),
                            endpoint(2, "Second", EndpointDirection::output)};
  expect(static_cast<bool>(registry.replace(initial)), "initial catalog should be accepted");
  const auto firstIDs = registry.endpoints()[0].objectIDs;
  const auto secondIDs = registry.endpoints()[1].objectIDs;

  const std::vector replacement{endpoint(2, "Renamed Second", EndpointDirection::output),
                                endpoint(1, "Renamed First", EndpointDirection::input)};
  expect(static_cast<bool>(registry.replace(replacement)), "replacement should be accepted");
  expect(registry.endpoints()[0].objectIDs == secondIDs,
         "second identity should keep object IDs after reorder");
  expect(registry.endpoints()[1].objectIDs == firstIDs,
         "first identity should keep object IDs after reorder");
}

void testLookupCoversEveryObjectAndBothUIDs() {
  EndpointRegistry registry;
  const std::vector definitions{endpoint(1, "Device", EndpointDirection::input)};
  expect(static_cast<bool>(registry.replace(definitions)), "catalog should be accepted");
  const auto& published = registry.endpoints()[0];

  const DriverObjectAddress visibleDevice =
      registry.findObject(published.objectIDs.visibleDevice).value();
  expect(visibleDevice.kind == DriverObjectKind::visibleDevice,
         "visible device should resolve to its role");
  expect(registry.findObject(published.objectIDs.visibleStream)->kind ==
             DriverObjectKind::visibleStream,
         "visible stream should resolve to its role");
  expect(registry.findObject(published.objectIDs.companionDevice)->kind ==
             DriverObjectKind::companionDevice,
         "companion device should resolve to its role");
  expect(registry.findObject(published.objectIDs.companionStream)->kind ==
             DriverObjectKind::companionStream,
         "companion stream should resolve to its role");
  expect(registry.findDeviceByUID(published.visibleDeviceUID) == published.objectIDs.visibleDevice,
         "visible UID should resolve");
  expect(registry.findDeviceByUID(published.companionDeviceUID) ==
             published.objectIDs.companionDevice,
         "companion UID should resolve");
  expect(!registry.findObject(999999).has_value(), "unknown object should not resolve");
}

void testRejectsAmbiguousOrUnsafeCatalogsWithoutMutation() {
  EndpointRegistry registry;
  const std::vector initial{endpoint(1, "Safe", EndpointDirection::input)};
  expect(static_cast<bool>(registry.replace(initial)), "initial catalog should be accepted");
  const auto retained = registry.endpoints()[0];

  std::vector duplicateIdentity{endpoint(2, "First", EndpointDirection::input),
                                endpoint(2, "Second", EndpointDirection::output)};
  auto result = registry.replace(duplicateIdentity);
  expect(result.error == EndpointRegistryError::duplicateIdentifier,
         "duplicate identity should be rejected");

  std::vector duplicateName{endpoint(2, "Duplicate", EndpointDirection::input),
                            endpoint(3, "duplicate", EndpointDirection::output)};
  result = registry.replace(duplicateName);
  expect(result.error == EndpointRegistryError::duplicateName,
         "case-insensitive duplicate name should be rejected");

  auto invalidFormat = endpoint(2, "Invalid", EndpointDirection::input);
  invalidFormat.channelCount = 0;
  result = registry.replace(std::span(&invalidFormat, 1));
  expect(result.error == EndpointRegistryError::invalidChannelCount,
         "zero channels should be rejected");

  expect(registry.endpoints().size() == 1 &&
             registry.endpoints()[0].definition == retained.definition,
         "failed replacement should preserve prior catalog");
}

void testIdentifierAndUIDFormattingIsDeterministic() {
  const EndpointIdentifier value{
      0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
      0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
  };
  expect(EndpointRegistry::identifierString(value) == "00112233-4455-6677-8899-aabbccddeeff",
         "identifier should use canonical lowercase UUID formatting");
  const EndpointDefinition definition{
      .identifier = value,
      .name = "Device",
      .direction = EndpointDirection::output,
  };
  expect(EndpointRegistry::visibleDeviceUID(definition) ==
             std::string(product_configuration::deviceUIDPrefix) +
                 "00112233-4455-6677-8899-aabbccddeeff.output",
         "visible UID should be stable and direction-specific");
}

void testRemovedEndpointSlotIsReusedWithoutGrowingObjectRange() {
  EndpointRegistry registry;
  const std::vector initial{endpoint(1, "First", EndpointDirection::input),
                            endpoint(2, "Second", EndpointDirection::output)};
  expect(static_cast<bool>(registry.replace(initial)), "initial catalog should be accepted");
  const auto removedIDs = registry.endpoints()[0].objectIDs;
  const auto retainedIDs = registry.endpoints()[1].objectIDs;

  const std::vector replacement{endpoint(2, "Second", EndpointDirection::output),
                                endpoint(3, "Third", EndpointDirection::input)};
  expect(static_cast<bool>(registry.replace(replacement)), "replacement should be accepted");
  expect(registry.endpoints()[0].objectIDs == retainedIDs,
         "retained endpoint should preserve its allocation");
  expect(registry.endpoints()[1].objectIDs == removedIDs,
         "new endpoint should reuse the available bounded slot");
}

struct TestCase final {
  std::string_view name;
  void (*body)();
};

} // namespace

int main() {
  const TestCase tests[] = {
      {"publishes paired devices", testPublishesVisibleAndOppositeDirectionCompanion},
      {"retains IDs across updates", testStableIdentityRetainsObjectIDsAcrossRenameAndReorder},
      {"looks up objects and UIDs", testLookupCoversEveryObjectAndBothUIDs},
      {"rejects unsafe catalogs", testRejectsAmbiguousOrUnsafeCatalogsWithoutMutation},
      {"formats stable identities", testIdentifierAndUIDFormattingIsDeterministic},
      {"reuses removed slots", testRemovedEndpointSlotIsReusedWithoutGrowingObjectRange},
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
  std::cout << "All endpoint registry tests passed\n";
  return EXIT_SUCCESS;
}
