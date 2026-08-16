#include "EndpointRegistry.hpp"

#include "ProductConfiguration.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace rilliya::audio_driver {
namespace {

[[nodiscard]] bool isZeroIdentifier(const EndpointIdentifier& identifier) noexcept {
  return std::all_of(identifier.begin(), identifier.end(),
                     [](std::uint8_t byte) { return byte == 0; });
}

[[nodiscard]] bool isASCIIControl(std::uint8_t byte) noexcept {
  return byte < 0x20 || byte == 0x7F;
}

[[nodiscard]] std::string ASCIINameKey(std::string_view name) {
  std::string key(name);
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char character) {
    if (character >= 'A' && character <= 'Z') {
      return static_cast<char>(character - 'A' + 'a');
    }
    return static_cast<char>(character);
  });
  return key;
}

[[nodiscard]] std::string directionComponent(EndpointDirection direction) {
  return direction == EndpointDirection::input ? "input" : "output";
}

} // namespace

EndpointDirection PublishedEndpoint::companionDirection() const noexcept {
  return definition.direction == EndpointDirection::input ? EndpointDirection::output
                                                          : EndpointDirection::input;
}

std::uint32_t PublishedEndpoint::inputDeviceObjectID() const noexcept {
  return definition.direction == EndpointDirection::input ? objectIDs.visibleDevice
                                                          : objectIDs.companionDevice;
}

std::uint32_t PublishedEndpoint::inputStreamObjectID() const noexcept {
  return definition.direction == EndpointDirection::input ? objectIDs.visibleStream
                                                          : objectIDs.companionStream;
}

std::uint32_t PublishedEndpoint::outputDeviceObjectID() const noexcept {
  return definition.direction == EndpointDirection::output ? objectIDs.visibleDevice
                                                           : objectIDs.companionDevice;
}

std::uint32_t PublishedEndpoint::outputStreamObjectID() const noexcept {
  return definition.direction == EndpointDirection::output ? objectIDs.visibleStream
                                                           : objectIDs.companionStream;
}

EndpointRegistryResult EndpointRegistry::replace(std::span<const EndpointDefinition> definitions) {
  const EndpointRegistryResult validation = validate(definitions);
  if (!validation) {
    return validation;
  }

  std::vector<PublishedEndpoint> replacement;
  std::vector<IdentityAllocation> replacementAllocations;
  std::array<bool, maximumEndpointCount> usedSlots{};
  replacement.reserve(definitions.size());
  replacementAllocations.reserve(definitions.size());
  for (const EndpointDefinition& definition : definitions) {
    const std::optional<std::size_t> slot = existingSlot(definition.identifier);
    if (slot.has_value()) {
      usedSlots[*slot] = true;
    }
  }

  for (const EndpointDefinition& definition : definitions) {
    std::optional<std::size_t> slot = existingSlot(definition.identifier);
    if (!slot.has_value()) {
      const auto freeSlot = std::find(usedSlots.begin(), usedSlots.end(), false);
      if (freeSlot == usedSlots.end()) {
        return {EndpointRegistryError::tooManyEndpoints, replacement.size()};
      }
      slot = static_cast<std::size_t>(std::distance(usedSlots.begin(), freeSlot));
      usedSlots[*slot] = true;
    }
    const EndpointObjectIDs allocation = objectIDs(*slot);
    replacementAllocations.push_back({definition.identifier, *slot, allocation});
    replacement.push_back(PublishedEndpoint{
        .definition = definition,
        .objectIDs = allocation,
        .visibleDeviceUID = visibleDeviceUID(definition),
        .companionDeviceUID = companionDeviceUID(definition),
    });
  }

  endpoints_ = std::move(replacement);
  allocations_ = std::move(replacementAllocations);
  return {};
}

std::span<const PublishedEndpoint> EndpointRegistry::endpoints() const noexcept {
  return endpoints_;
}

std::optional<DriverObjectAddress>
EndpointRegistry::findObject(std::uint32_t objectID) const noexcept {
  for (std::size_t index = 0; index < endpoints_.size(); ++index) {
    const EndpointObjectIDs& objectIDs = endpoints_[index].objectIDs;
    if (objectID == objectIDs.visibleDevice) {
      return DriverObjectAddress{objectID, index, DriverObjectKind::visibleDevice};
    }
    if (objectID == objectIDs.visibleStream) {
      return DriverObjectAddress{objectID, index, DriverObjectKind::visibleStream};
    }
    if (objectID == objectIDs.companionDevice) {
      return DriverObjectAddress{objectID, index, DriverObjectKind::companionDevice};
    }
    if (objectID == objectIDs.companionStream) {
      return DriverObjectAddress{objectID, index, DriverObjectKind::companionStream};
    }
  }
  return std::nullopt;
}

