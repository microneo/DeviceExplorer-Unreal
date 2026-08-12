#pragma once

#include "DeviceExplorerHostCore.h"

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include <memory>
#include <string>

namespace DeviceExplorer::Host
{
class HostPeerNetwork final : public std::enable_shared_from_this<HostPeerNetwork>
{
public:
	HostPeerNetwork(asio::io_context& Io, HostConfig Config);
	~HostPeerNetwork();

	bool Start(std::string& OutError);
	void Stop();
	std::string DiagnosticsJson() const;
	std::string RosterJson() const;
	std::string BoundAddress() const;
	std::uint16_t BoundPort() const;
	void Discover(PeerCandidate Candidate);
	std::uint64_t KnownDeviceSession(const std::string& DeviceId) const;
	void LocalDeviceAttached(RosterDevice Device);
	void LocalDeviceDetached(const std::string& DeviceId, std::uint64_t DeviceSession);

private:
	struct Implementation;
	std::unique_ptr<Implementation> Impl;
};
}    // namespace DeviceExplorer::Host
