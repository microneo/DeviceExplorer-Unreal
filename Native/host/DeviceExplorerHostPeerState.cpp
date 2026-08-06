#include "DeviceExplorerHostPeerState.h"

#include <algorithm>
#include <limits>

namespace DeviceExplorer::Host
{
KnownHostSessions::KnownHostSessions(const std::size_t InMaximumEntries, const std::chrono::seconds InTimeToLive)
	: MaximumEntries(InMaximumEntries), TimeToLive(InTimeToLive)
{
}

void KnownHostSessions::Touch(const std::unordered_map<std::string, Entry>::iterator Iterator)
{
	Lru.splice(Lru.end(), Lru, Iterator->second.LruPosition);
	Iterator->second.LruPosition = std::prev(Lru.end());
}

std::uint64_t KnownHostSessions::Get(const std::string& NodeId, const std::chrono::steady_clock::time_point Now)
{
	const auto Found = Entries.find(NodeId);
	if (Found == Entries.end()) return 0;
	if (Now - Found->second.LastSeen > TimeToLive)
	{
		Lru.erase(Found->second.LruPosition);
		Entries.erase(Found);
		return 0;
	}
	Touch(Found);
	return Found->second.Session;
}

void KnownHostSessions::Remember(const std::string& NodeId,
	                             const std::uint64_t Session,
	                             const std::chrono::steady_clock::time_point Now)
{
	if (NodeId.empty() || Session == 0 || MaximumEntries == 0) return;
	const auto Found = Entries.find(NodeId);
	if (Found != Entries.end())
	{
		Found->second.Session = std::max(Found->second.Session, Session);
		Found->second.LastSeen = Now;
		Touch(Found);
		return;
	}
	while (Entries.size() >= MaximumEntries && !Lru.empty())
	{
		Entries.erase(Lru.front());
		Lru.pop_front();
	}
	Lru.push_back(NodeId);
	Entries.emplace(NodeId, Entry{ Session, Now, std::prev(Lru.end()) });
}

void KnownHostSessions::Expire(const std::chrono::steady_clock::time_point Now)
{
	for (auto Iterator = Entries.begin(); Iterator != Entries.end();)
	{
		if (Now - Iterator->second.LastSeen <= TimeToLive)
		{
			++Iterator;
			continue;
		}
		Lru.erase(Iterator->second.LruPosition);
		Iterator = Entries.erase(Iterator);
	}
}

PeerHandshakeDecision EvaluatePeerHello(const PeerIdentity& Local,
	                                    const Wire::PeerHello& Remote,
	                                    const std::uint64_t KnownRemoteSession,
	                                    const std::int32_t LocalProtocolMin,
	                                    const std::int32_t LocalProtocolMax)
{
	PeerHandshakeDecision Decision;
	Decision.Ack.KnownHostSession = std::max(KnownRemoteSession,
		Remote.NodeId == Local.NodeId ? Local.HostSession : std::uint64_t{ 0 });
	if (Remote.ClusterId != Local.ClusterId)
	{
		Decision.Ack.Result = Wire::PeerHelloResult::Rejected;
		Decision.Ack.Reason = "cluster mismatch";
		return Decision;
	}
	const std::int32_t Negotiated = std::min(LocalProtocolMax, Remote.ProtocolMax);
	if (Negotiated < std::max(LocalProtocolMin, Remote.ProtocolMin))
	{
		Decision.Ack.Result = Wire::PeerHelloResult::Rejected;
		Decision.Ack.Reason = "peer protocol ranges do not overlap";
		return Decision;
	}
	if (Remote.NodeId == Local.NodeId)
	{
		if (Remote.InstanceId == Local.InstanceId)
		{
			Decision.Ack.Result = Wire::PeerHelloResult::Rejected;
			Decision.Ack.Reason = "self connection";
			return Decision;
		}
		Decision.Ack.Result = Wire::PeerHelloResult::IdentityCollision;
		Decision.Ack.Reason = "node identity collision";
		return Decision;
	}
	if (KnownRemoteSession > Remote.HostSession)
	{
		Decision.Ack.Result = Wire::PeerHelloResult::StaleSession;
		Decision.Ack.Reason = "a newer host session is known";
		return Decision;
	}
	Decision.Ack.NegotiatedVersion = Negotiated;
	Decision.Ack.Result = Wire::PeerHelloResult::Accepted;
	Decision.Establish = true;
	Decision.ReplaceOlderSession = Remote.HostSession > KnownRemoteSession;
	return Decision;
}

bool ShouldApplyKnownLocalSession(const std::uint64_t LocalSession,
	                              const std::uint64_t KnownSession,
	                              const std::uint64_t MaximumJump,
	                              std::uint64_t& OutNewSession)
{
	if (KnownSession <= LocalSession || KnownSession == std::numeric_limits<std::uint64_t>::max()) return false;
	if (KnownSession - LocalSession > MaximumJump) return false;
	OutNewSession = KnownSession + 1;
	return true;
}
}    // namespace DeviceExplorer::Host
