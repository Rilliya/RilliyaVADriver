#include "AudioServerPlugInDriver.hpp"

#include "DriverCatalogCodec.hpp"
#include "DriverRuntime.hpp"

#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rilliya::audio_driver {
namespace {

constexpr std::uint32_t defaultBufferFrameSize = 512;
constexpr std::uint32_t zeroTimestampPeriod = 16384;
constexpr std::size_t devicesPerEndpoint = 2;

[[nodiscard]] AudioStreamBasicDescription streamFormat(const EndpointDefinition& definition) {
  const UInt32 bytesPerFrame = static_cast<UInt32>(sizeof(float)) * definition.channelCount;
  return AudioStreamBasicDescription{
      .mSampleRate = definition.sampleRate,
      .mFormatID = kAudioFormatLinearPCM,
      .mFormatFlags = static_cast<AudioFormatFlags>(kAudioFormatFlagIsFloat) |
                      static_cast<AudioFormatFlags>(kAudioFormatFlagsNativeEndian) |
                      static_cast<AudioFormatFlags>(kAudioFormatFlagIsPacked),
      .mBytesPerPacket = bytesPerFrame,
      .mFramesPerPacket = 1,
      .mBytesPerFrame = bytesPerFrame,
      .mChannelsPerFrame = definition.channelCount,
      .mBitsPerChannel = 8 * static_cast<UInt32>(sizeof(float)),
      .mReserved = 0,
  };
}

[[nodiscard]] bool scopeIncludesDirection(AudioObjectPropertyScope scope,
                                          EndpointDirection direction) noexcept {
  if (scope == kAudioObjectPropertyScopeGlobal) {
    return true;
  }
  if (direction == EndpointDirection::input) {
    return scope == kAudioObjectPropertyScopeInput;
  }
  return scope == kAudioObjectPropertyScopeOutput;
}

[[nodiscard]] std::optional<std::size_t> endpointSlot(AudioObjectID objectID) noexcept {
  if (objectID < EndpointRegistry::firstEndpointObjectID) {
    return std::nullopt;
  }
  const std::size_t slot =
      static_cast<std::size_t>((objectID - EndpointRegistry::firstEndpointObjectID) / 4);
  if (slot >= EndpointRegistry::maximumEndpointCount) {
    return std::nullopt;
  }
  return slot;
}

class EndpointClock final {
public:
  EndpointClock() {
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) == KERN_SUCCESS && timebase.numer != 0) {
      hostTicksPerSecond_ = 1'000'000'000.0 * static_cast<double>(timebase.denom) /
                            static_cast<double>(timebase.numer);
    }
  }

  void configure(double sampleRate) noexcept {
    const double ticksPerFrame = hostTicksPerSecond_ / sampleRate;
    ticksPerFrameBits_.store(std::bit_cast<std::uint64_t>(ticksPerFrame),
                             std::memory_order_release);
  }

  void start() noexcept {
    if (runningReferenceCount_.fetch_add(1, std::memory_order_acq_rel) == 0) {
      anchorHostTime_.store(mach_absolute_time(), std::memory_order_release);
      seed_.fetch_add(1, std::memory_order_acq_rel);
    }
  }

  void stop() noexcept {
    std::uint32_t current = runningReferenceCount_.load(std::memory_order_acquire);
    while (current != 0 &&
           !runningReferenceCount_.compare_exchange_weak(
               current, current - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
  }

  void timestamp(Float64& sampleTime, UInt64& hostTime, UInt64& seed) const noexcept {
    const UInt64 anchor = anchorHostTime_.load(std::memory_order_acquire);
    const UInt64 now = mach_absolute_time();
    const double ticksPerFrame =
        std::bit_cast<double>(ticksPerFrameBits_.load(std::memory_order_acquire));
    const UInt64 elapsedTicks = now >= anchor ? now - anchor : 0;
    const double elapsedFrames =
        ticksPerFrame > 0 ? static_cast<double>(elapsedTicks) / ticksPerFrame : 0;
    const UInt64 periodCount = static_cast<UInt64>(elapsedFrames) / zeroTimestampPeriod;
    const UInt64 wholeFrames = periodCount * zeroTimestampPeriod;
    sampleTime = static_cast<Float64>(wholeFrames);
    hostTime = anchor +
               static_cast<UInt64>(std::llround(static_cast<double>(wholeFrames) * ticksPerFrame));
    seed = seed_.load(std::memory_order_acquire);
  }

private:
  double hostTicksPerSecond_ = 1'000'000'000.0;
  std::atomic<std::uint64_t> ticksPerFrameBits_{std::bit_cast<std::uint64_t>(1.0)};
  std::atomic<std::uint32_t> runningReferenceCount_{0};
  std::atomic<UInt64> anchorHostTime_{0};
  std::atomic<UInt64> seed_{1};
};

struct ObjectContext final {
  PublishedEndpoint endpoint;
  DriverObjectKind kind = DriverObjectKind::visibleDevice;

  [[nodiscard]] bool isDevice() const noexcept {
    return kind == DriverObjectKind::visibleDevice || kind == DriverObjectKind::companionDevice;
  }

  [[nodiscard]] bool isVisible() const noexcept {
    return kind == DriverObjectKind::visibleDevice || kind == DriverObjectKind::visibleStream;
  }

  [[nodiscard]] EndpointDirection direction() const noexcept {
    return isVisible() ? endpoint.definition.direction : endpoint.companionDirection();
  }

  [[nodiscard]] AudioObjectID deviceObjectID() const noexcept {
    if (kind == DriverObjectKind::visibleDevice || kind == DriverObjectKind::visibleStream) {
      return endpoint.objectIDs.visibleDevice;
    }
    return endpoint.objectIDs.companionDevice;
  }

  [[nodiscard]] AudioObjectID streamObjectID() const noexcept {
    if (kind == DriverObjectKind::visibleDevice || kind == DriverObjectKind::visibleStream) {
      return endpoint.objectIDs.visibleStream;
    }
    return endpoint.objectIDs.companionStream;
  }

  [[nodiscard]] std::string deviceUID() const {
    return isVisible() ? endpoint.visibleDeviceUID : endpoint.companionDeviceUID;
  }

  [[nodiscard]] std::string displayName() const {
    if (isVisible()) {
      return endpoint.definition.name;
    }
    return std::string(product_configuration::internalDeviceNamePrefix) + endpoint.definition.name +
           (direction() == EndpointDirection::input ? " Reader" : " Feeder");
  }
};

template <typename Value>
[[nodiscard]] OSStatus writeScalar(UInt32 dataSize, UInt32* outputDataSize, void* outputData,
                                   const Value& value) noexcept {
  if (outputDataSize == nullptr || outputData == nullptr) {
    return kAudioHardwareIllegalOperationError;
  }
  if (dataSize < sizeof(Value)) {
    return kAudioHardwareBadPropertySizeError;
  }
  std::memcpy(outputData, &value, sizeof(Value));
  *outputDataSize = sizeof(Value);
  return noErr;
}

[[nodiscard]] OSStatus writeCFString(UInt32 dataSize, UInt32* outputDataSize, void* outputData,
                                     CFStringRef string) noexcept {
  if (dataSize < sizeof(CFStringRef) || outputDataSize == nullptr || outputData == nullptr) {
    return dataSize < sizeof(CFStringRef) ? kAudioHardwareBadPropertySizeError
                                          : kAudioHardwareIllegalOperationError;
  }
  const CFStringRef retained = static_cast<CFStringRef>(CFRetain(string));
  std::memcpy(outputData, &retained, sizeof(retained));
  *outputDataSize = sizeof(retained);
  return noErr;
}

[[nodiscard]] OSStatus writeUTF8String(UInt32 dataSize, UInt32* outputDataSize, void* outputData,
                                       std::string_view string) noexcept {
  const CFStringRef value =
      CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(string.data()),
                              static_cast<CFIndex>(string.size()), kCFStringEncodingUTF8, false);
  if (value == nullptr) {
    return kAudioHardwareUnspecifiedError;
  }
  const OSStatus status = writeCFString(dataSize, outputDataSize, outputData, value);
  CFRelease(value);
  return status;
}

