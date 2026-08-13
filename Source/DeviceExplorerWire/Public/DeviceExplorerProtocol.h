#pragma once

#include <cstdint>
#include <string_view>

namespace DeviceExplorer
{
inline constexpr std::int32_t HostManifestVersion = 1;
inline constexpr std::string_view HostVersion = "0.5.0";
inline constexpr std::int32_t DeviceProtocolVersion = 11;
inline constexpr std::int32_t WebApiVersion = 1;
inline constexpr std::int32_t PeerProtocolVersion = 3;

// Kept during D1-D2 so existing UE consumers do not need a protocol change.
inline constexpr std::int32_t ProtocolVersion = DeviceProtocolVersion;
}    // namespace DeviceExplorer
