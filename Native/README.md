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

## Standalone host migration

The first D2 vertical slice builds two additional targets:

- `dexp-host-core`, a process-independent static library owning the standalone
  Asio event loop and listeners;
- `dexp-host`, a thin executable for arguments, signals, logging, and process
  exit.

It already exposes real listeners with bounded HTTP headers, loopback-only
dashboard binding, IPv4-only device binding, port `0` support, and these routes
on both listeners:

```text
GET /health
GET /host-manifest
```

The dashboard listener additionally exposes `GET /api/config`. The manifest is
the same portable schema served by the existing UE host and printed by:

```sh
build/native/dexp-host --version-json
```

This executable is intentionally a migration checkpoint, not yet a replacement
for `DeviceExplorerHost`: `/device/connect` returns `503` until authenticated
device sessions and the existing dashboard API move into `dexp-host-core`. The
UE host remains the production path for at least that milestone.

Standalone Asio is pinned by commit and fetched during CMake configure. For an
offline or centrally managed dependency, point CMake at an existing checkout:

```sh
cmake -S Native -B build/native -DDEVICEEXPLORER_ASIO_ROOT=/path/to/asio
```

The path must contain `include/asio.hpp`. It never enters `DeviceExplorerWire`.
Convenience wrappers configure, build, and run the complete native test suite:

```powershell
.\Scripts\BuildDeviceExplorerNativeHost.ps1
```

```sh
Scripts/BuildDeviceExplorerNativeHost.sh
```

`dexp-host-integration-tests` binds both listeners to ephemeral ports and checks
health, the compatibility manifest, the config response, failure routes, and
the dashboard loopback invariant over real TCP.
