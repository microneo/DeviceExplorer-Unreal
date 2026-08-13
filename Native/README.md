# DeviceExplorer native build

This directory is the CMake composition root for code that does not depend on
Unreal Engine. D1 currently provides `dexp-wire`, a sans-I/O library for HTTP
WebSocket upgrades, RFC 6455 frames, DeviceExplorer DNS-SD packets, strict JSON,
and protocol message envelopes, plus `dexp-wire-tests` and the
`dexp-autobahn-agent` adapter. The standalone CMake target supports C++17; UE
5.7+ compiles the same sources as C++20 because that is the engine's minimum
supported module standard.

```sh
cmake -S Native -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel
ctest --test-dir build/native --output-on-failure
python Scripts/CheckWireSources.py
```

Run the same build with `-DCMAKE_UNITY_BUILD=ON` when checking changes locally.
CI compiles both normal and Unity configurations on Windows, macOS, and Linux;
the Unity leg catches anonymous-namespace collisions that separate translation
units cannot expose.

The wire sources remain canonical under `Source/DeviceExplorerWire` so UBT and
CMake compile the same files. `sources.cmake` is explicit by design; the check
script fails if a source or public header is added without updating it. It also
rejects socket, thread, file-system, Unreal, exception, and RTTI dependencies at
the wire boundary.

D1 contains WebSocket framing, mDNS, strict JSON, and the `type`/`request_id`
message envelope. The host and UE client share the WebSocket and mDNS codecs.
The UE client can use either the engine WebSockets implementation or the
sans-I/O codec through its built-in `FSocket` adapter. The Unreal runtime
intentionally keeps Unreal's JSON model; the neutral DOM is available
to standalone consumers without forcing a second parser into that path. No
socket, timer, thread, file-system, Unreal, exception, or RTTI dependency is
permitted in `dexp-wire`.

`ParseMdnsAnnouncement` accepts only zero-valued datagram padding after the
declared records. Presentation names escape literal dots and backslashes so a
parsed name can be encoded without losing DNS label boundaries. A normal host
shutdown, including the installed Ctrl+C handler, sends the TTL=0 announcement;
forced process termination cannot run cleanup and therefore relies on TTL
expiry.

After building the UE host with this module, rerun the protocol smoke with
`--expect-close-reply`. The flag verifies the RFC 6455 close handshake added by
the new host adapter while the default mode remains usable against the D0
legacy baseline.

See `Tests/Autobahn/README.md` for both directions of the RFC 6455 conformance
run and the explicitly excluded performance/compression groups.

## Standalone host

D2 builds two additional targets:

- `dexp-host-core`, a process-independent static library owning the standalone
  Asio event loop and listeners;
- `dexp-host`, a thin executable for arguments, signals, logging, and process
  exit.

The host exposes bounded HTTP and WebSocket parsing, a loopback-only dashboard,
an IPv4 device listener, port `0` support, and these routes on both listeners:

```text
GET /health
GET /host-manifest
```

The dashboard serves the committed WebUI and the complete protocol-10 API for
device registry, logs, console, commands, file browsing, data modules, streamed
file/directory transfers, and trace forwarding. The device listener accepts
mutually authenticated WebSocket sessions and credentialed streaming uploads.
It also advertises `_deviceexplorer._tcp.local.` with the token fingerprint.

The manifest is the same portable schema served by the legacy UE host and is
printed without opening listeners by:

```sh
build/native/dexp-host --version-json
```

`dexp-host` uses a per-user lock so two editor processes do not start competing
hosts. Pass `--state-dir` to isolate integration tests. Transfers are written to
`.part` files and atomically renamed only after the declared content length has
arrived; stale transfers and disconnected devices expire automatically.

The same state directory contains an atomically replaced `identity.json`.
`NodeId` remains stable for that OS user, `HostSession` is incremented and
persisted before listeners or mDNS start, and `InstanceId` changes for every
process. `/health` and `/api/config` expose the three values for diagnostics.
Peer rollback reconciliation uses the same `AdvancePast()` operation: a stale
process persists `max_seen + 1` before it closes provisional links, immediately
reannounces the corrected session, and reconnects.

Authenticated devices are bounded: the registry accepts at most 1024 entries
and each device log ring is limited both by line count and by 16 MiB of payload.
Pending request ids are accepted only from the device channel to which the
request was sent.

## Peer control plane

The first D3 control-plane slice is opt-in. Start two hosts in the same cluster
with:

```sh
build/native/dexp-host --enable-distributed --cluster-id studio \
  --peer-secret-file /path/to/user-only/peer-secret
```

Each host binds an ephemeral peer port by default and adds `cluster_id`,
`node_id`, `host_session`, `instance_id`, the peer port, and protocol range to
its existing DNS-SD TXT record. The lower `NodeId` initiates a direct link.
For networks without multicast, add one or more numeric IPv4 seeds:

