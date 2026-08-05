#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
plugin_root="$(cd -- "$script_dir/.." && pwd)"

build_dir="$plugin_root/build/native-host"
configuration="Release"
install=0
install_root=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      [[ $# -ge 2 ]] || { echo "--build-dir requires a value" >&2; exit 2; }
      build_dir="$2"
      shift 2
      ;;
    --configuration)
      [[ $# -ge 2 ]] || { echo "--configuration requires a value" >&2; exit 2; }
      configuration="$2"
      shift 2
      ;;
    --install)
      install=1
      shift
      ;;
    --install-root)
      [[ $# -ge 2 ]] || { echo "--install-root requires a value" >&2; exit 2; }
      install=1
      install_root="$2"
      shift 2
      ;;
    --help)
      echo "usage: $0 [--build-dir PATH] [--configuration NAME] [--install] [--install-root PATH]"
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

arguments=("$build_dir" "$configuration")
if [[ $install -eq 1 ]]; then
  arguments+=(--install)
  if [[ -n "$install_root" ]]; then
    arguments+=("$install_root")
  fi
fi

exec "$script_dir/BuildDeviceExplorerNativeHost.sh" "${arguments[@]}"
