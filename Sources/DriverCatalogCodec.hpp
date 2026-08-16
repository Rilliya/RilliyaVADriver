#pragma once

#include "EndpointRegistry.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rilliya::audio_driver {

constexpr std::uint64_t driverCatalogSchemaVersion = 1;

struct DriverEndpointCatalog final {
  std::uint64_t revision = 0;
  std::vector<EndpointDefinition> endpoints;
};

enum class DriverCatalogCodecError : std::uint8_t {
  none,
  rootIsNotDictionary,
  missingSchemaVersion,
  unsupportedSchemaVersion,
  missingRevision,
  invalidRevision,
  missingEndpoints,
  endpointsIsNotArray,
  tooManyEndpoints,
  endpointIsNotDictionary,
  missingIdentifier,
  invalidIdentifier,
  missingName,
  invalidName,
  missingDirection,
  invalidDirection,
  missingSampleRate,
  invalidSampleRate,
  missingChannelCount,
  invalidChannelCount,
  duplicateIdentifier,
  duplicateName,
};

struct DriverCatalogDecodeResult final {
  DriverCatalogCodecError error = DriverCatalogCodecError::none;
  std::size_t endpointIndex = 0;
  DriverEndpointCatalog catalog;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == DriverCatalogCodecError::none;
  }
};

/// Decodes a strictly bounded catalog from the public Core Foundation property-list surface.
///
/// The returned catalog is populated only after the entire property list has passed validation.
[[nodiscard]] DriverCatalogDecodeResult decodeDriverCatalog(CFPropertyListRef propertyList);

/// Creates a retained property-list dictionary suitable for driver storage and custom properties.
/// The caller owns the returned object.
[[nodiscard]] CFDictionaryRef createDriverCatalogPropertyList(const DriverEndpointCatalog& catalog);

} // namespace rilliya::audio_driver