```sh
build/native/dexp-host --enable-distributed --cluster-id studio \
  --peer-secret-file /path/to/user-only/peer-secret \
  --peer-seed 192.0.2.10:42112
```

The peer listener uses bounded length-prefixed JSON frames, a symmetric
`peer_hello`/`peer_hello_ack`, fixed version negotiation, a 64 KiB control
queue, bounded handshake/peer/session caches, reconnect backoff, and
application ping/pong. Both sides authenticate the complete handshake decision
with HMAC-SHA-256 over fresh nonces and both host identities. After that proof,
every control frame is encrypted and authenticated with a transcript-bound,
directional XChaCha20-Poly1305 key and a strictly monotonic sequence number.
Replay, reordering, plaintext downgrade, and modification close the link. The peer secret is
never published through DNS-SD; every host in a cluster must be configured with
the same high-entropy value. Prefer `--peer-secret-file`: the host reads it once
and removes it before opening listeners. The Editor uses this path so the secret
does not appear in the child process command line. `--peer-secret` remains for
backward-compatible manual launches. `ClusterId` remains only a discovery boundary.

`GET /api/peers` reports live links, authentication failures, identity
collisions, expired discovery candidates, and anomalous sessions. A peer that
remembers a newer local `HostSession` can trigger the persisted correction path
only after its proof is verified. Remote reads, writes, logs, and transfers
remain disabled until their explicit opt-in phases.

Peer protocol 3 is intentionally not wire-compatible with protocol 2. Upgrade
all hosts in a cluster together: during a mixed-version rollout the two groups
reject each other with `peer protocol ranges do not overlap`, while local device
and dashboard paths continue to work independently.

## Device identity and distributed roster

The runtime client stores `DeviceId` and `NextDeviceSession` together in
`Saved/DeviceExplorer/device-identity.txt`. It atomically reserves and persists
a monotonically increasing session before every socket attempt. A corrupt file
or a machine-marker mismatch creates a new `DeviceId`; a remembered newer
session produces a negative `attach_ack`, advances the file, and reconnects
without publishing the stale owner.

This attach contract raises the device protocol to 11; runtime clients and hosts
must be rebuilt together. A protocol-11 hello without both `device_session` and
`connection_id` is rejected before it can enter the local registry or roster.

Authenticated peer links exchange revisioned, bounded owner snapshots and
attach/detach deltas. Every update is tied to the `NodeId` and `HostSession` of
the protected link. Session precedes revision in the apply order; a revision gap
requests a fresh snapshot, and a delayed detach removes only an exact
`DeviceId`/`DeviceSession` match. Equal maximum device sessions from different
owners are exposed as `ambiguous_owner` and fence the local socket instead of
choosing a random winner.

`GET /api/roster` exposes this thin distributed view. It is separate from
`GET /api/devices`: the latter remains the local, operable registry until remote
read/write routing is implemented. Roster snapshots are split into bounded
parts of at most 16 devices, while the aggregate cache defaults to 4096 entries.
When the active link to an owner closes, its rows become `unreachable`; a link
for the same owner and host session restores them without churn, otherwise they
are removed after the default 60-second grace period.

The Editor's distributed settings are user settings under `Saved/Config`, not
project defaults. If an earlier development build already wrote `PeerSecret`
to `Config/DefaultEditorPerProjectUserSettings.ini`, remove that line from the
project and rotate the secret once.

Standalone Asio is pinned by commit and fetched during CMake configure. For an
offline or centrally managed dependency, point CMake at an existing checkout:

```sh
cmake -S Native -B build/native \
  -DDEVICEEXPLORER_ASIO_ROOT=/path/to/asio \
  -DDEVICEEXPLORER_MONOCYPHER_ROOT=/path/to/monocypher
```

The paths must contain `include/asio.hpp` and `src/monocypher.c`, respectively.
Neither dependency enters `DeviceExplorerWire`.
Convenience wrappers configure, build, and run the complete native test suite:

```powershell
.\Scripts\BuildDeviceExplorerNativeHost.ps1
```

```sh
Scripts/BuildDeviceExplorerNativeHost.sh
```

On macOS and Linux, `Scripts/BuildDeviceExplorer.sh --install` provides the
user-facing option form used by the Editor workflow.

Add `-Install` on PowerShell or `--install` as the third shell argument to copy
the executable into a versioned per-user directory and atomically switch
`current.txt`. The Editor prefers this installed native host after checking its
manifest and falls back to the legacy project binary for one migration
milestone. Running binaries are never overwritten.

The build id is regenerated at build time, not configure time. Source archives
without Git metadata can provide `-DDEVICEEXPLORER_BUILD_ID_OVERRIDE=...`.

`dexp-host-integration-tests` checks listener and routing invariants over real
TCP. When Python is available, `dexp-host-production-smoke` additionally starts
an isolated executable and verifies mutual authentication, logs, a proxied
command, streaming upload/download, trace forwarding, WebUI security headers,
and the per-user singleton lock.
