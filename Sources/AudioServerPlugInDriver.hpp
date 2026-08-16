#pragma once

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>

#include "ProductConfiguration.hpp"

namespace rilliya::audio_driver {

constexpr AudioObjectPropertySelector endpointCatalogProperty = 'rlct';

} // namespace rilliya::audio_driver

extern "C" void* RILLIYA_VA_DRIVER_FACTORY_SYMBOL(CFAllocatorRef allocator,
                                                  CFUUIDRef requestedTypeUUID);
