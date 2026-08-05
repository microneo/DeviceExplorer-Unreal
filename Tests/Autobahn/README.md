# Autobahn WebSocket conformance

`dexp-autobahn-agent` is a small cross-platform socket adapter around the
canonical `DeviceExplorerWire` codecs. It exposes both sides required by
Autobahn|Testsuite without adding I/O to the wire library:

- `server` is an RFC 6455 echo server for Autobahn's fuzzing client;
- `client` drives `/getCaseCount`, every `/runCase`, and `/updateReports` on
  Autobahn's fuzzing server;
- `self-test` runs a client and server pair over loopback and is part of CTest.

Build and check the adapter first:

```sh
cmake -S Native -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --config Release --parallel
ctest --test-dir build/native -C Release --output-on-failure
```

The runner uses the pinned official image
`crossbario/autobahn-testsuite:25.10.1`. Run both directions from the repository
root:

```sh
Tests/Autobahn/RunAutobahn.sh server build/native/dexp-autobahn-agent
Tests/Autobahn/RunAutobahn.sh client build/native/dexp-autobahn-agent
```

On Windows:

```powershell
Tests/Autobahn/RunAutobahn.ps1 server
Tests/Autobahn/RunAutobahn.ps1 client
```

Reports are written under `build/autobahn-reports/clients` and
`build/autobahn-reports/servers`. Cases `9.*` are intentionally excluded from
the conformance gate because they are long-running performance cases. Cases
`12.*` and `13.*` require permessage-deflate, which DeviceExplorer does not
negotiate. No RFC 6455 correctness group is excluded.

The runners wait for the selected listener instead of assuming Docker or the
native agent starts within a fixed delay. The Linux runner also supplies the
`host.docker.internal` mapping explicitly for engines without Docker Desktop.
