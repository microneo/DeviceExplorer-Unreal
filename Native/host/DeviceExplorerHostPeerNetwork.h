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
	std::string BoundAddress() const;
	std::uint16_t BoundPort() const;
	void Discover(PeerCandidate Candidate);

private:
	struct Implementation;
	std::unique_ptr<Implementation> Impl;
};
}    // namespace DeviceExplorer::Host
