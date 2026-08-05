#include "DeviceExplorerHostManifest.h"

#include <charconv>
#include <string_view>
#include <utility>

namespace DeviceExplorer::Wire
{
namespace
{
void SetManifestError(JsonError* OutError, const JsonError Error)
{
	if (OutError != nullptr) *OutError = Error;
}

bool ReadInt32(const JsonValue& Object, const std::string_view Name, std::int32_t& OutValue)
{
	const JsonValue* Value = Object.FindMember(Name);
	const std::string* Number = Value != nullptr ? Value->TryGetNumberText() : nullptr;
	if (Number == nullptr || Number->empty()) return false;
	std::int32_t Parsed = 0;
	const std::from_chars_result Result = std::from_chars(Number->data(), Number->data() + Number->size(), Parsed);
	if (Result.ec != std::errc{} || Result.ptr != Number->data() + Number->size()) return false;
	OutValue = Parsed;
	return true;
}

bool ReadString(const JsonValue& Object, const std::string_view Name, std::string& OutValue)
{
	const JsonValue* Value = Object.FindMember(Name);
	const std::string* String = Value != nullptr ? Value->TryGetString() : nullptr;
	if (String == nullptr || String->empty()) return false;
	OutValue = *String;
	return true;
}

bool AddInt32(JsonValue& Object, std::string Name, const std::int32_t Value)
{
	JsonValue Json;
	Json.SetSignedInteger(Value);
	return Object.InsertMember(std::move(Name), std::move(Json));
}

bool AddString(JsonValue& Object, std::string Name, const std::string& Value)
{
	JsonValue Json;
	if (!Json.SetString(Value)) return false;
	return Object.InsertMember(std::move(Name), std::move(Json));
}

bool IsValidRange(const ProtocolVersionRange Range, const bool bAllowUnsupported)
{
	if (Range.Minimum < 0 || Range.Maximum < Range.Minimum) return false;
	if (!bAllowUnsupported && Range.Minimum == 0) return false;
	return Range.Minimum != 0 || Range.Maximum == 0;
}
}    // namespace

bool IsValidHostManifest(const HostManifest& Manifest)
{
	return Manifest.ManifestVersion == HostManifestVersion && !Manifest.HostVersionText.empty() &&
	       !Manifest.BuildId.empty() && IsValidRange(Manifest.DeviceProtocol, false) &&
	       IsValidRange(Manifest.WebApi, false) && IsValidRange(Manifest.PeerProtocol, true);
}

bool SerializeHostManifest(const HostManifest& Manifest, std::string& OutJson, JsonError* OutError)
{
	if (!IsValidHostManifest(Manifest))
	{
		SetManifestError(OutError, JsonError::InvalidInput);
		return false;
	}

	JsonValue Root;
	Root.SetObject();
	if (!AddInt32(Root, "manifest_version", Manifest.ManifestVersion) ||
	    !AddString(Root, "host_version", Manifest.HostVersionText) ||
	    !AddString(Root, "build_id", Manifest.BuildId) ||
	    !AddInt32(Root, "device_protocol_min", Manifest.DeviceProtocol.Minimum) ||
	    !AddInt32(Root, "device_protocol_max", Manifest.DeviceProtocol.Maximum) ||
	    !AddInt32(Root, "web_api_min", Manifest.WebApi.Minimum) ||
	    !AddInt32(Root, "web_api_max", Manifest.WebApi.Maximum) ||
	    !AddInt32(Root, "peer_protocol_min", Manifest.PeerProtocol.Minimum) ||
	    !AddInt32(Root, "peer_protocol_max", Manifest.PeerProtocol.Maximum))
	{
		SetManifestError(OutError, JsonError::InvalidUnicode);
		return false;
	}
	return SerializeJson(Root, OutJson, {}, OutError);
}

bool ParseHostManifest(const ByteView Bytes, HostManifest& OutManifest, JsonError* OutError)
{
	JsonValue Root;
	JsonError Error = JsonError::None;
	if (!ParseJson(Bytes, Root, {}, &Error))
	{
		SetManifestError(OutError, Error);
		return false;
	}
	if (Root.GetType() != JsonType::Object)
	{
		SetManifestError(OutError, JsonError::RootNotObject);
		return false;
	}

	HostManifest Parsed;
	if (!ReadInt32(Root, "manifest_version", Parsed.ManifestVersion) ||
	    !ReadString(Root, "host_version", Parsed.HostVersionText) ||
	    !ReadString(Root, "build_id", Parsed.BuildId) ||
	    !ReadInt32(Root, "device_protocol_min", Parsed.DeviceProtocol.Minimum) ||
	    !ReadInt32(Root, "device_protocol_max", Parsed.DeviceProtocol.Maximum) ||
	    !ReadInt32(Root, "web_api_min", Parsed.WebApi.Minimum) ||
	    !ReadInt32(Root, "web_api_max", Parsed.WebApi.Maximum) ||
	    !ReadInt32(Root, "peer_protocol_min", Parsed.PeerProtocol.Minimum) ||
	    !ReadInt32(Root, "peer_protocol_max", Parsed.PeerProtocol.Maximum) ||
	    !IsValidHostManifest(Parsed))
	{
		SetManifestError(OutError, JsonError::InvalidInput);
		return false;
	}

	OutManifest = std::move(Parsed);
	SetManifestError(OutError, JsonError::None);
	return true;
}
}    // namespace DeviceExplorer::Wire
