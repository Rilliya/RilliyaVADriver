#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

product_name="$(plutil -extract productName raw Configuration/ProductConfiguration.json)"
factory_identifier="$(plutil -extract factoryUUID raw Configuration/ProductConfiguration.json)"
factory_symbol_expected="$(plutil -extract factorySymbol raw Configuration/ProductConfiguration.json)"
driver_bundle="${1:-.build/DerivedData/Build/Products/Debug/$product_name.driver}"
driver_binary="$driver_bundle/Contents/MacOS/$product_name"
driver_info="$driver_bundle/Contents/Info.plist"
bundle_test_binary=".build/DriverCoreTests/DriverBundleTests"
audio_server_plugin_type="443ABAB8-E7B3-491A-B985-BEB9187030DB"

[ -f "$driver_binary" ]
[ -f "$driver_info" ]
plutil -lint "$driver_info"

factory_symbol="$(/usr/libexec/PlistBuddy -c "Print :CFPlugInFactories:$factory_identifier" \
  "$driver_info")"
registered_factory="$(/usr/libexec/PlistBuddy \
  -c "Print :CFPlugInTypes:$audio_server_plugin_type:0" \
  "$driver_info")"

[ "$factory_symbol" = "$factory_symbol_expected" ]
[ "$registered_factory" = "$factory_identifier" ]
nm -gU "$driver_binary" | grep -q "_${factory_symbol_expected}$"

for architecture in $(lipo -archs "$driver_binary"); do
  unexpected_dependencies="$(otool -arch "$architecture" -L "$driver_binary" | \
    tail -n +2 | awk '{print $1}' | grep -Ev '^(/System/Library/|/usr/lib/)' || true)"
  [ -z "$unexpected_dependencies" ]
done

mkdir -p "$(dirname "$bundle_test_binary")"
xcrun clang++ \
  -std=c++20 \
  -O2 \
  -Wall \
  -Wextra \
  -Wconversion \
  -Werror \
  Tests/DriverBundleTests.cpp \
  -framework CoreAudio \
  -framework CoreFoundation \
  -o "$bundle_test_binary"

"$bundle_test_binary" "$driver_bundle"
