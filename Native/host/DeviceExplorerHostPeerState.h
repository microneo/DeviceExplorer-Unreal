#pragma once

#include "DeviceExplorerPeerProtocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>

namespace DeviceExplorer::Host
{
struct PeerIdentity
{
	std::string ClusterId;
	std::string NodeId;
	std::uint64_t HostSession = 0;
	std::string InstanceId;
};

struct PeerHandshakeDecision
{
	Wire::PeerHelloAck Ack;
	bool Establish = false;
	bool ReplaceOlderSession = false;
};

class KnownHostSessions final
{
public:
	KnownHostSessions(std::size_t MaximumEntries, std::chrono::seconds TimeToLive);

	std::uint64_t Get(const std::string& NodeId, std::chrono::steady_clock::time_point Now);
	void Remember(const std::string& NodeId, std::uint64_t Session, std::chrono::steady_clock::time_point Now);
	void Expire(std::chrono::steady_clock::time_point Now);
	std::size_t Size() const { return Entries.size(); }

private:
	struct Entry
	{
		std::uint64_t Session = 0;
		std::chrono::steady_clock::time_point LastSeen;
		std::list<std::string>::iterator LruPosition;
	};

	void Touch(std::unordered_map<std::string, Entry>::iterator Iterator);

	std::size_t MaximumEntries;
	std::chrono::seconds TimeToLive;
	std::list<std::string> Lru;
	std::unordered_map<std::string, Entry> Entries;
};

PeerHandshakeDecision EvaluatePeerHello(const PeerIdentity& Local,
	                                    const Wire::PeerHello& Remote,
	                                    std::uint64_t KnownRemoteSession,
	                                    std::int32_t LocalProtocolMin,
	                                    std::int32_t LocalProtocolMax);

bool ShouldApplyKnownLocalSession(std::uint64_t LocalSession,
	                              std::uint64_t KnownSession,
	                              std::uint64_t MaximumJump,
	                              std::uint64_t& OutNewSession);
}    // namespace DeviceExplorer::Host
