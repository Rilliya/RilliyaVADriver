#include "AudioServerPlugInDriver.hpp"

#include "DriverCatalogCodec.hpp"
#include "EndpointRegistry.hpp"

#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CFPlugInCOM.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using rilliya::audio_driver::createDriverCatalogPropertyList;
using rilliya::audio_driver::decodeDriverCatalog;
using rilliya::audio_driver::DriverCatalogDecodeResult;
using rilliya::audio_driver::DriverEndpointCatalog;
using rilliya::audio_driver::endpointCatalogProperty;
using rilliya::audio_driver::EndpointDefinition;
using rilliya::audio_driver::EndpointDirection;
using rilliya::audio_driver::EndpointIdentifier;
using rilliya::audio_driver::PublishedEndpoint;
using rilliya::audio_driver::resetDriverStateForTesting;
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

struct FakeHostState final {
  CFPropertyListRef storage = nullptr;
  std::uint32_t propertyNotificationCount = 0;

  ~FakeHostState() {
    if (storage != nullptr) {
      CFRelease(storage);
    }
  }
};

FakeHostState fakeHostState;

OSStatus propertiesChanged(AudioServerPlugInHostRef, AudioObjectID objectID, UInt32 addressCount,
                           const AudioObjectPropertyAddress*) {
  expect(objectID == kAudioObjectPlugInObject, "device-list notification should target plug-in");
  expect(addressCount == 2, "device-list notification should publish both changed selectors");
  ++fakeHostState.propertyNotificationCount;
  return noErr;
}

OSStatus copyFromStorage(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef* outputData) {
  if (outputData == nullptr) {
    return kAudioHardwareIllegalOperationError;
  }
  *outputData = fakeHostState.storage == nullptr
                    ? nullptr
                    : static_cast<CFPropertyListRef>(CFRetain(fakeHostState.storage));
  return noErr;
}

OSStatus writeToStorage(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef data) {
  if (fakeHostState.storage != nullptr) {
    CFRelease(fakeHostState.storage);
  }
  fakeHostState.storage = static_cast<CFPropertyListRef>(CFRetain(data));
  return noErr;
}

OSStatus deleteFromStorage(AudioServerPlugInHostRef, CFStringRef) {
  if (fakeHostState.storage != nullptr) {
    CFRelease(fakeHostState.storage);
    fakeHostState.storage = nullptr;
  }
  return noErr;
}

OSStatus requestDeviceConfigurationChange(AudioServerPlugInHostRef, AudioObjectID, UInt64, void*) {
  return noErr;
}

AudioServerPlugInHostInterface fakeHost{
    propertiesChanged,
    copyFromStorage,
    writeToStorage,
    deleteFromStorage,
    requestDeviceConfigurationChange,
};

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

AudioServerPlugInDriverRef requireDriver() {
  void* factoryResult = RILLIYA_VA_DRIVER_FACTORY_SYMBOL(nullptr, kAudioServerPlugInTypeUUID);
  expect(factoryResult != nullptr, "factory should return the public driver interface");
  return static_cast<AudioServerPlugInDriverRef>(factoryResult);
}

AudioObjectPropertyAddress
address(AudioObjectPropertySelector selector,
        AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
  return AudioObjectPropertyAddress{selector, scope, kAudioObjectPropertyElementMain};
}

void setCatalog(AudioServerPlugInDriverRef driver, const DriverEndpointCatalog& catalog,
                OSStatus expectedStatus = noErr) {
  CFDictionaryRef propertyList = createDriverCatalogPropertyList(catalog);
  expect(propertyList != nullptr, "catalog encoding should succeed");
  CFPropertyListRef value = propertyList;
  const AudioObjectPropertyAddress catalogAddress = address(endpointCatalogProperty);
  const OSStatus status = (*driver)->SetPropertyData(
      driver, kAudioObjectPlugInObject, 0, &catalogAddress, 0, nullptr, sizeof(value), &value);
  CFRelease(propertyList);
  expect(status == expectedStatus, "catalog setter should return expected status");
}

