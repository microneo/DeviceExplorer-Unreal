# Contributing

Use a focused branch and keep runtime, host, and WebUI changes independently
reviewable where possible.

Before opening a pull request:

1. Build the affected Unreal targets.
2. Run `npm ci` and `npm run build` in `WebUI`.
3. Commit the generated `Resources/Web` bundle.
4. Confirm that `rg -i "token|secret|password"` does not expose credentials.

Protocol changes must update `DeviceExplorer::ProtocolVersion`, the WebUI
manifest version, and compatibility handling.
