#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

xcrun swift scripts/generate-product-configuration.swift \
  Configuration/ProductConfiguration.json \
  Generated/ProductConfiguration.hpp \
  Generated/ProductConfiguration.xcconfig \
  Resources/Info.plist
