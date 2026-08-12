#include "DeviceExplorerHostPeerCrypto.h"

#include "DeviceExplorerProtocol.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
#define CHECK(Expression) do { if (!(Expression)) { std::cerr << "check failed: " #Expression " at line " << __LINE__ << '\n'; return EXIT_FAILURE; } } while (false)

DeviceExplorer::Wire::PeerHello Hello(const std::string& Node, const std::string& Instance, const std::string& Nonce)
{
	DeviceExplorer::Wire::PeerHello Result;
	Result.ClusterId = "test-cluster";
	Result.NodeId = Node;
	Result.HostSession = 7;
	Result.InstanceId = Instance;
	Result.ProtocolMin = DeviceExplorer::PeerProtocolVersion;
	Result.ProtocolMax = DeviceExplorer::PeerProtocolVersion;
	Result.ConnectionNonce = Nonce;
	return Result;
}
}    // namespace

int main()
{
	using DeviceExplorer::Host::PeerChannelCrypto;
	const auto A = Hello("node-a", "instance-a", "00112233445566778899aabbccddeeff");
	const auto B = Hello("node-b", "instance-b", "ffeeddccbbaa99887766554433221100");
	PeerChannelCrypto AChannel;
	PeerChannelCrypto BChannel;
	CHECK(AChannel.Initialize("a peer secret that is definitely at least thirty two bytes", A, B,
	                          DeviceExplorer::PeerProtocolVersion));
	CHECK(BChannel.Initialize("a peer secret that is definitely at least thirty two bytes", B, A,
	                          DeviceExplorer::PeerProtocolVersion));

	std::vector<std::uint8_t> Envelope;
	std::string PlainText;
	CHECK(AChannel.Encrypt("{\"type\":\"peer_ping\"}", Envelope));
	CHECK(BChannel.Decrypt({ reinterpret_cast<const char*>(Envelope.data()), Envelope.size() }, PlainText));
	CHECK(PlainText == "{\"type\":\"peer_ping\"}");
	CHECK(!BChannel.Decrypt({ reinterpret_cast<const char*>(Envelope.data()), Envelope.size() }, PlainText));

	CHECK(BChannel.Encrypt("{\"type\":\"peer_pong\"}", Envelope));
	CHECK(AChannel.Decrypt({ reinterpret_cast<const char*>(Envelope.data()), Envelope.size() }, PlainText));
	CHECK(PlainText == "{\"type\":\"peer_pong\"}");

	PeerChannelCrypto WrongSecret;
	CHECK(WrongSecret.Initialize("a different peer secret that is also sufficiently long", B, A,
	                             DeviceExplorer::PeerProtocolVersion));
	CHECK(AChannel.Encrypt("protected", Envelope));
	CHECK(!WrongSecret.Decrypt({ reinterpret_cast<const char*>(Envelope.data()), Envelope.size() }, PlainText));

	PeerChannelCrypto TamperSender;
	PeerChannelCrypto TamperReceiver;
	CHECK(TamperSender.Initialize("a peer secret that is definitely at least thirty two bytes", A, B,
	                              DeviceExplorer::PeerProtocolVersion));
	CHECK(TamperReceiver.Initialize("a peer secret that is definitely at least thirty two bytes", B, A,
	                                DeviceExplorer::PeerProtocolVersion));
	CHECK(TamperSender.Encrypt("protected", Envelope));
	Envelope[Envelope.size() / 2] ^= 0x80;
	CHECK(!TamperReceiver.Decrypt({ reinterpret_cast<const char*>(Envelope.data()), Envelope.size() }, PlainText));

	std::cout << "peer channel crypto tests passed\n";
	return EXIT_SUCCESS;
}
