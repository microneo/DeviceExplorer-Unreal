# DeviceExplorer native build

This directory is the CMake composition root for code that does not depend on
Unreal Engine. D1 currently provides `dexp-wire`, a C++17 sans-I/O library for
HTTP WebSocket upgrades and RFC 6455 frames, plus `dexp-wire-tests`.

```sh
cmake -S Native -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel
ctest --test-dir build/native --output-on-failure
python Scripts/CheckWireSources.py
```

The wire sources remain canonical under `Source/DeviceExplorerWire` so UBT and
CMake compile the same files. `sources.cmake` is explicit by design; the check
script fails if a source or public header is added without updating it.

Current D1 boundary: this slice contains the transport framing used by the
legacy UE host. The mDNS codec, message/JSON codec, built-in UE client transport,
and Autobahn run belong to the following D1 slices. No socket, timer, thread,
file-system, Unreal, exception, or RTTI dependency is permitted in `dexp-wire`.

After building the UE host with this module, rerun the protocol-9 smoke with
`--expect-close-reply`. The flag verifies the RFC 6455 close handshake added by
the new host adapter while the default mode remains usable against the D0
legacy baseline.
