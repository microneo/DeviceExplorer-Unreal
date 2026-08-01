#!/usr/bin/env bash
set -euo pipefail

scenario="default"
port="5173"
skip_install="0"

usage() {
  echo "Usage: $(basename "$0") [--scenario default|offline|empty|errors] [--port N] [--skip-install]" >&2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --scenario) scenario="${2:-}"; shift 2 ;;
    --port) port="${2:-}"; shift 2 ;;
    --skip-install) skip_install="1"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage; exit 1 ;;
  esac
done

case "${scenario}" in
  default) mode="mock" ;;
  offline|empty|errors) mode="mock-${scenario}" ;;
  *) echo "Unknown scenario: ${scenario}" >&2; usage; exit 1 ;;
esac

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
plugin_root="$(cd -- "${script_dir}/.." && pwd)"
web_root="${plugin_root}/WebUI"
npm_cache_root="${TMPDIR:-/tmp}/deviceexplorer-npm-cache"

command -v node >/dev/null 2>&1 || {
  echo "Node.js was not found. Install a current Node.js LTS release to run DeviceExplorer WebUI." >&2
  exit 1
}
command -v npm >/dev/null 2>&1 || {
  echo "npm was not found." >&2
  exit 1
}

cd "${web_root}"
if [[ "${skip_install}" == "0" && ! -d node_modules ]]; then
  npm ci --cache "${npm_cache_root}"
fi

echo "DeviceExplorer WebUI mock (${scenario}) on http://127.0.0.1:${port}"
echo "No Unreal Engine or DeviceExplorerHost required. Press Ctrl+C to stop."
exec npx vite --mode "${mode}" --port "${port}"
