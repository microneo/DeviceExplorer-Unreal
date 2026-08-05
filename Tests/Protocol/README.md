# Protocol compatibility smoke

`Scripts/CaptureProtocolSmoke.py` is the black-box check for the current UE host.
It exercises both HTTP listeners, performs an RFC 6455 upgrade and mutual token
proof, attaches a minimal device, checks ping/pong, verifies the device through
the dashboard API, and queries the host over mDNS.

Start the host with a disposable deterministic token. The token itself never
crosses the network, but a capture contains the challenge transcript and proof,
so do not use a project or production token for a shareable fixture:

```powershell
DeviceExplorerHost.exe -DevicePort=18081 -DashboardPort=18080 -Token=protocol-smoke-token-0123456789abcdef
python Scripts/CaptureProtocolSmoke.py `
  --token protocol-smoke-token-0123456789abcdef `
  --output Saved/DeviceExplorer/protocol-capture.json
```

Use `--host <IPv4>` when the script and host are on different machines. mDNS is
enabled by default; `--skip-mdns` is only for environments where multicast is
known to be unavailable. The output stores the exact HTTP, WebSocket, and mDNS
bytes as base64 plus decoded mDNS record boundaries. Captures are deliberately
written below `Saved/` and are not committed: host name, PID, addresses, and the
chosen token are environment-specific.

The check is successful only when all of these hold:

- both `/health` endpoints return `{"status":"ok"}`;
- `/api/config` reports the current device protocol and requested device port;
- the WebSocket accept key and ping/pong behavior match RFC 6455;
- both sides prove the token before a masked `hello` appears in `/api/devices`;
- the mDNS answer contains the DeviceExplorer service and expected token fingerprint.
