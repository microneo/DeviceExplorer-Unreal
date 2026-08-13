#include "DeviceExplorerHostPeerCrypto.h"

#include <monocypher.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <tuple>

namespace DeviceExplorer::Host
{
namespace
{
constexpr std::array<std::uint8_t, 4> EnvelopeMagic{ 'D', 'X', 'P', 3 };
constexpr std::size_t SequenceBytes = 8;
constexpr std::size_t TagBytes = 16;
constexpr std::size_t HeaderBytes = EnvelopeMagic.size() + SequenceBytes;

using SecretBytes = std::array<std::uint8_t, 32>;

void AppendField(std::string& Target, const std::string_view Value)
{
	Target.append(std::to_string(Value.size()));
	Target.push_back(':');
	Target.append(Value.data(), Value.size());
	Target.push_back('\n');
}

void AppendHello(std::string& Target, const Wire::PeerHello& Hello)
{
	AppendField(Target, Hello.ClusterId);
	AppendField(Target, Hello.NodeId);
	AppendField(Target, std::to_string(Hello.HostSession));
	AppendField(Target, Hello.InstanceId);
	AppendField(Target, std::to_string(Hello.ProtocolMin));
	AppendField(Target, std::to_string(Hello.ProtocolMax));
	AppendField(Target, Hello.ConnectionNonce);
}

auto HelloOrder(const Wire::PeerHello& Hello)
{
	return std::tie(Hello.NodeId, Hello.HostSession, Hello.InstanceId, Hello.ConnectionNonce);
}

SecretBytes Derive(const SecretBytes& Root, const std::string_view Label)
{
	SecretBytes Result{};
	crypto_blake2b_keyed(Result.data(), Result.size(), Root.data(), Root.size(),
	                     reinterpret_cast<const std::uint8_t*>(Label.data()), Label.size());
	return Result;
}

std::array<std::uint8_t, 16> Prefix(const SecretBytes& Root, const std::string_view Label)
{
	std::array<std::uint8_t, 16> Result{};
	crypto_blake2b_keyed(Result.data(), Result.size(), Root.data(), Root.size(),
	                     reinterpret_cast<const std::uint8_t*>(Label.data()), Label.size());
	return Result;
}

void WriteSequence(std::uint8_t* Target, const std::uint64_t Value)
{
	for (std::size_t Index = 0; Index < SequenceBytes; ++Index)
	{
		Target[Index] = static_cast<std::uint8_t>((Value >> ((SequenceBytes - 1 - Index) * 8)) & 0xFF);
	}
}

std::uint64_t ReadSequence(const std::uint8_t* Source)
{
	std::uint64_t Result = 0;
	for (std::size_t Index = 0; Index < SequenceBytes; ++Index) Result = (Result << 8) | Source[Index];
	return Result;
}

std::array<std::uint8_t, 24> MakeChannelNonce(const std::array<std::uint8_t, 16>& PrefixValue,
	                                   const std::uint64_t Sequence)
{
	std::array<std::uint8_t, 24> Nonce{};
	std::copy(PrefixValue.begin(), PrefixValue.end(), Nonce.begin());
	WriteSequence(Nonce.data() + PrefixValue.size(), Sequence);
	return Nonce;
}
}    // namespace

PeerChannelCrypto::~PeerChannelCrypto()
{
	Reset();
}

bool PeerChannelCrypto::Initialize(const std::string_view Secret,
	                               const Wire::PeerHello& LocalHello,
	                               const Wire::PeerHello& RemoteHello,
	                               const std::int32_t NegotiatedVersion)
{
	Reset();
	if (Secret.size() < 32 || Secret.size() > 1024 || NegotiatedVersion <= 0 ||
	    LocalHello.ConnectionNonce.empty() || RemoteHello.ConnectionNonce.empty() ||
	    HelloOrder(LocalHello) == HelloOrder(RemoteHello))
	{
		return false;
	}

	SecretBytes SecretKey{};
	crypto_blake2b(SecretKey.data(), SecretKey.size(),
	               reinterpret_cast<const std::uint8_t*>(Secret.data()), Secret.size());
	std::string Transcript = "deviceexplorer-peer-channel-v3\n";
	const bool LocalFirst = HelloOrder(LocalHello) < HelloOrder(RemoteHello);
	const Wire::PeerHello& First = LocalFirst ? LocalHello : RemoteHello;
	const Wire::PeerHello& Second = LocalFirst ? RemoteHello : LocalHello;
	AppendHello(Transcript, First);
	AppendHello(Transcript, Second);
	AppendField(Transcript, std::to_string(NegotiatedVersion));

	SecretBytes Root{};
	crypto_blake2b_keyed(Root.data(), Root.size(), SecretKey.data(), SecretKey.size(),
	                     reinterpret_cast<const std::uint8_t*>(Transcript.data()), Transcript.size());
	SecretBytes FirstToSecondKey = Derive(Root, "first-to-second-key");
	SecretBytes SecondToFirstKey = Derive(Root, "second-to-first-key");
	auto FirstToSecondNonce = Prefix(Root, "first-to-second-nonce");
	auto SecondToFirstNonce = Prefix(Root, "second-to-first-nonce");
	if (LocalFirst)
	{
		SendKey = FirstToSecondKey;
		ReceiveKey = SecondToFirstKey;
		SendNoncePrefix = FirstToSecondNonce;
		ReceiveNoncePrefix = SecondToFirstNonce;
	}
	else
	{
		SendKey = SecondToFirstKey;
		ReceiveKey = FirstToSecondKey;
		SendNoncePrefix = SecondToFirstNonce;
		ReceiveNoncePrefix = FirstToSecondNonce;
	}
	crypto_wipe(SecretKey.data(), SecretKey.size());
	crypto_wipe(Root.data(), Root.size());
	crypto_wipe(FirstToSecondKey.data(), FirstToSecondKey.size());
	crypto_wipe(SecondToFirstKey.data(), SecondToFirstKey.size());
	crypto_wipe(FirstToSecondNonce.data(), FirstToSecondNonce.size());
	crypto_wipe(SecondToFirstNonce.data(), SecondToFirstNonce.size());
	NextSendSequence = 0;
	NextReceiveSequence = 0;
	Initialized = true;
	return true;
}

bool PeerChannelCrypto::Encrypt(const std::string_view PlainText, std::vector<std::uint8_t>& OutEnvelope)
{
	if (!Initialized || PlainText.empty() || NextSendSequence == std::numeric_limits<std::uint64_t>::max()) return false;
	OutEnvelope.resize(HeaderBytes + PlainText.size() + TagBytes);
	std::copy(EnvelopeMagic.begin(), EnvelopeMagic.end(), OutEnvelope.begin());
	WriteSequence(OutEnvelope.data() + EnvelopeMagic.size(), NextSendSequence);
	const auto Nonce = MakeChannelNonce(SendNoncePrefix, NextSendSequence);
	std::uint8_t* const CipherText = OutEnvelope.data() + HeaderBytes;
	std::uint8_t* const Tag = CipherText + PlainText.size();
	crypto_aead_lock(CipherText, Tag, SendKey.data(), Nonce.data(), OutEnvelope.data(), HeaderBytes,
	                 reinterpret_cast<const std::uint8_t*>(PlainText.data()), PlainText.size());
	++NextSendSequence;
	return true;
}

bool PeerChannelCrypto::Decrypt(const std::string_view Envelope, std::string& OutPlainText)
{
	if (!Initialized || Envelope.size() <= HeaderBytes + TagBytes ||
	    !std::equal(EnvelopeMagic.begin(), EnvelopeMagic.end(),
	                reinterpret_cast<const std::uint8_t*>(Envelope.data())))
	{
		return false;
	}
	const auto* const Bytes = reinterpret_cast<const std::uint8_t*>(Envelope.data());
	const std::uint64_t Sequence = ReadSequence(Bytes + EnvelopeMagic.size());
	if (Sequence != NextReceiveSequence || Sequence == std::numeric_limits<std::uint64_t>::max()) return false;
	const std::size_t CipherTextBytes = Envelope.size() - HeaderBytes - TagBytes;
	const std::uint8_t* const CipherText = Bytes + HeaderBytes;
	const std::uint8_t* const Tag = CipherText + CipherTextBytes;
	OutPlainText.resize(CipherTextBytes);
	const auto Nonce = MakeChannelNonce(ReceiveNoncePrefix, Sequence);
	if (crypto_aead_unlock(reinterpret_cast<std::uint8_t*>(OutPlainText.data()), Tag, ReceiveKey.data(), Nonce.data(),
	                       Bytes, HeaderBytes, CipherText, CipherTextBytes) != 0)
	{
		crypto_wipe(OutPlainText.data(), OutPlainText.size());
		OutPlainText.clear();
		return false;
	}
	++NextReceiveSequence;
	return true;
}

void PeerChannelCrypto::Reset()
{
	crypto_wipe(SendKey.data(), SendKey.size());
	crypto_wipe(ReceiveKey.data(), ReceiveKey.size());
	crypto_wipe(SendNoncePrefix.data(), SendNoncePrefix.size());
	crypto_wipe(ReceiveNoncePrefix.data(), ReceiveNoncePrefix.size());
	NextSendSequence = 0;
	NextReceiveSequence = 0;
	Initialized = false;
}
}    // namespace DeviceExplorer::Host
