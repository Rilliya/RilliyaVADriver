#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

configuration="${1:-Debug}"
derived_data="${2:-.build/DerivedData}"

case "$configuration" in
  Debug | Release) ;;
  *)
    echo "Configuration must be Debug or Release" >&2
    exit 64
    ;;
esac

./scripts/generate-project.sh

xcodebuild build \
  -project RilliyaVADriver.xcodeproj \
  -scheme RilliyaVADriver \
  -configuration "$configuration" \
  -destination 'generic/platform=macOS' \
  -derivedDataPath "$derived_data" \
  CODE_SIGNING_ALLOWED=NO \
  CODE_SIGNING_REQUIRED=NO

product_name="$(plutil -extract productName raw Configuration/ProductConfiguration.json)"
./scripts/check-bundle.sh \
  "$derived_data/Build/Products/$configuration/$product_name.driver"

echo "$configuration RilliyaVADriver bundle passed validation"