/// Reads back the catalog the driver reports, which is what the application asks it for.
CFDictionaryRef catalogProperty(AudioServerPlugInDriverRef driver) {
  const AudioObjectPropertyAddress catalogAddress = address(endpointCatalogProperty);
  CFPropertyListRef value = nullptr;
  UInt32 dataSize = 0;
  const OSStatus status =
      (*driver)->GetPropertyData(driver, kAudioObjectPlugInObject, 0, &catalogAddress, 0, nullptr,
                                 sizeof(value), &dataSize, &value);
  expect(status == noErr, "catalog getter should succeed");
  return static_cast<CFDictionaryRef>(value);
}

std::vector<AudioObjectID> deviceList(AudioServerPlugInDriverRef driver) {
  const AudioObjectPropertyAddress listAddress = address(kAudioPlugInPropertyDeviceList);
  UInt32 dataSize = 0;
  expect((*driver)->GetPropertyDataSize(driver, kAudioObjectPlugInObject, 0, &listAddress, 0,
                                        nullptr, &dataSize) == noErr,
         "device-list size should be readable");
  std::vector<AudioObjectID> result(dataSize / sizeof(AudioObjectID));
  UInt32 outputSize = 0;
  expect((*driver)->GetPropertyData(driver, kAudioObjectPlugInObject, 0, &listAddress, 0, nullptr,
                                    dataSize, &outputSize, result.data()) == noErr,
         "device list should be readable");
  expect(outputSize == dataSize, "device list should fill its advertised size");
  return result;
}

template <typename Value>
Value property(AudioServerPlugInDriverRef driver, AudioObjectID objectID,
               AudioObjectPropertySelector selector,
               AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
  const AudioObjectPropertyAddress propertyAddress = address(selector, scope);
  Value value{};
  UInt32 outputSize = 0;
  expect((*driver)->GetPropertyData(driver, objectID, 0, &propertyAddress, 0, nullptr,
                                    sizeof(value), &outputSize, &value) == noErr,
         "property should be readable");
  expect(outputSize == sizeof(value), "property should return its scalar size");
  return value;
}

std::string stringProperty(AudioServerPlugInDriverRef driver, AudioObjectID objectID,
                           AudioObjectPropertySelector selector) {
  const CFStringRef value = property<CFStringRef>(driver, objectID, selector);
  std::array<char, 256> buffer{};
  const bool converted = CFStringGetCString(
      value, buffer.data(), static_cast<CFIndex>(buffer.size()), kCFStringEncodingUTF8);
  CFRelease(value);
  expect(converted, "string property should contain bounded UTF-8");
  return buffer.data();
}

void testFactoryAndInterfaceDiscovery() {
  const CFUUIDRef wrongType = CFUUIDCreate(kCFAllocatorDefault);
  expect(RILLIYA_VA_DRIVER_FACTORY_SYMBOL(nullptr, wrongType) == nullptr,
         "factory should reject unrelated plug-in type");
  CFRelease(wrongType);

  AudioServerPlugInDriverRef driver = requireDriver();
  void* queried = nullptr;
  const HRESULT result =
      (*driver)->QueryInterface(driver, CFUUIDGetUUIDBytes(IUnknownUUID), &queried);
  expect(result == S_OK && queried == driver, "driver should implement IUnknown");
  (*driver)->Release(driver);
  expect(stringProperty(driver, kAudioObjectPlugInObject, kAudioPlugInPropertyBundleID) ==
             product_configuration::bundleIdentifier,
         "plug-in should publish the bundle identity used for HAL discovery");
}

