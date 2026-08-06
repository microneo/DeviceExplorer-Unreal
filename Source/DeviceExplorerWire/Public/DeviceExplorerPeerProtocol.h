#pragma once

#include "DeviceExplorerJson.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace DeviceExplorer::Wire
{
inline constexpr std::size_t PeerFrameHeaderBytes = 4;
inline constexpr std::size_t MaximumPeerMessageBytes = 60 * 1024;

enum class PeerAddressFamily : std::uint8_t
{
	IPv4,
	IPv6
};

struct PeerEndpoint
{
	std::string Address;
	std::uint16_t Port = 0;
	PeerAddressFamily Family = PeerAddressFamily::IPv4;
};

struct PeerHello
{
	std::string ClusterId;
	std::string NodeId;
	std::uint64_t HostSession = 0;
	std::string InstanceId;
	std::int32_t ProtocolMin = 0;
	std::int32_t ProtocolMax = 0;
	std::string ConnectionNonce;
};

enum class PeerHelloResult : std::uint8_t
{
	Accepted,
	StaleSession,
	IdentityCollision,
	Rejected
};

struct PeerHelloAck
{
	std::int32_t NegotiatedVersion = 0;
	std::uint64_t KnownHostSession = 0;
	PeerHelloResult Result = PeerHelloResult::Rejected;
	std::string Reason;
	std::string Proof;
};

enum class PeerMessageType : std::uint8_t
{
	Hello,
	HelloAck,
	Ping,
	Pong
};

struct PeerMessage
{
	PeerMessageType Type = PeerMessageType::Ping;
	PeerHello Hello;
	PeerHelloAck HelloAck;
};

enum class PeerProtocolError : std::uint8_t
{
	None,
	InvalidInput,
	FrameTooLarge,
	MalformedJson,
	WrongMessageType,
	MissingField,
	InvalidField,
	UnsupportedResult
};

DEVICEEXPLORERWIRE_API bool EncodePeerFrame(std::string_view Payload,
	                                         std::vector<std::uint8_t>& OutFrame,
	                                         PeerProtocolError* OutError = nullptr);

class DEVICEEXPLORERWIRE_API PeerFrameDecoder
{
public:
	explicit PeerFrameDecoder(std::size_t InMaximumMessageBytes = MaximumPeerMessageBytes);

	bool Feed(ByteView Bytes, std::vector<std::string>& OutMessages, PeerProtocolError* OutError = nullptr);
	void Reset();
	std::size_t BufferedBytes() const { return Buffer.size(); }

private:
	std::size_t MaximumMessageBytes;
	std::vector<std::uint8_t> Buffer;
};

DEVICEEXPLORERWIRE_API bool SerializePeerHello(const PeerHello& Hello,
	                                            std::string& OutJson,
	                                            PeerProtocolError* OutError = nullptr);
DEVICEEXPLORERWIRE_API bool ParsePeerHello(ByteView Json,
	                                        PeerHello& OutHello,
	                                        PeerProtocolError* OutError = nullptr);
DEVICEEXPLORERWIRE_API bool SerializePeerHelloAck(const PeerHelloAck& Ack,
	                                               std::string& OutJson,
	                                               PeerProtocolError* OutError = nullptr);
DEVICEEXPLORERWIRE_API bool ParsePeerHelloAck(ByteView Json,
	                                           PeerHelloAck& OutAck,
	                                           PeerProtocolError* OutError = nullptr);
DEVICEEXPLORERWIRE_API bool ParsePeerMessage(ByteView Json,
	                                          PeerMessage& OutMessage,
	                                          PeerProtocolError* OutError = nullptr);
DEVICEEXPLORERWIRE_API std::string ComputePeerHelloAckProof(std::string_view Secret,
	                                                        const PeerHello& Prover,
	                                                        const PeerHello& Verifier,
	                                                        const PeerHelloAck& Ack);
DEVICEEXPLORERWIRE_API const char* PeerProtocolErrorText(PeerProtocolError Error);
}    // namespace DeviceExplorer::Wire
