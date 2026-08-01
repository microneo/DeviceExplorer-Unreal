# Widgets

Every widget `FDeviceExplorerModuleBuilder` can register, the value its getter
returns, and how the dashboard renders it. The same set is rendered live by the
**Widget Gallery** module of the WebUI mock:

```bash
cd Plugins/DeviceExplorer/WebUI
npm ci
npm run dev
```

Field helpers exist on both the module builder and the section handle returned
by `Section(...)`, so the calls below chain from either. Getters run on the
module refresh interval set with `RefreshMs`; keep them cheap.

## Readouts

Read-only fields. The getter is polled, there is no setter.

| Call | Getter returns | Rendered as |
| --- | --- | --- |
| `Readonly` | `bool` | Dot plus `true` / `false` |
| `Readonly` | any arithmetic type | Formatted number with the optional unit |
| `Readonly` | `FString`, `FText`, `FName` | Plain text |
| `Badge` | `double` | Large readout tinted by `WarnAbove` / `ErrorAbove` |
| `Meter` | `double` value and `double` max | Progress bar with `value / max` |
| `Series` | `double` or `TArray<double>` | Sparkline with warn and error guides |
| `Status` | `FDeviceExplorerStatus` | Pill tinted by the tone |
| `Table` | `TArray<TArray<FString>>` | Grid using the column names passed to the call |
| `Json` | `TSharedPtr<FJsonObject>` | Key / value rows plus a copy button |
| `Artifact` | `TArray<FDeviceExplorerArtifact>` | File rows with size, age, and a link to the Files tab |
| `Path` | `FString` | Path text with a Browse button |

```cpp
Section.Badge(TEXT("frame_ms"), TEXT("Frame time"), [this] { return GetFrameTimeMs(); },
    { .Unit = TEXT("ms"), .WarnAbove = 25.0, .ErrorAbove = 40.0 });

Section.Meter(TEXT("snapshots"), TEXT("Snapshots"),
    [this] { return Snapshots.Num(); },
    [this] { return MaxSnapshots; });

// A single value appends to the history the dashboard keeps; an array replaces
// the whole window.
Section.Series(TEXT("frame_history"), TEXT("Frame time"), [this] { return GetFrameTimeMs(); },
    { .WarnAbove = 25.0 });

Section.Status(TEXT("capture"), TEXT("Capture"),
    [this] { return FDeviceExplorerStatus{ TEXT("Recording"), EDeviceExplorerStatusTone::Active }; });

Section.Table(TEXT("subsystems"), TEXT("Subsystems"),
    { TEXT("Subsystem"), TEXT("ms"), TEXT("calls") },
    [this]
    {
        TArray<TArray<FString>> Rows;
        for (const FSubsystemTiming& Timing : GetTimings())
        {
            Rows.Add({ Timing.Name, FString::SanitizeFloat(Timing.Milliseconds), FString::FromInt(Timing.Calls) });
        }
        return Rows;
    });

Section.Artifact(TEXT("captures"), TEXT("Saved/Diagnostics"),
    [this]
    {
        TArray<FDeviceExplorerArtifact> Entries;
        Entries.Add({ TEXT("snapshot-001.json"), TEXT("8.7 KB"), TEXT("2 min ago") });
        return Entries;
    });
```

Rows shorter than the column list are padded with empty cells; extra cells are
dropped. `Artifact` and `Path` only display — downloads go through a `FileRoot`
registered on the module.

## Editors

Two-way fields. The setter may return `void`, `bool`, or
`FDeviceExplorerWriteResult`; returning a result lets the dashboard show the
validation message next to the field.

| Call | Value type | Rendered as |
| --- | --- | --- |
| `Toggle` | `bool` | Switch |
| `Number` | any arithmetic type | Input, slider, or both, per `Display` |
| `String` | `FString` | Single-line input |
| `Text` | `FString` | Multi-line textarea, `Rows` high |
| `Enum` | `FString` from the option list | Select, or segments when `Display` is `Segmented` |
| `Flags` | `TArray<FString>` from the option list | Toggleable chips |
| `Vector` | `FVector` | Three numeric axes |
| `Color` | `FColor` or `FLinearColor` | Color picker with the hex value |

