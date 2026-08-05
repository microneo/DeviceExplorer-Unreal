#include "DeviceExplorerHostCore.h"

#include "DeviceExplorerHostManifest.h"

#include <asio.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace
{
int Failures = 0;

void Check(const bool Condition, const char* Expression, const int Line)
{
	if (!Condition)
	{
		std::cerr << "line " << Line << ": check failed: " << Expression << '\n';
		++Failures;
	}
}

#define CHECK(Expression) Check((Expression), #Expression, __LINE__)

std::string Get(const std::string& Address, const std::uint16_t Port, const std::string& Path)
{
	asio::io_context Io;
	asio::ip::tcp::socket Socket(Io);
	asio::error_code Error;
	Socket.connect({ asio::ip::make_address(Address, Error), Port }, Error);
	if (Error) return {};
	const std::string Request =
		"GET " + Path + " HTTP/1.1\r\nHost: " + Address + ':' + std::to_string(Port) +
		"\r\nConnection: close\r\n\r\n";
	asio::write(Socket, asio::buffer(Request), Error);
	if (Error) return {};
	std::string Response;
	std::array<char, 4096> Buffer{};
	for (;;)
	{
		const std::size_t Read = Socket.read_some(asio::buffer(Buffer), Error);
		Response.append(Buffer.data(), Read);
		if (Error == asio::error::eof) break;
		if (Error) return {};
	}
	return Response;
}

std::string Body(const std::string& Response)
{
	const std::size_t Separator = Response.find("\r\n\r\n");
	return Separator == std::string::npos ? std::string{} : Response.substr(Separator + 4);
}
}    // namespace

int main()
{
	DeviceExplorer::Host::HostConfig Config;
	Config.DashboardPort = 0;
	Config.DeviceAddress = "127.0.0.1";
	Config.DevicePort = 0;
	Config.Token = "integration-test-token-that-is-long-enough";
	Config.BuildId = "integration-test";
	Config.NodeId = "11111111-1111-4111-8111-111111111111";
	Config.HostSession = 7;
	Config.InstanceId = "22222222-2222-4222-8222-222222222222";
	DeviceExplorer::Host::HostCore Host(std::move(Config));
	std::string Error;
	CHECK(Host.Start(Error));
	const DeviceExplorer::Host::BoundEndpoints Endpoints = Host.GetBoundEndpoints();
	CHECK(Endpoints.DashboardPort != 0);
	CHECK(Endpoints.DevicePort != 0);
	CHECK(Endpoints.DashboardPort != Endpoints.DevicePort);

	std::atomic<bool> Done{ false };
	std::thread EventLoop(
		[&Host, &Done]
		{
			while (!Done.load()) Host.RunFor(std::chrono::milliseconds(20));
		});

	const std::string DashboardHealth = Get(Endpoints.DashboardAddress, Endpoints.DashboardPort, "/health");
	const std::string DeviceHealth = Get(Endpoints.DeviceAddress, Endpoints.DevicePort, "/health");
	CHECK(DashboardHealth.find("HTTP/1.1 200 OK") == 0);
	CHECK(Body(DashboardHealth).find("\"status\":\"ok\"") != std::string::npos);
	CHECK(Body(DashboardHealth).find("\"node_id\":\"11111111-1111-4111-8111-111111111111\"") != std::string::npos);
	CHECK(Body(DashboardHealth).find("\"host_session\":7") != std::string::npos);
	CHECK(DeviceHealth.find("HTTP/1.1 200 OK") == 0);

	const std::string ManifestResponse = Get(Endpoints.DashboardAddress, Endpoints.DashboardPort, "/host-manifest");
	const std::string ManifestBody = Body(ManifestResponse);
	DeviceExplorer::Wire::HostManifest Manifest;
	CHECK(DeviceExplorer::Wire::ParseHostManifest(
		{ reinterpret_cast<const std::uint8_t*>(ManifestBody.data()), ManifestBody.size() }, Manifest));
	CHECK(Manifest.BuildId == "integration-test");

	const std::string ConfigResponse = Get(Endpoints.DashboardAddress, Endpoints.DashboardPort, "/api/config");
	CHECK(ConfigResponse.find("HTTP/1.1 200 OK") == 0);
	CHECK(Body(ConfigResponse).find("\"device_port\":" + std::to_string(Endpoints.DevicePort)) != std::string::npos);
	CHECK(Body(ConfigResponse).find("\"instance_id\":\"22222222-2222-4222-8222-222222222222\"") != std::string::npos);
	CHECK(Get(Endpoints.DashboardAddress, Endpoints.DashboardPort, "/missing").find("HTTP/1.1 404 Not Found") == 0);
	CHECK(Get(Endpoints.DeviceAddress, Endpoints.DevicePort, "/device/connect").find("HTTP/1.1 400 Bad Request") == 0);

	Done.store(true);
	EventLoop.join();
	Host.Stop();

	DeviceExplorer::Host::HostConfig UnsafeConfig;
	UnsafeConfig.DashboardAddress = "0.0.0.0";
	UnsafeConfig.DashboardPort = 0;
	UnsafeConfig.DevicePort = 0;
	UnsafeConfig.Token = "integration-test-token-that-is-long-enough";
	UnsafeConfig.NodeId = "11111111-1111-4111-8111-111111111111";
	UnsafeConfig.HostSession = 7;
	UnsafeConfig.InstanceId = "22222222-2222-4222-8222-222222222222";
	DeviceExplorer::Host::HostCore UnsafeHost(std::move(UnsafeConfig));
	CHECK(!UnsafeHost.Start(Error));
	CHECK(Error.find("loopback") != std::string::npos);
	if (Failures != 0)
	{
		std::cerr << Failures << " test(s) failed\n";
		return 1;
	}
	std::cout << "DeviceExplorer native host integration tests passed\n";
	return 0;
}
