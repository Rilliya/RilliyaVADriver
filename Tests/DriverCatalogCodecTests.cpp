#include "DriverCatalogCodec.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using rilliya::audio_driver::createDriverCatalogPropertyList;
using rilliya::audio_driver::decodeDriverCatalog;
using rilliya::audio_driver::DriverCatalogCodecError;
using rilliya::audio_driver::DriverEndpointCatalog;
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

EndpointDefinition endpoint(std::uint8_t suffix, std::string name, EndpointDirection direction) {
  EndpointIdentifier identifier{};
  identifier[15] = suffix;
  return EndpointDefinition{
      .identifier = identifier,
      .name = std::move(name),
      .direction = direction,
      .sampleRate = 48000,
      .channelCount = 2,
  };
}

void testRoundTripsValidatedCatalog() {
  const DriverEndpointCatalog catalog{
      .revision = 42,
      .endpoints = {endpoint(1, "Remote Microphone", EndpointDirection::input),
                    endpoint(2, "Broadcast Mix", EndpointDirection::output)},
  };
  CFDictionaryRef encoded = createDriverCatalogPropertyList(catalog);
  expect(encoded != nullptr, "encoding should succeed");
  const auto decoded = decodeDriverCatalog(encoded);
  CFRelease(encoded);

  expect(static_cast<bool>(decoded), "encoded catalog should decode");
  expect(decoded.catalog.revision == catalog.revision, "revision should round trip");
  expect(decoded.catalog.endpoints == catalog.endpoints, "endpoints should round trip");
}

void testRejectsWrongRootAndMissingFields() {
  const auto wrongRoot = decodeDriverCatalog(CFSTR("not a dictionary"));
  expect(wrongRoot.error == DriverCatalogCodecError::rootIsNotDictionary,
         "non-dictionary root should be rejected");

  CFMutableDictionaryRef empty = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  const auto missingSchema = decodeDriverCatalog(empty);
  CFRelease(empty);
  expect(missingSchema.error == DriverCatalogCodecError::missingSchemaVersion,
         "missing schema should be diagnosed");
}

void testRejectsUnicodeEquivalentDuplicateNames() {
  const DriverEndpointCatalog catalog{
      .revision = 1,
      .endpoints = {endpoint(1, "Caf\xC3\xA9", EndpointDirection::input),
                    endpoint(2, "CAFE\xCC\x81", EndpointDirection::output)},
  };
  CFDictionaryRef encoded = createDriverCatalogPropertyList(catalog);
  expect(encoded != nullptr, "encoding should succeed");
  const auto decoded = decodeDriverCatalog(encoded);
  CFRelease(encoded);
  expect(decoded.error == DriverCatalogCodecError::duplicateName,
         "canonical and case-equivalent names should be rejected");
  expect(decoded.endpointIndex == 1, "duplicate should identify the later endpoint");
}

void testRejectsMalformedEndpointWithoutReturningPartialCatalog() {
  DriverEndpointCatalog catalog{
      .revision = 1,
      .endpoints = {endpoint(1, "Valid", EndpointDirection::input),
                    endpoint(2, "Invalid", EndpointDirection::output)},
  };
  CFDictionaryRef immutable = createDriverCatalogPropertyList(catalog);
  expect(immutable != nullptr, "encoding should succeed");
  CFMutableDictionaryRef root = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, immutable);
  CFRelease(immutable);
  const auto endpoints = static_cast<CFArrayRef>(CFDictionaryGetValue(root, CFSTR("endpoints")));
  CFMutableArrayRef mutableEndpoints = CFArrayCreateMutableCopy(kCFAllocatorDefault, 0, endpoints);
  const auto second = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(mutableEndpoints, 1));
  CFMutableDictionaryRef invalid = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, second);
  CFDictionarySetValue(invalid, CFSTR("channelCount"), CFSTR("two"));
  CFArraySetValueAtIndex(mutableEndpoints, 1, invalid);
  CFDictionarySetValue(root, CFSTR("endpoints"), mutableEndpoints);
  CFRelease(invalid);
  CFRelease(mutableEndpoints);

  const auto decoded = decodeDriverCatalog(root);
  CFRelease(root);
  expect(decoded.error == DriverCatalogCodecError::invalidChannelCount,
         "invalid field type should be rejected");
  expect(decoded.endpointIndex == 1, "invalid field should identify its endpoint");
  expect(decoded.catalog.endpoints.empty(), "failure should not expose a partial catalog");
}

struct TestCase final {
  std::string_view name;
  void (*body)();
};

} // namespace

int main() {
  const TestCase tests[] = {
      {"round trips catalog", testRoundTripsValidatedCatalog},
      {"rejects invalid root", testRejectsWrongRootAndMissingFields},
      {"rejects unicode duplicate names", testRejectsUnicodeEquivalentDuplicateNames},
      {"rejects partial malformed catalog",
       testRejectsMalformedEndpointWithoutReturningPartialCatalog},
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
  std::cout << "All driver catalog codec tests passed\n";
  return EXIT_SUCCESS;
}
