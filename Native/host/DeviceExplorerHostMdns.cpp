#include "DeviceExplorerHostMdns.h"

#include "DeviceExplorerAuthPrimitives.h"
#include "DeviceExplorerMdns.h"
#include "DeviceExplorerProtocol.h"

#include <asio.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <Iphlpapi.h>
#include <WinSock2.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

namespace DeviceExplorer::Host
{
namespace
{
using Udp = asio::ip::udp;
constexpr std::uint16_t MdnsPort = 5353;
constexpr std::size_t MaximumDatagramBytes = 9000;

std::string SafeLabel(std::string Value)
{
	for (char& Character : Value)
	{
		if (!std::isalnum(static_cast<unsigned char>(Character)) && Character != '-') Character = '-';
	}
	if (Value.size() > 50) Value.resize(50);
	while (!Value.empty() && Value.front() == '-') Value.erase(Value.begin());
	while (!Value.empty() && Value.back() == '-') Value.pop_back();
	return Value.empty() ? "deviceexplorer" : Value;
}

std::string RandomSuffix()
{
	std::random_device Random;
	static constexpr char Hex[] = "0123456789abcdef";
	std::string Result(8, '\0');
	for (char& Character : Result) Character = Hex[Random() & 0x0F];
	return Result;
}

void EnumerateInterfaceAddresses(std::set<std::uint32_t>& Addresses)
{
#if defined(_WIN32)
	ULONG Size = 16 * 1024;
	std::vector<std::uint8_t> Buffer(Size);
	IP_ADAPTER_ADDRESSES* Adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(Buffer.data());
	ULONG Result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
	                                   nullptr, Adapters, &Size);
	if (Result == ERROR_BUFFER_OVERFLOW)
	{
		Buffer.resize(Size);
		Adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(Buffer.data());
		Result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
		                            nullptr, Adapters, &Size);
	}
	if (Result != NO_ERROR) return;
	for (const IP_ADAPTER_ADDRESSES* Adapter = Adapters; Adapter != nullptr; Adapter = Adapter->Next)
	{
		if (Adapter->OperStatus != IfOperStatusUp) continue;
		for (const IP_ADAPTER_UNICAST_ADDRESS* Address = Adapter->FirstUnicastAddress; Address != nullptr; Address = Address->Next)
		{
			if (Address->Address.lpSockaddr == nullptr || Address->Address.lpSockaddr->sa_family != AF_INET) continue;
			const sockaddr_in* IPv4 = reinterpret_cast<const sockaddr_in*>(Address->Address.lpSockaddr);
			Addresses.insert(ntohl(IPv4->sin_addr.s_addr));
		}
	}
#else
	ifaddrs* Head = nullptr;
	if (getifaddrs(&Head) != 0) return;
	for (const ifaddrs* Interface = Head; Interface != nullptr; Interface = Interface->ifa_next)
	{
		if (Interface->ifa_addr == nullptr || Interface->ifa_addr->sa_family != AF_INET ||
		    (Interface->ifa_flags & IFF_UP) == 0) continue;
		const sockaddr_in* IPv4 = reinterpret_cast<const sockaddr_in*>(Interface->ifa_addr);
		Addresses.insert(ntohl(IPv4->sin_addr.s_addr));
	}
	freeifaddrs(Head);
#endif
}
}    // namespace

struct MdnsAdvertiser::Implementation
{
	Implementation(asio::io_context& InIo, const HostConfig& InConfig, BoundEndpoints InEndpoints)
		: Io(InIo), Config(InConfig), Endpoints(std::move(InEndpoints)), Socket(Io), Timer(Io), QueryTimer(Io),
		  Multicast(asio::ip::make_address_v4("224.0.0.251"), MdnsPort)
	{
		const std::string Machine = SafeLabel(asio::ip::host_name());
		Announcement.InstanceName = "DeviceExplorer-" + Machine + '-' + RandomSuffix() + '.' + Wire::DeviceExplorerMdnsServiceName;
		Announcement.HostName = Machine + "-deviceexplorer.local";
		Announcement.TokenFingerprint = Wire::Auth::ComputeTokenFingerprint(Config.Token);
		Announcement.DevicePort = Endpoints.DevicePort;
		Announcement.DashboardPort = Endpoints.DashboardPort;
		Announcement.ProtocolVersion = DeviceProtocolVersion;
		if (Config.EnableDistributedMode && Endpoints.PeerPort != 0)
		{
			Announcement.ClusterId = Config.ClusterId;
			Announcement.NodeId = Config.NodeId;
			Announcement.HostSession = Config.HostSession;
			Announcement.InstanceId = Config.InstanceId;
			Announcement.PeerPort = Endpoints.PeerPort;
			Announcement.PeerProtocolMinimum = PeerProtocolVersion;
			Announcement.PeerProtocolMaximum = PeerProtocolVersion;
		}
	}

