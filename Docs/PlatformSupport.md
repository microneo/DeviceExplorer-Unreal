# Platform support

DeviceExplorer separates three support axes: the Unreal runtime client, automatic
discovery, and the standalone host/Editor workflow. Manual connection remains the
fallback whenever a target has Unreal's `Sockets` module but no discovery provider.

| Platform family | Runtime client | Automatic discovery | Native host / Editor |
| --- | --- | --- | --- |
| Windows | Engine or Builtin WebSocket | portable mDNS | supported |
| macOS | Engine or Builtin WebSocket | Network.framework Bonjour | supported |
| Linux | Engine or Builtin WebSocket | portable mDNS | supported |
| iOS | Engine or Builtin WebSocket | Network.framework Bonjour | host/Editor not applicable |
| tvOS | Builtin WebSocket | Network.framework Bonjour | host/Editor not applicable |
| visionOS | Builtin WebSocket | Network.framework Bonjour | host/Editor not applicable |
| Android | Engine or Builtin WebSocket | ephemeral-port mDNS query | host/Editor not applicable |
| Unreal server targets | Builtin or Engine when present | optional | manual endpoint recommended |

Protocol v1 remains IPv4-only. A manual endpoint and token can be provided in
`GameUserSettings.ini` or with `DeviceExplorer.Connect`, so a discovery failure
does not make the runtime client unusable.

## Apple local-network deployment

Every packaged Apple application that browses for DeviceExplorer must include
`NSBonjourServices` with `_deviceexplorer._tcp` and a user-facing
`NSLocalNetworkUsageDescription`. For iOS and tvOS, add the following to
`IOSRuntimeSettings.AdditionalPlistData`; for visionOS, put the same XML in
`VisionOSRuntimeSettings.AdditionalPlistData`:

```xml
<key>NSBonjourServices</key>
<array><string>_deviceexplorer._tcp</string></array>
<key>NSLocalNetworkUsageDescription</key>
<string>Connects development builds to local DeviceExplorer diagnostic tools.</string>
```

Add the same two keys to a packaged macOS game's Info.plist. The CMake-built
`dexp-host` embeds them directly in its Mach-O Info.plist section. A changed
signature or bundle identity can make macOS ask for local-network access again;
that is expected OS behavior and must be included in release testing.

## Linux workflow

The Editor module is not platform-filtered. From a Linux checkout, build, test,
and install the host with:

```sh
Scripts/BuildDeviceExplorer.sh --install
```

The Editor invokes the same CMake build and side-by-side installer when no
compatible native host is installed. The legacy UE Program host remains only as
the one-milestone compatibility fallback.

## Manual acceptance record

An unchecked item means unverified, not unsupported. Record the UE version,
OS/device version, and date beside each completed row.

| Target | Build four modules | Client starts | Manual connect | Discovery | Logs | Console result | File | Directory archive | Host restart reconnect |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Win64 |  |  |  |  |  |  |  |  |  |
| macOS |  |  |  |  |  |  |  |  |  |
| Linux |  |  |  |  |  |  |  |  |  |
| iOS |  |  |  |  |  |  |  |  |  |
| tvOS |  |  |  |  |  |  |  |  |  |
| visionOS |  |  |  |  |  |  |  |  |  |
| Android |  |  |  |  |  |  |  |  |  |
