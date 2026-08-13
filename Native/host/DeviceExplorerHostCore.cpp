#include "DeviceExplorerHostCore.h"

#include "DeviceExplorerHostManifest.h"
#include "DeviceExplorerHostPeerNetwork.h"
#include "DeviceExplorerHostRuntime.h"

#include <asio.hpp>

#include <memory>
#include <utility>

namespace DeviceExplorer::Host
{
namespace
{
using Tcp = asio::ip::tcp;

enum class ListenerKind : std::uint8_t
{
	Dashboard,
	Device
};
}    // namespace

struct HostCore::Implementation
{
	explicit Implementation(HostConfig InConfig)
		: Config(std::move(InConfig)), DashboardAcceptor(Io), DeviceAcceptor(Io)
	{
	}

	void Log(const LogLevel Level, const std::string& Message) const
	{
		if (Config.Log) Config.Log(Level, Message);
	}

	bool Bind(Tcp::acceptor& Acceptor,
	          const std::string& AddressText,
	          const std::uint16_t Port,
	          const bool RequireLoopback,
	          std::string& OutError)
	{
		asio::error_code Error;
		const asio::ip::address Address = asio::ip::make_address(AddressText, Error);
		if (Error)
		{
			OutError = "invalid listen address '" + AddressText + "': " + Error.message();
			return false;
		}
		if (!Address.is_v4() || (RequireLoopback && !Address.is_loopback()))
		{
			OutError = RequireLoopback ? "dashboard address must be an IPv4 loopback address" : "device address must be IPv4";
			return false;
		}
		const Tcp::endpoint Endpoint(Address, Port);
		Acceptor.open(Endpoint.protocol(), Error);
		if (!Error) Acceptor.set_option(Tcp::acceptor::reuse_address(true), Error);
		if (!Error) Acceptor.bind(Endpoint, Error);
		if (!Error) Acceptor.listen(asio::socket_base::max_listen_connections, Error);
		if (Error)
		{
			OutError = "cannot listen on " + AddressText + ':' + std::to_string(Port) + ": " + Error.message();
			asio::error_code Ignored;
			Acceptor.close(Ignored);
			return false;
		}
		return true;
	}

	void Accept(Tcp::acceptor& Acceptor, const ListenerKind Kind)
	{
		Tcp::acceptor* const AcceptorPtr = &Acceptor;
		Acceptor.async_accept([this, AcceptorPtr, Kind](const asio::error_code& Error, Tcp::socket Socket)
		{
			if (!Error)
			{
				Runtime->Accept(std::move(Socket), Kind == ListenerKind::Dashboard);
			}
			else if (Running && Error != asio::error::operation_aborted)
			{
				Log(LogLevel::Warning, "accept failed: " + Error.message());
			}
			if (Running && AcceptorPtr->is_open()) Accept(*AcceptorPtr, Kind);
		});
	}

