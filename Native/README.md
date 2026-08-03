# DeviceExplorer native build

This directory is the CMake composition root for code that does not depend on
Unreal Engine. D1 currently provides `dexp-wire`, a sans-I/O library for HTTP
WebSocket upgrades, RFC 6455 frames, DeviceExplorer DNS-SD packets, strict JSON,
and protocol message envelopes, plus `dexp-wire-tests`. The standalone CMake
target supports C++17; UE 5.7+ compiles the same sources as C++20 because that is
the engine's minimum supported module standard.

```sh
cmake -S Native -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel
ctest --test-dir build/native --output-on-failure
python Scripts/CheckWireSources.py
```

The wire sources remain canonical under `Source/DeviceExplorerWire` so UBT and
CMake compile the same files. `sources.cmake` is explicit by design; the check
script fails if a source or public header is added without updating it.

Current D1 boundary: this slice contains WebSocket framing, mDNS, strict JSON,
and the `type`/`request_id` message envelope. The legacy UE host and client use
the WebSocket and mDNS portions; adopting the neutral JSON model belongs with
the standalone host and built-in UE client transport slices. The built-in
transport and Autobahn run remain. No socket, timer, thread, file-system,
Unreal, exception, or RTTI dependency is permitted in `dexp-wire`.

After building the UE host with this module, rerun the protocol-9 smoke with
`--expect-close-reply`. The flag verifies the RFC 6455 close handshake added by
the new host adapter while the default mode remains usable against the D0
legacy baseline.
