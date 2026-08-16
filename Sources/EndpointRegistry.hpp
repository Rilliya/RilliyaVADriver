#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rilliya::audio_driver {

using EndpointIdentifier = std::array<std::uint8_t, 16>;

enum class EndpointDirection : std::uint8_t {
  input,
  output,
};

struct EndpointDefinition final {
  EndpointIdentifier identifier{};
  std::string name;
  EndpointDirection direction = EndpointDirection::input;
  double sampleRate = 48000.0;
  std::uint32_t channelCount = 2;

  [[nodiscard]] bool operator==(const EndpointDefinition&) const = default;
};

enum class DriverObjectKind : std::uint8_t {
  visibleDevice,
  visibleStream,
  companionDevice,
  companionStream,
};

struct DriverObjectAddress final {
  std::uint32_t objectID = 0;
  std::size_t endpointIndex = 0;
  DriverObjectKind kind = DriverObjectKind::visibleDevice;

  [[nodiscard]] bool operator==(const DriverObjectAddress&) const = default;
};

struct EndpointObjectIDs final {
  std::uint32_t visibleDevice = 0;
  std::uint32_t visibleStream = 0;
  std::uint32_t companionDevice = 0;
  std::uint32_t companionStream = 0;

  [[nodiscard]] bool operator==(const EndpointObjectIDs&) const = default;
};

struct PublishedEndpoint final {
  EndpointDefinition definition;
  EndpointObjectIDs objectIDs;
  std::string visibleDeviceUID;
  std::string companionDeviceUID;

  [[nodiscard]] EndpointDirection companionDirection() const noexcept;
  [[nodiscard]] std::uint32_t inputDeviceObjectID() const noexcept;
  [[nodiscard]] std::uint32_t inputStreamObjectID() const noexcept;
  [[nodiscard]] std::uint32_t outputDeviceObjectID() const noexcept;
  [[nodiscard]] std::uint32_t outputStreamObjectID() const noexcept;
};

enum class EndpointRegistryError : std::uint8_t {
  none,
  tooManyEndpoints,
  zeroIdentifier,
  duplicateIdentifier,
  emptyName,
  nameTooLong,
  nameContainsControlCharacter,
  duplicateName,
  invalidSampleRate,
  invalidChannelCount,
};

struct EndpointRegistryResult final {
  EndpointRegistryError error = EndpointRegistryError::none;
  std::size_t endpointIndex = 0;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == EndpointRegistryError::none;
  }
};

/// A deterministic, bounded mapping from persistent endpoint identities to HAL object IDs.
///
/// This control-plane type performs no HAL calls. A successful replacement keeps object IDs for
/// unchanged identities, assigns IDs to new identities, and publishes one visible device plus one
/// hidden opposite-direction companion for every endpoint.
class EndpointRegistry final {
public:
  static constexpr std::size_t maximumEndpointCount = 32;
  static constexpr std::size_t maximumNameByteCount = 128;
  static constexpr std::uint32_t firstEndpointObjectID = 1000;

  EndpointRegistry() = default;

  [[nodiscard]] EndpointRegistryResult replace(std::span<const EndpointDefinition> definitions);
  [[nodiscard]] std::span<const PublishedEndpoint> endpoints() const noexcept;
  [[nodiscard]] std::optional<DriverObjectAddress>
  findObject(std::uint32_t objectID) const noexcept;
  [[nodiscard]] std::optional<std::uint32_t> findDeviceByUID(std::string_view uid) const noexcept;

  [[nodiscard]] static std::string identifierString(const EndpointIdentifier& identifier);
  [[nodiscard]] static std::string visibleDeviceUID(const EndpointDefinition& endpoint);
  [[nodiscard]] static std::string companionDeviceUID(const EndpointDefinition& endpoint);

private:
  struct IdentityAllocation final {
    EndpointIdentifier identifier{};
    std::size_t slot = 0;
    EndpointObjectIDs objectIDs;
  };

  [[nodiscard]] static EndpointRegistryResult
  validate(std::span<const EndpointDefinition> definitions);
  [[nodiscard]] std::optional<std::size_t>
  existingSlot(const EndpointIdentifier& identifier) const noexcept;
  [[nodiscard]] static EndpointObjectIDs objectIDs(std::size_t slot) noexcept;

  std::vector<PublishedEndpoint> endpoints_;
  std::vector<IdentityAllocation> allocations_;
};

} // namespace rilliya::audio_driver
