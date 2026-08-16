#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

./scripts/check-product-configuration.sh
./scripts/test-product-configuration.sh
xcrun swift format lint --strict scripts/generate-product-configuration.swift

build_directory=".build/DriverCoreTests"
ring_test_binary="$build_directory/RealtimeAudioRingTests"
registry_test_binary="$build_directory/EndpointRegistryTests"
catalog_test_binary="$build_directory/DriverCatalogCodecTests"
ring_tsan_binary="$build_directory/RealtimeAudioRingTests-TSan"
runtime_test_binary="$build_directory/DriverRuntimeTests"
interface_test_binary="$build_directory/AudioServerPlugInDriverTests"
driver_binary="$build_directory/RilliyaVADriver"

mkdir -p "$build_directory"

xcrun clang-format --dry-run --Werror \
  Sources/RealtimeAudioRing.hpp \
  Sources/RealtimeAudioRing.cpp \
  Sources/EndpointRegistry.hpp \
  Sources/EndpointRegistry.cpp \
  Sources/DriverCatalogCodec.hpp \
  Sources/DriverCatalogCodec.cpp \
  Sources/DriverRuntime.hpp \
  Sources/DriverRuntime.cpp \
  Sources/AudioServerPlugInDriver.hpp \
  Sources/AudioServerPlugInDriver.cpp \
  Tests/RealtimeAudioRingTests.cpp \
  Tests/EndpointRegistryTests.cpp \
  Tests/DriverCatalogCodecTests.cpp \
  Tests/DriverRuntimeTests.cpp \
  Tests/AudioServerPlugInDriverTests.cpp \
  Tests/DriverBundleTests.cpp

xcrun clang++ \
  -std=c++20 \
  -O1 \
  -g \
  -Wall \
  -Wextra \
  -Wconversion \
  -Werror \
  -fsanitize=address,undefined \
  -fno-omit-frame-pointer \
  -I Sources \
  -I Generated \
  Sources/RealtimeAudioRing.cpp \
  Tests/RealtimeAudioRingTests.cpp \
  -o "$ring_test_binary"

ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$ring_test_binary"

xcrun clang++ \
  -std=c++20 \
  -O1 \
  -g \
  -Wall \
  -Wextra \
  -Wconversion \
  -Werror \
  -fsanitize=thread \
  -fno-omit-frame-pointer \
  -I Sources \
  -I Generated \
  Sources/RealtimeAudioRing.cpp \
  Tests/RealtimeAudioRingTests.cpp \
  -o "$ring_tsan_binary"

"$ring_tsan_binary"

xcrun clang++ \
  -std=c++20 \
  -O1 \
  -g \
  -Wall \
  -Wextra \
  -Wconversion \
  -Werror \
  -fsanitize=address,undefined \
  -fno-omit-frame-pointer \
  -I Sources \
  -I Generated \
  Sources/EndpointRegistry.cpp \
  Tests/EndpointRegistryTests.cpp \
  -o "$registry_test_binary"

ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$registry_test_binary"

xcrun clang++ \
  -std=c++20 \
  -O1 \
  -g \
  -Wall \
  -Wextra \
  -Wconversion \
  -Werror \
  -fsanitize=address,undefined \
  -fno-omit-frame-pointer \
  -I Sources \
  -I Generated \
  Sources/EndpointRegistry.cpp \
  Sources/DriverCatalogCodec.cpp \
  Tests/DriverCatalogCodecTests.cpp \
  -framework CoreFoundation \
  -o "$catalog_test_binary"

ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$catalog_test_binary"

xcrun clang++ \
  -std=c++20 \
  -O1 \
  -g \
  -Wall \
  -Wextra \
  -Wconversion \
  -Werror \
  -fsanitize=address,undefined \
  -fno-omit-frame-pointer \
  -I Sources \
  -I Generated \
  Sources/RealtimeAudioRing.cpp \
  Sources/EndpointRegistry.cpp \
  Sources/DriverRuntime.cpp \
  Tests/DriverRuntimeTests.cpp \
  -o "$runtime_test_binary"

ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$runtime_test_binary"

xcrun clang++ \
  -std=c++20 \
  -O1 \
  -g \
  -Wall \
  -Wextra \
  -Wconversion \
  -Werror \
  -fsanitize=address,undefined \
  -fno-omit-frame-pointer \
  -I Sources \
  -I Generated \
  Sources/RealtimeAudioRing.cpp \
  Sources/EndpointRegistry.cpp \
  Sources/DriverCatalogCodec.cpp \
  Sources/DriverRuntime.cpp \
  Sources/AudioServerPlugInDriver.cpp \
  Tests/AudioServerPlugInDriverTests.cpp \
  -framework CoreAudio \
  -framework CoreFoundation \
  -o "$interface_test_binary"

ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$interface_test_binary"

xcrun clang++ \
  -std=c++20 \
  -O2 \
  -Wall \
  -Wextra \
  -Wconversion \
  -Werror \
  -I Sources \
  -I Generated \
  Sources/RealtimeAudioRing.cpp \
  Sources/EndpointRegistry.cpp \
  Sources/DriverCatalogCodec.cpp \
  Sources/DriverRuntime.cpp \
  Sources/AudioServerPlugInDriver.cpp \
  -framework CoreAudio \
  -framework CoreFoundation \
  -bundle \
  -o "$driver_binary"

factory_identifier="$(plutil -extract factoryUUID raw Configuration/ProductConfiguration.json)"
factory_symbol_expected="$(plutil -extract factorySymbol raw Configuration/ProductConfiguration.json)"
nm -gU "$driver_binary" | grep -q "_${factory_symbol_expected}$"
plutil -lint Resources/Info.plist

audio_server_plugin_type="443ABAB8-E7B3-491A-B985-BEB9187030DB"
factory_symbol="$(/usr/libexec/PlistBuddy -c "Print :CFPlugInFactories:$factory_identifier" \
  Resources/Info.plist)"
registered_factory="$(/usr/libexec/PlistBuddy \
  -c "Print :CFPlugInTypes:$audio_server_plugin_type:0" \
  Resources/Info.plist)"

[ "$factory_symbol" = "$factory_symbol_expected" ]
[ "$registered_factory" = "$factory_identifier" ]

./scripts/build.sh Debug .build/DerivedData-Debug
./scripts/build.sh Release .build/DerivedData-Release

echo "All RilliyaVADriver checks passed"
