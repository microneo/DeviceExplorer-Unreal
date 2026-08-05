#include "DeviceExplorerHostIdentity.h"

#include "DeviceExplorerJson.h"

#include <array>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <limits>
#include <random>
#include <string_view>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <sys/stat.h>
#endif

namespace DeviceExplorer::Host
{
namespace
{
constexpr std::uint64_t IdentitySchemaVersion = 1;
constexpr std::size_t MaximumIdentityBytes = 4096;

using Json = Wire::JsonValue;

std::string NewUuid()
{
	std::random_device Random;
	std::array<std::uint8_t, 16> Bytes{};
	for (std::uint8_t& Byte : Bytes) Byte = static_cast<std::uint8_t>(Random());
	Bytes[6] = static_cast<std::uint8_t>((Bytes[6] & 0x0F) | 0x40);
	Bytes[8] = static_cast<std::uint8_t>((Bytes[8] & 0x3F) | 0x80);

	static constexpr char Hex[] = "0123456789abcdef";
	std::string Result;
	Result.reserve(36);
	for (std::size_t Index = 0; Index < Bytes.size(); ++Index)
	{
		if (Index == 4 || Index == 6 || Index == 8 || Index == 10) Result.push_back('-');
		Result.push_back(Hex[Bytes[Index] >> 4]);
		Result.push_back(Hex[Bytes[Index] & 0x0F]);
	}
	return Result;
}

bool IsUuid(const std::string_view Value)
{
	if (Value.size() != 36) return false;
	for (std::size_t Index = 0; Index < Value.size(); ++Index)
	{
		if (Index == 8 || Index == 13 || Index == 18 || Index == 23)
		{
			if (Value[Index] != '-') return false;
			continue;
		}
		const char Character = Value[Index];
		if (!((Character >= '0' && Character <= '9') || (Character >= 'a' && Character <= 'f'))) return false;
	}
	return true;
}

const Json* Member(const Json& Object, const std::string_view Name)
{
	return Object.GetType() == Wire::JsonType::Object ? Object.FindMember(Name) : nullptr;
}

bool UnsignedMember(const Json& Object, const std::string_view Name, std::uint64_t& OutValue)
{
	const Json* Value = Member(Object, Name);
	const std::string* Text = Value == nullptr ? nullptr : Value->TryGetNumberText();
	if (Text == nullptr) return false;
	const std::from_chars_result Parsed = std::from_chars(Text->data(), Text->data() + Text->size(), OutValue);
	return Parsed.ec == std::errc{} && Parsed.ptr == Text->data() + Text->size();
}

bool ParseIdentity(const std::string& Text, HostIdentity& OutIdentity)
{
	Json Root;
	Wire::JsonLimits Limits;
	Limits.MaximumDocumentBytes = MaximumIdentityBytes;
	Limits.MaximumStringBytes = 128;
	Limits.MaximumDepth = 4;
	Limits.MaximumNodeCount = 8;
	if (!Wire::ParseJson({ reinterpret_cast<const std::uint8_t*>(Text.data()), Text.size() }, Root, Limits) ||
	    Root.GetType() != Wire::JsonType::Object) return false;

	std::uint64_t Version = 0;
	std::uint64_t Session = 0;
	const Json* NodeValue = Member(Root, "node_id");
	const std::string* NodeId = NodeValue == nullptr ? nullptr : NodeValue->TryGetString();
	if (!UnsignedMember(Root, "schema", Version) || Version != IdentitySchemaVersion || NodeId == nullptr ||
	    !IsUuid(*NodeId) || !UnsignedMember(Root, "host_session", Session) || Session == 0) return false;

	OutIdentity.NodeId = *NodeId;
	OutIdentity.HostSession = Session;
	return true;
}

bool SerializeIdentity(const HostIdentity& Identity, std::string& OutText)
{
	Json Root;
	Root.SetObject();
	Json Version;
	Version.SetUnsignedInteger(IdentitySchemaVersion);
	Json Node;
	(void) Node.SetString(Identity.NodeId);
	Json Session;
	Session.SetUnsignedInteger(Identity.HostSession);
	return Root.InsertMember("schema", std::move(Version)) &&
	       Root.InsertMember("node_id", std::move(Node)) &&
	       Root.InsertMember("host_session", std::move(Session)) &&
	       Wire::SerializeJson(Root, OutText);
}

bool ReadIdentityFile(const std::filesystem::path& Path, std::string& OutText, std::string& OutError)
{
	std::ifstream File(Path, std::ios::binary);
	if (!File)
	{
		OutError = "cannot open the host identity file";
		return false;
	}
	File.seekg(0, std::ios::end);
	const std::streamoff Size = File.tellg();
	if (Size < 0 || static_cast<std::uint64_t>(Size) > MaximumIdentityBytes)
	{
		OutError = "host identity file is too large";
		return false;
	}
	File.seekg(0, std::ios::beg);
	OutText.resize(static_cast<std::size_t>(Size));
	if (Size != 0) File.read(OutText.data(), Size);
	if (!File)
	{
		OutError = "cannot read the host identity file";
		return false;
	}
	return true;
}

bool ReplaceFile(const std::filesystem::path& Temporary,
	             const std::filesystem::path& Destination,
	             std::string& OutError)
{
#if defined(_WIN32)
	if (!MoveFileExW(Temporary.c_str(), Destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		OutError = "cannot replace the host identity file (Windows error " + std::to_string(GetLastError()) + ')';
		return false;
	}
#else
	if (std::rename(Temporary.c_str(), Destination.c_str()) != 0)
	{
		OutError = "cannot replace the host identity file";
		return false;
	}
	(void) chmod(Destination.c_str(), S_IRUSR | S_IWUSR);
#endif
	return true;
}
}    // namespace

HostIdentityStore::HostIdentityStore(std::filesystem::path InStateDirectory)
	: StateDirectory(std::move(InStateDirectory))
{
}

std::filesystem::path HostIdentityStore::IdentityPath() const
{
	return StateDirectory / "identity.json";
}

bool HostIdentityStore::LoadAndAdvance(HostIdentity& OutIdentity, std::string& OutError) const
{
	HostIdentity Stored;
	const std::filesystem::path Path = IdentityPath();
	std::error_code FileError;
	const bool Exists = std::filesystem::exists(Path, FileError);
	if (FileError)
	{
		OutError = "cannot inspect the host identity file: " + FileError.message();
		return false;
	}
	if (Exists)
	{
		std::string Text;
		if (!ReadIdentityFile(Path, Text, OutError)) return false;
		if (!ParseIdentity(Text, Stored))
		{
			Stored = {};
		}
	}
	if (Stored.NodeId.empty())
	{
		Stored.NodeId = NewUuid();
		Stored.HostSession = 1;
	}
	else
	{
		if (Stored.HostSession == std::numeric_limits<std::uint64_t>::max())
		{
			OutError = "host session counter is exhausted";
			return false;
		}
		++Stored.HostSession;
	}
	Stored.InstanceId = NewUuid();
	if (!Persist(Stored, OutError)) return false;
	OutIdentity = std::move(Stored);
	OutError.clear();
	return true;
}

bool HostIdentityStore::AdvancePast(const std::uint64_t KnownSession,
	                               HostIdentity& InOutIdentity,
	                               std::string& OutError) const
{
	if (KnownSession < InOutIdentity.HostSession)
	{
		OutError.clear();
		return true;
	}
	if (KnownSession == std::numeric_limits<std::uint64_t>::max())
	{
		OutError = "known host session counter is exhausted";
		return false;
	}
	InOutIdentity.HostSession = KnownSession + 1;
	return Persist(InOutIdentity, OutError);
}

bool HostIdentityStore::Persist(const HostIdentity& Identity, std::string& OutError) const
{
	if (!IsUuid(Identity.NodeId) || Identity.HostSession == 0)
	{
		OutError = "refusing to persist an invalid host identity";
		return false;
	}
	std::error_code DirectoryError;
	std::filesystem::create_directories(StateDirectory, DirectoryError);
	if (DirectoryError)
	{
		OutError = "cannot create the host identity directory: " + DirectoryError.message();
		return false;
	}
	std::string Text;
	if (!SerializeIdentity(Identity, Text))
	{
		OutError = "cannot serialize the host identity";
		return false;
	}
	Text.push_back('\n');
	std::filesystem::path Temporary = IdentityPath();
	Temporary += std::filesystem::path("." + NewUuid() + ".tmp");
	{
		std::ofstream File(Temporary, std::ios::binary | std::ios::trunc);
		if (!File)
		{
			OutError = "cannot create a temporary host identity file";
			return false;
		}
		File.write(Text.data(), static_cast<std::streamsize>(Text.size()));
		File.flush();
		if (!File)
		{
			File.close();
			std::error_code Ignored;
			std::filesystem::remove(Temporary, Ignored);
			OutError = "cannot write the host identity file";
			return false;
		}
	}
#if !defined(_WIN32)
	(void) chmod(Temporary.c_str(), S_IRUSR | S_IWUSR);
#endif
	if (!ReplaceFile(Temporary, IdentityPath(), OutError))
	{
		std::error_code Ignored;
		std::filesystem::remove(Temporary, Ignored);
		return false;
	}
	OutError.clear();
	return true;
}
}    // namespace DeviceExplorer::Host