[[nodiscard]] OSStatus writeObjectIDs(UInt32 dataSize, UInt32* outputDataSize, void* outputData,
                                      std::span<const AudioObjectID> values) noexcept {
  if (outputDataSize == nullptr || (outputData == nullptr && !values.empty())) {
    return kAudioHardwareIllegalOperationError;
  }
  const std::size_t capacity = dataSize / sizeof(AudioObjectID);
  const std::size_t count = std::min(capacity, values.size());
  if (count > 0) {
    std::memcpy(outputData, values.data(), count * sizeof(AudioObjectID));
  }
  *outputDataSize = static_cast<UInt32>(count * sizeof(AudioObjectID));
  return noErr;
}

class DriverState final {
public:
  [[nodiscard]] OSStatus initialize(AudioServerPlugInHostRef host) {
    if (host == nullptr) {
      return kAudioHardwareIllegalOperationError;
    }
    {
      std::lock_guard lock(stateMutex_);
      host_ = host;
    }

    CFPropertyListRef stored = nullptr;
    if (host->CopyFromStorage != nullptr &&
        host->CopyFromStorage(host, product_configuration::catalogStorageKey, &stored) == noErr &&
        stored != nullptr) {
      const DriverCatalogDecodeResult decoded = decodeDriverCatalog(stored);
      CFRelease(stored);
      if (decoded) {
        std::lock_guard lock(stateMutex_);
        if (runtimes_.replace(decoded.catalog.endpoints)) {
          catalog_ = decoded.catalog;
          configureClocks();
        }
      }
    }
    return noErr;
  }

  [[nodiscard]] std::optional<ObjectContext> context(AudioObjectID objectID) const {
    std::lock_guard lock(stateMutex_);
    const std::optional<DriverObjectAddress> address = runtimes_.registry().findObject(objectID);
    if (!address.has_value()) {
      return std::nullopt;
    }
    return ObjectContext{runtimes_.registry().endpoints()[address->endpointIndex], address->kind};
  }

  [[nodiscard]] std::vector<AudioObjectID> deviceList() const {
    std::lock_guard lock(stateMutex_);
    std::vector<AudioObjectID> devices;
    devices.reserve(runtimes_.registry().endpoints().size() * devicesPerEndpoint);
    for (const PublishedEndpoint& endpoint : runtimes_.registry().endpoints()) {
      devices.push_back(endpoint.objectIDs.visibleDevice);
      devices.push_back(endpoint.objectIDs.companionDevice);
    }
    return devices;
  }

  [[nodiscard]] AudioObjectID deviceForUID(CFStringRef uid) const {
    if (uid == nullptr) {
      return kAudioObjectUnknown;
    }
    std::array<char, 512> buffer{};
    if (!CFStringGetCString(uid, buffer.data(), static_cast<CFIndex>(buffer.size()),
                            kCFStringEncodingUTF8)) {
      return kAudioObjectUnknown;
    }
    std::lock_guard lock(stateMutex_);
    return runtimes_.registry().findDeviceByUID(buffer.data()).value_or(kAudioObjectUnknown);
  }

  [[nodiscard]] CFDictionaryRef createCatalogPropertyList() const {
    std::lock_guard lock(stateMutex_);
    return createDriverCatalogPropertyList(catalog_);
  }

  [[nodiscard]] OSStatus setCatalog(CFPropertyListRef propertyList) {
    const DriverCatalogDecodeResult decoded = decodeDriverCatalog(propertyList);
    if (!decoded) {
      return kAudioHardwareIllegalOperationError;
    }
    const CFDictionaryRef persisted = createDriverCatalogPropertyList(decoded.catalog);
    if (persisted == nullptr) {
      return kAudioHardwareUnspecifiedError;
    }

    std::lock_guard updateLock(updateMutex_);
    DriverEndpointCatalog previous;
    AudioServerPlugInHostRef host = nullptr;
    {
      std::lock_guard stateLock(stateMutex_);
      if (runtimes_.anyDeviceIsRunning()) {
        CFRelease(persisted);
        return kAudioHardwareIllegalOperationError;
      }
      if (decoded.catalog.revision <= catalog_.revision) {
        CFRelease(persisted);
        return kAudioHardwareIllegalOperationError;
      }
      previous = catalog_;
      const DriverRuntimeResult result = runtimes_.replace(decoded.catalog.endpoints);
      if (!result) {
        CFRelease(persisted);
        return result.error == DriverRuntimeError::deviceIsRunning
                   ? kAudioHardwareIllegalOperationError
                   : kAudioHardwareUnspecifiedError;
      }
      catalog_ = decoded.catalog;
      configureClocks();
      host = host_;
    }

    OSStatus storageStatus = noErr;
    if (host != nullptr && host->WriteToStorage != nullptr) {
      storageStatus =
          host->WriteToStorage(host, product_configuration::catalogStorageKey, persisted);
    }
    CFRelease(persisted);
    if (storageStatus != noErr) {
      std::lock_guard stateLock(stateMutex_);
      if (runtimes_.replace(previous.endpoints)) {
        catalog_ = std::move(previous);
        configureClocks();
      }
      return storageStatus;
    }

    notifyDeviceListChanged(host);
    return noErr;
  }

