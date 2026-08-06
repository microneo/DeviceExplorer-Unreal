#pragma once

#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace DeviceExplorer::Host
{
enum class LogLevel : std::uint8_t
{
	Information,
	Warning,
	Error
};

using LogCallback = std::function<void(LogLevel, const std::string&)>;

struct PeerSeed
{
	std::string Address;
	std::uint16_t Port = 0;
};

struct PeerCandidate
{
	std::string ClusterId;
	std::string NodeId;
	std::uint64_t HostSession = 0;
	std::string InstanceId;
	std::string Address;
	std::uint16_t Port = 0;
	std::int32_t ProtocolMinimum = 0;
	std::int32_t ProtocolMaximum = 0;
};

struct HostConfig
{
	std::string DashboardAddress = "127.0.0.1";
	std::uint16_t DashboardPort = 18080;
	std::string DeviceAddress = "0.0.0.0";
	std::uint16_t DevicePort = 18081;
	std::string Token;
	std::string WebRoot;
	std::string TransferDirectory;
	std::string NodeId;
	std::uint64_t HostSession = 0;
	std::string InstanceId;
	bool EnableDistributedMode = false;
	std::string ClusterId;
	std::string PeerAddress = "0.0.0.0";
	std::uint16_t PeerPort = 0;
	std::vector<PeerSeed> PeerSeeds;
	std::size_t MaximumPeers = 199;
	std::size_t MaximumInboundHandshakes = 32;
	std::size_t MaximumQueuedControlBytes = 64 * 1024;
	std::size_t MaximumKnownHostSessions = 1024;
	std::size_t MaximumCandidateMembers = 1024;
	std::size_t MaximumDialAttemptsPerSecond = 10;
	std::uint64_t MaximumHostSessionCorrection = 1000000;
	std::chrono::seconds PeerHandshakeTimeout{ 5 };
	std::chrono::seconds PeerPingInterval{ 5 };
	std::chrono::seconds PeerSuspectTimeout{ 15 };
	std::uint16_t TracePort = 1981;
	std::size_t LogCapacity = 100000;
	std::size_t LogCapacityBytes = 16 * 1024 * 1024;
	std::size_t MaximumDevices = 1024;
	std::uint64_t MaximumTransferBytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;
	std::chrono::milliseconds RequestTimeout{ 10000 };
	std::chrono::seconds TransferTtl{ 1800 };
	std::chrono::seconds DisconnectedDeviceTtl{ 3600 };
	std::string BuildId = "unknown";
	LogCallback Log;
	std::shared_ptr<std::atomic<std::uint64_t>> LiveHostSession;
	std::function<bool(std::uint64_t, std::uint64_t&, std::string&)> ApplyHostSessionCorrection;
	std::shared_ptr<std::function<void()>> RequestMdnsReannounce;
	std::function<std::string()> PeerDiagnostics;
	std::function<void(PeerCandidate)> PeerDiscovered;
};

struct BoundEndpoints
{
	std::string DashboardAddress;
	std::uint16_t DashboardPort = 0;
	std::string DeviceAddress;
	std::uint16_t DevicePort = 0;
	std::string PeerAddress;
	std::uint16_t PeerPort = 0;
};

class HostCore
{
public:
	explicit HostCore(HostConfig Config);
	~HostCore();
	HostCore(const HostCore&) = delete;
	HostCore& operator=(const HostCore&) = delete;
	HostCore(HostCore&&) = delete;
	HostCore& operator=(HostCore&&) = delete;

	bool Start(std::string& OutError);
	void RunFor(std::chrono::milliseconds Duration);
	void Stop();
	bool IsRunning() const;
	BoundEndpoints GetBoundEndpoints() const;
	std::string GetPeerDiagnosticsJson() const;

private:
	struct Implementation;
	std::unique_ptr<Implementation> Impl;
};
}    // namespace DeviceExplorer::Host
