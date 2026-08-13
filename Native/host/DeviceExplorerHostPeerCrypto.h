#pragma once

#include "DeviceExplorerPeerProtocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace DeviceExplorer::Host
{
class PeerChannelCrypto final
{
public:
	PeerChannelCrypto() = default;
	~PeerChannelCrypto();
	PeerChannelCrypto(const PeerChannelCrypto&) = delete;
	PeerChannelCrypto& operator=(const PeerChannelCrypto&) = delete;

	bool Initialize(std::string_view Secret,
	                const Wire::PeerHello& LocalHello,
	                const Wire::PeerHello& RemoteHello,
	                std::int32_t NegotiatedVersion);
	bool Encrypt(std::string_view PlainText, std::vector<std::uint8_t>& OutEnvelope);
	bool Decrypt(std::string_view Envelope, std::string& OutPlainText);
	void Reset();
	bool IsInitialized() const { return Initialized; }
	std::uint64_t SendSequence() const { return NextSendSequence; }
	std::uint64_t ReceiveSequence() const { return NextReceiveSequence; }

private:
	std::array<std::uint8_t, 32> SendKey{};
	std::array<std::uint8_t, 32> ReceiveKey{};
	std::array<std::uint8_t, 16> SendNoncePrefix{};
	std::array<std::uint8_t, 16> ReceiveNoncePrefix{};
	std::uint64_t NextSendSequence = 0;
	std::uint64_t NextReceiveSequence = 0;
	bool Initialized = false;
};
}    // namespace DeviceExplorer::Host
