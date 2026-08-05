#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
plugin_root="$(cd -- "$script_dir/.." && pwd)"
build_dir="${1:-$plugin_root/build/native-host}"
configuration="${2:-Release}"

configure_args=(
  -S "$plugin_root/Native"
  -B "$build_dir"
  "-DCMAKE_BUILD_TYPE=$configuration"
  -DDEVICEEXPLORER_BUILD_HOST=ON
)
if [[ -n "${DEVICEEXPLORER_ASIO_ROOT:-}" ]]; then
  configure_args+=("-DDEVICEEXPLORER_ASIO_ROOT=$DEVICEEXPLORER_ASIO_ROOT")
fi

cmake "${configure_args[@]}"
cmake --build "$build_dir" --config "$configuration" --parallel
ctest --test-dir "$build_dir" -C "$configuration" --output-on-failure

echo "DeviceExplorer native host built in $build_dir."
