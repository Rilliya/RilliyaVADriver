#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
export CODE_SIGN_IDENTITY="${CODE_SIGN_IDENTITY:--}"

./scripts/check-product-configuration.sh
xcodegen generate
