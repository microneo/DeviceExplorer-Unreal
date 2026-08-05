#pragma once

#include "DeviceExplorerHostCore.h"

#include <asio/io_context.hpp>

#include <memory>

namespace DeviceExplorer::Host
{
class MdnsAdvertiser final
{
public:
	MdnsAdvertiser(asio::io_context& Io, const HostConfig& Config, BoundEndpoints Endpoints);
	~MdnsAdvertiser();

	bool Start();
	void Stop();

private:
	struct Implementation;
	std::unique_ptr<Implementation> Impl;
};
}    // namespace DeviceExplorer::Host
