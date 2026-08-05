#pragma once

#include <cstdint>

namespace DeviceExplorer
{
inline constexpr std::int32_t DeviceProtocolVersion = 10;

// Kept during D1-D2 so existing UE consumers do not need a protocol change.
inline constexpr std::int32_t ProtocolVersion = DeviceProtocolVersion;
}    // namespace DeviceExplorer
