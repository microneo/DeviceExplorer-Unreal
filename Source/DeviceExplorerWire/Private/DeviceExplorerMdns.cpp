#include "DeviceExplorerMdns.h"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace DeviceExplorer::Wire
{
namespace
{
constexpr std::size_t DnsHeaderBytes = 12;
constexpr std::size_t MaximumDnsPacketBytes = 65535;
constexpr std::size_t MaximumQuestions = 128;
constexpr std::size_t MaximumRecords = 512;
constexpr std::size_t MaximumNameJumps = 64;
constexpr std::uint16_t DnsClassMask = 0x7FFF;
constexpr std::uint16_t DnsClassIn = 1;
constexpr std::uint16_t DnsTypeA = 1;
constexpr std::uint16_t DnsTypePtr = 12;
constexpr std::uint16_t DnsTypeTxt = 16;
constexpr std::uint16_t DnsTypeSrv = 33;
constexpr std::uint16_t DnsTypeAny = 255;

void SetError(MdnsError* OutError, const MdnsError Error)
{
	if (OutError != nullptr)
	{
		*OutError = Error;
	}
}

char LowerDnsAscii(const char Character)
{
	return Character >= 'A' && Character <= 'Z' ? static_cast<char>(Character - 'A' + 'a') : Character;
}

bool DecodePresentationName(std::string_view Name, std::vector<std::string>& OutLabels)
{
	OutLabels.clear();
	if (Name.empty()) return false;
	if (Name.back() == '.') Name.remove_suffix(1);
	if (Name.empty()) return false;

	std::size_t WireBytes = 1;    // root label
	std::size_t Offset = 0;
	while (Offset < Name.size())
	{
		std::string Label;
		while (Offset < Name.size() && Name[Offset] != '.')
		{
			const char Character = Name[Offset++];
			if (Character != '\\')
			{
				Label.push_back(Character);
				continue;
			}
			if (Offset >= Name.size()) return false;
			const char Escaped = Name[Offset];
			if (Escaped >= '0' && Escaped <= '9')
			{
				if (Name.size() - Offset < 3 || Name[Offset + 1] < '0' || Name[Offset + 1] > '9' ||
				    Name[Offset + 2] < '0' || Name[Offset + 2] > '9')
				{
					return false;
				}
				const unsigned Value = static_cast<unsigned>(Name[Offset] - '0') * 100U +
				                       static_cast<unsigned>(Name[Offset + 1] - '0') * 10U +
				                       static_cast<unsigned>(Name[Offset + 2] - '0');
				if (Value > 255) return false;
				Label.push_back(static_cast<char>(Value));
				Offset += 3;
			}
			else
			{
				Label.push_back(Escaped);
				++Offset;
			}
		}
		if (Label.empty() || Label.size() > 63 ||
		    !IsValidWebSocketUtf8({ reinterpret_cast<const std::uint8_t*>(Label.data()), Label.size() }))
		{
			return false;
		}
		WireBytes += Label.size() + 1;
		if (WireBytes > 255) return false;
		OutLabels.push_back(std::move(Label));
		if (Offset < Name.size()) ++Offset;
	}
	return !OutLabels.empty();
}

bool NamesEqual(const std::string_view A, const std::string_view B)
{
	std::vector<std::string> LabelsA;
	std::vector<std::string> LabelsB;
	if (!DecodePresentationName(A, LabelsA) || !DecodePresentationName(B, LabelsB) ||
	    LabelsA.size() != LabelsB.size())
	{
		return false;
	}
	for (std::size_t LabelIndex = 0; LabelIndex < LabelsA.size(); ++LabelIndex)
	{
		if (LabelsA[LabelIndex].size() != LabelsB[LabelIndex].size()) return false;
		for (std::size_t ByteIndex = 0; ByteIndex < LabelsA[LabelIndex].size(); ++ByteIndex)
		{
			if (LowerDnsAscii(LabelsA[LabelIndex][ByteIndex]) != LowerDnsAscii(LabelsB[LabelIndex][ByteIndex]))
			{
				return false;
			}
		}
	}
	return true;
}

void AddU16(std::vector<std::uint8_t>& Buffer, const std::uint16_t Value)
{
	Buffer.push_back(static_cast<std::uint8_t>((Value >> 8) & 0xFF));
	Buffer.push_back(static_cast<std::uint8_t>(Value & 0xFF));
}

void AddU32(std::vector<std::uint8_t>& Buffer, const std::uint32_t Value)
{
	Buffer.push_back(static_cast<std::uint8_t>((Value >> 24) & 0xFF));
	Buffer.push_back(static_cast<std::uint8_t>((Value >> 16) & 0xFF));
	Buffer.push_back(static_cast<std::uint8_t>((Value >> 8) & 0xFF));
	Buffer.push_back(static_cast<std::uint8_t>(Value & 0xFF));
}

bool ReadU16(const ByteView Packet, const std::size_t Offset, std::uint16_t& OutValue)
{
	if (Offset > Packet.Size || Packet.Size - Offset < 2) return false;
	OutValue = (static_cast<std::uint16_t>(Packet.Data[Offset]) << 8) | Packet.Data[Offset + 1];
	return true;
}

bool ReadU32(const ByteView Packet, const std::size_t Offset, std::uint32_t& OutValue)
{
	if (Offset > Packet.Size || Packet.Size - Offset < 4) return false;
	OutValue = (static_cast<std::uint32_t>(Packet.Data[Offset]) << 24) |
	           (static_cast<std::uint32_t>(Packet.Data[Offset + 1]) << 16) |
	           (static_cast<std::uint32_t>(Packet.Data[Offset + 2]) << 8) |
	           Packet.Data[Offset + 3];
	return true;
}

bool AddName(std::vector<std::uint8_t>& Buffer, std::string_view Name)
{
	std::vector<std::string> Labels;
	if (!DecodePresentationName(Name, Labels)) return false;
	const std::size_t BeginSize = Buffer.size();
	for (const std::string& Label : Labels)
	{
		Buffer.push_back(static_cast<std::uint8_t>(Label.size()));
		Buffer.insert(Buffer.end(), Label.begin(), Label.end());
	}
	Buffer.push_back(0);
	if (Buffer.size() - BeginSize > 255)
	{
		Buffer.resize(BeginSize);
		return false;
	}
	return true;
}

bool AppendPresentationLabel(const std::string_view Label, std::string& OutName)
{
	if (!IsValidWebSocketUtf8(
		    { reinterpret_cast<const std::uint8_t*>(Label.data()), Label.size() }))
	{
		return false;
	}
	if (!OutName.empty()) OutName.push_back('.');
	for (const unsigned char Value : Label)
	{
		if (Value == '.' || Value == '\\')
		{
			OutName.push_back('\\');
			OutName.push_back(static_cast<char>(Value));
		}
		else if (Value < 0x20 || Value == 0x7F)
		{
			OutName.push_back('\\');
			OutName.push_back(static_cast<char>('0' + Value / 100));
			OutName.push_back(static_cast<char>('0' + (Value / 10) % 10));
			OutName.push_back(static_cast<char>('0' + Value % 10));
		}
		else
		{
			OutName.push_back(static_cast<char>(Value));
		}
	}
	return true;
}

bool ReadName(const ByteView Packet, std::size_t& InOutOffset, std::string& OutName)
{
	OutName.clear();
	std::size_t Offset = InOutOffset;
	std::size_t ResumeOffset = std::numeric_limits<std::size_t>::max();
	std::size_t JumpCount = 0;
	std::size_t ExpandedWireBytes = 1;
	for (;;)
	{
		if (Offset >= Packet.Size) return false;
		const std::uint8_t LengthByte = Packet.Data[Offset];
		if ((LengthByte & 0xC0) == 0xC0)
		{
			std::uint16_t PointerWord = 0;
			if (!ReadU16(Packet, Offset, PointerWord)) return false;
			const std::size_t PointerOffset = PointerWord & 0x3FFF;
			if (PointerOffset >= Offset || ++JumpCount > MaximumNameJumps) return false;
			if (ResumeOffset == std::numeric_limits<std::size_t>::max()) ResumeOffset = Offset + 2;
			Offset = PointerOffset;
			continue;
		}
		if ((LengthByte & 0xC0) != 0) return false;
		if (LengthByte == 0)
		{
			++Offset;
			break;
		}
		const std::size_t LabelLength = LengthByte;
		const std::size_t LabelStart = Offset + 1;
		if (LabelStart > Packet.Size || Packet.Size - LabelStart < LabelLength) return false;
		ExpandedWireBytes += LabelLength + 1;
		if (ExpandedWireBytes > 255 ||
		    !AppendPresentationLabel(
			    std::string_view(reinterpret_cast<const char*>(Packet.Data + LabelStart), LabelLength), OutName))
		{
			return false;
		}
		Offset = LabelStart + LabelLength;
	}
	InOutOffset = ResumeOffset == std::numeric_limits<std::size_t>::max() ? Offset : ResumeOffset;
	return !OutName.empty();
}

bool AddRecord(std::vector<std::uint8_t>& Packet,
	           const std::string_view Name,
	           const std::uint16_t Type,
	           const std::uint16_t Class,
	           const std::uint32_t TimeToLive,
	           const std::vector<std::uint8_t>& Data)
{
	if (Data.size() > std::numeric_limits<std::uint16_t>::max() || !AddName(Packet, Name)) return false;
	AddU16(Packet, Type);
	AddU16(Packet, Class);
	AddU32(Packet, TimeToLive);
	AddU16(Packet, static_cast<std::uint16_t>(Data.size()));
	Packet.insert(Packet.end(), Data.begin(), Data.end());
	return true;
}

bool AddTxtString(std::vector<std::uint8_t>& Data, const std::string_view Text)
{
	if (Text.size() > 255 || !IsValidWebSocketUtf8(
		                         { reinterpret_cast<const std::uint8_t*>(Text.data()), Text.size() }))
	{
		return false;
	}
	Data.push_back(static_cast<std::uint8_t>(Text.size()));
	Data.insert(Data.end(), Text.begin(), Text.end());
	return true;
}

struct ResourceRecord
{
	std::string Name;
	std::uint16_t Type = 0;
	std::uint16_t Class = 0;
	std::uint32_t TimeToLive = 0;
	std::size_t DataOffset = 0;
	std::size_t DataLength = 0;
};

bool ReadResourceRecord(const ByteView Packet, std::size_t& InOutOffset, ResourceRecord& OutRecord)
{
	if (!ReadName(Packet, InOutOffset, OutRecord.Name) ||
	    InOutOffset > Packet.Size || Packet.Size - InOutOffset < 10)
	{
		return false;
	}
	std::uint16_t DataLength = 0;
	if (!ReadU16(Packet, InOutOffset, OutRecord.Type) ||
	    !ReadU16(Packet, InOutOffset + 2, OutRecord.Class) ||
	    !ReadU32(Packet, InOutOffset + 4, OutRecord.TimeToLive) ||
	    !ReadU16(Packet, InOutOffset + 8, DataLength))
	{
		return false;
	}
	InOutOffset += 10;
	if (InOutOffset > Packet.Size || Packet.Size - InOutOffset < DataLength) return false;
	OutRecord.DataOffset = InOutOffset;
	OutRecord.DataLength = DataLength;
	InOutOffset += DataLength;
	return true;
}

bool IsInClass(const ResourceRecord& Record)
{
	return (Record.Class & DnsClassMask) == DnsClassIn;
}

bool ParseDecimal(const std::string_view Text, const std::uint32_t Maximum, std::uint32_t& OutValue)
{
	if (Text.empty()) return false;
	std::uint32_t Value = 0;
	for (const char Character : Text)
	{
		if (Character < '0' || Character > '9') return false;
		const std::uint32_t Digit = static_cast<std::uint32_t>(Character - '0');
		if (Value > (Maximum - Digit) / 10) return false;
		Value = Value * 10 + Digit;
	}
	OutValue = Value;
	return true;
}

MdnsAnnouncementParseResult AnnouncementError(const MdnsError Error)
{
	return { MdnsStatus::Error, Error, {} };
}

void IncludeTtl(std::uint32_t& InOutTtl, const std::uint32_t Value)
{
	InOutTtl = std::min(InOutTtl, Value);
}
}    // namespace

bool EncodeMdnsQuery(const std::string_view ServiceName,
	                 std::vector<std::uint8_t>& OutPacket,
	                 MdnsError* OutError)
{
	std::vector<std::uint8_t> Packet;
	Packet.reserve(64);
	AddU16(Packet, 0);
	AddU16(Packet, 0);
	AddU16(Packet, 1);
	AddU16(Packet, 0);
	AddU16(Packet, 0);
	AddU16(Packet, 0);
	if (!AddName(Packet, ServiceName))
	{
		SetError(OutError, MdnsError::InvalidName);
		return false;
	}
	AddU16(Packet, DnsTypePtr);
	AddU16(Packet, DnsClassIn);
	OutPacket = std::move(Packet);
	SetError(OutError, MdnsError::None);
	return true;
}

MdnsQueryParseResult ParseMdnsQuery(const ByteView Packet,
	                                const std::string_view ServiceName,
	                                const std::string_view InstanceName,
	                                const std::string_view HostName)
{
	if ((Packet.Size > 0 && Packet.Data == nullptr) || Packet.Size < DnsHeaderBytes)
	{
		return { MdnsStatus::Error, MdnsError::InvalidInput, {} };
	}
	if (Packet.Size > MaximumDnsPacketBytes)
	{
		return { MdnsStatus::Error, MdnsError::PacketTooLarge, {} };
	}
	std::uint16_t Flags = 0;
	std::uint16_t QuestionCount = 0;
	if (!ReadU16(Packet, 2, Flags) || !ReadU16(Packet, 4, QuestionCount) ||
	    (Flags & 0x8000) != 0 || QuestionCount > MaximumQuestions)
	{
		return { MdnsStatus::Error, MdnsError::MalformedPacket, {} };
	}

	std::size_t Offset = DnsHeaderBytes;
	MdnsQueryMatch Match;
	bool Matched = false;
	for (std::uint16_t Index = 0; Index < QuestionCount; ++Index)
	{
		std::string Name;
		std::uint16_t Type = 0;
		std::uint16_t Class = 0;
		if (!ReadName(Packet, Offset, Name) || Offset > Packet.Size || Packet.Size - Offset < 4 ||
		    !ReadU16(Packet, Offset, Type) || !ReadU16(Packet, Offset + 2, Class))
		{
			return { MdnsStatus::Error, MdnsError::MalformedPacket, {} };
		}
		Offset += 4;
		if ((Class & DnsClassMask) != DnsClassIn) continue;
		const bool ServiceMatch = NamesEqual(Name, ServiceName) && (Type == DnsTypePtr || Type == DnsTypeAny);
		const bool InstanceMatch = NamesEqual(Name, InstanceName) &&
		                           (Type == DnsTypeSrv || Type == DnsTypeTxt || Type == DnsTypeAny);
		const bool HostMatch = NamesEqual(Name, HostName) && (Type == DnsTypeA || Type == DnsTypeAny);
		if (ServiceMatch || InstanceMatch || HostMatch)
		{
			if (!Matched)
			{
				Match = { std::move(Name), Type };
				Matched = true;
			}
		}
	}
	return Matched
		       ? MdnsQueryParseResult{ MdnsStatus::Complete, MdnsError::None, std::move(Match) }
		       : MdnsQueryParseResult{ MdnsStatus::NoMatch, MdnsError::None, {} };
}

bool EncodeMdnsAnnouncement(const MdnsServiceAnnouncement& Announcement,
	                        std::vector<std::uint8_t>& OutPacket,
	                        MdnsError* OutError)
{
	if (Announcement.DevicePort == 0 || Announcement.InstanceName.empty() ||
	    Announcement.HostName.empty() || Announcement.Token.empty() ||
	    Announcement.IPv4Addresses.empty() || Announcement.IPv4Addresses.size() > 65532)
	{
		SetError(OutError, MdnsError::InvalidAnnouncement);
		return false;
	}

	std::vector<std::uint8_t> Records;
	std::vector<std::uint8_t> PtrData;
	if (!AddName(PtrData, Announcement.InstanceName) ||
	    !AddRecord(Records, Announcement.ServiceName, DnsTypePtr, DnsClassIn,
	               Announcement.TimeToLive, PtrData))
	{
		SetError(OutError, MdnsError::InvalidName);
		return false;
	}

	std::vector<std::uint8_t> SrvData;
	AddU16(SrvData, 0);
	AddU16(SrvData, 0);
	AddU16(SrvData, Announcement.DevicePort);
	if (!AddName(SrvData, Announcement.HostName) ||
	    !AddRecord(Records, Announcement.InstanceName, DnsTypeSrv, 0x8001,
	               Announcement.TimeToLive, SrvData))
	{
		SetError(OutError, MdnsError::InvalidName);
		return false;
	}

	std::vector<std::uint8_t> TxtData;
	const std::string Version = "version=" + std::to_string(Announcement.ProtocolVersion);
	const std::string Token = "token=" + Announcement.Token;
	const std::string DashboardPort = "ui_port=" + std::to_string(Announcement.DashboardPort);
	if (!AddTxtString(TxtData, Version) || !AddTxtString(TxtData, Token) ||
	    !AddTxtString(TxtData, DashboardPort) ||
	    !AddRecord(Records, Announcement.InstanceName, DnsTypeTxt, 0x8001,
	               Announcement.TimeToLive, TxtData))
	{
		SetError(OutError, MdnsError::InvalidAnnouncement);
		return false;
	}

	for (const std::array<std::uint8_t, 4>& Address : Announcement.IPv4Addresses)
	{
		const std::vector<std::uint8_t> AddressBytes(Address.begin(), Address.end());
		if (!AddRecord(Records, Announcement.HostName, DnsTypeA, 0x8001,
		               Announcement.TimeToLive, AddressBytes))
		{
			SetError(OutError, MdnsError::InvalidName);
			return false;
		}
	}

	std::vector<std::uint8_t> Packet;
	Packet.reserve(DnsHeaderBytes + Records.size());
	AddU16(Packet, 0);
	AddU16(Packet, 0x8400);
	AddU16(Packet, 0);
	AddU16(Packet, static_cast<std::uint16_t>(3 + Announcement.IPv4Addresses.size()));
	AddU16(Packet, 0);
	AddU16(Packet, 0);
	Packet.insert(Packet.end(), Records.begin(), Records.end());
	if (Packet.size() > MaximumDnsPacketBytes)
	{
		SetError(OutError, MdnsError::PacketTooLarge);
		return false;
	}
	OutPacket = std::move(Packet);
	SetError(OutError, MdnsError::None);
	return true;
}

MdnsAnnouncementParseResult ParseMdnsAnnouncement(const ByteView Packet,
	                                               const std::string_view ExpectedServiceName)
{
	if ((Packet.Size > 0 && Packet.Data == nullptr) || Packet.Size < DnsHeaderBytes)
	{
		return AnnouncementError(MdnsError::InvalidInput);
	}
	if (Packet.Size > MaximumDnsPacketBytes) return AnnouncementError(MdnsError::PacketTooLarge);

	std::uint16_t Flags = 0;
	std::uint16_t QuestionCount = 0;
	std::uint16_t AnswerCount = 0;
	std::uint16_t AuthorityCount = 0;
	std::uint16_t AdditionalCount = 0;
	if (!ReadU16(Packet, 2, Flags) || !ReadU16(Packet, 4, QuestionCount) ||
	    !ReadU16(Packet, 6, AnswerCount) || !ReadU16(Packet, 8, AuthorityCount) ||
	    !ReadU16(Packet, 10, AdditionalCount) || (Flags & 0x8000) == 0 ||
	    QuestionCount > MaximumQuestions)
	{
		return AnnouncementError(MdnsError::MalformedPacket);
	}
	const std::size_t RecordCount = static_cast<std::size_t>(AnswerCount) + AuthorityCount + AdditionalCount;
	if (RecordCount > MaximumRecords) return AnnouncementError(MdnsError::MalformedPacket);

	std::size_t Offset = DnsHeaderBytes;
	for (std::uint16_t Index = 0; Index < QuestionCount; ++Index)
	{
		std::string Discard;
		if (!ReadName(Packet, Offset, Discard) || Offset > Packet.Size || Packet.Size - Offset < 4)
		{
			return AnnouncementError(MdnsError::MalformedPacket);
		}
		Offset += 4;
	}

	std::vector<ResourceRecord> Records;
	Records.reserve(RecordCount);
	for (std::size_t Index = 0; Index < RecordCount; ++Index)
	{
		ResourceRecord Record;
		if (!ReadResourceRecord(Packet, Offset, Record))
		{
			return AnnouncementError(MdnsError::MalformedPacket);
		}
		Records.push_back(std::move(Record));
	}
	// A few DNS-SD responders pad their UDP payload with zero bytes. Accept only
	// that unambiguous padding; non-zero trailing data still indicates bad counts.
	while (Offset < Packet.Size && Packet.Data[Offset] == 0) ++Offset;
	if (Offset != Packet.Size) return AnnouncementError(MdnsError::MalformedPacket);

	MdnsServiceAnnouncement Announcement;
	Announcement.ServiceName = std::string(ExpectedServiceName);
	std::uint32_t MinimumTtl = std::numeric_limits<std::uint32_t>::max();
	for (const ResourceRecord& Record : Records)
	{
		if (Record.Type != DnsTypePtr || !IsInClass(Record) ||
		    !NamesEqual(Record.Name, ExpectedServiceName))
		{
			continue;
		}
		std::size_t NameOffset = Record.DataOffset;
		std::string InstanceName;
		if (!ReadName(Packet, NameOffset, InstanceName) ||
		    NameOffset > Record.DataOffset + Record.DataLength)
		{
			return AnnouncementError(MdnsError::MalformedPacket);
		}
		Announcement.InstanceName = std::move(InstanceName);
		IncludeTtl(MinimumTtl, Record.TimeToLive);
		break;
	}
	if (Announcement.InstanceName.empty())
	{
		return { MdnsStatus::NoMatch, MdnsError::MissingService, {} };
	}

	for (const ResourceRecord& Record : Records)
	{
		if (Record.Type != DnsTypeSrv || !IsInClass(Record) ||
		    !NamesEqual(Record.Name, Announcement.InstanceName) || Record.DataLength < 7)
		{
			continue;
		}
		std::uint16_t Port = 0;
		std::size_t NameOffset = Record.DataOffset + 6;
		if (!ReadU16(Packet, Record.DataOffset + 4, Port) || Port == 0 ||
		    !ReadName(Packet, NameOffset, Announcement.HostName) ||
		    NameOffset > Record.DataOffset + Record.DataLength)
		{
			return AnnouncementError(MdnsError::MalformedPacket);
		}
		Announcement.DevicePort = Port;
		IncludeTtl(MinimumTtl, Record.TimeToLive);
		break;
	}
	if (Announcement.DevicePort == 0 || Announcement.HostName.empty())
	{
		return AnnouncementError(MdnsError::MissingServiceRecord);
	}

	for (const ResourceRecord& Record : Records)
	{
		if (Record.Type != DnsTypeTxt || !IsInClass(Record) ||
		    !NamesEqual(Record.Name, Announcement.InstanceName))
		{
			continue;
		}
		std::size_t TxtOffset = Record.DataOffset;
		const std::size_t TxtEnd = Record.DataOffset + Record.DataLength;
		while (TxtOffset < TxtEnd)
		{
			const std::size_t Length = Packet.Data[TxtOffset++];
			if (Length > TxtEnd - TxtOffset) return AnnouncementError(MdnsError::MalformedPacket);
			const std::string_view Entry(reinterpret_cast<const char*>(Packet.Data + TxtOffset), Length);
			if (!IsValidWebSocketUtf8({ Packet.Data + TxtOffset, Length }))
			{
				return AnnouncementError(MdnsError::MalformedPacket);
			}
			const std::size_t Equals = Entry.find('=');
			if (Equals != std::string_view::npos)
			{
				const std::string_view Key = Entry.substr(0, Equals);
				const std::string_view Value = Entry.substr(Equals + 1);
				std::uint32_t ParsedNumber = 0;
				if (NamesEqual(Key, "token")) Announcement.Token = std::string(Value);
				else if (NamesEqual(Key, "version") &&
				         ParseDecimal(Value, static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()), ParsedNumber))
				{
					Announcement.ProtocolVersion = static_cast<std::int32_t>(ParsedNumber);
				}
				else if (NamesEqual(Key, "ui_port") && ParseDecimal(Value, 65535, ParsedNumber))
				{
					Announcement.DashboardPort = static_cast<std::uint16_t>(ParsedNumber);
				}
			}
			TxtOffset += Length;
		}
		IncludeTtl(MinimumTtl, Record.TimeToLive);
		break;
	}
	if (Announcement.Token.empty()) return AnnouncementError(MdnsError::MissingToken);

	for (const ResourceRecord& Record : Records)
	{
		if (Record.Type == DnsTypeA && IsInClass(Record) && Record.DataLength == 4 &&
		    NamesEqual(Record.Name, Announcement.HostName))
		{
			Announcement.IPv4Addresses.push_back({ Packet.Data[Record.DataOffset],
			                                       Packet.Data[Record.DataOffset + 1],
			                                       Packet.Data[Record.DataOffset + 2],
			                                       Packet.Data[Record.DataOffset + 3] });
			IncludeTtl(MinimumTtl, Record.TimeToLive);
		}
	}
	Announcement.TimeToLive = MinimumTtl == std::numeric_limits<std::uint32_t>::max() ? 0 : MinimumTtl;
	return { MdnsStatus::Complete, MdnsError::None, std::move(Announcement) };
}

const char* MdnsErrorText(const MdnsError Error)
{
	switch (Error)
	{
		case MdnsError::None: return "none";
		case MdnsError::InvalidInput: return "invalid mDNS input";
		case MdnsError::PacketTooLarge: return "mDNS packet exceeds 65535 bytes";
		case MdnsError::MalformedPacket: return "malformed mDNS packet";
		case MdnsError::InvalidName: return "invalid DNS name";
		case MdnsError::InvalidAnnouncement: return "invalid mDNS announcement";
		case MdnsError::MissingService: return "mDNS service not present";
		case MdnsError::MissingServiceRecord: return "mDNS SRV record not present";
		case MdnsError::MissingToken: return "mDNS token TXT entry not present";
	}
	return "unknown mDNS error";
}
}    // namespace DeviceExplorer::Wire
