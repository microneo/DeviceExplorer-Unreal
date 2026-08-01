# DeviceExplorer

DeviceExplorer is a local-network diagnostics plugin for Unreal Engine. It
connects development builds to a standalone browser dashboard without requiring
a cable or a running Editor.

Features:

- live logs with category and verbosity filters;
- runtime console catalog, autocomplete, history, and command execution;
- sandboxed file browsing and downloads, including directory archives;
- trace and profiling artifact transfer;
- reusable C++ runtime modules rendered automatically by the WebUI;
- standalone host plus Editor start, stop, restart, and dashboard actions;
- mDNS discovery with manual address and token fallback.

The runtime client is excluded from Shipping builds. The dashboard listener is
localhost-only.

## Requirements

- Unreal Engine 5;
- Win64 or macOS for the Editor launcher;
- Node.js 22 or newer only when modifying the WebUI.

`Resources/Web` contains the production dashboard, so users of the plugin do
not need Node.js.

## Installation

1. Copy this directory to `<Project>/Plugins/DeviceExplorer`.
2. Enable `DeviceExplorer` in the project.
3. Regenerate project files.
4. Build the Editor target.

Modules that register commands, roots, or dashboards add this dependency:

```csharp
PrivateDependencyModuleNames.Add("DeviceExplorerCore");
```

For iOS discovery, append this to
`[/Script/IOSRuntimeSettings.IOSRuntimeSettings]` in `DefaultEngine.ini`:

```ini
AdditionalPlistData=<key>NSBonjourServices</key><array><string>_ue-deviceexplorer._tcp</string></array><key>NSLocalNetworkUsageDescription</key><string>Connects development builds to local diagnostic tools.</string>
```

If `AdditionalPlistData` already exists, extend its value instead of adding a
second property. Manual connection remains available:

```text
-DeviceExplorerServer=<ip>:<port> -DeviceExplorerToken=<token>
```

## Components

| Module | Purpose |
| --- | --- |
| `DeviceExplorerCore` | Runtime-safe registry and C++ Builder API |
| `DeviceExplorer` | Non-Shipping client embedded in the build |
| `DeviceExplorerHost` | Standalone HTTP, WebSocket, mDNS, and file-transfer host |
| `DeviceExplorerEditor` | Editor toolbar, menu, settings, and host process control |

## Build and run the host

The build script installs the small `DeviceExplorerHost.Target.cs` wrapper into
the project's `Source` directory when it is missing, builds the WebUI, then
builds the standalone target:

```powershell
.\Plugins\DeviceExplorer\Scripts\BuildDeviceExplorer.ps1 `
  -Project .\YourProject.uproject `
  -EngineDir C:\Unreal\UE_5.x `
  -Platform Win64 `
  -Configuration Development
```

`UE_ENGINE_DIR` can replace `-EngineDir`. The same script can be configured as a
Rider External Tool. Use `-WebOnly` for a frontend-only build and
`-SkipWebInstall` when dependencies are already installed.

The host can also be controlled from the DeviceExplorer toolbar button or from
**Tools → DeviceExplorer**. Preferences are under
**Editor Preferences → Plugins → DeviceExplorer**.

## C++ module Builder

The Builder separates module metadata, pages, sections, and fields. Page and
section handles retain their location, so fields remain in the intended group
even when setup code is split into helper functions.

