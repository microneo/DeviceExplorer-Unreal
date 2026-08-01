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
2. Regenerate project files and build the Editor target.
3. Click **DeviceExplorer** in the Editor status bar.

The host is a standalone program. On the first press the Editor offers to build
it and takes care of the rest.

Modules that register commands, roots, or dashboards let the plugin add its own
dependency from their `Build.cs`:

```csharp
DeviceExplorerPlugin.AddDependency(this);
```

It adds `DeviceExplorerCore` when the plugin is enabled for the target and
defines `WITH_DEVICEEXPLORER=0` when it is not, which keeps registration code
compiling with the plugin turned off:

```cpp
#if WITH_DEVICEEXPLORER
#include "DeviceExplorerModuleBuilder.h"
#endif
```

Pass `true` for a public dependency. A plain
`PrivateDependencyModuleNames.Add("DeviceExplorerCore")` still works when the
plugin is never disabled — UnrealBuildTool fails outright on a dependency to a
disabled plugin, which is what the helper exists to avoid. It is defined by the
plugin itself, so a project that deletes `Plugins/DeviceExplorer` removes the
`AddDependency` call as well; disabling the plugin in the `.uproject` needs no
change.

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
| `DeviceExplorerEditor` | Status bar item, menu, settings, and host process control |

## Build and run the host

The host is started, stopped, restarted, and opened from the DeviceExplorer
item in the Editor status bar or from **Tools → DeviceExplorer**. Preferences
are under **Editor Preferences → Plugins → DeviceExplorer**.

Building it by hand is only needed outside the Editor. The build script
installs the small `DeviceExplorerHost.Target.cs` wrapper into the project's
`Source` directory when it is missing, then builds the standalone target:

```powershell
.\Plugins\DeviceExplorer\Scripts\BuildDeviceExplorer.ps1 `
  -EngineDir C:\Unreal\UE_5.x `
  -Platform Win64 `
  -Configuration Development
```

`-Project` is optional while the plugin sits in `<Project>/Plugins` and the
project directory holds a single `.uproject`. `UE_ENGINE_DIR` can replace
`-EngineDir`. The same script can be configured as a Rider External Tool.

The committed dashboard is used as is. Pass `-Web` to rebuild it first, and
`-SkipWebInstall` with it when npm dependencies are already installed.

`Scripts\InstallDeviceExplorerHostTarget.ps1` installs the host target on its
own, which is what an IDE workflow needs before project files are generated.
It refuses to touch an existing `DeviceExplorerHost.Target.cs` that differs
from the template unless `-Force` is passed.

To rebuild the host with every Editor build, add it as a pre-build target in
the project's editor target:

```csharp
PreBuildTargets.Add(new TargetInfo(
    "DeviceExplorerHost",
    Target.Platform,
    UnrealTargetConfiguration.Development,
    Target.Architectures,
    Target.ProjectFile,
    null));
```

`Development` is required either way: the Editor starts the host by its
unsuffixed executable name.

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

DeviceExplorer is available under the Apache License 2.0.