	void Log(const LogLevel Level, const std::string& Message) const
	{
		if (Config.Log) Config.Log(Level, Message);
	}

	bool Start()
	{
		std::set<std::uint32_t> Unique;
		EnumerateInterfaceAddresses(Unique);
		for (const std::uint32_t AddressValue : Unique)
		{
			const asio::ip::address_v4 Address(AddressValue);
			const std::array<unsigned char, 4> Bytes = Address.to_bytes();
			if (Address.is_loopback() || Address.is_unspecified() || (Bytes[0] == 169 && Bytes[1] == 254)) continue;
			Interfaces.push_back(Address);
			Announcement.IPv4Addresses.push_back(Bytes);
		}
		if (Interfaces.empty()) Interfaces.push_back(asio::ip::address_v4::loopback());
		if (Announcement.IPv4Addresses.empty()) Announcement.IPv4Addresses.push_back({ 127, 0, 0, 1 });

		asio::error_code Error;
		Socket.open(Udp::v4(), Error);
		if (!Error) Socket.set_option(asio::socket_base::reuse_address(true), Error);
		if (!Error) Socket.set_option(asio::ip::multicast::enable_loopback(true), Error);
		if (!Error) Socket.set_option(asio::ip::multicast::hops(255), Error);
		if (!Error) Socket.bind(Udp::endpoint(Udp::v4(), MdnsPort), Error);
		if (Error)
		{
			Log(LogLevel::Warning, "mDNS advertisement did not start: " + Error.message());
			asio::error_code Ignored;
			Socket.close(Ignored);
			return false;
		}
		for (const asio::ip::address_v4& Interface : Interfaces)
		{
			asio::error_code JoinError;
			Socket.set_option(asio::ip::multicast::join_group(Multicast.address().to_v4(), Interface), JoinError);
			if (!JoinError) JoinedInterfaces.push_back(Interface);
		}
		if (JoinedInterfaces.empty())
		{
			Log(LogLevel::Warning, "mDNS advertisement did not start: no multicast-capable IPv4 interface");
			asio::error_code Ignored;
			Socket.close(Ignored);
			return false;
		}
		Running = true;
		Receive();
		Announce(120);
		Schedule();
		if (Config.EnableDistributedMode) ScheduleStartupQuery();
		return true;
	}

	void Receive()
	{
		Socket.async_receive_from(asio::buffer(Buffer), Sender, [this](const asio::error_code& Error, const std::size_t Bytes)
		{
			if (!Error && Bytes != 0)
			{
				const Wire::MdnsQueryParseResult Query = Wire::ParseMdnsQuery(
					{ Buffer.data(), Bytes }, Announcement.ServiceName, Announcement.InstanceName, Announcement.HostName);
				if (Query.Status == Wire::MdnsStatus::Complete)
				{
					if (Sender.port() == MdnsPort) Announce(120);
					else Send(Sender, 120);
				}
				else if (Config.EnableDistributedMode && Config.PeerDiscovered)
				{
					const Wire::MdnsAnnouncementParseResult Parsed = Wire::ParseMdnsAnnouncement({ Buffer.data(), Bytes });
					const Wire::MdnsServiceAnnouncement& Remote = Parsed.Announcement;
					if (Parsed.Status == Wire::MdnsStatus::Complete && Remote.PeerPort != 0 &&
					    !(Remote.NodeId == Config.NodeId && Remote.HostSession == Config.HostSession &&
					      Remote.InstanceId == Config.InstanceId))
					{
						PeerCandidate Candidate;
						Candidate.ClusterId = Remote.ClusterId;
						Candidate.NodeId = Remote.NodeId;
						Candidate.HostSession = Remote.HostSession;
						Candidate.InstanceId = Remote.InstanceId;
						Candidate.Address = Sender.address().to_string();
						Candidate.Port = Remote.PeerPort;
						Candidate.ProtocolMinimum = Remote.PeerProtocolMinimum;
						Candidate.ProtocolMaximum = Remote.PeerProtocolMaximum;
						Config.PeerDiscovered(std::move(Candidate));
					}
				}
			}
			if (Running && Error != asio::error::operation_aborted) Receive();
		});
	}