```cpp
#include "DeviceExplorerCoreModule.h"
#include "DeviceExplorerModuleBuilder.h"
#include "Misc/Paths.h"

namespace
{
const FName RuntimeMonitorOwner(TEXT("RuntimeMonitor"));
}

void FRuntimeMonitorModule::StartupModule()
{
    FDeviceExplorerModuleBuilder Builder(
        RuntimeMonitorOwner,
        TEXT("runtime_monitor"),
        TEXT("Runtime Monitor"));

    Builder
        .Description(TEXT("Runtime state and diagnostic controls."))
        .Icon(TEXT("pulse"))
        .RefreshMs(1000)
        .FileRoot(
            TEXT("runtime_artifacts"),
            FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Diagnostics")),
            TEXT("Runtime artifacts"));

    auto Overview = Builder.Page(
        TEXT("overview"),
        TEXT("Overview"),
        { .Description = TEXT("Current session state."), .Icon = TEXT("pulse") });

    Overview.Section(
            TEXT("state"),
            TEXT("State"),
            { .Columns = 3, .Style = EDeviceExplorerSectionStyle::Stats })
        .Readonly(TEXT("active"), TEXT("Active"), [this] { return IsActive(); })
        .Badge(
            TEXT("frame_ms"),
            TEXT("Frame time"),
            [this] { return GetFrameTimeMs(); },
            { .Unit = TEXT("ms"), .WarnAbove = 25.0, .ErrorAbove = 40.0 });

    Overview.Section(
            TEXT("controls"),
            TEXT("Controls"),
            { .Style = EDeviceExplorerSectionStyle::Toolbar })
        .Action(
            TEXT("capture"),
            TEXT("Capture snapshot"),
            [this] { CaptureSnapshot(); },
            { .Description = TEXT("Writes a diagnostic snapshot."),
              .Style = EDeviceExplorerActionStyle::Primary })
        .Command(
            TEXT("stat unit"),
            TEXT("Stat unit"),
            TEXT("Prints frame timing statistics."));

    Builder.Page(TEXT("tuning"), TEXT("Tuning"))
        .Section(
            TEXT("thresholds"),
            TEXT("Thresholds"),
            { .Columns = 2, .Apply = EDeviceExplorerApply::Manual,
              .Style = EDeviceExplorerSectionStyle::Settings })
        .Toggle(
            TEXT("enabled"),
            TEXT("Enabled"),
            [this] { return IsEnabled(); },
            [this](bool Value) { SetEnabled(Value); })
        .Number(
            TEXT("limit_ms"),
            TEXT("Limit"),
            [this] { return GetLimitMs(); },
            [this](double Value)
            {
                if (Value <= 0.0)
                {
                    return FDeviceExplorerWriteResult::Failure(TEXT("Must be greater than zero"));
                }
                SetLimitMs(Value);
                return FDeviceExplorerWriteResult::Success();
            },
            { .Min = 1.0, .Max = 100.0, .Step = 0.5, .Unit = TEXT("ms"),
              .Display = EDeviceExplorerNumberDisplay::SliderAndInput });

    Builder.SettingsObject(GetMutableDefault<URuntimeMonitorSettings>());
    Builder.Register();
}

void FRuntimeMonitorModule::ShutdownModule()
{
    if (FDeviceExplorerCoreModule::IsAvailable())
    {
        FDeviceExplorerCoreModule::Get().UnregisterOwner(RuntimeMonitorOwner);
    }
}
```

Builder setters may return `void`, `bool`, or `FDeviceExplorerWriteResult`.
Actions may return `void`, `bool`, or `FDeviceExplorerModuleResult`. Explicit
result types let the WebUI show validation and runtime errors without custom
protocol code.

Available field helpers are `Readonly`, `Badge`, `Meter`, `Toggle`, `Number`,
`String`, `Enum`, `Action`, and `Command`. `Object` reflects selected editable
`UObject` properties. `SettingsObject` creates a dedicated page with manual
apply, category sections, persistence, and collapsible cards.

Section styles are `Default`, `Stats`, `Toolbar`, and `Settings`. Number display
modes are `Auto`, `Input`, `Slider`, and `SliderAndInput`. Action styles are
`Default`, `Primary`, and `Danger`.

Keep getters fast. Cache expensive diagnostics outside the provider and return
snapshots. Do not expose credentials, personal data, or unrestricted file-system
paths.

## WebUI development

Start `DeviceExplorerHost`, then run:

```bash
cd Plugins/DeviceExplorer/WebUI
npm ci
npm run dev
```

Open `http://127.0.0.1:5173`. Vite proxies host requests to
`http://127.0.0.1:18080` by default. Override it with
`DEVICEEXPLORER_HOST_URL`.

Build the committed dashboard bundle with one command:

```powershell
.\Plugins\DeviceExplorer\Scripts\BuildWebUI.ps1
```

or:

```bash
./Plugins/DeviceExplorer/Scripts/BuildWebUI.sh
```

Frontend changes do not require a C++ rebuild. Commit both `WebUI` sources and
the generated `Resources/Web` output.

## License

DeviceExplorer is available under the MIT License.
