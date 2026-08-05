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
}    // namespace

struct MdnsAdvertiser::Implementation
{
	Implementation(asio::io_context& InIo, const HostConfig& InConfig, BoundEndpoints InEndpoints)
		: Io(InIo), Config(InConfig), Endpoints(std::move(InEndpoints)), Socket(Io), Timer(Io),
		  Multicast(asio::ip::make_address_v4("224.0.0.251"), MdnsPort)
	{
		const std::string Machine = SafeLabel(asio::ip::host_name());
		Announcement.InstanceName = "DeviceExplorer-" + Machine + '-' + RandomSuffix() + '.' + Wire::DeviceExplorerMdnsServiceName;
		Announcement.HostName = Machine + "-deviceexplorer.local";
		Announcement.TokenFingerprint = Wire::Auth::ComputeTokenFingerprint(Config.Token);
		Announcement.DevicePort = Endpoints.DevicePort;
		Announcement.DashboardPort = Endpoints.DashboardPort;
		Announcement.ProtocolVersion = DeviceProtocolVersion;
	}

	void Log(const LogLevel Level, const std::string& Message) const
	{
		if (Config.Log) Config.Log(Level, Message);
	}

	bool Start()
	{
		std::set<std::uint32_t> Unique;
		TcpResolve(Unique);
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
			Socket.close();
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
			Socket.close();
			return false;
		}
		Running = true;
		Receive();
		Announce(120);
		Schedule();
		return true;
	}

	void TcpResolve(std::set<std::uint32_t>& Addresses)
	{
		asio::ip::tcp::resolver Resolver(Io);
		asio::error_code Error;
		const auto Results = Resolver.resolve(asio::ip::host_name(), "0", Error);
		if (Error) return;
		for (const auto& Result : Results)
		{
			if (Result.endpoint().address().is_v4()) Addresses.insert(Result.endpoint().address().to_v4().to_uint());
		}
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
			}
			if (Running && Error != asio::error::operation_aborted) Receive();
		});
	}

	void Schedule()
	{
		Timer.expires_after(std::chrono::seconds(30));
		Timer.async_wait([this](const asio::error_code& Error)
		{
			if (!Error && Running)
			{
				Announce(120);
				Schedule();
			}
		});
	}

	void Announce(const std::uint32_t Ttl)
	{
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
	Udp::endpoint Multicast;
	Udp::endpoint Sender;
	std::array<std::uint8_t, MaximumDatagramBytes> Buffer{};
	Wire::MdnsServiceAnnouncement Announcement;
	std::vector<asio::ip::address_v4> Interfaces;
	std::vector<asio::ip::address_v4> JoinedInterfaces;
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

void MdnsAdvertiser::Stop()
{
	Impl->Stop();
}
}    // namespace DeviceExplorer::Host