void testCatalogPublishesVisibleAndHiddenPairs() {
  AudioServerPlugInDriverRef driver = requireDriver();
  expect((*driver)->Initialize(driver, &fakeHost) == noErr, "driver should initialize");
  setCatalog(driver, DriverEndpointCatalog{
                         .revision = 1,
                         .endpoints = {endpoint(1, "Remote Microphone", EndpointDirection::input),
                                       endpoint(2, "Broadcast Mix", EndpointDirection::output)},
                     });
  const std::vector<AudioObjectID> devices = deviceList(driver);
  expect(devices.size() == 4, "two endpoints should publish four devices");
  expect(fakeHostState.storage != nullptr, "accepted catalog should be persisted by the host");
  expect(fakeHostState.propertyNotificationCount == 1,
         "accepted catalog should notify the host exactly once");
  setCatalog(driver,
             DriverEndpointCatalog{
                 .revision = 1,
                 .endpoints = {endpoint(1, "Stale Name", EndpointDirection::input)},
             },
             kAudioHardwareIllegalOperationError);
  expect(deviceList(driver).size() == 4,
         "stale catalog revisions should not replace published devices");

  expect(property<UInt32>(driver, devices[0], kAudioDevicePropertyIsHidden) == 0,
         "visible endpoint should not be hidden");
  expect(property<UInt32>(driver, devices[1], kAudioDevicePropertyIsHidden) == 1,
         "companion endpoint should be hidden");
  expect(property<UInt32>(driver, devices[2], kAudioDevicePropertyIsHidden) == 0,
         "second visible endpoint should not be hidden");
  expect(property<UInt32>(driver, devices[3], kAudioDevicePropertyIsHidden) == 1,
         "second companion endpoint should be hidden");

  const AudioStreamBasicDescription format = property<AudioStreamBasicDescription>(
      driver, devices[0] + 1, kAudioStreamPropertyVirtualFormat);
  expect(format.mSampleRate == 48000 && format.mChannelsPerFrame == 2,
         "stream should expose the configured PCM format");
  expect(format.mFormatID == kAudioFormatLinearPCM &&
             (format.mFormatFlags & kAudioFormatFlagIsFloat) != 0,
         "stream should expose native packed Float32 PCM");
}

/// A driver that is loaded again must publish the endpoints it was told about before.
///
/// coreaudiod restarts whenever a driver is installed, a machine wakes, or anything else asks it
/// to, and every restart loads the plug-in afresh. The catalog is written to host storage for
/// exactly this reason, so what matters is not that the storage is written but that a new instance
/// reading it back ends up publishing the same devices.
///
/// The tests around this one drive one instance from an empty start. The seam between "the
/// catalog was stored" and "a new instance publishes it" is what this covers, and it is where a
/// catalog that restored while its devices did not was able to hide.
void testStoredCatalogSurvivesAReload() {
  {
    AudioServerPlugInDriverRef driver = requireDriver();
    expect((*driver)->Initialize(driver, &fakeHost) == noErr, "driver should initialize");
    setCatalog(driver, DriverEndpointCatalog{
                           .revision = 2,
                           .endpoints = {endpoint(1, "Virtual Input", EndpointDirection::input)},
                       });
    expect(deviceList(driver).size() == 2, "one endpoint should publish its device pair");
    expect(fakeHostState.storage != nullptr, "the catalog should reach host storage");
  }

  // The driver is a process-wide singleton, so asking the factory again returns the same state.
  // Clearing it is what stands in for coreaudiod loading the plug-in afresh; without this the
  // second Initialize would simply find the catalog still in memory and prove nothing.
  resetDriverStateForTesting();
  expect(deviceList(requireDriver()).empty(), "a reset driver should publish nothing");

  AudioServerPlugInDriverRef reloaded = requireDriver();
  expect((*reloaded)->Initialize(reloaded, &fakeHost) == noErr,
         "reloaded driver should initialize");

  expect(deviceList(reloaded).size() == 2,
         "a reloaded driver should publish the endpoints it stored, not an empty device list");

  const CFDictionaryRef restored = catalogProperty(reloaded);
  expect(restored != nullptr, "a reloaded driver should report the catalog it stored");
  const DriverCatalogDecodeResult decoded = decodeDriverCatalog(restored);
  CFRelease(restored);
  expect(static_cast<bool>(decoded), "the restored catalog should decode");
  expect(decoded.catalog.revision == 2, "the restored catalog should keep its revision");
  expect(decoded.catalog.endpoints.size() == 1, "the restored catalog should keep its endpoint");

  // What the catalog says and what the device list publishes must agree: reporting an endpoint
  // while publishing no device is the shape this failure took.
  expect(deviceList(reloaded).size() == decoded.catalog.endpoints.size() * 2,
         "the published devices should match the endpoints the catalog reports");

  // The fake host's storage outlives one test, and a revision left behind here would be refused
  // as stale by whichever test runs next.
  deleteFromStorage(nullptr, nullptr);
  resetDriverStateForTesting();
}

