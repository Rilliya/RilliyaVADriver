// Generated from Configuration/ProductConfiguration.json. Do not edit directly.
#pragma once

#include <CoreFoundation/CoreFoundation.h>

#include <string_view>

#define RILLIYA_VA_DRIVER_FACTORY_SYMBOL RilliyaVADriverFactory

namespace rilliya::audio_driver::product_configuration {

inline constexpr std::string_view productName = "RilliyaVADriver";
inline constexpr std::string_view bundleIdentifier = "moe.uwucocoa.rilliya.virtual-audio-driver";
inline constexpr std::string_view packageIdentifier = "moe.uwucocoa.rilliya.virtual-audio-driver.pkg";
inline constexpr std::string_view factoryUUID = "961AA42D-4018-4A54-81A5-AF308770CF08";
inline constexpr std::string_view factorySymbol = "RilliyaVADriverFactory";
inline constexpr std::string_view deviceUIDPrefix = "moe.uwucocoa.rilliya.virtual.";
inline const CFStringRef catalogStorageKey = CFSTR("moe.uwucocoa.rilliya.virtual.catalog.v1");
inline const CFStringRef manufacturerName = CFSTR("Rilliya");
inline const CFStringRef plugInName = CFSTR("Rilliya Virtual Audio");
inline const CFStringRef modelUID = CFSTR("moe.uwucocoa.rilliya.virtual-audio");
inline constexpr std::string_view internalDeviceNamePrefix = "Rilliya Internal ";

} // namespace rilliya::audio_driver::product_configuration