  [[nodiscard]] OSStatus startIO(AudioObjectID deviceObjectID, UInt32 clientID) {
    std::lock_guard lock(stateMutex_);
    EndpointRuntime* runtime = runtimes_.runtimeForObject(deviceObjectID);
    if (runtime == nullptr) {
      return kAudioHardwareBadObjectError;
    }
    const DriverRuntimeResult result = runtime->startIO(deviceObjectID, clientID);
    if (!result) {
      return runtimeStatus(result.error);
    }
    clocks_[*endpointSlot(deviceObjectID)].start();
    return noErr;
  }

  [[nodiscard]] OSStatus stopIO(AudioObjectID deviceObjectID, UInt32 clientID) {
    std::lock_guard lock(stateMutex_);
    EndpointRuntime* runtime = runtimes_.runtimeForObject(deviceObjectID);
    if (runtime == nullptr) {
      return kAudioHardwareBadObjectError;
    }
    const DriverRuntimeResult result = runtime->stopIO(deviceObjectID, clientID);
    if (!result) {
      return runtimeStatus(result.error);
    }
    clocks_[*endpointSlot(deviceObjectID)].stop();
    return noErr;
  }

  [[nodiscard]] OSStatus getZeroTimestamp(AudioObjectID deviceObjectID, Float64& sampleTime,
                                          UInt64& hostTime, UInt64& seed) const noexcept {
    EndpointRuntime* runtime = runtimes_.runtimeForObject(deviceObjectID);
    const std::optional<std::size_t> slot = endpointSlot(deviceObjectID);
    if (runtime == nullptr || !slot.has_value() ||
        (deviceObjectID != runtime->endpoint().objectIDs.visibleDevice &&
         deviceObjectID != runtime->endpoint().objectIDs.companionDevice)) {
      return kAudioHardwareBadObjectError;
    }
    clocks_[*slot].timestamp(sampleTime, hostTime, seed);
    return noErr;
  }

  [[nodiscard]] OSStatus doIO(AudioObjectID deviceObjectID, AudioObjectID streamObjectID,
                              UInt32 clientID, UInt32 operationID, UInt32 frameCount,
                              void* mainBuffer) noexcept {
    EndpointRuntime* runtime = runtimes_.runtimeForObject(deviceObjectID);
    if (runtime == nullptr || mainBuffer == nullptr) {
      return runtime == nullptr ? kAudioHardwareBadObjectError
                                : kAudioHardwareIllegalOperationError;
    }
    const PublishedEndpoint& endpoint = runtime->endpoint();
    const bool streamMatches = (deviceObjectID == endpoint.objectIDs.visibleDevice &&
                                streamObjectID == endpoint.objectIDs.visibleStream) ||
                               (deviceObjectID == endpoint.objectIDs.companionDevice &&
                                streamObjectID == endpoint.objectIDs.companionStream);
    if (!streamMatches) {
      return kAudioHardwareBadObjectError;
    }

    if (operationID == kAudioServerPlugInIOOperationReadInput &&
        deviceObjectID == endpoint.inputDeviceObjectID()) {
      const DriverRuntimeReadResult result =
          runtime->readInput(deviceObjectID, clientID, static_cast<float*>(mainBuffer), frameCount);
      return result.error == DriverRuntimeError::none ||
                     result.error == DriverRuntimeError::clientNotRunning
                 ? noErr
                 : runtimeStatus(result.error);
    }
    if (operationID == kAudioServerPlugInIOOperationWriteMix &&
        deviceObjectID == endpoint.outputDeviceObjectID()) {
      return runtimeStatus(
          runtime->writeMix(deviceObjectID, static_cast<const float*>(mainBuffer), frameCount)
              .error);
    }
    return kAudioHardwareUnsupportedOperationError;
  }

  [[nodiscard]] UInt32 runningClientCount(AudioObjectID deviceObjectID) const noexcept {
    EndpointRuntime* runtime = runtimes_.runtimeForObject(deviceObjectID);
    return runtime == nullptr ? 0 : runtime->runningClientCount(deviceObjectID);
  }

private:
  void configureClocks() noexcept {
    for (const PublishedEndpoint& endpoint : runtimes_.registry().endpoints()) {
      const std::optional<std::size_t> slot = endpointSlot(endpoint.objectIDs.visibleDevice);
      if (slot.has_value()) {
        clocks_[*slot].configure(endpoint.definition.sampleRate);
      }
    }
  }

  static void notifyDeviceListChanged(AudioServerPlugInHostRef host) noexcept {
    if (host == nullptr || host->PropertiesChanged == nullptr) {
      return;
    }
    const AudioObjectPropertyAddress addresses[]{
        {kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal,
         kAudioObjectPropertyElementMain},
        {kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal,
         kAudioObjectPropertyElementMain},
    };
    host->PropertiesChanged(host, kAudioObjectPlugInObject, 2, addresses);
  }

  [[nodiscard]] static OSStatus runtimeStatus(DriverRuntimeError error) noexcept {
    switch (error) {
    case DriverRuntimeError::none:
      return noErr;
    case DriverRuntimeError::unknownObject:
      return kAudioHardwareBadObjectError;
    case DriverRuntimeError::clientNotRunning:
      return kAudioHardwareNotRunningError;
    case DriverRuntimeError::wrongDeviceDirection:
      return kAudioHardwareUnsupportedOperationError;
    case DriverRuntimeError::clientLimitReached:
    case DriverRuntimeError::allocationFailed:
      return kAudioHardwareUnspecifiedError;
    case DriverRuntimeError::deviceIsRunning:
    case DriverRuntimeError::invalidBuffer:
      return kAudioHardwareIllegalOperationError;
    }
  }

