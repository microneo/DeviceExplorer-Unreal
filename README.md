# DeviceExplorer

DeviceExplorer is a local-network diagnostics plugin for Unreal Engine. It
connects development builds to a standalone browser dashboard without requiring
a cable or a running Editor.

Features:

- live logs with a client-side category filter and a Verbosity tab that sets
  per-category levels on the running build;
- runtime console catalog spanning console variables and commands, exec
  functions, stat commands, and show flags, with history and command execution
  that reports both direct and logged output;
- sandboxed file browsing and downloads, including directory archives;
- trace and profiling artifact transfer;
- reusable C++ runtime modules rendered automatically by the WebUI;
- standalone host plus Editor start, stop, restart, and dashboard actions;
- mDNS discovery with manual address fallback, and a mutually authenticated device
  link.

The runtime client is excluded from Shipping builds. The dashboard listener is
localhost-only. A client needs the host's session token before it can connect, and it
never learns it from the network: discovery only advertises a fingerprint, and host
and client each prove they hold the token before any data is exchanged.

The Editor writes a token into the project settings the first time it starts the
host, so any build packaged from the project carries it and connects on its own —
including a build installed on a device, which has no command line to pass one.
`-DeviceExplorerToken=<token>` overrides it for a one-off session. Because the token
is committed with the project and packaged into its builds, everyone holding either
one can reach a host running that token; clear **Project Settings → Plugins →
DeviceExplorer (Project) → Session Token** to go back to a token per host session.

## Requirements

- Unreal Engine 5;
- Win64, macOS, or Linux for the Editor launcher;
- Node.js 22 or newer only when modifying the WebUI.

Device transport and discovery are IPv4-only in protocol v1. IPv6 endpoint
metadata is represented internally so it can be added without changing the
source-provider API, but IPv6 literals and IPv6-only networks are not supported
yet.

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

The macro follows the `DeviceExplorer` client module, not plugin enablement
alone: nothing reads the registry without the client, so `Shipping` and any
other configuration its `TargetConfigurationDenyList` covers gets
`WITH_DEVICEEXPLORER=0`. Guard on the macro rather than `!UE_BUILD_SHIPPING`.

Pass `true` for a public dependency. A plain
`PrivateDependencyModuleNames.Add("DeviceExplorerCore")` still works when the
plugin is never disabled — UnrealBuildTool fails outright on a dependency to a
disabled plugin, which is what the helper exists to avoid. It is defined by the
plugin itself, so a project that deletes `Plugins/DeviceExplorer` removes the
`AddDependency` call as well; disabling the plugin in the `.uproject` needs no
change.

For iOS and tvOS discovery, append this to
`[/Script/IOSRuntimeSettings.IOSRuntimeSettings]` in `DefaultEngine.ini`:

```ini
AdditionalPlistData=<key>NSBonjourServices</key><array><string>_deviceexplorer._tcp</string></array><key>NSLocalNetworkUsageDescription</key><string>Connects development builds to local diagnostic tools.</string>
```

If `AdditionalPlistData` already exists, extend its value instead of adding a
second property. visionOS uses the corresponding `VisionOSRuntimeSettings`
section; packaged macOS applications need the same plist keys. The complete
matrix and release checklist are in [Docs/PlatformSupport.md](Docs/PlatformSupport.md).
Manual connection remains available:

```text
-DeviceExplorerServer=<ip>:<port> -DeviceExplorerToken=<token>
```

Neither flag is needed when the project carries a session token; the address flag
only skips discovery. The Editor prints the current token when it starts the host.

The same fallback can be stored in the user-local
`Saved/Config/<Platform>/GameUserSettings.ini` rather than a project config:

```ini
[/Script/DeviceExplorer.Settings]
Endpoint=192.0.2.10:42111
Token=<token>
```

The client has two WebSocket transports. `Auto` uses Unreal's `WebSockets`
module when it is available and otherwise falls back to the built-in `FSocket`
adapter. Select one explicitly before the client module starts with either:

```ini
[/Script/DeviceExplorer.Settings]
Transport=Builtin
```

