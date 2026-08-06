#!/usr/bin/env python3
import json
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
errors: list[str] = []


def require(condition: bool, message: str) -> None:
    if not condition:
        errors.append(message)


descriptor = json.loads((ROOT / "DeviceExplorer.uplugin").read_text(encoding="utf-8"))
editor = next((module for module in descriptor["Modules"] if module["Name"] == "DeviceExplorerEditor"), None)
require(editor is not None, "DeviceExplorerEditor is missing from DeviceExplorer.uplugin")
require(editor is not None and "PlatformAllowList" not in editor, "DeviceExplorerEditor must remain available on Linux")

build_sources = "\n".join(path.read_text(encoding="utf-8") for path in ROOT.glob("Source/**/*.Build.cs"))
direct_platform = re.compile(r"UnrealTargetPlatform\.(?:Win64|Linux|Mac|IOS|TVOS|VisionOS)")
require(not direct_platform.search(build_sources), "public Build.cs conditions must use platform groups, not named public platforms")
require("UnrealPlatformGroup.Apple" in build_sources, "Apple frameworks must be selected through UnrealPlatformGroup.Apple")

discovery = (ROOT / "Source/DeviceExplorer/Private/DeviceExplorerDiscovery.cpp").read_text(encoding="utf-8")
apple = (ROOT / "Source/DeviceExplorer/Private/Apple/DeviceExplorerDiscoveryApple.mm").read_text(encoding="utf-8")
require("#if PLATFORM_APPLE" in discovery and "CreateAppleDeviceExplorerEndpointSource" in discovery,
        "Apple runtime targets must select the Apple discovery provider")
require("nw_browser_create" in apple and "Network/Network.h" in apple, "Apple discovery must use Network.framework browsing")
require("nw_connection_create" in apple and "nw_path_copy_effective_remote_endpoint" in apple,
        "Apple Bonjour resolution must stay on Network.framework")
require("NSNetService" not in apple and "-Wdeprecated-declarations" not in apple,
        "Apple discovery must not fall back to deprecated NSNetService resolution")
require("@available(macOS 10.15, iOS 13.0, tvOS 13.0, *)" in apple,
        "Network.framework Bonjour availability must match the SDK declarations")

workflow = (ROOT / ".github/workflows/native-wire.yml").read_text(encoding="utf-8")
require("ubuntu-latest" in workflow and "windows-latest" in workflow and "macos-latest" in workflow,
        "native CI must retain Windows, macOS, and Linux")

if errors:
    for error in errors:
        print(f"platform boundary check failed: {error}", file=sys.stderr)
    raise SystemExit(1)
print("platform boundary checks passed")
