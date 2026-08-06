#pragma once

#include "DeviceExplorerWebSocket.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace DeviceExplorer::Wire
{
inline constexpr const char* DeviceExplorerMdnsServiceName = "_deviceexplorer._tcp.local";

enum class MdnsStatus : std::uint8_t
{
	Complete,
	NoMatch,
	Error
};

enum class MdnsError : std::uint8_t
{
	None,
	InvalidInput,
	PacketTooLarge,
	MalformedPacket,
	InvalidName,
	InvalidAnnouncement,
	MissingService,
	MissingServiceRecord,
	MissingFingerprint
};

struct MdnsQueryMatch
{
	std::string Name;
	std::uint16_t Type = 0;
};

struct MdnsServiceAnnouncement
{
	std::string ServiceName = DeviceExplorerMdnsServiceName;
	std::string InstanceName;
	std::string HostName;
	// Public label from the host's session token; the token itself never leaves the host.
	std::string TokenFingerprint;
	std::uint16_t DevicePort = 0;
	std::uint16_t DashboardPort = 0;
	std::int32_t ProtocolVersion = 0;
	std::string ClusterId;
	std::string NodeId;
	std::uint64_t HostSession = 0;
	std::string InstanceId;
	std::uint16_t PeerPort = 0;
	std::int32_t PeerProtocolMinimum = 0;
	std::int32_t PeerProtocolMaximum = 0;
	std::uint32_t TimeToLive = 120;
	std::vector<std::array<std::uint8_t, 4>> IPv4Addresses;
};

struct MdnsQueryParseResult
{
	MdnsStatus Status = MdnsStatus::NoMatch;
	MdnsError Error = MdnsError::None;
	MdnsQueryMatch Match;
};

struct MdnsAnnouncementParseResult
{
	MdnsStatus Status = MdnsStatus::NoMatch;
	MdnsError Error = MdnsError::None;
	MdnsServiceAnnouncement Announcement;
};

DEVICEEXPLORERWIRE_API bool EncodeMdnsQuery(std::string_view ServiceName,
	                 std::vector<std::uint8_t>& OutPacket,
	                 MdnsError* OutError = nullptr);

DEVICEEXPLORERWIRE_API MdnsQueryParseResult ParseMdnsQuery(ByteView Packet,
	                                std::string_view ServiceName,
	                                std::string_view InstanceName,
	                                std::string_view HostName);

DEVICEEXPLORERWIRE_API bool EncodeMdnsAnnouncement(const MdnsServiceAnnouncement& Announcement,
	                        std::vector<std::uint8_t>& OutPacket,
	                        MdnsError* OutError = nullptr);

DEVICEEXPLORERWIRE_API MdnsAnnouncementParseResult ParseMdnsAnnouncement(ByteView Packet,
	                                               std::string_view ExpectedServiceName = DeviceExplorerMdnsServiceName);

DEVICEEXPLORERWIRE_API const char* MdnsErrorText(MdnsError Error);
}    // namespace DeviceExplorer::Wire
