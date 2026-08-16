#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

temporary_directory="$(mktemp -d "${TMPDIR:-/tmp}/rilliya-va-configuration.XXXXXX")"
trap 'rm -rf "$temporary_directory"' EXIT

xcrun swift scripts/generate-product-configuration.swift \
  Configuration/ProductConfiguration.json \
  "$temporary_directory/ProductConfiguration.hpp" \
  "$temporary_directory/ProductConfiguration.xcconfig" \
  "$temporary_directory/Info.plist"

diff -u Generated/ProductConfiguration.hpp "$temporary_directory/ProductConfiguration.hpp"
diff -u Generated/ProductConfiguration.xcconfig "$temporary_directory/ProductConfiguration.xcconfig"
diff -u Resources/Info.plist "$temporary_directory/Info.plist"
plutil -lint Resources/Info.plist
