#pragma once

#include "DeviceExplorerJson.h"
#include "DeviceExplorerProtocol.h"

#include <cstdint>
#include <string>

namespace DeviceExplorer::Wire
{
struct ProtocolVersionRange
{
	std::int32_t Minimum = 0;
	std::int32_t Maximum = 0;
};

struct HostManifest
{
	std::int32_t ManifestVersion = HostManifestVersion;
	std::string HostVersionText{ HostVersion };
	std::string BuildId = "unknown";
	ProtocolVersionRange DeviceProtocol{ DeviceProtocolVersion, DeviceProtocolVersion };
	ProtocolVersionRange WebApi{ WebApiVersion, WebApiVersion };
	ProtocolVersionRange PeerProtocol{ PeerProtocolVersion, PeerProtocolVersion };
};

DEVICEEXPLORERWIRE_API bool IsValidHostManifest(const HostManifest& Manifest);
DEVICEEXPLORERWIRE_API bool SerializeHostManifest(const HostManifest& Manifest,
                                                  std::string& OutJson,
                                                  JsonError* OutError = nullptr);
DEVICEEXPLORERWIRE_API bool ParseHostManifest(ByteView Bytes,
                                              HostManifest& OutManifest,
                                              JsonError* OutError = nullptr);
}    // namespace DeviceExplorer::Wire
