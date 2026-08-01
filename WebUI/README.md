# DeviceExplorer WebUI

The editable dashboard source lives here. Vite writes the production bundle to
`../Resources/Web`, which is committed and distributed with the plugin.

## Mock development

The default development command runs without Unreal Engine or
`DeviceExplorerHost`:

```bash
npm ci
npm run dev
```

Open `http://127.0.0.1:5173`. Vite serves an in-memory REST mock with online and
offline devices, logs, console objects and execution, nested files, transfers,
downloads, traces, generic module data, and a schema-driven module covering all
Builder controls.

Additional scenarios:

```bash
npm run dev:offline
npm run dev:empty
npm run dev:errors
```

- `offline`: all devices are disconnected;
- `empty`: no devices have been discovered;
- `errors`: device discovery and logs work while console, files, and module data
  return timeout errors.

The same scenarios are available through a launcher script that installs
dependencies on first run:

```powershell
.\Scripts\RunWebUIMock.ps1 -Scenario offline -Open
```

```bash
./Scripts/RunWebUIMock.sh --scenario offline
```

Mock data is stored in `mock/fixtures/default.json`. The mock implements only
the REST contract consumed by WebUI; it does not duplicate WebSocket, mDNS, or
device-side behavior. Protocol version is declared once in
`protocol-version.js` and shared by the mock and production UI manifest. When
the folder is inside the plugin, Vite also verifies that it matches
`DeviceExplorerTypes.h` before starting or building.

## Development with a real host

Start `DeviceExplorerHost`, then run:

```bash
npm run dev:host
```

Requests under `/api` and `/health` are proxied to
`http://127.0.0.1:18080`. Override it with `DEVICEEXPLORER_HOST_URL`.

## Update the plugin bundle

From the project root:

```powershell
.\Plugins\DeviceExplorer\Scripts\BuildWebUI.ps1
```

On macOS or Linux:

```bash
./Plugins/DeviceExplorer/Scripts/BuildWebUI.sh
```

Node.js is a build-time dependency only. Users of the packaged plugin consume
the static files already present in `Resources/Web`.
