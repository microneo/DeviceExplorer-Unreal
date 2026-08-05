#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: $0 EXECUTABLE [INSTALL_ROOT]" >&2
  exit 2
fi

executable="$(cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")"
if [[ ! -x "$executable" ]]; then
  echo "native host executable not found: $executable" >&2
  exit 1
fi

if [[ $# -eq 2 ]]; then
  install_root="$2"
elif [[ "$(uname -s)" == "Darwin" ]]; then
  install_root="${HOME:?HOME is not set}/Library/Application Support/DeviceExplorer/Host"
else
  install_root="${XDG_STATE_HOME:-${HOME:?HOME is not set}/.local/state}/DeviceExplorer/Host"
fi

build_id="$($executable --version-json | python3 -c 'import json,sys; print(json.load(sys.stdin)["build_id"])')"
build_id="$(printf '%s' "$build_id" | tr -c 'A-Za-z0-9._-' '_')"
if [[ -z "$build_id" ]]; then
  echo "the native host returned an empty build id" >&2
  exit 1
fi

version_directory="$install_root/versions/$build_id"
installed_executable="$version_directory/DeviceExplorerHost"
mkdir -p -- "$version_directory"
# Copying straight onto a running host fails with ETXTBSY; a rename replaces the
# directory entry instead and leaves the running process on the old inode.
staged="$version_directory/DeviceExplorerHost.$$.tmp"
cp -- "$executable" "$staged"
chmod +x "$staged"
mv -f -- "$staged" "$installed_executable"

temporary_pointer="$install_root/current.txt.$$.tmp"
printf 'versions/%s/DeviceExplorerHost\n' "$build_id" > "$temporary_pointer"
mv -f -- "$temporary_pointer" "$install_root/current.txt"
echo "Installed DeviceExplorer native host $build_id in $version_directory."