or `-DeviceExplorerTransport=Builtin`. Accepted values are `Auto`, `Engine`,
and `Builtin`. The value is read once when the client module starts. The
built-in path uses the shared `DeviceExplorerWire`
handshake and RFC 6455 codecs, including masked client frames, partial I/O,
fragmentation, ping/pong, bounded buffering, and close replies. The protocol
JSON messages are unchanged.

At runtime, `DeviceExplorer.Connect <ip>:<port> <token>` pins the client to a
specific host. `DeviceExplorer.Unpin` resumes automatic selection. A live
automatic connection remains sticky when other hosts are announced; after a
disconnect the last working host is tried first and other candidates follow
with per-endpoint backoff.

The token is an authentication secret: discovery advertises only a fingerprint of
it, and host and client each prove they hold it before any data is exchanged. A
token in `DefaultEngine.ini` is committed with the project and packaged into its
builds, so treat it as shared with everyone who has either.

## Components

| Module | Purpose |
| --- | --- |
| `DeviceExplorerCore` | Runtime-safe registry and C++ Builder API |
| `DeviceExplorerWire` | Sans-I/O HTTP, WebSocket, mDNS, and JSON/message codecs |
| `DeviceExplorer` | Non-Shipping client embedded in the build |
| `DeviceExplorerHost` | Standalone HTTP, WebSocket, mDNS, and file-transfer host |
| `DeviceExplorerEditor` | Status bar item, menu, settings, and host process control |

## Build and run the host

The host is started, stopped, restarted, and opened from the DeviceExplorer
item in the Editor status bar or from **Tools → DeviceExplorer**. Preferences
are under **Editor Preferences → Plugins → DeviceExplorer**.

Building it by hand is only needed outside the Editor. The native host uses the
same CMake build on Windows, macOS, and Linux and does not require Unreal Engine
source:

```powershell
.\Plugins\DeviceExplorer\Scripts\BuildDeviceExplorerNativeHost.ps1 -Install
```

```sh
Plugins/DeviceExplorer/Scripts/BuildDeviceExplorer.sh --install
```

The install is side-by-side and switches `current.txt` atomically, including
when the previous executable is still running. The Editor invokes this path
when no compatible native host is installed. The UE Program target and its
older PowerShell build script remain available for one compatibility milestone,
but are no longer the normal build path.

The committed dashboard is used as is. Pass `-Web` to rebuild it first, and
`-SkipWebInstall` with it when npm dependencies are already installed.

`Scripts\InstallDeviceExplorerHostTarget.ps1` installs the legacy host target on its
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

Readouts are `Readonly`, `Badge`, `Meter`, `Series`, `Status`, `Table`, `Json`,
`Artifact`, and `Path`. Editors are `Toggle`, `Number`, `String`, `Text`,
`Enum`, `Flags`, `Vector`, and `Color`. Actions are `Action`, `Button`, and
`Command`. `Object` reflects selected editable `UObject` properties.
`SettingsObject` creates a dedicated page with manual apply, category sections,
persistence, and collapsible cards. [Docs/Widgets.md](Docs/Widgets.md) documents
every widget, the value its getter returns, and how it renders.

Section styles are `Default`, `Stats`, `Toolbar`, `Settings`, and `Hero`. Number
display modes are `Auto`, `Input`, `Slider`, and `SliderAndInput`. Action styles
are `Default`, `Primary`, and `Danger`.

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

## Known issues

- A build ignores announcements whose fingerprint does not match its provisioned
  token, so several hosts can share a network. A build with no token stays
  disconnected instead of attaching to whichever host answered first.
- Platform coverage is partial. The Editor integration is Win64 and Mac only, and
  discovery is exercised on desktop and iOS devices. The iOS Simulator does not
  see the mDNS responses, so it needs the manual address flags. Consoles are
  untested.
- The host builds only against a source engine. Installed engine builds cannot
  build the `DeviceExplorerHost` program target.

## License

DeviceExplorer is available under the Apache License 2.0.
