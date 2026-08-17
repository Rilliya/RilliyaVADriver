#pragma once

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>

#include "ProductConfiguration.hpp"

namespace rilliya::audio_driver {

constexpr AudioObjectPropertySelector endpointCatalogProperty = 'rlct';

/// Returns the process-wide driver state to the state a freshly loaded plug-in has.
///
/// For tests only. The driver is a singleton, so asking the factory again hands back the same
/// state; without this nothing could cover what happens when coreaudiod loads the plug-in afresh
/// and it has to restore the catalog it stored.
void resetDriverStateForTesting();

} // namespace rilliya::audio_driver

extern "C" void* RILLIYA_VA_DRIVER_FACTORY_SYMBOL(CFAllocatorRef allocator,
                                                  CFUUIDRef requestedTypeUUID);