  mutable std::mutex stateMutex_;
  std::mutex updateMutex_;
  AudioServerPlugInHostRef host_ = nullptr;
  DriverEndpointCatalog catalog_;
  DriverRuntimeRegistry runtimes_;
  std::array<EndpointClock, EndpointRegistry::maximumEndpointCount> clocks_;
};

DriverState& driverState() {
  static DriverState state;
  return state;
}

std::atomic<ULONG> referenceCount{1};

HRESULT queryInterface(void* driver, REFIID requestedUUID, LPVOID* outputInterface);
ULONG addReference(void* driver);
ULONG releaseReference(void* driver);
OSStatus initialize(AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef host);
OSStatus createDevice(AudioServerPlugInDriverRef driver, CFDictionaryRef description,
                      const AudioServerPlugInClientInfo* clientInfo,
                      AudioObjectID* outputDeviceObjectID);
OSStatus destroyDevice(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID);
OSStatus addDeviceClient(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID,
                         const AudioServerPlugInClientInfo* clientInfo);
OSStatus removeDeviceClient(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID,
                            const AudioServerPlugInClientInfo* clientInfo);
OSStatus performDeviceConfigurationChange(AudioServerPlugInDriverRef driver,
                                          AudioObjectID deviceObjectID, UInt64 changeAction,
                                          void* changeInfo);
OSStatus abortDeviceConfigurationChange(AudioServerPlugInDriverRef driver,
                                        AudioObjectID deviceObjectID, UInt64 changeAction,
                                        void* changeInfo);
Boolean hasProperty(AudioServerPlugInDriverRef driver, AudioObjectID objectID,
                    pid_t clientProcessID, const AudioObjectPropertyAddress* address);
OSStatus isPropertySettable(AudioServerPlugInDriverRef driver, AudioObjectID objectID,
                            pid_t clientProcessID, const AudioObjectPropertyAddress* address,
                            Boolean* isSettable);
OSStatus getPropertyDataSize(AudioServerPlugInDriverRef driver, AudioObjectID objectID,
                             pid_t clientProcessID, const AudioObjectPropertyAddress* address,
                             UInt32 qualifierDataSize, const void* qualifierData, UInt32* dataSize);
OSStatus getPropertyData(AudioServerPlugInDriverRef driver, AudioObjectID objectID,
                         pid_t clientProcessID, const AudioObjectPropertyAddress* address,
                         UInt32 qualifierDataSize, const void* qualifierData, UInt32 dataSize,
                         UInt32* outputDataSize, void* outputData);
OSStatus setPropertyData(AudioServerPlugInDriverRef driver, AudioObjectID objectID,
                         pid_t clientProcessID, const AudioObjectPropertyAddress* address,
                         UInt32 qualifierDataSize, const void* qualifierData, UInt32 dataSize,
                         const void* data);
OSStatus startIO(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt32 clientID);
OSStatus stopIO(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt32 clientID);
OSStatus getZeroTimeStamp(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID,
                          UInt32 clientID, Float64* sampleTime, UInt64* hostTime, UInt64* seed);
OSStatus willDoIOOperation(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID,
                           UInt32 clientID, UInt32 operationID, Boolean* willDo,
                           Boolean* willDoInPlace);
OSStatus beginIOOperation(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID,
                          UInt32 clientID, UInt32 operationID, UInt32 ioBufferFrameSize,
                          const AudioServerPlugInIOCycleInfo* ioCycleInfo);
OSStatus doIOOperation(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID,
                       AudioObjectID streamObjectID, UInt32 clientID, UInt32 operationID,
                       UInt32 ioBufferFrameSize, const AudioServerPlugInIOCycleInfo* ioCycleInfo,
                       void* mainBuffer, void* secondaryBuffer);
OSStatus endIOOperation(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID,
                        UInt32 clientID, UInt32 operationID, UInt32 ioBufferFrameSize,
                        const AudioServerPlugInIOCycleInfo* ioCycleInfo);

AudioServerPlugInDriverInterface driverInterface{
    nullptr,
    queryInterface,
    addReference,
    releaseReference,
    initialize,
    createDevice,
    destroyDevice,
    addDeviceClient,
    removeDeviceClient,
    performDeviceConfigurationChange,
    abortDeviceConfigurationChange,
    hasProperty,
    isPropertySettable,
    getPropertyDataSize,
    getPropertyData,
    setPropertyData,
    startIO,
    stopIO,
    getZeroTimeStamp,
    willDoIOOperation,
    beginIOOperation,
    doIOOperation,
    endIOOperation,
};
AudioServerPlugInDriverInterface* driverInterfacePointer = &driverInterface;
AudioServerPlugInDriverRef driverReference = &driverInterfacePointer;

[[nodiscard]] bool isValidDriver(const void* driver) noexcept { return driver == driverReference; }

