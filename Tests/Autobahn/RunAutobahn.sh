#!/usr/bin/env bash
set -euo pipefail

mode="${1:-}"
agent="${2:-build/native/dexp-autobahn-agent}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
reports="$root/build/autobahn-reports"
image="crossbario/autobahn-testsuite:25.10.1"
container="deviceexplorer-autobahn-server"

wait_for_port() {
  local host="$1"
  local port="$2"
  for ((attempt = 0; attempt < 100; ++attempt)); do
    if exec 3<>"/dev/tcp/$host/$port" 2>/dev/null; then
      exec 3<&-
      exec 3>&-
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for $host:$port" >&2
  return 1
}

# Docker publishes the port before wstest listens behind it, so a plain connect succeeds
# against a proxy that cannot forward yet and the agent then fails to read the case count.
# Only an upgrade the fuzzing server itself answers proves readiness.
wait_for_fuzzing_server() {
  local host="$1"
  local port="$2"
  local status
  for ((attempt = 0; attempt < 100; ++attempt)); do
    if exec 3<>"/dev/tcp/$host/$port" 2>/dev/null; then
      printf 'GET /getCaseCount HTTP/1.1\r\nHost: %s:%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n' \
        "$host" "$port" >&3
      status=""
      IFS= read -r -t 2 -u 3 status || true
      exec 3<&-
      exec 3>&-
      if [[ "$status" == HTTP/1.1\ 101* ]]; then
        return 0
      fi
    fi
    sleep 0.2
  done
  echo "Timed out waiting for the Autobahn fuzzing server on $host:$port" >&2
  return 1
}

if [[ ! -x "$agent" ]]; then
  echo "Autobahn agent not found: $agent" >&2
  exit 2
fi
mkdir -p "$reports/clients" "$reports/servers"

if [[ "$mode" == "client" ]]; then
  docker rm -f "$container" >/dev/null 2>&1 || true
  docker run --rm -d \
    --name "$container" \
    -p 9001:9001 \
    -v "$root/Tests/Autobahn:/config:ro" \
    -v "$reports:/reports" \
    "$image" >/dev/null
  cleanup() { docker stop "$container" >/dev/null 2>&1 || true; }
  trap cleanup EXIT
  wait_for_fuzzing_server 127.0.0.1 9001
  "$agent" client --host 127.0.0.1 --port 9001 --agent DeviceExplorerWire
elif [[ "$mode" == "server" ]]; then
  "$agent" server --host 0.0.0.0 --port 9002 &
  agent_pid=$!
  cleanup() { kill "$agent_pid" >/dev/null 2>&1 || true; }
  trap cleanup EXIT
  wait_for_port 127.0.0.1 9002
  docker run --rm \
    --add-host host.docker.internal:host-gateway \
    -v "$root/Tests/Autobahn:/config:ro" \
    -v "$reports:/reports" \
    "$image" wstest --mode fuzzingclient --spec /config/fuzzingclient.json
else
  echo "usage: $0 <client|server> [path-to-dexp-autobahn-agent]" >&2
  exit 2
fi
