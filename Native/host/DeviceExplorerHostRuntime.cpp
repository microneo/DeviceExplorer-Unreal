#include "DeviceExplorerHostRuntime.h"

#include "DeviceExplorerHostMdns.h"

#include "DeviceExplorerAuthPrimitives.h"
#include "DeviceExplorerHttpUpgrade.h"
#include "DeviceExplorerJson.h"
#include "DeviceExplorerProtocol.h"
#include "DeviceExplorerWebSocket.h"

#include <asio.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <deque>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace DeviceExplorer::Host
{
namespace RuntimeDetail
{
using Tcp = asio::ip::tcp;
using Json = Wire::JsonValue;

constexpr std::size_t MaximumHeaderBytes = 64 * 1024;
constexpr std::size_t MaximumJsonBytes = 1024 * 1024;
constexpr std::size_t MaximumWebSocketBytes = 8 * 1024 * 1024;
constexpr std::size_t MaximumLogMessageBytes = 64 * 1024;

std::string LowerAscii(std::string Value)
{
	std::transform(Value.begin(), Value.end(), Value.begin(), [](const unsigned char Character)
	{
		return static_cast<char>(std::tolower(Character));
	});
	return Value;
}

std::string TrimAscii(std::string_view Value)
{
	while (!Value.empty() && (Value.front() == ' ' || Value.front() == '\t')) Value.remove_prefix(1);
	while (!Value.empty() && (Value.back() == ' ' || Value.back() == '\t')) Value.remove_suffix(1);
	return { Value.data(), Value.size() };
}

bool ParseUnsigned(const std::string_view Text, std::uint64_t& OutValue)
{
	if (Text.empty()) return false;
	const auto Result = std::from_chars(Text.data(), Text.data() + Text.size(), OutValue);
	return Result.ec == std::errc{} && Result.ptr == Text.data() + Text.size();
}

std::string UrlDecode(const std::string_view Value, const bool FormEncoded)
{
	const auto Hex = [](const char Character) -> int
	{
		if (Character >= '0' && Character <= '9') return Character - '0';
		if (Character >= 'a' && Character <= 'f') return Character - 'a' + 10;
		if (Character >= 'A' && Character <= 'F') return Character - 'A' + 10;
		return -1;
	};
	std::string Result;
	Result.reserve(Value.size());
	for (std::size_t Index = 0; Index < Value.size(); ++Index)
	{
		if (FormEncoded && Value[Index] == '+')
		{
			Result.push_back(' ');
		}
		else if (Value[Index] == '%' && Index + 2 < Value.size())
		{
			const int High = Hex(Value[Index + 1]);
			const int Low = Hex(Value[Index + 2]);
			if (High < 0 || Low < 0) return {};
			Result.push_back(static_cast<char>((High << 4) | Low));
			Index += 2;
		}
		else
		{
			Result.push_back(Value[Index]);
		}
	}
	return Result;
}

std::string MakeId()
{
	std::array<std::uint8_t, 16> Bytes{};
	std::random_device Random;
	for (std::uint8_t& Byte : Bytes) Byte = static_cast<std::uint8_t>(Random());
	static constexpr char Hex[] = "0123456789abcdef";
	std::string Result(Bytes.size() * 2, '\0');
	for (std::size_t Index = 0; Index < Bytes.size(); ++Index)
	{
		Result[Index * 2] = Hex[Bytes[Index] >> 4];
		Result[Index * 2 + 1] = Hex[Bytes[Index] & 0x0F];
	}
	return Result;
}

std::string IsoNow()
{
	const std::time_t Time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	std::tm Utc{};
#if defined(_WIN32)
	gmtime_s(&Utc, &Time);
#else
	gmtime_r(&Time, &Utc);
#endif
	std::ostringstream Stream;
	Stream << std::put_time(&Utc, "%Y-%m-%dT%H:%M:%SZ");
	return Stream.str();
}

const Json* Member(const Json& Object, const std::string_view Name)
{
	return Object.FindMember(Name);
}

std::string StringMember(const Json& Object, const std::string_view Name)
{
	const Json* Value = Member(Object, Name);
	const std::string* String = Value ? Value->TryGetString() : nullptr;
	return String ? *String : std::string{};
}

bool BoolMember(const Json& Object, const std::string_view Name, const bool Default = false)
{
	const Json* Value = Member(Object, Name);
	bool Result = Default;
	return Value && Value->TryGetBoolean(Result) ? Result : Default;
}

std::int64_t IntegerMember(const Json& Object, const std::string_view Name, const std::int64_t Default = 0)
{
	const Json* Value = Member(Object, Name);
	const std::string* Number = Value ? Value->TryGetNumberText() : nullptr;
	if (!Number) return Default;
	std::int64_t Result = 0;
	const auto Parsed = std::from_chars(Number->data(), Number->data() + Number->size(), Result);
	return Parsed.ec == std::errc{} && Parsed.ptr == Number->data() + Number->size() ? Result : Default;
}

void AddString(Json& Object, std::string Name, std::string Value)
{
	Json JsonValue;
	(void) JsonValue.SetString(std::move(Value));
	(void) Object.InsertMember(std::move(Name), std::move(JsonValue));
}

void AddInteger(Json& Object, std::string Name, const std::int64_t Value)
{
	Json JsonValue;
	JsonValue.SetSignedInteger(Value);
	(void) Object.InsertMember(std::move(Name), std::move(JsonValue));
}

void AddUnsigned(Json& Object, std::string Name, const std::uint64_t Value)
{
	Json JsonValue;
	JsonValue.SetUnsignedInteger(Value);
	(void) Object.InsertMember(std::move(Name), std::move(JsonValue));
}

void AddBoolean(Json& Object, std::string Name, const bool Value)
{
	Json JsonValue;
	JsonValue.SetBoolean(Value);
	(void) Object.InsertMember(std::move(Name), std::move(JsonValue));
}

void AddCopy(Json& Object, std::string Name, const Json* Value, const bool ArrayFallback = false)
{
	Json Copy;
	if (Value)
	{
		Copy = *Value;
	}
	else if (ArrayFallback)
	{
		Copy.SetArray();
	}
	else
	{
		Copy.SetObject();
	}
	(void) Object.InsertMember(std::move(Name), std::move(Copy));
}

std::string Serialize(const Json& Value)
{
	std::string Result;
	return Wire::SerializeJson(Value, Result) ? Result : std::string{};
}

Json ErrorJson(const std::string& Message)
{
	Json Root;
	Root.SetObject();
	AddString(Root, "error", Message);
	return Root;
}

struct HttpRequest
{
	std::string Method;
	std::string Target;
	std::string Path;
	std::map<std::string, std::string> Query;
	std::map<std::string, std::string> Headers;
	std::string Body;
	std::uint64_t ContentLength = 0;
	std::string RawHeader;

	std::string Header(const std::string& Name) const
	{
		const auto Found = Headers.find(Name);
		return Found == Headers.end() ? std::string{} : Found->second;
	}
};

bool ParseRequest(const std::string_view Header, HttpRequest& Out)
{
	const std::size_t LineEnd = Header.find("\r\n");
	if (LineEnd == std::string_view::npos) return false;
	const std::string_view Line = Header.substr(0, LineEnd);
	const std::size_t First = Line.find(' ');
	const std::size_t Second = First == std::string_view::npos ? First : Line.find(' ', First + 1);
	if (First == std::string_view::npos || Second == std::string_view::npos || Line.substr(Second + 1) != "HTTP/1.1") return false;
	Out.Method = LowerAscii(std::string(Line.substr(0, First)));
	std::transform(Out.Method.begin(), Out.Method.end(), Out.Method.begin(), [](const unsigned char Character)
	{
		return static_cast<char>(std::toupper(Character));
	});
	Out.Target.assign(Line.substr(First + 1, Second - First - 1));
	if (Out.Target.empty() || Out.Target.front() != '/') return false;
	const std::size_t Question = Out.Target.find('?');
	Out.Path = UrlDecode(std::string_view(Out.Target).substr(0, Question), false);
	if (Out.Path.empty()) return false;
	if (Question != std::string::npos)
	{
		std::string_view Query = std::string_view(Out.Target).substr(Question + 1);
		while (!Query.empty())
		{
			const std::size_t Ampersand = Query.find('&');
			const std::string_view Pair = Query.substr(0, Ampersand);
			const std::size_t Equal = Pair.find('=');
			if (Equal != std::string_view::npos)
			{
				Out.Query[UrlDecode(Pair.substr(0, Equal), true)] = UrlDecode(Pair.substr(Equal + 1), true);
			}
			if (Ampersand == std::string_view::npos) break;
			Query.remove_prefix(Ampersand + 1);
		}
	}

	std::size_t Offset = LineEnd + 2;
	while (Offset + 2 <= Header.size())
	{
		const std::size_t End = Header.find("\r\n", Offset);
		if (End == std::string_view::npos) return false;
		if (End == Offset) break;
		const std::string_view HeaderLine = Header.substr(Offset, End - Offset);
		const std::size_t Colon = HeaderLine.find(':');
		if (Colon == std::string_view::npos) return false;
		const std::string Name = LowerAscii(TrimAscii(HeaderLine.substr(0, Colon)));
		if (Name.empty() || Out.Headers.count(Name) != 0) return false;
		Out.Headers.emplace(Name, TrimAscii(HeaderLine.substr(Colon + 1)));
		Offset = End + 2;
	}
	const std::string Length = Out.Header("content-length");
	if (!Length.empty() && !ParseUnsigned(Length, Out.ContentLength)) return false;
	Out.RawHeader.assign(Header.data(), Header.size());
	return true;
}

struct HttpResponse
{
	int Status = 200;
	std::string ContentType = "application/json; charset=utf-8";
	std::string Body;
	std::vector<std::pair<std::string, std::string>> Headers;
};

std::string StatusText(const int Status)
{
	switch (Status)
	{
		case 101: return "Switching Protocols";
		case 200: return "OK";
		case 202: return "Accepted";
		case 400: return "Bad Request";
		case 401: return "Unauthorized";
		case 403: return "Forbidden";
		case 404: return "Not Found";
		case 405: return "Method Not Allowed";
		case 408: return "Request Timeout";
		case 409: return "Conflict";
		case 411: return "Length Required";
		case 413: return "Content Too Large";
		case 500: return "Internal Server Error";
		case 502: return "Bad Gateway";
		case 503: return "Service Unavailable";
		case 504: return "Gateway Timeout";
		default: return "Error";
	}
}

std::string EncodeResponse(const HttpResponse& Response)
{
	std::string Result = "HTTP/1.1 " + std::to_string(Response.Status) + " " + StatusText(Response.Status) + "\r\n";
	Result += "Connection: close\r\nContent-Type: " + Response.ContentType + "\r\nContent-Length: " + std::to_string(Response.Body.size());
	Result += "\r\nX-Content-Type-Options: nosniff\r\n";
	for (const auto& Header : Response.Headers) Result += Header.first + ": " + Header.second + "\r\n";
	Result += "\r\n";
	Result += Response.Body;
	return Result;
}

std::string ContentTypeForPath(const std::filesystem::path& Path)
{
	const std::string Extension = LowerAscii(Path.extension().string());
	if (Extension == ".html") return "text/html; charset=utf-8";
	if (Extension == ".css") return "text/css; charset=utf-8";
	if (Extension == ".js") return "text/javascript; charset=utf-8";
	if (Extension == ".svg") return "image/svg+xml";
	if (Extension == ".png") return "image/png";
	if (Extension == ".json") return "application/json; charset=utf-8";
	return "application/octet-stream";
}

class DeviceChannel
{
public:
	virtual ~DeviceChannel() = default;
	virtual void SendText(std::string Text) = 0;
	virtual void Close() = 0;
	virtual std::string LocalAddress() const = 0;
};

struct LogEntry
{
	std::uint64_t Sequence = 0;
	std::string Timestamp;
	std::string Category;
	std::string Verbosity;
	std::string Message;
};

struct DeviceState
{
	std::string Id;
	std::string Name;
	std::string ProjectName;
	std::string EngineVersion;
	std::string Platform;
	std::string Configuration;
	std::string BuildVersion;
	std::string RemoteAddress;
	std::int64_t ProtocolVersion = 0;
	std::int64_t UptimeSeconds = 0;
	bool Connected = false;
	std::string ConnectedAt;
	std::string LastSeen;
	std::uint64_t DroppedLogs = 0;
	std::uint64_t NextSequence = 1;
	Json Capabilities;
	Json Commands;
	Json FileRoots;
	Json DataModules;
	std::deque<LogEntry> Logs;
	std::weak_ptr<DeviceChannel> Channel;
	std::chrono::steady_clock::time_point LastActivity = std::chrono::steady_clock::now();
};

struct PendingRequest
{
	explicit PendingRequest(asio::io_context& Io) : Timer(Io) {}
	asio::steady_timer Timer;
	std::function<void(std::string)> Complete;
};

struct TransferState
{
	std::string Id;
	std::string DeviceId;
	std::string Root;
	std::string RelativePath;
	std::string Filename;
	std::string State;
	std::string Error;
	std::string LocalPath;
	std::string UploadSecret;
	std::uint64_t Bytes = 0;
	std::string CreatedAt;
	std::string UpdatedAt;
	std::chrono::steady_clock::time_point LastActivity = std::chrono::steady_clock::now();
};

std::string SafeFilename(const std::string& Value)
{
	std::string Result = std::filesystem::path(Value).filename().string();
	for (char& Character : Result)
	{
		if (Character == '"' || Character == '\r' || Character == '\n') Character = '_';
	}
	return Result.empty() ? "download.bin" : Result;
}

std::string NormalizeRelativePath(const std::string& Value)
{
	std::filesystem::path Result;
	for (const std::filesystem::path& Part : std::filesystem::path(Value))
	{
		if (Part == "." || Part.empty()) continue;
		if (Part == ".." || Part.is_absolute()) return {};
		Result /= Part;
	}
	return Result.generic_string();
}

struct HostState : public std::enable_shared_from_this<HostState>
{
	HostState(asio::io_context& InIo, const HostConfig& InConfig, BoundEndpoints InEndpoints, std::string InManifest)
		: Io(InIo), Config(InConfig), Endpoints(std::move(InEndpoints)), Manifest(std::move(InManifest)), MaintenanceTimer(Io)
	{
	}

	void StartMaintenance()
	{
		MaintenanceTimer.expires_after(std::chrono::minutes(1));
		std::weak_ptr<HostState> WeakSelf = shared_from_this();
		MaintenanceTimer.async_wait([WeakSelf](const asio::error_code& Error)
		{
			if (Error) return;
			if (const std::shared_ptr<HostState> Self = WeakSelf.lock())
			{
				Self->CleanupExpiredState();
				Self->StartMaintenance();
			}
		});
	}

	void StopMaintenance()
	{
		MaintenanceTimer.cancel();
	}

	void CleanupExpiredState()
	{
		const auto Now = std::chrono::steady_clock::now();
		for (auto Iterator = Transfers.begin(); Iterator != Transfers.end();)
		{
			if (Now - Iterator->second.LastActivity <= Config.TransferTtl)
			{
				++Iterator;
				continue;
			}
			std::error_code Ignored;
			std::filesystem::remove(Iterator->second.LocalPath, Ignored);
			std::filesystem::remove(Iterator->second.LocalPath + ".part", Ignored);
			Iterator = Transfers.erase(Iterator);
		}
		for (auto Iterator = Devices.begin(); Iterator != Devices.end();)
		{
			if (!Iterator->second.Connected && Now - Iterator->second.LastActivity > Config.DisconnectedDeviceTtl)
			{
				Iterator = Devices.erase(Iterator);
			}
			else
			{
				++Iterator;
			}
		}
	}

	void Log(const LogLevel Level, const std::string& Message) const
	{
		if (Config.Log) Config.Log(Level, Message);
	}

	void Attach(const std::shared_ptr<DeviceChannel>& Channel, const Json& Hello, const std::string& RemoteAddress)
	{
		const std::string Id = StringMember(Hello, "device_id");
		if (Id.empty()) return;
		DeviceState& Device = Devices[Id];
		if (const std::shared_ptr<DeviceChannel> Previous = Device.Channel.lock())
		{
			if (Previous != Channel) Previous->Close();
		}
		Device.Id = Id;
		Device.Name = StringMember(Hello, "name");
		Device.ProjectName = StringMember(Hello, "project_name");
		Device.EngineVersion = StringMember(Hello, "engine_version");
		Device.Platform = StringMember(Hello, "platform");
		Device.Configuration = StringMember(Hello, "configuration");
		Device.BuildVersion = StringMember(Hello, "build_version");
		Device.RemoteAddress = RemoteAddress;
		Device.ProtocolVersion = IntegerMember(Hello, "protocol_version");
		Device.UptimeSeconds = IntegerMember(Hello, "uptime_seconds");
		Device.Connected = true;
		Device.ConnectedAt = IsoNow();
		Device.LastSeen = Device.ConnectedAt;
		Device.LastActivity = std::chrono::steady_clock::now();
		Device.Channel = Channel;
		Device.Capabilities = Member(Hello, "capabilities") ? *Member(Hello, "capabilities") : Json{};
		Device.Commands = Member(Hello, "commands") ? *Member(Hello, "commands") : Json{};
		Device.FileRoots = Member(Hello, "file_roots") ? *Member(Hello, "file_roots") : Json{};
		Device.DataModules = Member(Hello, "data_modules") ? *Member(Hello, "data_modules") : Json{};
		if (Device.Capabilities.GetType() != Wire::JsonType::Array) Device.Capabilities.SetArray();
		if (Device.Commands.GetType() != Wire::JsonType::Array) Device.Commands.SetArray();
		if (Device.FileRoots.GetType() != Wire::JsonType::Array) Device.FileRoots.SetArray();
		if (Device.DataModules.GetType() != Wire::JsonType::Array) Device.DataModules.SetArray();
		Log(LogLevel::Information, "device connected: " + Device.Name + " (" + Id + ")");
	}

	void Detach(const std::shared_ptr<DeviceChannel>& Channel)
	{
		for (auto& Pair : Devices)
		{
			if (Pair.second.Channel.lock() == Channel)
			{
				Pair.second.Channel.reset();
				Pair.second.Connected = false;
				Pair.second.LastSeen = IsoNow();
				Pair.second.LastActivity = std::chrono::steady_clock::now();
				break;
			}
		}
	}

	void OnMessage(const std::shared_ptr<DeviceChannel>& Channel, const Json& Message)
	{
		const std::string Type = StringMember(Message, "type");
		if (Type == "transfer_result" && !BoolMember(Message, "success"))
		{
			if (TransferState* Transfer = FindTransfer(StringMember(Message, "transfer_id")))
			{
				Transfer->State = "failed";
				Transfer->Error = StringMember(Message, "error");
					Transfer->UpdatedAt = IsoNow();
					Transfer->LastActivity = std::chrono::steady_clock::now();
			}
			return;
		}
		const std::string RequestId = StringMember(Message, "request_id");
		if (!RequestId.empty())
		{
			const auto Found = Pending.find(RequestId);
			if (Found != Pending.end())
			{
				const std::shared_ptr<PendingRequest> Request = Found->second;
				Pending.erase(Found);
				Request->Timer.cancel();
				Request->Complete(Serialize(Message));
				return;
			}
		}

		DeviceState* Device = nullptr;
		for (auto& Pair : Devices)
		{
			if (Pair.second.Channel.lock() == Channel)
			{
				Device = &Pair.second;
				break;
			}
		}
		if (!Device) return;
		Device->LastSeen = IsoNow();
		Device->LastActivity = std::chrono::steady_clock::now();
		if (Type == "heartbeat")
		{
			Device->UptimeSeconds = IntegerMember(Message, "uptime_seconds");
			Device->Connected = true;
			return;
		}
		if (Type != "log_batch") return;
		const std::int64_t Dropped = IntegerMember(Message, "dropped");
		if (Dropped > 0) Device->DroppedLogs += static_cast<std::uint64_t>(Dropped);
		const Json* EntriesValue = Member(Message, "entries");
		const std::vector<Json>* Entries = EntriesValue ? EntriesValue->TryGetArray() : nullptr;
		if (!Entries) return;
		for (const Json& Entry : *Entries)
		{
			LogEntry LogLine;
			LogLine.Sequence = Device->NextSequence++;
			LogLine.Timestamp = StringMember(Entry, "timestamp");
			LogLine.Category = StringMember(Entry, "category");
			LogLine.Verbosity = StringMember(Entry, "verbosity");
			LogLine.Message = StringMember(Entry, "message");
			if (LogLine.Timestamp.empty()) LogLine.Timestamp = IsoNow();
			if (LogLine.Category.size() > 256) LogLine.Category.resize(256);
			if (LogLine.Verbosity.size() > 64) LogLine.Verbosity.resize(64);
			if (LogLine.Message.size() > MaximumLogMessageBytes) LogLine.Message.resize(MaximumLogMessageBytes);
			Device->Logs.push_back(std::move(LogLine));
		}
		while (Device->Logs.size() > Config.LogCapacity)
		{
			Device->Logs.pop_front();
			++Device->DroppedLogs;
		}
	}

	std::string DevicesJson()
	{
		Json Array;
		Array.SetArray();
		std::vector<const DeviceState*> Ordered;
		for (const auto& Pair : Devices) Ordered.push_back(&Pair.second);
		std::sort(Ordered.begin(), Ordered.end(), [](const DeviceState* Left, const DeviceState* Right)
		{
			if (Left->Connected != Right->Connected) return Left->Connected;
			return Left->Name < Right->Name;
		});
		for (const DeviceState* Device : Ordered)
		{
			Json Item;
			Item.SetObject();
			AddString(Item, "id", Device->Id);
			AddString(Item, "name", Device->Name);
			AddString(Item, "project_name", Device->ProjectName);
			AddString(Item, "engine_version", Device->EngineVersion);
			AddString(Item, "platform", Device->Platform);
			AddString(Item, "configuration", Device->Configuration);
			AddString(Item, "build_version", Device->BuildVersion);
			AddInteger(Item, "protocol_version", Device->ProtocolVersion);
			AddInteger(Item, "uptime_seconds", Device->UptimeSeconds);
			AddBoolean(Item, "connected", Device->Connected);
			AddString(Item, "connected_at", Device->ConnectedAt);
			AddString(Item, "last_seen", Device->LastSeen);
			AddString(Item, "remote_address", Device->RemoteAddress);
			AddUnsigned(Item, "dropped_logs", Device->DroppedLogs);
			AddUnsigned(Item, "log_count", Device->Logs.size());
			AddCopy(Item, "capabilities", &Device->Capabilities, true);
			AddCopy(Item, "commands", &Device->Commands, true);
			AddCopy(Item, "file_roots", &Device->FileRoots, true);
			AddCopy(Item, "data_modules", &Device->DataModules, true);
			(void) Array.Append(std::move(Item));
		}
		Json Root;
		Root.SetObject();
		(void) Root.InsertMember("devices", std::move(Array));
		return Serialize(Root);
	}

	std::string LogsJson(const std::string& DeviceId,
	                    const std::uint64_t After,
	                    const std::string& Category,
	                    const std::string& Verbosity)
	{
		const auto Found = Devices.find(DeviceId);
		if (Found == Devices.end()) return {};
		const std::string CategoryLower = LowerAscii(Category);
		const std::string VerbosityLower = LowerAscii(Verbosity);
		Json Entries;
		Entries.SetArray();
		std::size_t Count = 0;
		for (const LogEntry& Entry : Found->second.Logs)
		{
			if (Entry.Sequence <= After) continue;
			if (!CategoryLower.empty() && LowerAscii(Entry.Category).find(CategoryLower) == std::string::npos) continue;
			if (!VerbosityLower.empty() && LowerAscii(Entry.Verbosity) != VerbosityLower) continue;
			Json Item;
			Item.SetObject();
			AddUnsigned(Item, "sequence", Entry.Sequence);
			AddString(Item, "timestamp", Entry.Timestamp);
			AddString(Item, "category", Entry.Category);
			AddString(Item, "verbosity", Entry.Verbosity);
			AddString(Item, "message", Entry.Message);
			(void) Entries.Append(std::move(Item));
			if (++Count == 2000) break;
		}
		Json Root;
		Root.SetObject();
		(void) Root.InsertMember("entries", std::move(Entries));
		AddUnsigned(Root, "dropped", Found->second.DroppedLogs);
		AddUnsigned(Root, "buffered", Found->second.Logs.size());
		AddUnsigned(Root, "capacity", Config.LogCapacity);
		return Serialize(Root);
	}

	bool SendRequest(const std::string& DeviceId, Json Message, std::function<void(std::string)> Complete)
	{
		const auto Found = Devices.find(DeviceId);
		if (Found == Devices.end() || !Found->second.Connected) return false;
		const std::shared_ptr<DeviceChannel> Channel = Found->second.Channel.lock();
		if (!Channel) return false;
		const std::string RequestId = MakeId();
		Json RequestIdValue;
		(void) RequestIdValue.SetString(RequestId);
		(void) Message.SetMember("request_id", std::move(RequestIdValue));
		const std::string Text = Serialize(Message);
		if (Text.empty()) return false;
		const std::shared_ptr<PendingRequest> Request = std::make_shared<PendingRequest>(Io);
		Request->Complete = std::move(Complete);
		Pending.emplace(RequestId, Request);
		Request->Timer.expires_after(Config.RequestTimeout);
		std::weak_ptr<HostState> WeakSelf = shared_from_this();
		Request->Timer.async_wait([WeakSelf, RequestId](const asio::error_code& Error)
		{
			if (Error) return;
			if (const std::shared_ptr<HostState> Self = WeakSelf.lock())
			{
				const auto TimedOut = Self->Pending.find(RequestId);
				if (TimedOut == Self->Pending.end()) return;
				const std::shared_ptr<PendingRequest> PendingRequestValue = TimedOut->second;
				Self->Pending.erase(TimedOut);
				PendingRequestValue->Complete({});
			}
		});
		Channel->SendText(Text);
		return true;
	}

	std::string CreateTransfer(const std::string& DeviceId,
	                         const std::string& RootName,
	                         const std::string& RelativePath,
	                         const bool Archive)
	{
		const auto Found = Devices.find(DeviceId);
		if (Found == Devices.end() || !Found->second.Connected) return {};
		const std::shared_ptr<DeviceChannel> Channel = Found->second.Channel.lock();
		if (!Channel || RootName.empty()) return {};
		const std::string Normalized = NormalizeRelativePath(RelativePath);
		if (!Archive && Normalized.empty()) return {};
		TransferState Transfer;
		Transfer.Id = MakeId();
		Transfer.DeviceId = DeviceId;
		Transfer.Root = RootName;
		Transfer.RelativePath = Normalized;
		Transfer.Filename = Archive ? SafeFilename(Normalized.empty() ? RootName : Normalized) + ".zip" : SafeFilename(Normalized);
		Transfer.State = "requested";
		Transfer.UploadSecret = MakeId();
		Transfer.CreatedAt = IsoNow();
		Transfer.UpdatedAt = Transfer.CreatedAt;
		std::filesystem::path Directory = Config.TransferDirectory.empty()
			? std::filesystem::temp_directory_path() / "DeviceExplorer" / "Transfers"
			: std::filesystem::path(Config.TransferDirectory);
		std::error_code Error;
		std::filesystem::create_directories(Directory, Error);
		if (Error) return {};
		Transfer.LocalPath = (Directory / (Transfer.Id + ".bin")).string();
		const std::string TransferId = Transfer.Id;
		Transfers.emplace(TransferId, Transfer);

		Json Message;
		Message.SetObject();
		AddString(Message, "type", "upload_file");
		AddString(Message, "request_id", MakeId());
		AddString(Message, "transfer_id", TransferId);
		AddString(Message, "root", RootName);
		AddString(Message, "path", Normalized);
		AddBoolean(Message, "archive", Archive);
		const std::string UploadHost = Channel->LocalAddress().empty() ? "127.0.0.1" : Channel->LocalAddress();
		AddString(Message, "upload_url", "http://" + UploadHost + ':' + std::to_string(Endpoints.DevicePort) +
		          "/device/transfers/" + TransferId + "?upload=" + Transfer.UploadSecret);
		const std::string Text = Serialize(Message);
		if (Text.empty())
		{
			Transfers.erase(TransferId);
			return {};
		}
		Channel->SendText(Text);
		return TransferJson(Transfers.at(TransferId));
	}

	std::string TransferJson(const TransferState& Transfer) const
	{
		Json Root;
		Root.SetObject();
		AddString(Root, "id", Transfer.Id);
		AddString(Root, "device_id", Transfer.DeviceId);
		AddString(Root, "root", Transfer.Root);
		AddString(Root, "path", Transfer.RelativePath);
		AddString(Root, "filename", Transfer.Filename);
		AddString(Root, "state", Transfer.State);
		AddUnsigned(Root, "bytes", Transfer.Bytes);
		AddString(Root, "error", Transfer.Error);
		AddString(Root, "created_at", Transfer.CreatedAt);
		AddString(Root, "updated_at", Transfer.UpdatedAt);
		return Serialize(Root);
	}

	TransferState* FindTransfer(const std::string& Id)
	{
		const auto Found = Transfers.find(Id);
		return Found == Transfers.end() ? nullptr : &Found->second;
	}

	asio::io_context& Io;
	HostConfig Config;
	BoundEndpoints Endpoints;
	std::string Manifest;
	std::unordered_map<std::string, DeviceState> Devices;
	std::unordered_map<std::string, std::shared_ptr<PendingRequest>> Pending;
	std::unordered_map<std::string, TransferState> Transfers;
	asio::steady_timer MaintenanceTimer;
};

class DeviceSession final : public DeviceChannel, public std::enable_shared_from_this<DeviceSession>
{
public:
	DeviceSession(Tcp::socket Socket, std::shared_ptr<HostState> State, std::string Header, std::string Prefix)
		: Socket(std::move(Socket)), State(std::move(State)), Header(std::move(Header)), Prefix(std::move(Prefix)),
		  Decoder(Wire::WebSocketRole::Server, { MaximumWebSocketBytes, MaximumWebSocketBytes, 1024 })
	{
		asio::error_code Error;
		RemoteAddress = this->Socket.remote_endpoint(Error).address().to_string();
		Error.clear();
		LocalEndpointAddress = this->Socket.local_endpoint(Error).address().to_string();
	}

	void Start()
	{
		Wire::WebSocketUpgradeRequest Request;
		const Wire::HttpUpgradeParseResult Parsed = Wire::ParseWebSocketUpgradeRequest(
			{ reinterpret_cast<const std::uint8_t*>(Header.data()), Header.size() }, Request);
		std::string Accept;
		if (Parsed.Status != Wire::HttpUpgradeStatus::Complete || Request.Target != "/device/connect" ||
		    !Wire::MakeWebSocketAccept(Request.Key, Accept))
		{
			CloseSocket();
			return;
		}
		const std::string Response = Wire::SerializeWebSocketUpgradeResponse(Accept);
		const std::shared_ptr<std::string> Bytes = std::make_shared<std::string>(Response);
		auto Self = shared_from_this();
		asio::async_write(Socket, asio::buffer(*Bytes), [Self, Bytes](const asio::error_code& Error, std::size_t)
		{
			if (Error)
			{
				Self->CloseSocket();
				return;
			}
			if (!Self->Prefix.empty()) Self->Consume(Self->Prefix);
			Self->Read();
		});
	}

	void SendText(std::string Text) override
	{
		Wire::WebSocketFrame Frame;
		Frame.Opcode = Wire::WebSocketOpcode::Text;
		Frame.Payload.assign(Text.begin(), Text.end());
		QueueFrame(std::move(Frame));
	}

	void Close() override
	{
		CloseSocket();
	}

	std::string LocalAddress() const override
	{
		return LocalEndpointAddress;
	}

private:
	void Read()
	{
		if (Closed) return;
		auto Self = shared_from_this();
		Socket.async_read_some(asio::buffer(ReadBuffer), [Self](const asio::error_code& Error, const std::size_t Bytes)
		{
			if (Error || Bytes == 0)
			{
				Self->CloseSocket();
				return;
			}
			Self->Consume(std::string_view(Self->ReadBuffer.data(), Bytes));
			Self->Read();
		});
	}

	void Consume(const std::string_view Bytes)
	{
		if (!Decoder.Consume({ reinterpret_cast<const std::uint8_t*>(Bytes.data()), Bytes.size() }))
		{
			CloseWithCode(Decoder.GetError() == Wire::WebSocketError::InvalidUtf8 ? 1007 : 1002);
			return;
		}
		Wire::WebSocketFrame Frame;
		while (Decoder.Drain(Frame)) HandleFrame(Frame);
	}

	void HandleFrame(const Wire::WebSocketFrame& Frame)
	{
		if (Frame.Opcode == Wire::WebSocketOpcode::Close)
		{
			QueueFrame(Frame);
			CloseAfterWrite = true;
			return;
		}
		if (Frame.Opcode == Wire::WebSocketOpcode::Ping)
		{
			Wire::WebSocketFrame Pong;
			Pong.Opcode = Wire::WebSocketOpcode::Pong;
			Pong.Payload = Frame.Payload;
			QueueFrame(std::move(Pong));
			return;
		}
		if (Frame.Opcode == Wire::WebSocketOpcode::Text)
		{
			Fragment.assign(Frame.Payload.begin(), Frame.Payload.end());
			Fragmenting = !Frame.Final;
		}
		else if (Frame.Opcode == Wire::WebSocketOpcode::Continuation && Fragmenting)
		{
			Fragment.append(reinterpret_cast<const char*>(Frame.Payload.data()), Frame.Payload.size());
			Fragmenting = !Frame.Final;
		}
		else
		{
			return;
		}
		if (Frame.Final)
		{
			HandleText(Fragment);
			Fragment.clear();
			Fragmenting = false;
		}
	}

	void HandleText(const std::string& Text)
	{
		Json Root;
		if (!Wire::ParseJson({ reinterpret_cast<const std::uint8_t*>(Text.data()), Text.size() }, Root) ||
		    Root.GetType() != Wire::JsonType::Object) return;
		const std::string Type = StringMember(Root, "type");
		if (!Authenticated)
		{
			HandleAuth(Type, Root);
			return;
		}
		if (Type == "hello")
		{
			State->Attach(shared_from_this(), Root, RemoteAddress);
			Attached = true;
			return;
		}
		State->OnMessage(shared_from_this(), Root);
	}

	void HandleAuth(const std::string& Type, const Json& Message)
	{
		if (Type == "auth_request")
		{
			ClientNonce = StringMember(Message, "client_nonce");
			if (IntegerMember(Message, "protocol_version") != DeviceProtocolVersion || !Wire::Auth::IsValidNonce(ClientNonce))
			{
				Reject("invalid protocol version or client nonce");
				return;
			}
			HostNonce = MakeId();
			Json Challenge;
			Challenge.SetObject();
			AddString(Challenge, "type", "auth_challenge");
			AddString(Challenge, "host_nonce", HostNonce);
			AddString(Challenge, "host_proof", Wire::Auth::ComputeProof(State->Config.Token, Wire::Auth::HostProofLabel, ClientNonce, HostNonce));
			SendText(Serialize(Challenge));
			return;
		}
		if (Type == "auth_response" && !HostNonce.empty())
		{
			const std::string Expected = Wire::Auth::ComputeProof(State->Config.Token, Wire::Auth::DeviceProofLabel, ClientNonce, HostNonce);
			if (!Wire::Auth::ConstantTimeEquals(StringMember(Message, "client_proof"), Expected))
			{
				Reject("invalid session token proof");
				return;
			}
			Authenticated = true;
			Json Accepted;
			Accepted.SetObject();
			AddString(Accepted, "type", "auth_ok");
			SendText(Serialize(Accepted));
			return;
		}
		Reject("expected authentication before any other message");
	}

	void Reject(const std::string& Reason)
	{
		Json Error = ErrorJson(Reason);
		Json Type;
		(void) Type.SetString("auth_error");
		(void) Error.InsertMember("type", std::move(Type));
		SendText(Serialize(Error));
		State->Log(LogLevel::Warning, "device authentication refused: " + Reason);
		CloseAfterWrite = true;
	}

	void CloseWithCode(const std::uint16_t Code)
	{
		Wire::WebSocketFrame CloseFrame;
		CloseFrame.Opcode = Wire::WebSocketOpcode::Close;
		CloseFrame.Payload = { static_cast<std::uint8_t>(Code >> 8), static_cast<std::uint8_t>(Code & 0xFF) };
		QueueFrame(std::move(CloseFrame));
		CloseAfterWrite = true;
	}

	void QueueFrame(Wire::WebSocketFrame Frame)
	{
		if (Closed) return;
		std::vector<std::uint8_t> Bytes;
		if (!Wire::EncodeWebSocketFrame(Frame, Wire::WebSocketRole::Server, 0, Bytes))
		{
			CloseSocket();
			return;
		}
		WriteQueue.push_back(std::make_shared<std::vector<std::uint8_t>>(std::move(Bytes)));
		if (WriteQueue.size() == 1) Write();
	}

	void Write()
	{
		if (Closed || WriteQueue.empty()) return;
		auto Self = shared_from_this();
		const std::shared_ptr<std::vector<std::uint8_t>> Bytes = WriteQueue.front();
		asio::async_write(Socket, asio::buffer(*Bytes), [Self, Bytes](const asio::error_code& Error, std::size_t)
		{
			if (Error)
			{
				Self->CloseSocket();
				return;
			}
			Self->WriteQueue.pop_front();
			if (!Self->WriteQueue.empty()) Self->Write();
			else if (Self->CloseAfterWrite) Self->CloseSocket();
		});
	}

	void CloseSocket()
	{
		if (Closed) return;
		Closed = true;
		if (Attached) State->Detach(shared_from_this());
		asio::error_code Ignored;
		Socket.shutdown(Tcp::socket::shutdown_both, Ignored);
		Socket.close(Ignored);
	}

	Tcp::socket Socket;
	std::shared_ptr<HostState> State;
	std::string Header;
	std::string Prefix;
	std::string RemoteAddress;
	std::string LocalEndpointAddress;
	Wire::WebSocketDecoder Decoder;
	std::array<char, 64 * 1024> ReadBuffer{};
	std::deque<std::shared_ptr<std::vector<std::uint8_t>>> WriteQueue;
	std::string Fragment;
	std::string ClientNonce;
	std::string HostNonce;
	bool Fragmenting = false;
	bool Authenticated = false;
	bool Attached = false;
	bool CloseAfterWrite = false;
	bool Closed = false;
};

class UploadSession final : public std::enable_shared_from_this<UploadSession>
{
public:
	UploadSession(Tcp::socket Socket, std::shared_ptr<HostState> State, HttpRequest Request)
		: Socket(std::move(Socket)), State(std::move(State)), Request(std::move(Request))
	{
	}

	void Start()
	{
		const std::string Prefix = "/device/transfers/";
		TransferId = Request.Path.substr(Prefix.size());
		TransferState* Transfer = State->FindTransfer(TransferId);
		const auto Credential = Request.Query.find("upload");
		if (!Transfer)
		{
			Reply(404, "Unknown transfer");
			return;
		}
		if (Credential == Request.Query.end() || Transfer->UploadSecret.empty() ||
		    !Wire::Auth::ConstantTimeEquals(Credential->second, Transfer->UploadSecret))
		{
			Reply(401, "Invalid upload credential");
			return;
		}
		if (Request.ContentLength == 0)
		{
			Reply(411, "Content-Length is required");
			return;
		}
		if (Request.ContentLength > State->Config.MaximumTransferBytes)
		{
			Reply(413, "Transfer is too large");
			return;
		}
		if (Transfer->State != "requested" && Transfer->State != "failed")
		{
			Reply(409, "Transfer is not uploadable");
			return;
		}
		FinalPath = Transfer->LocalPath;
		PartPath = FinalPath + ".part";
		File.open(PartPath, std::ios::binary | std::ios::trunc);
		if (!File)
		{
			Reply(500, "Cannot create transfer file");
			return;
		}
		Transfer->State = "uploading";
		Transfer->Error.clear();
		Transfer->Bytes = 0;
		Transfer->UpdatedAt = IsoNow();
		Transfer->LastActivity = std::chrono::steady_clock::now();
		const std::size_t PrefixBytes = static_cast<std::size_t>(std::min<std::uint64_t>(Request.Body.size(), Request.ContentLength));
		if (PrefixBytes != 0)
		{
			File.write(Request.Body.data(), static_cast<std::streamsize>(PrefixBytes));
			Written = PrefixBytes;
		}
		if (!File)
		{
			Fail("Cannot write transfer file");
			return;
		}
		UpdateProgress();
		if (Written == Request.ContentLength) Finish();
		else Read();
	}

private:
	void Read()
	{
		const std::size_t Wanted = static_cast<std::size_t>(std::min<std::uint64_t>(Buffer.size(), Request.ContentLength - Written));
		auto Self = shared_from_this();
		Socket.async_read_some(asio::buffer(Buffer.data(), Wanted), [Self](const asio::error_code& Error, const std::size_t Bytes)
		{
			if (Error || Bytes == 0)
			{
				Self->Fail("Upload ended before Content-Length");
				return;
			}
			Self->File.write(Self->Buffer.data(), static_cast<std::streamsize>(Bytes));
			if (!Self->File)
			{
				Self->Fail("Cannot write transfer file");
				return;
			}
			Self->Written += Bytes;
			Self->UpdateProgress();
			if (Self->Written == Self->Request.ContentLength) Self->Finish();
			else Self->Read();
		});
	}

	void UpdateProgress()
	{
		if (TransferState* Transfer = State->FindTransfer(TransferId))
		{
			Transfer->Bytes = Written;
			Transfer->UpdatedAt = IsoNow();
			Transfer->LastActivity = std::chrono::steady_clock::now();
		}
	}

	void Finish()
	{
		File.close();
		std::error_code Error;
		std::filesystem::remove(FinalPath, Error);
		Error.clear();
		std::filesystem::rename(PartPath, FinalPath, Error);
		if (Error)
		{
			Fail("Cannot finalize transfer file");
			return;
		}
		if (TransferState* Transfer = State->FindTransfer(TransferId))
		{
			Transfer->State = "ready";
			Transfer->Bytes = Written;
			Transfer->UpdatedAt = IsoNow();
			Transfer->LastActivity = std::chrono::steady_clock::now();
		}
		Json Result;
		Result.SetObject();
		AddString(Result, "status", "ready");
		AddUnsigned(Result, "bytes", Written);
		ReplyJson(200, Result);
	}

	void Fail(const std::string& Message)
	{
		File.close();
		std::error_code Ignored;
		std::filesystem::remove(PartPath, Ignored);
		if (TransferState* Transfer = State->FindTransfer(TransferId))
		{
			Transfer->State = "failed";
			Transfer->Error = Message;
			Transfer->UpdatedAt = IsoNow();
			Transfer->LastActivity = std::chrono::steady_clock::now();
		}
		Reply(500, Message);
	}

	void Reply(const int Status, const std::string& Message)
	{
		ReplyJson(Status, ErrorJson(Message));
	}

	void ReplyJson(const int Status, const Json& Body)
	{
		Response = EncodeResponse({ Status, "application/json; charset=utf-8", Serialize(Body), { { "Cache-Control", "no-store" } } });
		auto Self = shared_from_this();
		asio::async_write(Socket, asio::buffer(Response), [Self](const asio::error_code&, std::size_t)
		{
			asio::error_code Ignored;
			Self->Socket.shutdown(Tcp::socket::shutdown_both, Ignored);
			Self->Socket.close(Ignored);
		});
	}

	Tcp::socket Socket;
	std::shared_ptr<HostState> State;
	HttpRequest Request;
	std::string TransferId;
	std::string FinalPath;
	std::string PartPath;
	std::ofstream File;
	std::array<char, 64 * 1024> Buffer{};
	std::uint64_t Written = 0;
	std::string Response;
};

class DownloadSession final : public std::enable_shared_from_this<DownloadSession>
{
public:
	DownloadSession(Tcp::socket Socket, std::string Path, std::string Filename, const std::uint64_t Size)
		: Socket(std::move(Socket)), File(std::move(Path), std::ios::binary), Remaining(Size)
	{
		Header = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/octet-stream\r\nContent-Length: " +
		         std::to_string(Size) + "\r\nContent-Disposition: attachment; filename=\"" + SafeFilename(Filename) +
		         "\"\r\nX-Content-Type-Options: nosniff\r\n\r\n";
	}

	void Start()
	{
		if (!File)
		{
			Close();
			return;
		}
		auto Self = shared_from_this();
		asio::async_write(Socket, asio::buffer(Header), [Self](const asio::error_code& Error, std::size_t)
		{
			if (Error) Self->Close();
			else Self->WriteChunk();
		});
	}

private:
	void WriteChunk()
	{
		if (Remaining == 0)
		{
			Close();
			return;
		}
		const std::size_t Wanted = static_cast<std::size_t>(std::min<std::uint64_t>(Buffer.size(), Remaining));
		File.read(Buffer.data(), static_cast<std::streamsize>(Wanted));
		const std::size_t Read = static_cast<std::size_t>(File.gcount());
		if (Read == 0)
		{
			Close();
			return;
		}
		auto Self = shared_from_this();
		asio::async_write(Socket, asio::buffer(Buffer.data(), Read), [Self, Read](const asio::error_code& Error, std::size_t)
		{
			if (Error)
			{
				Self->Close();
				return;
			}
			Self->Remaining -= Read;
			Self->WriteChunk();
		});
	}

	void Close()
	{
		asio::error_code Ignored;
		Socket.shutdown(Tcp::socket::shutdown_both, Ignored);
		Socket.close(Ignored);
	}

	Tcp::socket Socket;
	std::ifstream File;
	std::uint64_t Remaining = 0;
	std::string Header;
	std::array<char, 64 * 1024> Buffer{};
};

class TraceForwardSession final : public std::enable_shared_from_this<TraceForwardSession>
{
public:
	using Completion = std::function<void(bool, std::string)>;

	TraceForwardSession(asio::io_context& Io, std::string Path, const std::uint16_t Port, Completion Complete)
		: Socket(Io), File(std::move(Path), std::ios::binary), Endpoint(asio::ip::address_v4::loopback(), Port),
		  Complete(std::move(Complete))
	{
	}

	void Start()
	{
		std::array<char, 4> Magic{};
		if (!File.read(Magic.data(), static_cast<std::streamsize>(Magic.size())) || Magic != std::array<char, 4>{ 'T', 'R', 'C', '2' })
		{
			Finish(false, "File does not start with TRC2");
			return;
		}
		File.clear();
		File.seekg(0);
		auto Self = shared_from_this();
		Socket.async_connect(Endpoint, [Self](const asio::error_code& Error)
		{
			if (Error)
			{
				Self->Finish(false, "Cannot connect to Unreal Trace Server on port " + std::to_string(Self->Endpoint.port()));
				return;
			}
			Self->WriteChunk();
		});
	}

private:
	void WriteChunk()
	{
		File.read(Buffer.data(), static_cast<std::streamsize>(Buffer.size()));
		const std::size_t Bytes = static_cast<std::size_t>(File.gcount());
		if (Bytes == 0)
		{
			Finish(File.eof(), File.eof() ? std::string{} : "Cannot read trace file");
			return;
		}
		auto Self = shared_from_this();
		asio::async_write(Socket, asio::buffer(Buffer.data(), Bytes), [Self](const asio::error_code& Error, std::size_t)
		{
			if (Error) Self->Finish(false, "Trace forwarding was interrupted");
			else Self->WriteChunk();
		});
	}

	void Finish(const bool Success, std::string Error)
	{
		asio::error_code Ignored;
		Socket.shutdown(Tcp::socket::shutdown_both, Ignored);
		Socket.close(Ignored);
		if (Complete)
		{
			Completion Callback = std::move(Complete);
			Callback(Success, std::move(Error));
		}
	}

	Tcp::socket Socket;
	std::ifstream File;
	Tcp::endpoint Endpoint;
	Completion Complete;
	std::array<char, 64 * 1024> Buffer{};
};

class HttpSession final : public std::enable_shared_from_this<HttpSession>
{
public:
	HttpSession(Tcp::socket Socket, std::shared_ptr<HostState> State, const bool Dashboard)
		: Socket(std::move(Socket)), State(std::move(State)), Dashboard(Dashboard)
	{
	}

	void Start()
	{
		auto Self = shared_from_this();
		asio::async_read_until(Socket, asio::dynamic_buffer(Buffer, MaximumHeaderBytes), "\r\n\r\n",
		                       [Self](const asio::error_code& Error, const std::size_t Bytes)
		{
			Self->OnHeader(Error, Bytes);
		});
	}

private:
	void OnHeader(const asio::error_code& Error, const std::size_t Bytes)
	{
		if (Error)
		{
			Close();
			return;
		}
		if (!ParseRequest(std::string_view(Buffer).substr(0, Bytes), Request))
		{
			ReplyJson(400, ErrorJson("Invalid HTTP request"));
			return;
		}
		Request.Body = Buffer.substr(Bytes);
		if (!Dashboard && Request.Method == "GET" && Request.Path == "/device/connect")
		{
			Wire::WebSocketUpgradeRequest Upgrade;
			const Wire::HttpUpgradeParseResult Parsed = Wire::ParseWebSocketUpgradeRequest(
				{ reinterpret_cast<const std::uint8_t*>(Request.RawHeader.data()), Request.RawHeader.size() }, Upgrade);
			if (Parsed.Status != Wire::HttpUpgradeStatus::Complete)
			{
				ReplyJson(400, ErrorJson("Invalid WebSocket upgrade"));
				return;
			}
			std::make_shared<DeviceSession>(std::move(Socket), State, Request.RawHeader, std::move(Request.Body))->Start();
			return;
		}
		if (!Dashboard && Request.Method == "PUT" && Request.Path.find("/device/transfers/") == 0)
		{
			if (Request.Body.size() > Request.ContentLength) Request.Body.resize(static_cast<std::size_t>(Request.ContentLength));
			std::make_shared<UploadSession>(std::move(Socket), State, std::move(Request))->Start();
			return;
		}
		const std::size_t Maximum = Request.Path.find("/device/transfers/") == 0
			? 0
			: MaximumJsonBytes;
		if (Request.ContentLength > Maximum && Maximum != 0)
		{
			ReplyJson(413, ErrorJson("Request body is too large"));
			return;
		}
		if (Request.ContentLength > Request.Body.size())
		{
			auto Self = shared_from_this();
			asio::async_read(Socket, asio::dynamic_buffer(Request.Body, static_cast<std::size_t>(Request.ContentLength)),
			                 asio::transfer_exactly(static_cast<std::size_t>(Request.ContentLength) - Request.Body.size()),
			                 [Self](const asio::error_code& ReadError, std::size_t)
			{
				if (ReadError) Self->ReplyJson(400, ErrorJson("Request body ended early"));
				else Self->Route();
			});
			return;
		}
		if (Request.Body.size() > Request.ContentLength) Request.Body.resize(static_cast<std::size_t>(Request.ContentLength));
		Route();
	}

	bool TrustedDashboard() const
	{
		const std::string Authority = ":" + std::to_string(State->Endpoints.DashboardPort);
		const std::string Host = Request.Header("host");
		if (Host != "127.0.0.1" + Authority && Host != "localhost" + Authority) return false;
		const std::string Origin = Request.Header("origin");
		if (!Origin.empty() && Origin != "http://127.0.0.1" + Authority && Origin != "http://localhost" + Authority) return false;
		if (Request.Method != "GET")
		{
			if (Request.Header("x-deviceexplorer-request") != "1") return false;
			if (LowerAscii(Request.Header("content-type")).find("application/json") != 0) return false;
		}
		return true;
	}

	void Route()
	{
		if (Dashboard && !TrustedDashboard())
		{
			ReplyJson(403, ErrorJson("Request rejected"));
			return;
		}
		if (Request.Method == "GET" && Request.Path == "/health")
		{
			Json Result;
			Result.SetObject();
			AddString(Result, "status", "ok");
			ReplyJson(200, Result);
			return;
		}
		if (Request.Method == "GET" && Request.Path == "/host-manifest")
		{
			Reply({ 200, "application/json; charset=utf-8", State->Manifest, { { "Cache-Control", "no-store" } } });
			return;
		}
		if (!Dashboard)
		{
			ReplyJson(404, ErrorJson("Route not found"));
			return;
		}
		if (Request.Method == "GET" && Request.Path == "/api/config")
		{
			Json Result;
			Result.SetObject();
			AddInteger(Result, "protocol_version", DeviceProtocolVersion);
			AddInteger(Result, "device_port", State->Endpoints.DevicePort);
			AddString(Result, "service_type", "_deviceexplorer._tcp.local.");
			ReplyJson(200, Result);
			return;
		}
		if (Request.Method == "GET" && Request.Path == "/api/devices")
		{
			Reply({ 200, "application/json; charset=utf-8", State->DevicesJson(), {} });
			return;
		}
		const std::string Prefix = "/api/devices/";
		if (Request.Path.find(Prefix) == 0)
		{
			const std::string Rest = Request.Path.substr(Prefix.size());
			const std::size_t Slash = Rest.find('/');
			if (Slash != std::string::npos)
			{
				RouteDevice(Rest.substr(0, Slash), Rest.substr(Slash + 1));
				return;
			}
		}
		const std::string TransferPrefix = "/api/transfers/";
		if (Request.Path.find(TransferPrefix) == 0)
		{
			RouteTransfer(Request.Path.substr(TransferPrefix.size()));
			return;
		}
		if (Request.Method == "GET")
		{
			ServeStatic();
			return;
		}
		ReplyJson(404, ErrorJson("Route not found"));
	}

	std::string Query(const std::string& Name) const
	{
		const auto Found = Request.Query.find(Name);
		return Found == Request.Query.end() ? std::string{} : Found->second;
	}

	void RouteDevice(const std::string& DeviceId, const std::string& Action)
	{
		if (Action == "logs" && Request.Method == "GET")
		{
			std::uint64_t After = 0;
			(void) ParseUnsigned(Query("after"), After);
			const std::string Body = State->LogsJson(DeviceId, After, Query("category"), Query("verbosity"));
			if (Body.empty()) ReplyJson(404, ErrorJson("Unknown device"));
			else Reply({ 200, "application/json; charset=utf-8", Body, {} });
			return;
		}

		Json Message;
		Message.SetObject();
		if (Action == "log-categories" && Request.Method == "GET")
		{
			AddString(Message, "type", "list_log_categories");
		}
		else if (Action == "console-objects" && Request.Method == "GET")
		{
			AddString(Message, "type", "list_console_objects");
			AddString(Message, "query", Query("q"));
			AddString(Message, "scope", Query("scope"));
			AddString(Message, "source", Query("source"));
			AddString(Message, "kind", Query("kind"));
			AddBoolean(Message, "index", Query("index") == "1");
			AddBoolean(Message, "refresh", Query("refresh") == "1");
			std::uint64_t Limit = 400;
			(void) ParseUnsigned(Query("limit"), Limit);
			AddUnsigned(Message, "limit", Limit == 0 ? 400 : std::min<std::uint64_t>(Limit, 2000));
		}
		else if (Action == "files" && Request.Method == "GET")
		{
			AddString(Message, "type", "list_files");
			AddString(Message, "root", Query("root"));
			AddString(Message, "path", Query("path"));
		}
		else if (Action == "module-data" && Request.Method == "GET" && !Query("module").empty())
		{
			AddString(Message, "type", "get_module_data");
			AddString(Message, "module", Query("module"));
		}
		else if (Request.Method == "POST")
		{
			Json Body;
			if (!Wire::ParseJson({ reinterpret_cast<const std::uint8_t*>(Request.Body.data()), Request.Body.size() }, Body) ||
			    Body.GetType() != Wire::JsonType::Object)
			{
				ReplyJson(400, ErrorJson("Invalid JSON"));
				return;
			}
			if (Action == "transfers")
			{
				const std::string Result = State->CreateTransfer(DeviceId,
				                                                StringMember(Body, "root"),
				                                                StringMember(Body, "path"),
				                                                BoolMember(Body, "archive"));
				if (Result.empty()) ReplyJson(404, ErrorJson("Device is offline or transfer path is invalid"));
				else Reply({ 202, "application/json; charset=utf-8", Result, { { "Cache-Control", "no-store" } } });
				return;
			}
			if (Action == "command")
			{
				AddString(Message, "type", "execute_command");
				AddString(Message, "command", StringMember(Body, "command"));
				AddString(Message, "command_id", StringMember(Body, "command_id"));
				AddString(Message, "arguments", StringMember(Body, "arguments"));
			}
			else if (Action == "log-verbosity" && Member(Body, "entries"))
			{
				AddString(Message, "type", "set_log_verbosity");
				AddCopy(Message, "entries", Member(Body, "entries"), true);
				AddBoolean(Message, "persist", BoolMember(Body, "persist"));
				AddBoolean(Message, "auto_revert", BoolMember(Body, "auto_revert", true));
			}
			else if (Action == "module-action" && !StringMember(Body, "module").empty() && !StringMember(Body, "action").empty())
			{
				AddString(Message, "type", "invoke_module_action");
				AddString(Message, "module", StringMember(Body, "module"));
				AddString(Message, "action", StringMember(Body, "action"));
				AddCopy(Message, "parameters", Member(Body, "parameters"));
			}
			else
			{
				ReplyJson(400, ErrorJson("Invalid request"));
				return;
			}
		}
		else
		{
			ReplyJson(404, ErrorJson("Route not found"));
			return;
		}

		auto Self = shared_from_this();
		if (!State->SendRequest(DeviceId, std::move(Message), [Self](std::string Result)
		{
			if (Result.empty()) Self->ReplyJson(504, ErrorJson("Device request timed out"));
			else Self->Reply({ 200, "application/json; charset=utf-8", std::move(Result), {} });
		}))
		{
			ReplyJson(404, ErrorJson("Device is offline"));
		}
	}

	void RouteTransfer(const std::string& Remainder)
	{
		const std::size_t Slash = Remainder.find('/');
		const std::string Id = Remainder.substr(0, Slash);
		const std::string Action = Slash == std::string::npos ? std::string{} : Remainder.substr(Slash + 1);
		TransferState* Transfer = State->FindTransfer(Id);
		if (!Transfer)
		{
			ReplyJson(404, ErrorJson("Unknown transfer"));
			return;
		}
		if (Action.empty() && Request.Method == "GET")
		{
			Reply({ 200, "application/json; charset=utf-8", State->TransferJson(*Transfer), { { "Cache-Control", "no-store" } } });
			return;
		}
		if (Action == "download" && Request.Method == "GET")
		{
			if (Transfer->State != "ready")
			{
				ReplyJson(409, ErrorJson("Transfer is not ready"));
				return;
			}
			std::error_code Error;
			const std::uint64_t Size = std::filesystem::file_size(Transfer->LocalPath, Error);
			if (Error)
			{
				ReplyJson(404, ErrorJson("Transfer file is missing"));
				return;
			}
			Replied = true;
			std::make_shared<DownloadSession>(std::move(Socket), Transfer->LocalPath, Transfer->Filename, Size)->Start();
			return;
		}
		if (Action == "trace" && Request.Method == "POST")
		{
			if (Transfer->State != "ready")
			{
				ReplyJson(409, ErrorJson("Transfer is not ready"));
				return;
			}
			auto Self = shared_from_this();
			std::make_shared<TraceForwardSession>(State->Io, Transfer->LocalPath, State->Config.TracePort,
				[Self](const bool Success, std::string Error)
				{
					if (!Success)
					{
						Self->ReplyJson(502, ErrorJson(Error.empty() ? "Trace forwarding failed" : Error));
						return;
					}
					Json Result;
					Result.SetObject();
					AddString(Result, "status", "sent");
					Self->ReplyJson(200, Result);
				})->Start();
			return;
		}
		ReplyJson(404, ErrorJson("Route not found"));
	}

	void ServeStatic()
	{
		if (State->Config.WebRoot.empty())
		{
			ReplyJson(404, ErrorJson("File not found"));
			return;
		}
		const std::string Relative = Request.Path == "/" ? "index.html" : Request.Path.substr(1);
		const std::string Normalized = NormalizeRelativePath(Relative);
		if (Normalized.empty() || Normalized != Relative)
		{
			ReplyJson(403, ErrorJson("Invalid path"));
			return;
		}
		std::error_code Error;
		const std::filesystem::path Root = std::filesystem::weakly_canonical(State->Config.WebRoot, Error);
		if (Error)
		{
			ReplyJson(404, ErrorJson("File not found"));
			return;
		}
		const std::filesystem::path Full = std::filesystem::weakly_canonical(Root / Normalized, Error);
		if (Error)
		{
			ReplyJson(404, ErrorJson("File not found"));
			return;
		}
		const std::filesystem::path RelativeCheck = std::filesystem::relative(Full, Root, Error);
		if (Error || RelativeCheck.empty() || *RelativeCheck.begin() == "..")
		{
			ReplyJson(403, ErrorJson("Invalid path"));
			return;
		}
		std::ifstream File(Full, std::ios::binary);
		if (!File)
		{
			ReplyJson(404, ErrorJson("File not found"));
			return;
		}
		std::string Body((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
		const bool Immutable = Normalized.find("assets/") == 0;
		Reply({ 200,
		        ContentTypeForPath(Full),
		        std::move(Body),
		        { { "Cache-Control", Immutable ? "public, max-age=31536000, immutable" : "no-cache" },
		          { "Content-Security-Policy", "default-src 'self'; connect-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; frame-ancestors 'none'" } } });
	}

	void ReplyJson(const int Status, const Json& Body)
	{
		Reply({ Status, "application/json; charset=utf-8", Serialize(Body), { { "Cache-Control", "no-store" } } });
	}

	void Reply(HttpResponse Response)
	{
		if (Replied) return;
		Replied = true;
		ResponseBytes = EncodeResponse(Response);
		auto Self = shared_from_this();
		asio::async_write(Socket, asio::buffer(ResponseBytes), [Self](const asio::error_code&, std::size_t)
		{
			Self->Close();
		});
	}

	void Close()
	{
		asio::error_code Ignored;
		Socket.shutdown(Tcp::socket::shutdown_both, Ignored);
		Socket.close(Ignored);
	}

	Tcp::socket Socket;
	std::shared_ptr<HostState> State;
	bool Dashboard = false;
	bool Replied = false;
	std::string Buffer;
	std::string ResponseBytes;
	HttpRequest Request;
};
}    // namespace RuntimeDetail

using RuntimeDetail::DeviceChannel;
using RuntimeDetail::HostState;
using RuntimeDetail::HttpSession;
using RuntimeDetail::Tcp;

struct HostRuntime::Implementation
{
	Implementation(asio::io_context& Io, const HostConfig& Config, BoundEndpoints Endpoints, std::string Manifest)
		: State(std::make_shared<HostState>(Io, Config, Endpoints, std::move(Manifest))),
		  Mdns(std::make_unique<MdnsAdvertiser>(Io, Config, std::move(Endpoints)))
	{
		State->StartMaintenance();
		(void) Mdns->Start();
	}

	std::shared_ptr<HostState> State;
	std::unique_ptr<MdnsAdvertiser> Mdns;
};

HostRuntime::HostRuntime(asio::io_context& Io,
	                     const HostConfig& Config,
	                     BoundEndpoints Endpoints,
	                     std::string ManifestJson)
	: Impl(std::make_unique<Implementation>(Io, Config, std::move(Endpoints), std::move(ManifestJson)))
{
}

HostRuntime::~HostRuntime() = default;

void HostRuntime::Accept(Tcp::socket Socket, const bool Dashboard)
{
	std::make_shared<HttpSession>(std::move(Socket), Impl->State, Dashboard)->Start();
}

void HostRuntime::Stop()
{
	Impl->Mdns->Stop();
	Impl->State->StopMaintenance();
	for (auto& Pair : Impl->State->Devices)
	{
		if (const std::shared_ptr<DeviceChannel> Channel = Pair.second.Channel.lock()) Channel->Close();
	}
	Impl->State->Devices.clear();
	for (auto& Pair : Impl->State->Pending)
	{
		Pair.second->Timer.cancel();
		Pair.second->Complete({});
	}
	Impl->State->Pending.clear();
}
}    // namespace DeviceExplorer::Host
