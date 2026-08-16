#pragma once

#include "EndpointRegistry.hpp"
#include "RealtimeAudioRing.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace rilliya::audio_driver {

enum class DriverRuntimeError : std::uint8_t {
  none,
  unknownObject,
  wrongDeviceDirection,
  clientLimitReached,
  clientNotRunning,
  deviceIsRunning,
  allocationFailed,
  invalidBuffer,
};

struct DriverRuntimeResult final {
  DriverRuntimeError error = DriverRuntimeError::none;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == DriverRuntimeError::none;
  }
};

struct DriverRuntimeReadResult final {
  DriverRuntimeError error = DriverRuntimeError::none;
  RealtimeAudioReadResult audio;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == DriverRuntimeError::none;
  }
};

/// The allocation-free IO state for one visible/hidden device pair.
class EndpointRuntime final {
public:
  static constexpr std::size_t maximumClientCountPerDevice = 64;

  explicit EndpointRuntime(PublishedEndpoint endpoint);
  ~EndpointRuntime() = default;

  EndpointRuntime(const EndpointRuntime&) = delete;
  EndpointRuntime& operator=(const EndpointRuntime&) = delete;

  [[nodiscard]] const PublishedEndpoint& endpoint() const noexcept;
  [[nodiscard]] bool isRunning() const noexcept;
  [[nodiscard]] std::uint32_t runningClientCount(std::uint32_t deviceObjectID) const noexcept;

  [[nodiscard]] DriverRuntimeResult startIO(std::uint32_t deviceObjectID,
                                            std::uint32_t clientID) noexcept;
  [[nodiscard]] DriverRuntimeResult stopIO(std::uint32_t deviceObjectID,
                                           std::uint32_t clientID) noexcept;
  [[nodiscard]] DriverRuntimeResult writeMix(std::uint32_t deviceObjectID,
                                             const float* interleavedSamples,
                                             std::size_t frameCount) noexcept;
  [[nodiscard]] DriverRuntimeReadResult readInput(std::uint32_t deviceObjectID,
                                                  std::uint32_t clientID,
                                                  float* interleavedDestination,
                                                  std::size_t frameCount) noexcept;

private:
  struct ClientSlot final {
    std::atomic<bool> active{false};
    std::uint32_t clientID = 0;
    std::uint32_t referenceCount = 0;
    std::optional<RealtimeAudioReaderToken> reader;
    std::atomic<std::uint32_t> readsInFlight{0};
  };

  class DeviceRunState final {
  public:
    [[nodiscard]] DriverRuntimeResult start(std::uint32_t clientID,
                                            RealtimeAudioRing* inputRing) noexcept;
    [[nodiscard]] DriverRuntimeResult stop(std::uint32_t clientID,
                                           RealtimeAudioRing* inputRing) noexcept;
    [[nodiscard]] DriverRuntimeReadResult read(std::uint32_t clientID, RealtimeAudioRing& ring,
                                               float* destination, std::size_t frameCount) noexcept;
    [[nodiscard]] std::uint32_t runningClientCount() const noexcept;

  private:
    std::array<ClientSlot, maximumClientCountPerDevice> clients_{};
    std::atomic<std::uint32_t> runningClientCount_{0};
  };

  [[nodiscard]] DeviceRunState* runState(std::uint32_t deviceObjectID) noexcept;
  [[nodiscard]] const DeviceRunState* runState(std::uint32_t deviceObjectID) const noexcept;
  [[nodiscard]] bool isInputDevice(std::uint32_t deviceObjectID) const noexcept;
  [[nodiscard]] bool isOutputDevice(std::uint32_t deviceObjectID) const noexcept;

  PublishedEndpoint endpoint_;
  RealtimeAudioRing ring_;
  DeviceRunState visibleRunState_;
  DeviceRunState companionRunState_;
};

/// A bounded set of realtime endpoint pairs addressed directly by deterministic HAL object IDs.
///
/// Catalog replacement is a control-plane operation and is rejected while any published device is
/// running. Realtime lookup performs one range check, one slot calculation, and one atomic load.
class DriverRuntimeRegistry final {
public:
  DriverRuntimeRegistry();

  [[nodiscard]] DriverRuntimeResult replace(std::span<const EndpointDefinition> definitions);
  [[nodiscard]] const EndpointRegistry& registry() const noexcept;
  [[nodiscard]] EndpointRuntime* runtimeForObject(std::uint32_t objectID) const noexcept;
  [[nodiscard]] bool anyDeviceIsRunning() const noexcept;

private:
  EndpointRegistry registry_;
  std::array<std::unique_ptr<EndpointRuntime>, EndpointRegistry::maximumEndpointCount>
      ownedRuntimes_;
  std::array<std::atomic<EndpointRuntime*>, EndpointRegistry::maximumEndpointCount>
      publishedRuntimes_;
};

} // namespace rilliya::audio_driver