```cpp
Section.Text(TEXT("boot_commands"), TEXT("Boot commands"),
    [this] { return BootCommands; },
    [this](const FString& Value) { BootCommands = Value; },
    { .Rows = 4 });

Section.Enum(TEXT("quality"), TEXT("Quality"),
    { TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Epic") },
    [this] { return QualityName(); },
    [this](const FString& Value) { return SetQualityByName(Value); },
    { .Display = EDeviceExplorerEnumDisplay::Segmented });

Section.Flags(TEXT("trace_channels"), TEXT("Trace channels"),
    { TEXT("cpu"), TEXT("gpu"), TEXT("frame"), TEXT("bookmark") },
    [this] { return EnabledChannels; },
    [this](const TArray<FString>& Value) { EnabledChannels = Value; });

Section.Vector(TEXT("spawn"), TEXT("Spawn point"),
    [this] { return SpawnLocation; },
    [this](const FVector& Value) { SpawnLocation = Value; },
    { .Step = 0.5 });

Section.Color(TEXT("tint"), TEXT("Debug tint"),
    [this] { return DebugTint; },
    [this](const FColor& Value) { DebugTint = Value; });
```

`Color` accepts `#RRGGBB` and rejects anything else with a field error.
`Enum` and `Flags` values are matched against the option list as plain strings,
so keep the getter and the list in the same vocabulary.

A section created with `.Apply = EDeviceExplorerApply::Manual` collects edits
and submits them together with Apply and Discard buttons; the default is
instant.

## Actions

| Call | Rendered as |
| --- | --- |
| `Action` | Button styled by `Style`, optionally two-step with `bRequiresConfirmation` |
| `Action` with inputs | Inline form plus a submit button |
| `Button` | `Action` shorthand for a description and a confirmation flag |
| `Command` | Console-line chip that runs the command on the device |

Handlers may return `void`, `bool`, or `FDeviceExplorerModuleResult`.

```cpp
Section.Action(TEXT("capture"), TEXT("Capture snapshot"), [this] { CaptureSnapshot(); },
    { .Description = TEXT("Writes a diagnostic snapshot."),
      .Style = EDeviceExplorerActionStyle::Primary });

Section.Action(TEXT("trace"), TEXT("Timed trace"),
    { { TEXT("seconds"), TEXT("Seconds"), EDeviceExplorerInputType::Number, TEXT("10") },
      { TEXT("label"), TEXT("Label"), EDeviceExplorerInputType::String, TEXT("boss_fight") } },
    [this](const FDeviceExplorerActionParameters& Parameters)
    {
        StartTrace(Parameters.GetNumber(TEXT("seconds"), 10.0), Parameters.GetString(TEXT("label")));
    },
    { .ActionLabel = TEXT("Trace") });
```

`FDeviceExplorerActionParameters` exposes `GetString`, `GetNumber`, and
`GetBool` with defaults for values the form did not send.

## Layout

Section styles, passed as `.Style` to `Section(...)`:

| Style | Use |
| --- | --- |
| `Default` | Labelled rows |
| `Stats` | Compact readouts without a card frame |
| `Toolbar` | Buttons and commands in a row |
| `Settings` | Editors sized for a settings page |
| `Hero` | One metric per field, with sparkline, average, and peak |

Other layout options: `Columns` sets the grid width, `bCollapsible` and
`bCollapsed` fold the card, `Span` widens a single field, and `Description`
adds help text to a page, section, or field.

The gallery page itself uses two renderer-only forms the Builder does not emit:
the `reference` section style, which documents every row with the call that
registers it, and the `actions` group, which packs several buttons into one row.
Registering the buttons in a `Toolbar` section gives the same row.

Number display modes are `Auto`, `Input`, `Slider`, and `SliderAndInput`;
`Auto` picks a slider and input when both `Min` and `Max` are set. Action styles
are `Default`, `Primary`, and `Danger`. Status tones are `Idle`, `Active`,
`Warn`, and `Error`. `Badge` draws a sparkline of the values it has seen when
`bSeries` is set.

## Reflected objects

`Object` registers the editable `UObject` properties it can map — booleans as
toggles, numerics as numbers with `ClampMin`, `ClampMax`, and `Units` metadata,
strings and names as inputs, enums as selects — grouped into sections by
property category. `SettingsObject` does the same on a dedicated page with
manual apply and persistence.

## Adding a widget

A widget exists on both sides of the protocol:

1. `EDeviceExplorerWidget` and the field descriptor in
   [DeviceExplorerTypes.h](../Source/DeviceExplorerCore/Public/DeviceExplorerTypes.h);
2. the builder call and its value binding in
   [DeviceExplorerModuleBuilder.cpp](../Source/DeviceExplorerCore/Private/DeviceExplorerModuleBuilder.cpp);
3. the wire mapping in `BuildSectionsJson` in
   [DeviceExplorerModule.cpp](../Source/DeviceExplorer/Private/DeviceExplorerModule.cpp);
4. the renderer in `buildFieldControl` in [app.js](../WebUI/src/app.js);
5. a gallery entry in [default.json](../WebUI/mock/fixtures/default.json) so the
   widget can be reviewed without a device.

Changing the field schema is a protocol change: bump
`DeviceExplorer::ProtocolVersion`, `WebUI/protocol-version.js`, and rebuild
`Resources/Web`.