/// Replays the exact sequence coreaudiod runs before it will publish a device.
///
/// Captured from a live macOS host: plug-in custom properties, the device list, then per device
/// its class, UID and streams in both scopes, then a stream's available physical formats, and
/// finally its control list. A driver may answer "none" to any of these, but refusing one stops
/// the walk: the plug-in loads, reports its devices, and the host silently publishes nothing,
/// which is indistinguishable from a driver that never started.
void testUIDTranslationAndDirectionSpecificProperties() {
  // Establishes what it needs rather than inheriting whatever ran before it. The driver is a
  // process-wide singleton, so a test that reads devices it did not publish passes or fails on
  // the order the tests happen to run in.
  resetDriverStateForTesting();
  AudioServerPlugInDriverRef driver = requireDriver();
  expect((*driver)->Initialize(driver, &fakeHost) == noErr, "driver should initialize");
  setCatalog(driver, DriverEndpointCatalog{
                         .revision = 1,
                         .endpoints = {endpoint(1, "Remote Microphone", EndpointDirection::input),
                                       endpoint(2, "Broadcast Mix", EndpointDirection::output)},
                     });
  const std::vector<AudioObjectID> devices = deviceList(driver);
  expect(devices.size() == 4, "the catalog this test set should publish four devices");
  const AudioObjectID inputStreamObjectID = devices[0] + 1;
  const AudioServerPlugInClientInfo clientInfo{};
  const AudioServerPlugInIOCycleInfo cycleInfo{};
  expect((*driver)->AddDeviceClient(driver, devices[0], &clientInfo) == noErr,
         "device-client registration should accept devices");
  expect((*driver)->AddDeviceClient(driver, inputStreamObjectID, &clientInfo) ==
             kAudioHardwareBadObjectError,
         "device-client registration should reject stream objects");
  expect((*driver)->BeginIOOperation(driver, inputStreamObjectID, 0, 0, 0, &cycleInfo) ==
             kAudioHardwareBadObjectError,
         "IO lifecycle calls should reject stream objects");
  CFStringRef uid = property<CFStringRef>(driver, devices[0], kAudioDevicePropertyDeviceUID);
  const AudioObjectPropertyAddress translationAddress =
      address(kAudioPlugInPropertyTranslateUIDToDevice);
  AudioObjectID translated = kAudioObjectUnknown;
  UInt32 outputSize = 0;
  expect((*driver)->GetPropertyData(driver, kAudioObjectPlugInObject, 0, &translationAddress,
                                    sizeof(uid), &uid, sizeof(translated), &outputSize,
                                    &translated) == noErr,
         "UID translation should succeed");
  CFRelease(uid);
  expect(translated == devices[0], "UID should resolve to its exact device");

  const AudioObjectID inputStream = property<AudioObjectID>(
      driver, devices[0], kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput);
  expect(inputStream == inputStreamObjectID, "input scope should expose the input stream");
  const AudioObjectPropertyAddress outputStreams =
      address(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
  UInt32 outputDataSize = 99;
  AudioObjectID unusedStream = kAudioObjectUnknown;
  expect((*driver)->GetPropertyData(driver, devices[0], 0, &outputStreams, 0, nullptr, 0,
                                    &outputDataSize, &unusedStream) == noErr,
         "opposite scope should return an empty list");
  expect(outputDataSize == 0, "opposite scope should contain no stream");
}

void testRealtimeIOBridgesCompanionPair() {
  AudioServerPlugInDriverRef driver = requireDriver();
  const std::vector<AudioObjectID> devices = deviceList(driver);
  const AudioObjectID visibleInput = devices[0];
  const AudioObjectID hiddenFeeder = devices[1];
  constexpr UInt32 inputClient = 11;
  constexpr UInt32 outputClient = 12;
  expect((*driver)->StartIO(driver, visibleInput, inputClient) == noErr,
         "visible input client should start");
  expect((*driver)->StartIO(driver, hiddenFeeder, outputClient) == noErr,
         "hidden feeder client should start");

  Boolean willDo = false;
  Boolean inPlace = false;
  expect((*driver)->WillDoIOOperation(driver, hiddenFeeder, outputClient,
                                      kAudioServerPlugInIOOperationWriteMix, &willDo,
                                      &inPlace) == noErr &&
             willDo && inPlace,
         "output companion should implement in-place WriteMix");

  std::vector<float> source{1.0F, -1.0F, 0.5F, -0.5F};
  const AudioServerPlugInIOCycleInfo cycle{};
  expect((*driver)->DoIOOperation(driver, hiddenFeeder, hiddenFeeder + 1, outputClient,
                                  kAudioServerPlugInIOOperationWriteMix, 2, &cycle, source.data(),
                                  nullptr) == noErr,
         "hidden feeder should accept output mix");
  std::vector<float> destination(4);
  expect((*driver)->DoIOOperation(driver, visibleInput, visibleInput + 1, inputClient,
                                  kAudioServerPlugInIOOperationReadInput, 2, &cycle,
                                  destination.data(), nullptr) == noErr,
         "visible input should provide captured frames");
  expect(destination == source, "paired devices should preserve every interleaved sample");

  Float64 sampleTime = -1;
  UInt64 hostTime = 0;
  UInt64 seed = 0;
  expect((*driver)->GetZeroTimeStamp(driver, visibleInput, inputClient, &sampleTime, &hostTime,
                                     &seed) == noErr,
         "running device should publish a timestamp");
  expect(sampleTime >= 0 && hostTime != 0 && seed != 0,
         "timestamp should include a valid timeline and seed");

  const DriverEndpointCatalog blockedCatalog{
      .revision = 2,
      .endpoints = {endpoint(1, "Renamed", EndpointDirection::input)},
  };
  setCatalog(driver, blockedCatalog, kAudioHardwareIllegalOperationError);
  expect((*driver)->StopIO(driver, hiddenFeeder, outputClient) == noErr,
         "hidden feeder should stop");
  expect((*driver)->StopIO(driver, visibleInput, inputClient) == noErr,
         "visible input should stop");
  setCatalog(driver, blockedCatalog);
  expect(deviceList(driver).size() == 2, "idle catalog replacement should take effect");
}

struct TestCase final {
  std::string_view name;
  void (*body)();
};

} // namespace

int main() {
  const TestCase tests[] = {
      {"discovers interface", testFactoryAndInterfaceDiscovery},
      {"publishes device pairs", testCatalogPublishesVisibleAndHiddenPairs},
      {"restores a stored catalog on reload", testStoredCatalogSurvivesAReload},
      {"translates device UIDs", testUIDTranslationAndDirectionSpecificProperties},
      {"bridges realtime IO", testRealtimeIOBridgesCompanionPair},
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
  std::cout << "All Audio Server Plug-in driver tests passed\n";
  return EXIT_SUCCESS;
}
