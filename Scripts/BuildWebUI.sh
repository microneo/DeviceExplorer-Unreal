#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
plugin_root="$(cd -- "${script_dir}/.." && pwd)"
web_root="${plugin_root}/WebUI"
output_root="${plugin_root}/Resources/Web"
npm_cache_root="${TMPDIR:-/tmp}/deviceexplorer-npm-cache"

command -v node >/dev/null 2>&1 || {
  echo "Node.js was not found. Install a current Node.js LTS release to rebuild DeviceExplorer WebUI." >&2
  exit 1
}
command -v npm >/dev/null 2>&1 || {
  echo "npm was not found." >&2
  exit 1
}

cd "${web_root}"
npm ci --cache "${npm_cache_root}"
npm run build

test -f "${output_root}/index.html"
test -f "${output_root}/ui-manifest.json"
echo "DeviceExplorer WebUI updated: ${output_root}"