	HostConfig Config;
	asio::io_context Io;
	Tcp::acceptor DashboardAcceptor;
	Tcp::acceptor DeviceAcceptor;
	BoundEndpoints Endpoints;
	std::string ManifestJson;
	std::shared_ptr<HostRuntime> Runtime;
	std::shared_ptr<HostPeerNetwork> PeerNetwork;
	bool Running = false;
};

HostCore::HostCore(HostConfig Config)
	: Impl(std::make_unique<Implementation>(std::move(Config)))
{
}

HostCore::~HostCore()
{
	Stop();
}

bool HostCore::Start(std::string& OutError)
{
	if (Impl->Running)
	{
		OutError = "host is already running";
		return false;
	}
	if (Impl->Config.Token.empty())
	{
		OutError = "session token is required";
		return false;
	}
	if (Impl->Config.MaximumDevices == 0 || Impl->Config.MaximumKnownDeviceSessions == 0 ||
	    Impl->Config.KnownDeviceSessionTtl.count() <= 0)
	{
		OutError = "device registry and remembered-session limits must be non-zero";
		return false;
	}
	if (Impl->Config.NodeId.empty() || Impl->Config.HostSession == 0 || Impl->Config.InstanceId.empty())
	{
		OutError = "persisted host identity is required";
		return false;
	}
	if (!Impl->Config.LiveHostSession)
	{
		Impl->Config.LiveHostSession = std::make_shared<std::atomic<std::uint64_t>>(Impl->Config.HostSession);
	}
	if (!Impl->Config.RequestMdnsReannounce)
	{
		Impl->Config.RequestMdnsReannounce = std::make_shared<std::function<void()>>();
	}
	if (!Impl->Config.FenceLocalDevice)
	{
		Impl->Config.FenceLocalDevice =
			std::make_shared<std::function<void(const std::string&, std::uint64_t)>>();
	}
	Wire::HostManifest Manifest;
	Manifest.BuildId = Impl->Config.BuildId.empty() ? "unknown" : Impl->Config.BuildId;
	if (!Wire::SerializeHostManifest(Manifest, Impl->ManifestJson))
	{
		OutError = "cannot serialize host manifest";
		return false;
	}
	if (!Impl->Bind(Impl->DashboardAcceptor, Impl->Config.DashboardAddress, Impl->Config.DashboardPort, true, OutError) ||
	    !Impl->Bind(Impl->DeviceAcceptor, Impl->Config.DeviceAddress, Impl->Config.DevicePort, false, OutError))
	{
		Stop();
		return false;
	}

	asio::error_code Error;
	const Tcp::endpoint Dashboard = Impl->DashboardAcceptor.local_endpoint(Error);
	if (Error)
	{
		OutError = "cannot read the dashboard endpoint: " + Error.message();
		Stop();
		return false;
	}
	const Tcp::endpoint Device = Impl->DeviceAcceptor.local_endpoint(Error);
	if (Error)
	{
		OutError = "cannot read the device endpoint: " + Error.message();
		Stop();
		return false;
	}
	Impl->Endpoints.DashboardAddress = Dashboard.address().to_string();
	Impl->Endpoints.DashboardPort = Dashboard.port();
	Impl->Endpoints.DeviceAddress = Device.address().to_string();
	Impl->Endpoints.DevicePort = Device.port();
	if (Impl->Config.EnableDistributedMode)
	{
		Impl->PeerNetwork = std::make_shared<HostPeerNetwork>(Impl->Io, Impl->Config);
		if (!Impl->PeerNetwork->Start(OutError))
		{
			Stop();
			return false;
		}
		Impl->Endpoints.PeerAddress = Impl->PeerNetwork->BoundAddress();
		Impl->Endpoints.PeerPort = Impl->PeerNetwork->BoundPort();
		const std::weak_ptr<HostPeerNetwork> WeakPeerNetwork = Impl->PeerNetwork;
		Impl->Config.PeerDiagnostics = [WeakPeerNetwork]
		{
			const std::shared_ptr<HostPeerNetwork> Network = WeakPeerNetwork.lock();
			return Network ? Network->DiagnosticsJson() : "{\"enabled\":false,\"peers\":[],\"peer_count\":0}";
		};
		Impl->Config.RosterDiagnostics = [WeakPeerNetwork]
		{
			const std::shared_ptr<HostPeerNetwork> Network = WeakPeerNetwork.lock();
			return Network ? Network->RosterJson() : "{\"devices\":[],\"device_count\":0,\"ambiguous_owners\":0}";
		};
		Impl->Config.LastKnownDeviceSession = [WeakPeerNetwork](const std::string& DeviceId)
		{
			const std::shared_ptr<HostPeerNetwork> Network = WeakPeerNetwork.lock();
			return Network ? Network->KnownDeviceSession(DeviceId) : std::uint64_t{ 0 };
		};
		Impl->Config.LocalDeviceAttached = [WeakPeerNetwork](RosterDevice Device)
		{
			if (const std::shared_ptr<HostPeerNetwork> Network = WeakPeerNetwork.lock())
			{
				Network->LocalDeviceAttached(std::move(Device));
			}
		};
		Impl->Config.LocalDeviceDetached = [WeakPeerNetwork](const std::string& DeviceId, const std::uint64_t DeviceSession)
		{
			if (const std::shared_ptr<HostPeerNetwork> Network = WeakPeerNetwork.lock())
			{
				Network->LocalDeviceDetached(DeviceId, DeviceSession);
			}
		};
		Impl->Config.PeerDiscovered = [WeakPeerNetwork](PeerCandidate Candidate)
		{
			if (const std::shared_ptr<HostPeerNetwork> Network = WeakPeerNetwork.lock())
			{
				Network->Discover(std::move(Candidate));
			}
		};
	}
	Impl->Runtime = std::make_shared<HostRuntime>(Impl->Io, Impl->Config, Impl->Endpoints, Impl->ManifestJson);
	const std::weak_ptr<HostRuntime> WeakRuntime = Impl->Runtime;
	*Impl->Config.RequestMdnsReannounce = [WeakRuntime]
	{
		if (const std::shared_ptr<HostRuntime> Runtime = WeakRuntime.lock()) Runtime->Reannounce();
	};
	*Impl->Config.FenceLocalDevice = [WeakRuntime](const std::string& DeviceId, const std::uint64_t ObservedSession)
	{
		if (const std::shared_ptr<HostRuntime> Runtime = WeakRuntime.lock())
		{
			Runtime->FenceLocalDevice(DeviceId, ObservedSession);
		}
	};
	Impl->Running = true;
	Impl->Accept(Impl->DashboardAcceptor, ListenerKind::Dashboard);
	Impl->Accept(Impl->DeviceAcceptor, ListenerKind::Device);
	Impl->Log(LogLevel::Information, "dashboard listening on " + Impl->Endpoints.DashboardAddress + ':' + std::to_string(Impl->Endpoints.DashboardPort));
	Impl->Log(LogLevel::Information, "device listener on " + Impl->Endpoints.DeviceAddress + ':' + std::to_string(Impl->Endpoints.DevicePort));
	Impl->Log(LogLevel::Information, "host identity " + Impl->Config.NodeId + " session " +
	          std::to_string(Impl->Config.LiveHostSession->load()));
	if (Impl->PeerNetwork)
	{
		Impl->Log(LogLevel::Information, "peer listener on " + Impl->Endpoints.PeerAddress + ':' +
		          std::to_string(Impl->Endpoints.PeerPort));
	}
	OutError.clear();
	return true;
}

void HostCore::RunFor(const std::chrono::milliseconds Duration)
{
	if (!Impl->Running) return;
	Impl->Io.restart();
	Impl->Io.run_for(Duration);
}

void HostCore::Stop()
{
	if (!Impl) return;
	Impl->Running = false;
	if (Impl->PeerNetwork) Impl->PeerNetwork->Stop();
	if (Impl->Runtime) Impl->Runtime->Stop();
	asio::error_code Ignored;
	Impl->DashboardAcceptor.cancel(Ignored);
	Impl->DashboardAcceptor.close(Ignored);
	Impl->DeviceAcceptor.cancel(Ignored);
	Impl->DeviceAcceptor.close(Ignored);
	// Drain cancellation handlers while their owners are still alive. HostCore can
	// be started again, so leaving stale handlers in the reusable io_context would
	// otherwise let them observe destroyed peer/runtime state after restart.
	Impl->Io.restart();
	while (Impl->Io.poll() != 0)
	{
	}
	Impl->Io.stop();
	Impl->PeerNetwork.reset();
	Impl->Runtime.reset();
}

bool HostCore::IsRunning() const
{
	return Impl->Running;
}

BoundEndpoints HostCore::GetBoundEndpoints() const
{
	return Impl->Endpoints;
}

std::string HostCore::GetPeerDiagnosticsJson() const
{
	return Impl->PeerNetwork ? Impl->PeerNetwork->DiagnosticsJson() : "{\"enabled\":false,\"peers\":[],\"peer_count\":0}";
}
}    // namespace DeviceExplorer::Host
