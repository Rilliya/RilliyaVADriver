#!/bin/bash

set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "Usage: $0 <arm64-driver> <intel-driver> <output-driver>" >&2
  exit 64
fi

arm64_driver=$1
intel_driver=$2
output_driver=$3
executable_path=Contents/MacOS/RilliyaVADriver

for driver in "$arm64_driver" "$intel_driver"; do
  if [[ ! -d "$driver" ]]; then
    echo "Driver does not exist: $driver" >&2
    exit 66
  fi
  plutil -lint "$driver/Contents/Info.plist"
done

if [[ -e "$output_driver" ]]; then
  echo "Output already exists: $output_driver" >&2
  exit 73
fi

mach_o_paths() {
  local driver=$1
  find "$driver" -type f -print0 | while IFS= read -r -d '' file_path; do
    if file -b "$file_path" | grep -q 'Mach-O'; then
      printf '%s\n' "${file_path#"$driver"/}"
    fi
  done | LC_ALL=C sort
}

arm64_mach_o_paths="$(mach_o_paths "$arm64_driver")"
intel_mach_o_paths="$(mach_o_paths "$intel_driver")"
if [[ "$arm64_mach_o_paths" != "$executable_path" || "$intel_mach_o_paths" != "$executable_path" ]]; then
  echo "Unexpected Mach-O layout; update Universal driver assembly before shipping new executables" >&2
  exit 65
fi

if [[ "$(lipo -archs "$arm64_driver/$executable_path")" != "arm64" ]]; then
  echo "Apple Silicon driver is not arm64-only" >&2
  exit 65
fi

if [[ "$(lipo -archs "$intel_driver/$executable_path")" != "x86_64" ]]; then
  echo "Intel driver is not x86_64-only" >&2
  exit 65
fi

if ! diff -qr \
  -x RilliyaVADriver \
  -x _CodeSignature \
  "$arm64_driver" \
  "$intel_driver" >/dev/null; then
  echo "Architecture-specific driver resources differ" >&2
  exit 65
fi

ditto "$arm64_driver" "$output_driver"
rm -rf "$output_driver/Contents/_CodeSignature"
lipo -create \
  "$arm64_driver/$executable_path" \
  "$intel_driver/$executable_path" \
  -output "$output_driver/$executable_path"
