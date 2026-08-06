#include "DeviceExplorerHostPeerState.h"

#include "DeviceExplorerProtocol.h"

#include <chrono>
#include <iostream>
#include <string>

namespace
{
int Failures = 0;

void Check(const bool Condition, const char* Expression, const int Line)
{
	if (Condition) return;
	std::cerr << "line " << Line << ": check failed: " << Expression << '\n';
	++Failures;
}

#define CHECK(Expression) Check((Expression), #Expression, __LINE__)

DeviceExplorer::Wire::PeerHello Hello(std::string Node, const std::uint64_t Session, std::string Instance)
{
	DeviceExplorer::Wire::PeerHello Result;
	Result.ClusterId = "studio";
	Result.NodeId = std::move(Node);
	Result.HostSession = Session;
	Result.InstanceId = std::move(Instance);
	Result.ProtocolMin = DeviceExplorer::PeerProtocolVersion;
	Result.ProtocolMax = DeviceExplorer::PeerProtocolVersion;
	Result.ConnectionNonce = "0123456789abcdef0123456789abcdef";
	return Result;
}
}    // namespace

int main()
{
	using namespace DeviceExplorer;
	using namespace DeviceExplorer::Host;
	const PeerIdentity Local{ "studio", "local", 10, "local-instance" };
	PeerHandshakeDecision Decision = EvaluatePeerHello(Local, Hello("remote", 4, "remote-instance"), 3,
	                                                    PeerProtocolVersion, PeerProtocolVersion);
	CHECK(Decision.Establish);
	CHECK(Decision.Ack.Result == Wire::PeerHelloResult::Accepted);
	CHECK(Decision.Ack.NegotiatedVersion == PeerProtocolVersion);

	Decision = EvaluatePeerHello(Local, Hello("remote", 4, "remote-instance"), 5,
	                            PeerProtocolVersion, PeerProtocolVersion);
	CHECK(!Decision.Establish);
	CHECK(Decision.Ack.Result == Wire::PeerHelloResult::StaleSession);
	CHECK(Decision.Ack.KnownHostSession == 5);

	Decision = EvaluatePeerHello(Local, Hello("local", 11, "clone-instance"), 0,
	                            PeerProtocolVersion, PeerProtocolVersion);
	CHECK(Decision.Ack.Result == Wire::PeerHelloResult::IdentityCollision);
	Decision = EvaluatePeerHello(Local, Hello("local", 10, "local-instance"), 0,
	                            PeerProtocolVersion, PeerProtocolVersion);
	CHECK(Decision.Ack.Result == Wire::PeerHelloResult::Rejected);

	Wire::PeerHello WrongCluster = Hello("remote", 5, "remote-instance");
	WrongCluster.ClusterId = "other";
	Decision = EvaluatePeerHello(Local, WrongCluster, 0, PeerProtocolVersion, PeerProtocolVersion);
	CHECK(Decision.Ack.Result == Wire::PeerHelloResult::Rejected);

	std::uint64_t Corrected = 0;
	CHECK(ShouldApplyKnownLocalSession(10, 12, 1000, Corrected));
	CHECK(Corrected == 13);
	CHECK(!ShouldApplyKnownLocalSession(10, 100000, 1000, Corrected));

	const auto Start = std::chrono::steady_clock::time_point{};
	KnownHostSessions Cache(2, std::chrono::seconds(10));
	Cache.Remember("one", 3, Start);
	Cache.Remember("one", 2, Start + std::chrono::seconds(1));
	CHECK(Cache.Get("one", Start + std::chrono::seconds(2)) == 3);
	Cache.Remember("two", 4, Start + std::chrono::seconds(3));
	Cache.Remember("three", 5, Start + std::chrono::seconds(4));
	CHECK(Cache.Size() == 2);
	CHECK(Cache.Get("one", Start + std::chrono::seconds(5)) == 0);
	Cache.Expire(Start + std::chrono::seconds(20));
	CHECK(Cache.Size() == 0);

	if (Failures == 0) std::cout << "DeviceExplorer peer state tests passed\n";
	return Failures == 0 ? 0 : 1;
}