[[nodiscard]] bool selectorIsCommon(AudioObjectPropertySelector selector) noexcept {
  switch (selector) {
  case kAudioObjectPropertyBaseClass:
  case kAudioObjectPropertyClass:
  case kAudioObjectPropertyOwner:
  case kAudioObjectPropertyName:
  case kAudioObjectPropertyManufacturer:
  case kAudioObjectPropertyOwnedObjects:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool selectorIsPlugIn(AudioObjectPropertySelector selector) noexcept {
  return selectorIsCommon(selector) || selector == kAudioObjectPropertyCustomPropertyInfoList ||
         selector == kAudioPlugInPropertyBundleID ||
         selector == kAudioPlugInPropertyResourceBundle ||
         selector == kAudioPlugInPropertyDeviceList ||
         selector == kAudioPlugInPropertyTranslateUIDToDevice ||
         selector == endpointCatalogProperty;
}

[[nodiscard]] bool selectorIsDevice(AudioObjectPropertySelector selector) noexcept {
  if (selectorIsCommon(selector)) {
    return true;
  }
  switch (selector) {
  case kAudioDevicePropertyDeviceUID:
  case kAudioDevicePropertyModelUID:
  case kAudioDevicePropertyTransportType:
  case kAudioDevicePropertyRelatedDevices:
  case kAudioDevicePropertyClockDomain:
  case kAudioDevicePropertyDeviceIsAlive:
  case kAudioDevicePropertyDeviceIsRunning:
  case kAudioDevicePropertyDeviceCanBeDefaultDevice:
  case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
  case kAudioDevicePropertyLatency:
  case kAudioDevicePropertyStreams:
  case kAudioDevicePropertyStreamConfiguration:
  case kAudioDevicePropertySafetyOffset:
  case kAudioDevicePropertyNominalSampleRate:
  case kAudioDevicePropertyAvailableNominalSampleRates:
  case kAudioDevicePropertyIsHidden:
  case kAudioDevicePropertyBufferFrameSize:
  case kAudioDevicePropertyBufferFrameSizeRange:
  case kAudioDevicePropertyZeroTimeStampPeriod:
  case kAudioDevicePropertyClockIsStable:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool selectorIsStream(AudioObjectPropertySelector selector) noexcept {
  if (selectorIsCommon(selector)) {
    return true;
  }
  switch (selector) {
  case kAudioStreamPropertyIsActive:
  case kAudioStreamPropertyDirection:
  case kAudioStreamPropertyTerminalType:
  case kAudioStreamPropertyStartingChannel:
  case kAudioStreamPropertyLatency:
  case kAudioStreamPropertyVirtualFormat:
  case kAudioStreamPropertyPhysicalFormat:
  case kAudioStreamPropertyAvailableVirtualFormats:
  case kAudioStreamPropertyAvailablePhysicalFormats:
    return true;
  default:
    return false;
  }
}

HRESULT queryInterface(void* driver, REFIID requestedUUID, LPVOID* outputInterface) {
  if (!isValidDriver(driver) || outputInterface == nullptr) {
    return E_NOINTERFACE;
  }
  *outputInterface = nullptr;
  const CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(kCFAllocatorDefault, requestedUUID);
  if (requested == nullptr) {
    return E_NOINTERFACE;
  }
  const bool supported =
      CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID);
  CFRelease(requested);
  if (!supported) {
    return E_NOINTERFACE;
  }
  addReference(driver);
  *outputInterface = driverReference;
  return S_OK;
}

ULONG addReference(void* driver) {
  return isValidDriver(driver) ? referenceCount.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
}

ULONG releaseReference(void* driver) {
  if (!isValidDriver(driver)) {
    return 0;
  }
  ULONG current = referenceCount.load(std::memory_order_relaxed);
  while (current > 1 &&
         !referenceCount.compare_exchange_weak(current, current - 1, std::memory_order_relaxed)) {
  }
  return current > 1 ? current - 1 : 1;
}

OSStatus initialize(AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef host) {
  return isValidDriver(driver) ? driverState().initialize(host) : kAudioHardwareBadObjectError;
}

OSStatus createDevice(AudioServerPlugInDriverRef driver, CFDictionaryRef,
                      const AudioServerPlugInClientInfo*, AudioObjectID*) {
  return isValidDriver(driver) ? kAudioHardwareUnsupportedOperationError
                               : kAudioHardwareBadObjectError;
}

OSStatus destroyDevice(AudioServerPlugInDriverRef driver, AudioObjectID) {
  return isValidDriver(driver) ? kAudioHardwareUnsupportedOperationError
                               : kAudioHardwareBadObjectError;
}

OSStatus addDeviceClient(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID,
                         const AudioServerPlugInClientInfo*) {
  if (!isValidDriver(driver)) {
    return kAudioHardwareBadObjectError;
  }
  const std::optional<ObjectContext> context = driverState().context(deviceObjectID);
  if (!context.has_value() || !context->isDevice()) {
    return kAudioHardwareBadObjectError;
  }
  return noErr;
}

OSStatus removeDeviceClient(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID,
                            const AudioServerPlugInClientInfo*) {
  return addDeviceClient(driver, deviceObjectID, nullptr);
}

OSStatus performDeviceConfigurationChange(AudioServerPlugInDriverRef driver,
                                          AudioObjectID deviceObjectID, UInt64, void*) {
  return addDeviceClient(driver, deviceObjectID, nullptr);
}

OSStatus abortDeviceConfigurationChange(AudioServerPlugInDriverRef driver,
                                        AudioObjectID deviceObjectID, UInt64, void*) {
  return addDeviceClient(driver, deviceObjectID, nullptr);
}

Boolean hasProperty(AudioServerPlugInDriverRef driver, AudioObjectID objectID, pid_t,
                    const AudioObjectPropertyAddress* address) {
  if (!isValidDriver(driver) || address == nullptr) {
    return false;
  }
  if (objectID == kAudioObjectPlugInObject) {
    return selectorIsPlugIn(address->mSelector);
  }
  const std::optional<ObjectContext> context = driverState().context(objectID);
  return context.has_value() && (context->isDevice() ? selectorIsDevice(address->mSelector)
                                                     : selectorIsStream(address->mSelector));
}

OSStatus isPropertySettable(AudioServerPlugInDriverRef driver, AudioObjectID objectID, pid_t,
                            const AudioObjectPropertyAddress* address, Boolean* isSettable) {
  if (!isValidDriver(driver)) {
    return kAudioHardwareBadObjectError;
  }
  if (address == nullptr || isSettable == nullptr) {
    return kAudioHardwareIllegalOperationError;
  }
  if (!hasProperty(driver, objectID, 0, address)) {
    return kAudioHardwareUnknownPropertyError;
  }
  *isSettable =
      objectID == kAudioObjectPlugInObject && address->mSelector == endpointCatalogProperty;
  return noErr;
}

OSStatus getPropertyDataSize(AudioServerPlugInDriverRef driver, AudioObjectID objectID, pid_t,
                             const AudioObjectPropertyAddress* address, UInt32, const void*,
                             UInt32* dataSize) {
  if (!isValidDriver(driver)) {
    return kAudioHardwareBadObjectError;
  }
  if (address == nullptr || dataSize == nullptr) {
    return kAudioHardwareIllegalOperationError;
  }
  if (!hasProperty(driver, objectID, 0, address)) {
    return kAudioHardwareUnknownPropertyError;
  }

  if (objectID == kAudioObjectPlugInObject) {
    switch (address->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
      *dataSize = sizeof(AudioClassID);
      return noErr;
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyManufacturer:
    case kAudioPlugInPropertyBundleID:
    case kAudioPlugInPropertyResourceBundle:
    case endpointCatalogProperty:
      *dataSize = sizeof(CFTypeRef);
      return noErr;
    case kAudioObjectPropertyOwnedObjects:
    case kAudioPlugInPropertyDeviceList:
      *dataSize = static_cast<UInt32>(driverState().deviceList().size() * sizeof(AudioObjectID));
      return noErr;
    case kAudioObjectPropertyCustomPropertyInfoList:
      *dataSize = sizeof(AudioServerPlugInCustomPropertyInfo);
      return noErr;
    case kAudioPlugInPropertyTranslateUIDToDevice:
      *dataSize = sizeof(AudioObjectID);
      return noErr;
    default:
      return kAudioHardwareUnknownPropertyError;
    }
  }

  const std::optional<ObjectContext> context = driverState().context(objectID);
  if (!context.has_value()) {
    return kAudioHardwareBadObjectError;
  }
  if (context->isDevice()) {
    switch (address->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioDevicePropertyTransportType:
    case kAudioDevicePropertyClockDomain:
    case kAudioDevicePropertyDeviceIsAlive:
    case kAudioDevicePropertyDeviceIsRunning:
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
    case kAudioDevicePropertyLatency:
    case kAudioDevicePropertySafetyOffset:
    case kAudioDevicePropertyIsHidden:
    case kAudioDevicePropertyBufferFrameSize:
    case kAudioDevicePropertyZeroTimeStampPeriod:
    case kAudioDevicePropertyClockIsStable:
      *dataSize = sizeof(UInt32);
      return noErr;
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyManufacturer:
    case kAudioDevicePropertyDeviceUID:
    case kAudioDevicePropertyModelUID:
      *dataSize = sizeof(CFStringRef);
      return noErr;
    case kAudioObjectPropertyOwnedObjects:
    case kAudioDevicePropertyStreams:
      *dataSize =
          scopeIncludesDirection(address->mScope, context->direction()) ? sizeof(AudioObjectID) : 0;
      return noErr;
    case kAudioDevicePropertyRelatedDevices:
      *dataSize = 2 * sizeof(AudioObjectID);
      return noErr;
    case kAudioDevicePropertyStreamConfiguration:
      *dataSize = sizeof(AudioBufferList);
      return noErr;
    case kAudioDevicePropertyNominalSampleRate:
      *dataSize = sizeof(Float64);
      return noErr;
    case kAudioDevicePropertyAvailableNominalSampleRates:
    case kAudioDevicePropertyBufferFrameSizeRange:
      *dataSize = sizeof(AudioValueRange);
      return noErr;
    default:
      return kAudioHardwareUnknownPropertyError;
    }
  }

  switch (address->mSelector) {
  case kAudioObjectPropertyBaseClass:
  case kAudioObjectPropertyClass:
  case kAudioObjectPropertyOwner:
  case kAudioStreamPropertyIsActive:
  case kAudioStreamPropertyDirection:
  case kAudioStreamPropertyTerminalType:
  case kAudioStreamPropertyStartingChannel:
  case kAudioStreamPropertyLatency:
    *dataSize = sizeof(UInt32);
    return noErr;
  case kAudioObjectPropertyName:
  case kAudioObjectPropertyManufacturer:
    *dataSize = sizeof(CFStringRef);
    return noErr;
  case kAudioObjectPropertyOwnedObjects:
    *dataSize = 0;
    return noErr;
  case kAudioStreamPropertyVirtualFormat:
  case kAudioStreamPropertyPhysicalFormat:
    *dataSize = sizeof(AudioStreamBasicDescription);
    return noErr;
  case kAudioStreamPropertyAvailableVirtualFormats:
  case kAudioStreamPropertyAvailablePhysicalFormats:
    *dataSize = sizeof(AudioStreamRangedDescription);
    return noErr;
  default:
    return kAudioHardwareUnknownPropertyError;
  }
}

OSStatus getPropertyData(AudioServerPlugInDriverRef driver, AudioObjectID objectID, pid_t,
                         const AudioObjectPropertyAddress* address, UInt32 qualifierDataSize,
                         const void* qualifierData, UInt32 dataSize, UInt32* outputDataSize,
                         void* outputData) {
  if (!isValidDriver(driver)) {
    return kAudioHardwareBadObjectError;
  }
  if (address == nullptr || outputDataSize == nullptr || (outputData == nullptr && dataSize != 0)) {
    return kAudioHardwareIllegalOperationError;
  }
  if (!hasProperty(driver, objectID, 0, address)) {
    return kAudioHardwareUnknownPropertyError;
  }

  if (objectID == kAudioObjectPlugInObject) {
    switch (address->mSelector) {
    case kAudioObjectPropertyBaseClass:
      return writeScalar(dataSize, outputDataSize, outputData, kAudioObjectClassID);
    case kAudioObjectPropertyClass:
      return writeScalar(dataSize, outputDataSize, outputData, kAudioPlugInClassID);
    case kAudioObjectPropertyOwner:
      return writeScalar(dataSize, outputDataSize, outputData, kAudioObjectUnknown);
    case kAudioObjectPropertyName:
      return writeCFString(dataSize, outputDataSize, outputData, product_configuration::plugInName);
    case kAudioObjectPropertyManufacturer:
      return writeCFString(dataSize, outputDataSize, outputData,
                           product_configuration::manufacturerName);
    case kAudioPlugInPropertyBundleID:
      return writeUTF8String(dataSize, outputDataSize, outputData,
                             product_configuration::bundleIdentifier);
    case kAudioObjectPropertyOwnedObjects:
    case kAudioPlugInPropertyDeviceList: {
      const std::vector<AudioObjectID> devices = driverState().deviceList();
      return writeObjectIDs(dataSize, outputDataSize, outputData, devices);
    }
    case kAudioObjectPropertyCustomPropertyInfoList: {
      const AudioServerPlugInCustomPropertyInfo info{
          endpointCatalogProperty, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList,
          kAudioServerPlugInCustomPropertyDataTypeNone};
      return writeScalar(dataSize, outputDataSize, outputData, info);
    }
    case kAudioPlugInPropertyResourceBundle:
      return writeCFString(dataSize, outputDataSize, outputData, CFSTR(""));
    case kAudioPlugInPropertyTranslateUIDToDevice: {
      if (qualifierDataSize != sizeof(CFStringRef) || qualifierData == nullptr) {
        return kAudioHardwareBadPropertySizeError;
      }
      CFStringRef uid = nullptr;
      std::memcpy(&uid, qualifierData, sizeof(uid));
      const AudioObjectID device = driverState().deviceForUID(uid);
      return writeScalar(dataSize, outputDataSize, outputData, device);
    }
    case endpointCatalogProperty: {
      const CFDictionaryRef catalog = driverState().createCatalogPropertyList();
      if (catalog == nullptr) {
        return kAudioHardwareUnspecifiedError;
      }
      const OSStatus status = writeScalar(dataSize, outputDataSize, outputData, catalog);
      if (status != noErr) {
        CFRelease(catalog);
      }
      return status;
    }
    default:
      return kAudioHardwareUnknownPropertyError;
    }
  }

  const std::optional<ObjectContext> context = driverState().context(objectID);
  if (!context.has_value()) {
    return kAudioHardwareBadObjectError;
  }
  const EndpointDefinition& definition = context->endpoint.definition;
  if (context->isDevice()) {
    switch (address->mSelector) {
    case kAudioObjectPropertyBaseClass:
      return writeScalar(dataSize, outputDataSize, outputData, kAudioObjectClassID);
    case kAudioObjectPropertyClass:
      return writeScalar(dataSize, outputDataSize, outputData, kAudioDeviceClassID);
    case kAudioObjectPropertyOwner:
      return writeScalar(dataSize, outputDataSize, outputData, kAudioObjectPlugInObject);
    case kAudioObjectPropertyName:
      return writeUTF8String(dataSize, outputDataSize, outputData, context->displayName());
    case kAudioObjectPropertyManufacturer:
      return writeCFString(dataSize, outputDataSize, outputData,
                           product_configuration::manufacturerName);
    case kAudioObjectPropertyOwnedObjects:
    case kAudioDevicePropertyStreams: {
      if (!scopeIncludesDirection(address->mScope, context->direction())) {
        *outputDataSize = 0;
        return noErr;
      }
      const std::array<AudioObjectID, 1> stream{context->streamObjectID()};
      return writeObjectIDs(dataSize, outputDataSize, outputData, stream);
    }
    case kAudioDevicePropertyDeviceUID:
      return writeUTF8String(dataSize, outputDataSize, outputData, context->deviceUID());
    case kAudioDevicePropertyModelUID:
      return writeCFString(dataSize, outputDataSize, outputData, product_configuration::modelUID);
    case kAudioDevicePropertyTransportType:
      return writeScalar(dataSize, outputDataSize, outputData, kAudioDeviceTransportTypeVirtual);
    case kAudioDevicePropertyRelatedDevices: {
      const std::array<AudioObjectID, 2> related{context->endpoint.objectIDs.visibleDevice,
                                                 context->endpoint.objectIDs.companionDevice};
      return writeObjectIDs(dataSize, outputDataSize, outputData, related);
    }
    case kAudioDevicePropertyClockDomain: {
      const UInt32 domain = static_cast<UInt32>(context->endpoint.objectIDs.visibleDevice);
      return writeScalar(dataSize, outputDataSize, outputData, domain);
    }
    case kAudioDevicePropertyDeviceIsAlive:
    case kAudioDevicePropertyClockIsStable: {
      const UInt32 value = 1;
      return writeScalar(dataSize, outputDataSize, outputData, value);
    }
    case kAudioDevicePropertyDeviceIsRunning: {
      const UInt32 value = driverState().runningClientCount(context->deviceObjectID()) == 0 ? 0 : 1;
      return writeScalar(dataSize, outputDataSize, outputData, value);
    }
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: {
      const UInt32 value = context->isVisible() ? 1 : 0;
      return writeScalar(dataSize, outputDataSize, outputData, value);
    }
    case kAudioDevicePropertyLatency:
    case kAudioDevicePropertySafetyOffset: {
      const UInt32 value = 0;
      return writeScalar(dataSize, outputDataSize, outputData, value);
    }
    case kAudioDevicePropertyStreamConfiguration: {
      if (dataSize < sizeof(AudioBufferList)) {
        return kAudioHardwareBadPropertySizeError;
      }
      auto* list = static_cast<AudioBufferList*>(outputData);
      list->mNumberBuffers = scopeIncludesDirection(address->mScope, context->direction()) ? 1 : 0;
      if (list->mNumberBuffers == 1) {
        list->mBuffers[0] = AudioBuffer{definition.channelCount, 0, nullptr};
      }
      *outputDataSize = sizeof(AudioBufferList);
      return noErr;
    }
    case kAudioDevicePropertyNominalSampleRate:
      return writeScalar(dataSize, outputDataSize, outputData, definition.sampleRate);
    case kAudioDevicePropertyAvailableNominalSampleRates: {
      const AudioValueRange range{definition.sampleRate, definition.sampleRate};
      return writeScalar(dataSize, outputDataSize, outputData, range);
    }
    case kAudioDevicePropertyIsHidden: {
      const UInt32 value = context->isVisible() ? 0 : 1;
      return writeScalar(dataSize, outputDataSize, outputData, value);
    }
    case kAudioDevicePropertyBufferFrameSize:
      return writeScalar(dataSize, outputDataSize, outputData, defaultBufferFrameSize);
    case kAudioDevicePropertyBufferFrameSizeRange: {
      const AudioValueRange range{32, 4096};
      return writeScalar(dataSize, outputDataSize, outputData, range);
    }
    case kAudioDevicePropertyZeroTimeStampPeriod:
      return writeScalar(dataSize, outputDataSize, outputData, zeroTimestampPeriod);
    default:
      return kAudioHardwareUnknownPropertyError;
    }
  }

  switch (address->mSelector) {
  case kAudioObjectPropertyBaseClass:
    return writeScalar(dataSize, outputDataSize, outputData, kAudioObjectClassID);
  case kAudioObjectPropertyClass:
    return writeScalar(dataSize, outputDataSize, outputData, kAudioStreamClassID);
  case kAudioObjectPropertyOwner:
    return writeScalar(dataSize, outputDataSize, outputData, context->deviceObjectID());
  case kAudioObjectPropertyName:
    return writeUTF8String(dataSize, outputDataSize, outputData, context->displayName());
  case kAudioObjectPropertyManufacturer:
    return writeCFString(dataSize, outputDataSize, outputData,
                         product_configuration::manufacturerName);
  case kAudioObjectPropertyOwnedObjects:
    *outputDataSize = 0;
    return noErr;
  case kAudioStreamPropertyIsActive: {
    const UInt32 value = 1;
    return writeScalar(dataSize, outputDataSize, outputData, value);
  }
  case kAudioStreamPropertyDirection: {
    const UInt32 value = context->direction() == EndpointDirection::input ? 1 : 0;
    return writeScalar(dataSize, outputDataSize, outputData, value);
  }
  case kAudioStreamPropertyTerminalType: {
    const UInt32 terminal = context->direction() == EndpointDirection::input
                                ? kAudioStreamTerminalTypeMicrophone
                                : kAudioStreamTerminalTypeSpeaker;
    return writeScalar(dataSize, outputDataSize, outputData, terminal);
  }
  case kAudioStreamPropertyStartingChannel: {
    const UInt32 channel = 1;
    return writeScalar(dataSize, outputDataSize, outputData, channel);
  }
  case kAudioStreamPropertyLatency: {
    const UInt32 latency = 0;
    return writeScalar(dataSize, outputDataSize, outputData, latency);
  }
  case kAudioStreamPropertyVirtualFormat:
  case kAudioStreamPropertyPhysicalFormat: {
    const AudioStreamBasicDescription format = streamFormat(definition);
    return writeScalar(dataSize, outputDataSize, outputData, format);
  }
  case kAudioStreamPropertyAvailableVirtualFormats:
  case kAudioStreamPropertyAvailablePhysicalFormats: {
    const AudioStreamRangedDescription format{
        .mFormat = streamFormat(definition),
        .mSampleRateRange = {definition.sampleRate, definition.sampleRate},
    };
    return writeScalar(dataSize, outputDataSize, outputData, format);
  }
  default:
    return kAudioHardwareUnknownPropertyError;
  }
}

OSStatus setPropertyData(AudioServerPlugInDriverRef driver, AudioObjectID objectID, pid_t,
                         const AudioObjectPropertyAddress* address, UInt32, const void*,
                         UInt32 dataSize, const void* data) {
  if (!isValidDriver(driver)) {
    return kAudioHardwareBadObjectError;
  }
  if (address == nullptr || data == nullptr) {
    return kAudioHardwareIllegalOperationError;
  }
  if (objectID != kAudioObjectPlugInObject || address->mSelector != endpointCatalogProperty) {
    return hasProperty(driver, objectID, 0, address) ? kAudioHardwareIllegalOperationError
                                                     : kAudioHardwareUnknownPropertyError;
  }
  if (dataSize != sizeof(CFPropertyListRef)) {
    return kAudioHardwareBadPropertySizeError;
  }
  CFPropertyListRef propertyList = nullptr;
  std::memcpy(&propertyList, data, sizeof(propertyList));
  return propertyList == nullptr ? kAudioHardwareIllegalOperationError
                                 : driverState().setCatalog(propertyList);
}

OSStatus startIO(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt32 clientID) {
  return isValidDriver(driver) ? driverState().startIO(deviceObjectID, clientID)
                               : kAudioHardwareBadObjectError;
}

OSStatus stopIO(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt32 clientID) {
  return isValidDriver(driver) ? driverState().stopIO(deviceObjectID, clientID)
                               : kAudioHardwareBadObjectError;
}

OSStatus getZeroTimeStamp(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt32,
                          Float64* sampleTime, UInt64* hostTime, UInt64* seed) {
  if (!isValidDriver(driver)) {
    return kAudioHardwareBadObjectError;
  }
  if (sampleTime == nullptr || hostTime == nullptr || seed == nullptr) {
    return kAudioHardwareIllegalOperationError;
  }
  return driverState().getZeroTimestamp(deviceObjectID, *sampleTime, *hostTime, *seed);
}

OSStatus willDoIOOperation(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt32,
                           UInt32 operationID, Boolean* willDo, Boolean* willDoInPlace) {
  if (!isValidDriver(driver)) {
    return kAudioHardwareBadObjectError;
  }
  if (willDo == nullptr || willDoInPlace == nullptr) {
    return kAudioHardwareIllegalOperationError;
  }
  const std::optional<ObjectContext> context = driverState().context(deviceObjectID);
  if (!context.has_value() || !context->isDevice()) {
    return kAudioHardwareBadObjectError;
  }
  *willDo = (operationID == kAudioServerPlugInIOOperationReadInput &&
             context->direction() == EndpointDirection::input) ||
            (operationID == kAudioServerPlugInIOOperationWriteMix &&
             context->direction() == EndpointDirection::output);
  *willDoInPlace = true;
  return noErr;
}

OSStatus beginIOOperation(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt32,
                          UInt32, UInt32, const AudioServerPlugInIOCycleInfo*) {
  if (!isValidDriver(driver)) {
    return kAudioHardwareBadObjectError;
  }
  const std::optional<ObjectContext> context = driverState().context(deviceObjectID);
  if (!context.has_value() || !context->isDevice()) {
    return kAudioHardwareBadObjectError;
  }
  return noErr;
}

OSStatus doIOOperation(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID,
                       AudioObjectID streamObjectID, UInt32 clientID, UInt32 operationID,
                       UInt32 ioBufferFrameSize, const AudioServerPlugInIOCycleInfo*,
                       void* mainBuffer, void*) {
  return isValidDriver(driver) ? driverState().doIO(deviceObjectID, streamObjectID, clientID,
                                                    operationID, ioBufferFrameSize, mainBuffer)
                               : kAudioHardwareBadObjectError;
}

OSStatus endIOOperation(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt32,
                        UInt32, UInt32, const AudioServerPlugInIOCycleInfo*) {
  return beginIOOperation(driver, deviceObjectID, 0, 0, 0, nullptr);
}

} // namespace
} // namespace rilliya::audio_driver

extern "C" void* RILLIYA_VA_DRIVER_FACTORY_SYMBOL(CFAllocatorRef, CFUUIDRef requestedTypeUUID) {
  if (requestedTypeUUID == nullptr || !CFEqual(requestedTypeUUID, kAudioServerPlugInTypeUUID)) {
    return nullptr;
  }
  return rilliya::audio_driver::driverReference;
}
