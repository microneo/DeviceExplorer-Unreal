#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace DeviceExplorer::Wire::Auth
{
inline constexpr std::string_view HostProofLabel = "deviceexplorer-host-v1";
inline constexpr std::string_view DeviceProofLabel = "deviceexplorer-device-v1";
inline constexpr std::size_t NonceHexLength = 32;

DEVICEEXPLORERWIRE_API bool IsValidNonce(std::string_view Value);

DEVICEEXPLORERWIRE_API std::string ComputeProof(std::string_view Token,
	                                            std::string_view Label,
	                                            std::string_view ClientNonce,
	                                            std::string_view HostNonce);

DEVICEEXPLORERWIRE_API std::string ComputeTokenFingerprint(std::string_view Token);

DEVICEEXPLORERWIRE_API bool ConstantTimeEquals(std::string_view Left, std::string_view Right);
}    // namespace DeviceExplorer::Wire::Auth
