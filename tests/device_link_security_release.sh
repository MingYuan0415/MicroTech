#!/bin/sh

set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
project_root="$(CDPATH= cd -- "$script_dir/.." && pwd)"
build_dir="${DEVICE_LINK_SECURITY_RELEASE_BUILD_DIR:-/tmp/mt-device-link-security-release}"

cmake -S "$project_root/layers/middleware/components/ble_runtime/tests/host" \
    -B "$build_dir" -G Ninja -DBLE_RUNTIME_SANITIZER=none
cmake --build "$build_dir" --target device_link_security_release
