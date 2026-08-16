#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

temporary_directory="$(mktemp -d "${TMPDIR:-/tmp}/rilliya-va-configuration-tests.XXXXXX")"
trap 'rm -rf "$temporary_directory"' EXIT

generate() {
  configuration="$1"
  output="$2"
  xcrun swift scripts/generate-product-configuration.swift \
    "$configuration" \
    "$output/ProductConfiguration.hpp" \
    "$output/ProductConfiguration.xcconfig" \
    "$output/Info.plist"
}

expect_failure() {
  configuration="$1"
  output="$2"
  if generate "$configuration" "$output" >/dev/null 2>&1; then
    echo "Expected product configuration generation to fail: $configuration" >&2
    exit 1
  fi
}

valid_configuration="$temporary_directory/valid.json"
cp Configuration/ProductConfiguration.json "$valid_configuration"
generate "$valid_configuration" "$temporary_directory/valid"

zero_uuid_configuration="$temporary_directory/zero-uuid.json"
cp Configuration/ProductConfiguration.json "$zero_uuid_configuration"
plutil -replace factoryUUID -string '00000000-0000-0000-0000-000000000000' \
  "$zero_uuid_configuration"
expect_failure "$zero_uuid_configuration" "$temporary_directory/zero-uuid"

duplicate_identity_configuration="$temporary_directory/duplicate-identity.json"
cp Configuration/ProductConfiguration.json "$duplicate_identity_configuration"
bundle_identifier="$(plutil -extract bundleIdentifier raw "$duplicate_identity_configuration")"
plutil -replace packageIdentifier -string "$bundle_identifier" "$duplicate_identity_configuration"
expect_failure "$duplicate_identity_configuration" "$temporary_directory/duplicate-identity"

invalid_prefix_configuration="$temporary_directory/invalid-prefix.json"
cp Configuration/ProductConfiguration.json "$invalid_prefix_configuration"
plutil -replace deviceUIDPrefix -string 'not a reverse DNS prefix' \
  "$invalid_prefix_configuration"
expect_failure "$invalid_prefix_configuration" "$temporary_directory/invalid-prefix"

downstream_configuration="$temporary_directory/downstream.json"
cp Configuration/ProductConfiguration.json "$downstream_configuration"
plutil -replace productName -string 'ExampleVADriver' "$downstream_configuration"
plutil -replace bundleIdentifier -string 'com.example.audio.driver' "$downstream_configuration"
plutil -replace packageIdentifier -string 'com.example.audio.driver.pkg' \
  "$downstream_configuration"
plutil -replace factoryUUID -string '79A0DA62-30D1-4C4B-B328-EBDDFB597731' \
  "$downstream_configuration"
plutil -replace factorySymbol -string 'ExampleVADriverFactory' "$downstream_configuration"
plutil -replace manufacturerName -string 'Example Audio' "$downstream_configuration"
plutil -replace plugInName -string 'Example Virtual Audio' "$downstream_configuration"
plutil -replace modelUID -string 'com.example.audio.model' "$downstream_configuration"
plutil -replace deviceUIDPrefix -string 'com.example.audio.device.' "$downstream_configuration"
plutil -replace catalogStorageKey -string 'com.example.audio.catalog.v1' \
  "$downstream_configuration"
plutil -replace internalDeviceNamePrefix -string 'Example Internal ' \
  "$downstream_configuration"
generate "$downstream_configuration" "$temporary_directory/downstream"

grep -q 'ExampleVADriverFactory' "$temporary_directory/downstream/ProductConfiguration.hpp"
grep -q 'com.example.audio.driver' \
  "$temporary_directory/downstream/ProductConfiguration.xcconfig"
if grep -R -q 'moe.uwucocoa\|Rilliya' "$temporary_directory/downstream"; then
  echo "Generated downstream configuration retained Rilliya product identity" >&2
  exit 1
fi