	void Schedule()
	{
		std::uniform_int_distribution<int> Delay(20, 30);
		Timer.expires_after(std::chrono::seconds(Delay(Random)));
		Timer.async_wait([this](const asio::error_code& Error)
		{
			if (!Error && Running)
			{
				Announce(120);
				Schedule();
			}
		});
	}

	void ScheduleStartupQuery()
	{
		std::uniform_int_distribution<int> Delay(50, 1000);
		QueryTimer.expires_after(std::chrono::milliseconds(Delay(Random)));
		QueryTimer.async_wait([this](const asio::error_code& Error)
		{
			if (!Error && Running) SendQuery();
		});
	}

	void SendQuery()
	{
		std::vector<std::uint8_t> Packet;
		if (!Wire::EncodeMdnsQuery(Announcement.ServiceName, Packet)) return;
		for (const asio::ip::address_v4& Interface : Interfaces)
		{
			asio::error_code Ignored;
			Socket.set_option(asio::ip::multicast::outbound_interface(Interface), Ignored);
			Socket.send_to(asio::buffer(Packet), Multicast, 0, Ignored);
		}
	}

	void Announce(const std::uint32_t Ttl)
	{
		if (Config.LiveHostSession) Announcement.HostSession = Config.LiveHostSession->load();
		for (const std::array<std::uint8_t, 4>& Bytes : Announcement.IPv4Addresses)
		{
			const asio::ip::address_v4 Address(Bytes);
			asio::error_code Ignored;
			Socket.set_option(asio::ip::multicast::outbound_interface(Address), Ignored);
			Send(Multicast, Ttl);
		}
	}

	void Send(const Udp::endpoint& Destination, const std::uint32_t Ttl)
	{
		Announcement.TimeToLive = Ttl;
		std::vector<std::uint8_t> Packet;
		if (!Wire::EncodeMdnsAnnouncement(Announcement, Packet)) return;
		asio::error_code Error;
		Socket.send_to(asio::buffer(Packet), Destination, 0, Error);
		if (Error && Running) Log(LogLevel::Warning, "mDNS send failed: " + Error.message());
	}

	void Stop()
	{
		if (!Running) return;
		Announce(0);
		Running = false;
		asio::error_code Ignored;
		Timer.cancel();
		QueryTimer.cancel();
		Socket.cancel(Ignored);
		for (const asio::ip::address_v4& Interface : JoinedInterfaces)
		{
			Socket.set_option(asio::ip::multicast::leave_group(Multicast.address().to_v4(), Interface), Ignored);
		}
		Socket.close(Ignored);
	}

	asio::io_context& Io;
	HostConfig Config;
	BoundEndpoints Endpoints;
	Udp::socket Socket;
	asio::steady_timer Timer;
	asio::steady_timer QueryTimer;
	Udp::endpoint Multicast;
	Udp::endpoint Sender;
	std::array<std::uint8_t, MaximumDatagramBytes> Buffer{};
	Wire::MdnsServiceAnnouncement Announcement;
	std::vector<asio::ip::address_v4> Interfaces;
	std::vector<asio::ip::address_v4> JoinedInterfaces;
	std::mt19937 Random{ std::random_device{}() };
	bool Running = false;
};

MdnsAdvertiser::MdnsAdvertiser(asio::io_context& Io, const HostConfig& Config, BoundEndpoints Endpoints)
	: Impl(std::make_unique<Implementation>(Io, Config, std::move(Endpoints)))
{
}

MdnsAdvertiser::~MdnsAdvertiser()
{
	Stop();
}

bool MdnsAdvertiser::Start()
{
	return Impl->Start();
}

void MdnsAdvertiser::Reannounce()
{
	if (Impl->Running) Impl->Announce(120);
}

void MdnsAdvertiser::Stop()
{
	Impl->Stop();
}
}    // namespace DeviceExplorer::Host
