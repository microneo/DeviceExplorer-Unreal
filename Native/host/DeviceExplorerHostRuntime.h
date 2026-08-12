#pragma once

#include "DeviceExplorerHostCore.h"

#include <asio/ip/tcp.hpp>
#include <asio/io_context.hpp>

#include <memory>
#include <string>

namespace DeviceExplorer::Host
{
class HostRuntime final : public std::enable_shared_from_this<HostRuntime>
{
public:
	HostRuntime(asio::io_context& Io,
	            const HostConfig& Config,
	            BoundEndpoints Endpoints,
	            std::string ManifestJson);
	~HostRuntime();

	void Accept(asio::ip::tcp::socket Socket, bool Dashboard);
	void Stop();
	void Reannounce();
	void FenceLocalDevice(const std::string& DeviceId, std::uint64_t ObservedSession);

private:
	struct Implementation;
	std::unique_ptr<Implementation> Impl;
};
}    // namespace DeviceExplorer::Host
