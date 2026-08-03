# Protocol 9 compatibility capture

`Scripts/CaptureProtocol9Golden.py` is the D0 black-box check for the legacy UE
host. It exercises both HTTP listeners, performs an RFC 6455 upgrade, attaches a
minimal device, checks ping/pong, verifies the device through the dashboard API,
and queries the host over mDNS.

Start the host with a deterministic token so the generated capture is safe to
compare and does not contain a random session token:

```powershell
DeviceExplorerHost.exe -DevicePort=18081 -DashboardPort=18080 -Token=protocol9-golden-token
python Scripts/CaptureProtocol9Golden.py `
  --token protocol9-golden-token `
  --output Saved/DeviceExplorer/protocol9-capture.json
```

Use `--host <IPv4>` when the script and host are on different machines. mDNS is
enabled by default; `--skip-mdns` is only for environments where multicast is
known to be unavailable. The output stores the exact HTTP, WebSocket, and mDNS
bytes as base64 plus decoded mDNS record boundaries. Captures are deliberately
written below `Saved/` and are not committed: host name, PID, addresses, and the
chosen token are environment-specific.

The check is successful only when all of these hold:

- both `/health` endpoints return `{"status":"ok"}`;
- `/api/config` reports device protocol 9 and the requested device port;
- the WebSocket accept key and ping/pong behavior match RFC 6455;
- a masked protocol-9 `hello` appears in `/api/devices`;
- the mDNS answer contains the DeviceExplorer service and expected token.
