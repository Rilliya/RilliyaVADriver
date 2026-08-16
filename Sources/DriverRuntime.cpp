#include "DriverRuntime.hpp"

#include <algorithm>
#include <new>
#include <utility>

namespace rilliya::audio_driver {
namespace {

constexpr std::size_t defaultRingCapacityFrames = 8192;

[[nodiscard]] std::optional<std::size_t> endpointSlot(std::uint32_t objectID) noexcept {
  if (objectID < EndpointRegistry::firstEndpointObjectID) {
    return std::nullopt;
  }
  const std::uint32_t relative = objectID - EndpointRegistry::firstEndpointObjectID;
  const std::size_t slot = relative / 4;
  if (slot >= EndpointRegistry::maximumEndpointCount) {
    return std::nullopt;
  }
  return slot;
}

} // namespace

EndpointRuntime::EndpointRuntime(PublishedEndpoint endpoint)
    : endpoint_(std::move(endpoint)), ring_(RealtimeAudioRingConfiguration{
                                          .channelCount = endpoint_.definition.channelCount,
                                          .capacityFrames = defaultRingCapacityFrames,
                                          .maximumReaderCount = maximumClientCountPerDevice,
                                      }) {}

const PublishedEndpoint& EndpointRuntime::endpoint() const noexcept { return endpoint_; }

bool EndpointRuntime::isRunning() const noexcept {
  return visibleRunState_.runningClientCount() != 0 || companionRunState_.runningClientCount() != 0;
}

std::uint32_t EndpointRuntime::runningClientCount(std::uint32_t deviceObjectID) const noexcept {
  const DeviceRunState* state = runState(deviceObjectID);
  return state == nullptr ? 0 : state->runningClientCount();
}

DriverRuntimeResult EndpointRuntime::startIO(std::uint32_t deviceObjectID,
                                             std::uint32_t clientID) noexcept {
  DeviceRunState* state = runState(deviceObjectID);
  if (state == nullptr) {
    return {DriverRuntimeError::unknownObject};
  }
  return state->start(clientID, isInputDevice(deviceObjectID) ? &ring_ : nullptr);
}

DriverRuntimeResult EndpointRuntime::stopIO(std::uint32_t deviceObjectID,
                                            std::uint32_t clientID) noexcept {
  DeviceRunState* state = runState(deviceObjectID);
  if (state == nullptr) {
    return {DriverRuntimeError::unknownObject};
  }
  return state->stop(clientID, isInputDevice(deviceObjectID) ? &ring_ : nullptr);
}

DriverRuntimeResult EndpointRuntime::writeMix(std::uint32_t deviceObjectID,
                                              const float* interleavedSamples,
                                              std::size_t frameCount) noexcept {
  if (!isOutputDevice(deviceObjectID)) {
    return {runState(deviceObjectID) == nullptr ? DriverRuntimeError::unknownObject
                                                : DriverRuntimeError::wrongDeviceDirection};
  }
  if (!ring_.write(interleavedSamples, frameCount)) {
    return {DriverRuntimeError::invalidBuffer};
  }
  return {};
}

DriverRuntimeReadResult EndpointRuntime::readInput(std::uint32_t deviceObjectID,
                                                   std::uint32_t clientID,
                                                   float* interleavedDestination,
                                                   std::size_t frameCount) noexcept {
  if (!isInputDevice(deviceObjectID)) {
    return {.error = runState(deviceObjectID) == nullptr
                         ? DriverRuntimeError::unknownObject
                         : DriverRuntimeError::wrongDeviceDirection};
  }
  if (interleavedDestination == nullptr) {
    return {.error = DriverRuntimeError::invalidBuffer};
  }
  DeviceRunState* state = runState(deviceObjectID);
  return state->read(clientID, ring_, interleavedDestination, frameCount);
}

DriverRuntimeReadResult EndpointRuntime::DeviceRunState::read(std::uint32_t clientID,
                                                              RealtimeAudioRing& ring,
                                                              float* destination,
                                                              std::size_t frameCount) noexcept {
  for (ClientSlot& slot : clients_) {
    if (!slot.active.load(std::memory_order_acquire) || slot.clientID != clientID) {
      continue;
    }
    slot.readsInFlight.fetch_add(1, std::memory_order_acquire);
    if (!slot.active.load(std::memory_order_acquire) || slot.clientID != clientID ||
        !slot.reader.has_value()) {
      slot.readsInFlight.fetch_sub(1, std::memory_order_release);
      break;
    }
    const RealtimeAudioReadResult result = ring.read(*slot.reader, destination, frameCount);
    slot.readsInFlight.fetch_sub(1, std::memory_order_release);
    return {.audio = result};
  }

  if (destination != nullptr) {
    std::fill_n(destination, frameCount * ring.channelCount(), 0.0F);
    return {.error = DriverRuntimeError::clientNotRunning};
  }
  return {.error = DriverRuntimeError::invalidBuffer};
}

DriverRuntimeResult EndpointRuntime::DeviceRunState::start(std::uint32_t clientID,
                                                           RealtimeAudioRing* inputRing) noexcept {
  for (ClientSlot& slot : clients_) {
    if (slot.active.load(std::memory_order_acquire) && slot.clientID == clientID) {
      ++slot.referenceCount;
      return {};
    }
  }

  for (ClientSlot& slot : clients_) {
    if (slot.active.load(std::memory_order_acquire) ||
        slot.readsInFlight.load(std::memory_order_acquire) != 0) {
      continue;
    }
    std::optional<RealtimeAudioReaderToken> reader;
    if (inputRing != nullptr) {
      reader = inputRing->registerReader();
      if (!reader.has_value()) {
        return {DriverRuntimeError::clientLimitReached};
      }
    }
    slot.clientID = clientID;
    slot.referenceCount = 1;
    slot.reader = reader;
    slot.active.store(true, std::memory_order_release);
    runningClientCount_.fetch_add(1, std::memory_order_acq_rel);
    return {};
  }
  return {DriverRuntimeError::clientLimitReached};
}

DriverRuntimeResult EndpointRuntime::DeviceRunState::stop(std::uint32_t clientID,
                                                          RealtimeAudioRing* inputRing) noexcept {
  for (ClientSlot& slot : clients_) {
    if (!slot.active.load(std::memory_order_acquire) || slot.clientID != clientID) {
      continue;
    }
    if (slot.referenceCount > 1) {
      --slot.referenceCount;
      return {};
    }

    slot.active.store(false, std::memory_order_release);
    if (inputRing != nullptr && slot.reader.has_value()) {
      inputRing->unregisterReader(*slot.reader);
    }
    runningClientCount_.fetch_sub(1, std::memory_order_acq_rel);
    return {};
  }
  return {DriverRuntimeError::clientNotRunning};
}

std::uint32_t EndpointRuntime::DeviceRunState::runningClientCount() const noexcept {
  return runningClientCount_.load(std::memory_order_acquire);
}

EndpointRuntime::DeviceRunState* EndpointRuntime::runState(std::uint32_t deviceObjectID) noexcept {
  if (deviceObjectID == endpoint_.objectIDs.visibleDevice) {
    return &visibleRunState_;
  }
  if (deviceObjectID == endpoint_.objectIDs.companionDevice) {
    return &companionRunState_;
  }
  return nullptr;
}

const EndpointRuntime::DeviceRunState*
EndpointRuntime::runState(std::uint32_t deviceObjectID) const noexcept {
  if (deviceObjectID == endpoint_.objectIDs.visibleDevice) {
    return &visibleRunState_;
  }
  if (deviceObjectID == endpoint_.objectIDs.companionDevice) {
    return &companionRunState_;
  }
  return nullptr;
}

bool EndpointRuntime::isInputDevice(std::uint32_t deviceObjectID) const noexcept {
  return deviceObjectID == endpoint_.inputDeviceObjectID();
}

bool EndpointRuntime::isOutputDevice(std::uint32_t deviceObjectID) const noexcept {
  return deviceObjectID == endpoint_.outputDeviceObjectID();
}

DriverRuntimeRegistry::DriverRuntimeRegistry() {
  for (auto& runtime : publishedRuntimes_) {
    runtime.store(nullptr, std::memory_order_relaxed);
  }
}

DriverRuntimeResult
DriverRuntimeRegistry::replace(std::span<const EndpointDefinition> definitions) {
  if (anyDeviceIsRunning()) {
    return {DriverRuntimeError::deviceIsRunning};
  }

  EndpointRegistry replacementRegistry = registry_;
  const EndpointRegistryResult registryResult = replacementRegistry.replace(definitions);
  if (!registryResult) {
    return {DriverRuntimeError::allocationFailed};
  }

  std::array<std::unique_ptr<EndpointRuntime>, EndpointRegistry::maximumEndpointCount> replacements;
  try {
    for (const PublishedEndpoint& endpoint : replacementRegistry.endpoints()) {
      const std::size_t slot = static_cast<std::size_t>(
          (endpoint.objectIDs.visibleDevice - EndpointRegistry::firstEndpointObjectID) / 4);
      replacements[slot] = std::make_unique<EndpointRuntime>(endpoint);
    }
  } catch (const std::bad_alloc&) {
    return {DriverRuntimeError::allocationFailed};
  } catch (const std::invalid_argument&) {
    return {DriverRuntimeError::allocationFailed};
  }

  for (auto& runtime : publishedRuntimes_) {
    runtime.store(nullptr, std::memory_order_release);
  }
  ownedRuntimes_ = std::move(replacements);
  registry_ = std::move(replacementRegistry);
  for (std::size_t slot = 0; slot < ownedRuntimes_.size(); ++slot) {
    publishedRuntimes_[slot].store(ownedRuntimes_[slot].get(), std::memory_order_release);
  }
  return {};
}

const EndpointRegistry& DriverRuntimeRegistry::registry() const noexcept { return registry_; }

EndpointRuntime* DriverRuntimeRegistry::runtimeForObject(std::uint32_t objectID) const noexcept {
  const std::optional<std::size_t> slot = endpointSlot(objectID);
  if (!slot.has_value()) {
    return nullptr;
  }
  EndpointRuntime* runtime = publishedRuntimes_[*slot].load(std::memory_order_acquire);
  if (runtime == nullptr) {
    return nullptr;
  }
  const EndpointObjectIDs& objectIDs = runtime->endpoint().objectIDs;
  if (objectID != objectIDs.visibleDevice && objectID != objectIDs.visibleStream &&
      objectID != objectIDs.companionDevice && objectID != objectIDs.companionStream) {
    return nullptr;
  }
  return runtime;
}

bool DriverRuntimeRegistry::anyDeviceIsRunning() const noexcept {
  for (const auto& runtime : publishedRuntimes_) {
    EndpointRuntime* value = runtime.load(std::memory_order_acquire);
    if (value != nullptr && value->isRunning()) {
      return true;
    }
  }
  return false;
}

} // namespace rilliya::audio_driver