std::optional<std::uint32_t>
EndpointRegistry::findDeviceByUID(std::string_view uid) const noexcept {
  for (const PublishedEndpoint& endpoint : endpoints_) {
    if (uid == endpoint.visibleDeviceUID) {
      return endpoint.objectIDs.visibleDevice;
    }
    if (uid == endpoint.companionDeviceUID) {
      return endpoint.objectIDs.companionDevice;
    }
  }
  return std::nullopt;
}

std::string EndpointRegistry::identifierString(const EndpointIdentifier& identifier) {
  constexpr std::size_t stringLength = 36;
  std::array<char, stringLength + 1> buffer{};
  std::snprintf(buffer.data(), buffer.size(),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                identifier[0], identifier[1], identifier[2], identifier[3], identifier[4],
                identifier[5], identifier[6], identifier[7], identifier[8], identifier[9],
                identifier[10], identifier[11], identifier[12], identifier[13], identifier[14],
                identifier[15]);
  return std::string(buffer.data(), stringLength);
}

std::string EndpointRegistry::visibleDeviceUID(const EndpointDefinition& endpoint) {
  return std::string(product_configuration::deviceUIDPrefix) +
         identifierString(endpoint.identifier) + "." + directionComponent(endpoint.direction);
}

std::string EndpointRegistry::companionDeviceUID(const EndpointDefinition& endpoint) {
  const std::string role = endpoint.direction == EndpointDirection::input ? "feeder" : "reader";
  return std::string(product_configuration::deviceUIDPrefix) +
         identifierString(endpoint.identifier) + ".internal." + role;
}

EndpointRegistryResult EndpointRegistry::validate(std::span<const EndpointDefinition> definitions) {
  if (definitions.size() > maximumEndpointCount) {
    return {EndpointRegistryError::tooManyEndpoints, maximumEndpointCount};
  }

  for (std::size_t index = 0; index < definitions.size(); ++index) {
    const EndpointDefinition& endpoint = definitions[index];
    if (isZeroIdentifier(endpoint.identifier)) {
      return {EndpointRegistryError::zeroIdentifier, index};
    }
    if (endpoint.name.empty()) {
      return {EndpointRegistryError::emptyName, index};
    }
    if (endpoint.name.size() > maximumNameByteCount) {
      return {EndpointRegistryError::nameTooLong, index};
    }
    if (std::any_of(endpoint.name.begin(), endpoint.name.end(), [](char character) {
          return isASCIIControl(static_cast<std::uint8_t>(character));
        })) {
      return {EndpointRegistryError::nameContainsControlCharacter, index};
    }
    if (!std::isfinite(endpoint.sampleRate) || endpoint.sampleRate < 1.0 ||
        endpoint.sampleRate > 768000.0 || std::trunc(endpoint.sampleRate) != endpoint.sampleRate) {
      return {EndpointRegistryError::invalidSampleRate, index};
    }
    if (endpoint.channelCount == 0 || endpoint.channelCount > 256) {
      return {EndpointRegistryError::invalidChannelCount, index};
    }

    for (std::size_t priorIndex = 0; priorIndex < index; ++priorIndex) {
      if (definitions[priorIndex].identifier == endpoint.identifier) {
        return {EndpointRegistryError::duplicateIdentifier, index};
      }
      if (ASCIINameKey(definitions[priorIndex].name) == ASCIINameKey(endpoint.name)) {
        return {EndpointRegistryError::duplicateName, index};
      }
    }
  }

  return {};
}

std::optional<std::size_t>
EndpointRegistry::existingSlot(const EndpointIdentifier& identifier) const noexcept {
  const auto iterator = std::find_if(
      allocations_.begin(), allocations_.end(),
      [&](const IdentityAllocation& allocation) { return allocation.identifier == identifier; });
  if (iterator == allocations_.end()) {
    return std::nullopt;
  }
  return iterator->slot;
}

EndpointObjectIDs EndpointRegistry::objectIDs(std::size_t slot) noexcept {
  const std::uint32_t base = firstEndpointObjectID + static_cast<std::uint32_t>(slot * 4);
  return EndpointObjectIDs{
      .visibleDevice = base,
      .visibleStream = base + 1,
      .companionDevice = base + 2,
      .companionStream = base + 3,
  };
}

} // namespace rilliya::audio_driver
