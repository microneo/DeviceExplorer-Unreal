#include "DeviceExplorerHttpUpgrade.h"
#include "DeviceExplorerHostManifest.h"
#include "DeviceExplorerJson.h"
#include "DeviceExplorerMdns.h"
#include "DeviceExplorerProtocol.h"
#include "DeviceExplorerWebSocket.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace DeviceExplorer::Wire;

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

std::vector<std::uint8_t> Bytes(const std::string& Text)
{
	return { Text.begin(), Text.end() };
}

void TestHttpUpgrade()
{
	CHECK(DeviceExplorer::DeviceProtocolVersion == 10);
	const std::string Key = "dGhlIHNhbXBsZSBub25jZQ==";
	std::string Accept;
	CHECK(MakeWebSocketAccept(Key, Accept));
	CHECK(Accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
	CHECK(!MakeWebSocketAccept("not-base64", Accept));
	const std::uint8_t Nonce[16] = {
		't', 'h', 'e', ' ', 's', 'a', 'm', 'p', 'l', 'e', ' ', 'n', 'o', 'n', 'c', 'e'
	};
	std::string GeneratedKey;
	CHECK(MakeWebSocketClientKey({ Nonce, sizeof(Nonce) }, GeneratedKey));
	CHECK(GeneratedKey == Key);
	CHECK(!MakeWebSocketClientKey({ Nonce, sizeof(Nonce) - 1 }, GeneratedKey));

	const std::string RequestBytes = SerializeWebSocketUpgradeRequest(
		"/device/connect?token=golden", "127.0.0.1:18081", Key, { { "X-Test", "one" } });
	CHECK(!RequestBytes.empty());
	for (std::size_t Prefix = 0; Prefix + 1 < RequestBytes.size(); ++Prefix)
	{
		WebSocketUpgradeRequest Request;
		const HttpUpgradeParseResult Result = ParseWebSocketUpgradeRequest(
			{ reinterpret_cast<const std::uint8_t*>(RequestBytes.data()), Prefix }, Request);
		CHECK(Result.Status == HttpUpgradeStatus::NeedMoreData);
	}

	WebSocketUpgradeRequest Request;
	const HttpUpgradeParseResult RequestResult = ParseWebSocketUpgradeRequest(
		{ reinterpret_cast<const std::uint8_t*>(RequestBytes.data()), RequestBytes.size() }, Request);
	CHECK(RequestResult.Status == HttpUpgradeStatus::Complete);
	CHECK(RequestResult.ConsumedBytes == RequestBytes.size());
	CHECK(Request.Target == "/device/connect?token=golden");
	CHECK(Request.Host == "127.0.0.1:18081");
	CHECK(Request.Key == Key);

	CHECK(MakeWebSocketAccept(Key, Accept));
	const std::string ResponseBytes = SerializeWebSocketUpgradeResponse(Accept);
	WebSocketUpgradeResponse Response;
	const HttpUpgradeParseResult ResponseResult = ParseWebSocketUpgradeResponse(
		{ reinterpret_cast<const std::uint8_t*>(ResponseBytes.data()), ResponseBytes.size() }, Accept, Response);
	CHECK(ResponseResult.Status == HttpUpgradeStatus::Complete);
	CHECK(Response.Accept == Accept);

	std::string BadRequest = RequestBytes;
	const std::size_t Version = BadRequest.find("Sec-WebSocket-Version: 13");
	CHECK(Version != std::string::npos);
	BadRequest.replace(Version, std::string("Sec-WebSocket-Version: 13").size(), "Sec-WebSocket-Version: 12");
	const HttpUpgradeParseResult BadResult = ParseWebSocketUpgradeRequest(
		{ reinterpret_cast<const std::uint8_t*>(BadRequest.data()), BadRequest.size() }, Request);
	CHECK(BadResult.Status == HttpUpgradeStatus::Error);
	CHECK(BadResult.Error == HttpUpgradeError::UnsupportedWebSocketVersion);

	std::string SpacedName = RequestBytes;
	const std::size_t HostHeader = SpacedName.find("Host:");
	CHECK(HostHeader != std::string::npos);
	SpacedName.insert(HostHeader + 4, " ");
	const HttpUpgradeParseResult SpacedNameResult = ParseWebSocketUpgradeRequest(
		{ reinterpret_cast<const std::uint8_t*>(SpacedName.data()), SpacedName.size() }, Request);
	CHECK(SpacedNameResult.Status == HttpUpgradeStatus::Error);
	CHECK(SpacedNameResult.Error == HttpUpgradeError::InvalidHeader);

	std::string DuplicateKey = RequestBytes;
	const std::size_t RequestEnd = DuplicateKey.find("\r\n\r\n");
	CHECK(RequestEnd != std::string::npos);
	DuplicateKey.insert(RequestEnd, "\r\nSec-WebSocket-Key: " + Key);
	const HttpUpgradeParseResult DuplicateKeyResult = ParseWebSocketUpgradeRequest(
		{ reinterpret_cast<const std::uint8_t*>(DuplicateKey.data()), DuplicateKey.size() }, Request);
	CHECK(DuplicateKeyResult.Status == HttpUpgradeStatus::Error);
	CHECK(DuplicateKeyResult.Error == HttpUpgradeError::InvalidWebSocketKey);

	std::string DuplicateHost = RequestBytes;
	const std::size_t DuplicateHostEnd = DuplicateHost.find("\r\n\r\n");
	CHECK(DuplicateHostEnd != std::string::npos);
	DuplicateHost.insert(DuplicateHostEnd, "\r\nHost: duplicate.example");
	const HttpUpgradeParseResult DuplicateHostResult = ParseWebSocketUpgradeRequest(
		{ reinterpret_cast<const std::uint8_t*>(DuplicateHost.data()), DuplicateHost.size() }, Request);
	CHECK(DuplicateHostResult.Status == HttpUpgradeStatus::Error);
	CHECK(DuplicateHostResult.Error == HttpUpgradeError::InvalidHost);

	std::string MissingHost = RequestBytes;
	const std::size_t HostLine = MissingHost.find("Host: 127.0.0.1:18081\r\n");
	CHECK(HostLine != std::string::npos);
	MissingHost.erase(HostLine, std::string("Host: 127.0.0.1:18081\r\n").size());
	const HttpUpgradeParseResult MissingHostResult = ParseWebSocketUpgradeRequest(
		{ reinterpret_cast<const std::uint8_t*>(MissingHost.data()), MissingHost.size() }, Request);
	CHECK(MissingHostResult.Status == HttpUpgradeStatus::Error);
	CHECK(MissingHostResult.Error == HttpUpgradeError::InvalidHost);

	std::string SplitRequestTokens = RequestBytes;
	const std::size_t RequestUpgrade = SplitRequestTokens.find("Upgrade: websocket");
	const std::size_t RequestConnection = SplitRequestTokens.find("Connection: Upgrade");
	CHECK(RequestUpgrade != std::string::npos);
	CHECK(RequestConnection != std::string::npos);
	SplitRequestTokens.replace(RequestUpgrade, std::string("Upgrade: websocket").size(),
	                           "Upgrade: h2c\r\nUpgrade: WebSocket");
	const std::size_t UpdatedConnection = SplitRequestTokens.find("Connection: Upgrade");
	CHECK(UpdatedConnection != std::string::npos);
	SplitRequestTokens.replace(UpdatedConnection, std::string("Connection: Upgrade").size(),
	                           "Connection: keep-alive\r\nConnection: Upgrade");
	const HttpUpgradeParseResult SplitRequestResult = ParseWebSocketUpgradeRequest(
		{ reinterpret_cast<const std::uint8_t*>(SplitRequestTokens.data()), SplitRequestTokens.size() }, Request);
	CHECK(SplitRequestResult.Status == HttpUpgradeStatus::Complete);

	std::string DuplicateAccept = ResponseBytes;
	const std::size_t ResponseEnd = DuplicateAccept.find("\r\n\r\n");
	CHECK(ResponseEnd != std::string::npos);
	DuplicateAccept.insert(ResponseEnd, "\r\nSec-WebSocket-Accept: " + Accept);
	const HttpUpgradeParseResult DuplicateAcceptResult = ParseWebSocketUpgradeResponse(
		{ reinterpret_cast<const std::uint8_t*>(DuplicateAccept.data()), DuplicateAccept.size() }, Accept, Response);
	CHECK(DuplicateAcceptResult.Status == HttpUpgradeStatus::Error);
	CHECK(DuplicateAcceptResult.Error == HttpUpgradeError::InvalidWebSocketAccept);

	std::string SplitResponseTokens = ResponseBytes;
	const std::size_t ResponseUpgrade = SplitResponseTokens.find("Upgrade: websocket");
	const std::size_t ResponseConnection = SplitResponseTokens.find("Connection: Upgrade");
	CHECK(ResponseUpgrade != std::string::npos);
	CHECK(ResponseConnection != std::string::npos);
	SplitResponseTokens.replace(ResponseUpgrade, std::string("Upgrade: websocket").size(),
	                            "Upgrade: h2c\r\nUpgrade: websocket");
	const std::size_t UpdatedResponseConnection = SplitResponseTokens.find("Connection: Upgrade");
	CHECK(UpdatedResponseConnection != std::string::npos);
	SplitResponseTokens.replace(UpdatedResponseConnection, std::string("Connection: Upgrade").size(),
	                            "Connection: keep-alive\r\nConnection: upgrade");
	const HttpUpgradeParseResult SplitResponseResult = ParseWebSocketUpgradeResponse(
		{ reinterpret_cast<const std::uint8_t*>(SplitResponseTokens.data()), SplitResponseTokens.size() }, Accept, Response);
	CHECK(SplitResponseResult.Status == HttpUpgradeStatus::Error);
	CHECK(SplitResponseResult.Error == HttpUpgradeError::InvalidUpgrade);

	std::string DuplicateResponseUpgrade = ResponseBytes;
	const std::size_t DuplicateUpgradeEnd = DuplicateResponseUpgrade.find("\r\n\r\n");
	CHECK(DuplicateUpgradeEnd != std::string::npos);
	DuplicateResponseUpgrade.insert(DuplicateUpgradeEnd, "\r\nUpgrade: websocket");
	const HttpUpgradeParseResult DuplicateUpgradeResult = ParseWebSocketUpgradeResponse(
		{ reinterpret_cast<const std::uint8_t*>(DuplicateResponseUpgrade.data()), DuplicateResponseUpgrade.size() }, Accept, Response);
	CHECK(DuplicateUpgradeResult.Status == HttpUpgradeStatus::Error);
	CHECK(DuplicateUpgradeResult.Error == HttpUpgradeError::InvalidUpgrade);
}

void TestHostManifest()
{
	HostManifest Source;
	Source.BuildId = "test-build-42";
	CHECK(IsValidHostManifest(Source));

	std::string Json;
	JsonError Error = JsonError::InvalidInput;
	CHECK(SerializeHostManifest(Source, Json, &Error));
	CHECK(Error == JsonError::None);
	CHECK(Json.find("\"host_version\":\"0.5.0\"") != std::string::npos);
	CHECK(Json.find("\"device_protocol_min\":10") != std::string::npos);

	HostManifest Parsed;
	CHECK(ParseHostManifest(
		{ reinterpret_cast<const std::uint8_t*>(Json.data()), Json.size() }, Parsed, &Error));
	CHECK(Parsed.ManifestVersion == DeviceExplorer::HostManifestVersion);
	CHECK(Parsed.HostVersionText == DeviceExplorer::HostVersion);
	CHECK(Parsed.BuildId == Source.BuildId);
	CHECK(Parsed.DeviceProtocol.Minimum == DeviceExplorer::DeviceProtocolVersion);
	CHECK(Parsed.DeviceProtocol.Maximum == DeviceExplorer::DeviceProtocolVersion);
	CHECK(Parsed.WebApi.Minimum == DeviceExplorer::WebApiVersion);
	CHECK(Parsed.PeerProtocol.Minimum == 0);

	Source.DeviceProtocol.Maximum = Source.DeviceProtocol.Minimum - 1;
	CHECK(!SerializeHostManifest(Source, Json, &Error));
	CHECK(Error == JsonError::InvalidInput);

	const std::string Invalid =
		"{\"manifest_version\":1,\"host_version\":\"0.5.0\",\"build_id\":\"x\","
		"\"device_protocol_min\":10,\"device_protocol_max\":10,\"web_api_min\":1,"
		"\"web_api_max\":1,\"peer_protocol_min\":0,\"peer_protocol_max\":1}";
	CHECK(!ParseHostManifest(
		{ reinterpret_cast<const std::uint8_t*>(Invalid.data()), Invalid.size() }, Parsed, &Error));
	CHECK(Error == JsonError::InvalidInput);
}

void TestMdnsCodec()
{
	std::vector<std::uint8_t> Query;
	MdnsError Error = MdnsError::InvalidInput;
	CHECK(EncodeMdnsQuery(DeviceExplorerMdnsServiceName, Query, &Error));
	CHECK(Error == MdnsError::None);
	CHECK(Query.size() > 12);
	const MdnsQueryParseResult QueryResult = ParseMdnsQuery(
		{ Query.data(), Query.size() },
		DeviceExplorerMdnsServiceName,
		"DeviceExplorer-test._deviceexplorer._tcp.local",
		"test-deviceexplorer.local");
	CHECK(QueryResult.Status == MdnsStatus::Complete);
	CHECK(QueryResult.Match.Name == DeviceExplorerMdnsServiceName);
	CHECK(QueryResult.Match.Type == 12);

	std::vector<std::uint8_t> CompressedQuery = Query;
	CHECK(CompressedQuery.size() >= 4);
	CompressedQuery[5] = 2;    // QDCOUNT
	CompressedQuery[CompressedQuery.size() - 4] = 0;
	CompressedQuery[CompressedQuery.size() - 3] = 28;    // first question is AAAA and does not match
	CompressedQuery.insert(CompressedQuery.end(), { 0xC0, 0x0C, 0, 12, 0, 1 });
	const MdnsQueryParseResult CompressedQueryResult = ParseMdnsQuery(
		{ CompressedQuery.data(), CompressedQuery.size() },
		DeviceExplorerMdnsServiceName,
		"DeviceExplorer-test._deviceexplorer._tcp.local",
		"test-deviceexplorer.local");
	CHECK(CompressedQueryResult.Status == MdnsStatus::Complete);
	CHECK(CompressedQueryResult.Match.Name == DeviceExplorerMdnsServiceName);

	std::vector<std::uint8_t> PointerLoop(12, 0);
	PointerLoop[5] = 1;
	PointerLoop.insert(PointerLoop.end(), { 0xC0, 0x0C, 0, 12, 0, 1 });
	const MdnsQueryParseResult PointerLoopResult = ParseMdnsQuery(
		{ PointerLoop.data(), PointerLoop.size() },
		DeviceExplorerMdnsServiceName,
		"DeviceExplorer-test._deviceexplorer._tcp.local",
		"test-deviceexplorer.local");
	CHECK(PointerLoopResult.Status == MdnsStatus::Error);
	CHECK(PointerLoopResult.Error == MdnsError::MalformedPacket);

	const MdnsQueryParseResult NoMatchQuery = ParseMdnsQuery(
		{ Query.data(), Query.size() },
		"_different._tcp.local",
		"different._different._tcp.local",
		"different.local");
	CHECK(NoMatchQuery.Status == MdnsStatus::NoMatch);

	MdnsServiceAnnouncement Source;
	Source.InstanceName = "DeviceExplorer-test._deviceexplorer._tcp.local";
	Source.HostName = "test-deviceexplorer.local";
	Source.TokenFingerprint = "0123456789abcdef";
	Source.DevicePort = 18081;
	Source.DashboardPort = 18080;
	Source.ProtocolVersion = DeviceExplorer::DeviceProtocolVersion;
	Source.TimeToLive = 120;
	Source.IPv4Addresses = { { 192, 168, 31, 134 }, { 10, 20, 30, 40 } };

	std::vector<std::uint8_t> Packet;
	CHECK(EncodeMdnsAnnouncement(Source, Packet, &Error));
	CHECK(Error == MdnsError::None);
	const MdnsAnnouncementParseResult Parsed = ParseMdnsAnnouncement({ Packet.data(), Packet.size() });
	CHECK(Parsed.Status == MdnsStatus::Complete);
	CHECK(Parsed.Announcement.ServiceName == Source.ServiceName);
	CHECK(Parsed.Announcement.InstanceName == Source.InstanceName);
	CHECK(Parsed.Announcement.HostName == Source.HostName);
	CHECK(Parsed.Announcement.TokenFingerprint == Source.TokenFingerprint);
	CHECK(Parsed.Announcement.DevicePort == Source.DevicePort);
	CHECK(Parsed.Announcement.DashboardPort == Source.DashboardPort);
	CHECK(Parsed.Announcement.ProtocolVersion == Source.ProtocolVersion);
	CHECK(Parsed.Announcement.TimeToLive == Source.TimeToLive);
	CHECK(Parsed.Announcement.IPv4Addresses == Source.IPv4Addresses);

	std::vector<std::uint8_t> WithOpt = Packet;
	WithOpt[11] = 1;    // ARCOUNT
	WithOpt.insert(WithOpt.end(), {
		0,             // root owner name
		0, 41,         // OPT
		0x10, 0,       // 4096-byte UDP payload
		0, 0, 0, 0,    // extended RCODE, version and flags
		0, 0           // empty option data
	});
	const MdnsAnnouncementParseResult WithOptParsed =
		ParseMdnsAnnouncement({ WithOpt.data(), WithOpt.size() });
	CHECK(WithOptParsed.Status == MdnsStatus::Complete);
	CHECK(WithOptParsed.Announcement.InstanceName == Source.InstanceName);

	MdnsServiceAnnouncement EscapedSource = Source;
	EscapedSource.InstanceName = "DeviceExplorer\\.Lab\\\\One._deviceexplorer._tcp.local";
	CHECK(EncodeMdnsAnnouncement(EscapedSource, Packet, &Error));
	const MdnsAnnouncementParseResult Escaped = ParseMdnsAnnouncement({ Packet.data(), Packet.size() });
	CHECK(Escaped.Status == MdnsStatus::Complete);
	CHECK(Escaped.Announcement.InstanceName == EscapedSource.InstanceName);
	std::vector<std::uint8_t> ReencodedEscaped;
	CHECK(EncodeMdnsAnnouncement(Escaped.Announcement, ReencodedEscaped, &Error));
	CHECK(ReencodedEscaped == Packet);
	std::vector<std::uint8_t> LiteralDotQuery;
	CHECK(EncodeMdnsQuery("literal\\.", LiteralDotQuery, &Error));
	const MdnsQueryParseResult LiteralDotResult = ParseMdnsQuery(
		{ LiteralDotQuery.data(), LiteralDotQuery.size() }, "literal\\.", "other.local", "other.local");
	CHECK(LiteralDotResult.Status == MdnsStatus::Complete);
	CHECK(LiteralDotResult.Match.Name == "literal\\.");

	std::vector<std::uint8_t> ZeroPadded = Packet;
	ZeroPadded.insert(ZeroPadded.end(), { 0, 0, 0, 0 });
	CHECK(ParseMdnsAnnouncement({ ZeroPadded.data(), ZeroPadded.size() }).Status == MdnsStatus::Complete);
	std::vector<std::uint8_t> NonZeroPadded = Packet;
	NonZeroPadded.push_back(1);
	CHECK(ParseMdnsAnnouncement({ NonZeroPadded.data(), NonZeroPadded.size() }).Error == MdnsError::MalformedPacket);

	Source.TimeToLive = 0;
	CHECK(EncodeMdnsAnnouncement(Source, Packet, &Error));
	const MdnsAnnouncementParseResult Goodbye = ParseMdnsAnnouncement({ Packet.data(), Packet.size() });
	CHECK(Goodbye.Status == MdnsStatus::Complete);
	CHECK(Goodbye.Announcement.TimeToLive == 0);

	std::vector<std::uint8_t> MissingFingerprint = Packet;
	const std::vector<std::uint8_t> FingerprintKey = Bytes("fp=");
	const auto FingerprintPosition = std::search(MissingFingerprint.begin(), MissingFingerprint.end(), FingerprintKey.begin(), FingerprintKey.end());
	CHECK(FingerprintPosition != MissingFingerprint.end());
	if (FingerprintPosition != MissingFingerprint.end())
	{
		*(FingerprintPosition + 1) = static_cast<std::uint8_t>('a');
	}
	const MdnsAnnouncementParseResult MissingFingerprintResult =
		ParseMdnsAnnouncement({ MissingFingerprint.data(), MissingFingerprint.size() });
	CHECK(MissingFingerprintResult.Status == MdnsStatus::Error);
	CHECK(MissingFingerprintResult.Error == MdnsError::MissingFingerprint);

	const MdnsAnnouncementParseResult OtherService =
		ParseMdnsAnnouncement({ Packet.data(), Packet.size() }, "_different._tcp.local");
	CHECK(OtherService.Status == MdnsStatus::NoMatch);
	CHECK(OtherService.Error == MdnsError::MissingService);

	CHECK(ParseMdnsAnnouncement({ nullptr, 1 }).Error == MdnsError::InvalidInput);
	CHECK(!EncodeMdnsQuery("bad..name", Query, &Error));
	CHECK(Error == MdnsError::InvalidName);
	Source.DevicePort = 0;
	CHECK(!EncodeMdnsAnnouncement(Source, Packet, &Error));
	CHECK(Error == MdnsError::InvalidAnnouncement);
}

void TestJsonCodec()
{
	const std::string Text =
		"{\"type\":\"hello\",\"request_id\":\"req-1\",\"number\":-12.5e+2,"
		"\"values\":[true,false,null],\"rocket\":\"\\uD83D\\uDE80\",\"escaped\":\"line\\nnext\"}";
	DeviceExplorerMessage Message;
	JsonError Error = JsonError::InvalidInput;
	CHECK(ParseDeviceExplorerMessage(
		{ reinterpret_cast<const std::uint8_t*>(Text.data()), Text.size() }, Message, {}, &Error));
	CHECK(Error == JsonError::None);
	CHECK(Message.Type == "hello");
	CHECK(Message.HasRequestId);
	CHECK(Message.RequestId == "req-1");
	const JsonValue* Number = Message.Root.FindMember("number");
	CHECK(Number != nullptr);
	CHECK(Number != nullptr && Number->TryGetNumberText() != nullptr);
	CHECK(Number != nullptr && Number->TryGetNumberText() != nullptr && *Number->TryGetNumberText() == "-12.5e+2");
	const JsonValue* Rocket = Message.Root.FindMember("rocket");
	CHECK(Rocket != nullptr);
	CHECK(Rocket != nullptr && Rocket->TryGetString() != nullptr);
	CHECK(Rocket != nullptr && Rocket->TryGetString() != nullptr && *Rocket->TryGetString() == "\xF0\x9F\x9A\x80");

	std::string Serialized;
	CHECK(SerializeDeviceExplorerMessage(Message, Serialized, {}, &Error));
	CHECK(Error == JsonError::None);
	DeviceExplorerMessage RoundTrip;
	CHECK(ParseDeviceExplorerMessage(
		{ reinterpret_cast<const std::uint8_t*>(Serialized.data()), Serialized.size() }, RoundTrip, {}, &Error));
	CHECK(RoundTrip.Type == Message.Type);
	CHECK(RoundTrip.RequestId == Message.RequestId);
	CHECK(RoundTrip.Root.FindMember("rocket") != nullptr);

	DeviceExplorerMessage Built;
	CHECK(MakeDeviceExplorerMessage("ping", "request-2", Built, &Error));
	JsonValue Sequence;
	Sequence.SetUnsignedInteger(42);
	CHECK(Built.Root.SetMember("sequence", std::move(Sequence)));
	CHECK(SerializeDeviceExplorerMessage(Built, Serialized, {}, &Error));
	CHECK(Serialized == "{\"type\":\"ping\",\"request_id\":\"request-2\",\"sequence\":42}");
	Built.Type = "stale";
	CHECK(!SerializeDeviceExplorerMessage(Built, Serialized, {}, &Error));
	CHECK(Error == JsonError::InvalidMessageType);

	JsonValue StringWithNull;
	CHECK(StringWithNull.SetString(std::string("a\0b", 3)));
	CHECK(SerializeJson(StringWithNull, Serialized, {}, &Error));
	CHECK(Serialized == "\"a\\u0000b\"");

	for (const char* NumberText : { "0", "-0", "1", "-12.5e+2", "1E9" })
	{
		CHECK(IsValidJsonNumber(NumberText));
	}
	for (const char* NumberText : { "", "-", "+1", "01", "1.", ".1", "1e", "nan", "inf" })
	{
		CHECK(!IsValidJsonNumber(NumberText));
	}

	const auto CheckMessageError = [&Error](const std::string& Invalid, const JsonError Expected)
	{
		DeviceExplorerMessage Ignored;
		CHECK(!ParseDeviceExplorerMessage(
			{ reinterpret_cast<const std::uint8_t*>(Invalid.data()), Invalid.size() }, Ignored, {}, &Error));
		CHECK(Error == Expected);
	};
	CheckMessageError("[]", JsonError::RootNotObject);
	CheckMessageError("{}", JsonError::MissingMessageType);
	CheckMessageError("{\"type\":1}", JsonError::InvalidMessageType);
	CheckMessageError("{\"type\":\"hello\",\"request_id\":1}", JsonError::InvalidRequestId);
	CheckMessageError("{\"type\":\"one\",\"type\":\"two\"}", JsonError::DuplicateKey);
	CheckMessageError("{\"type\":\"hello\"} trailing", JsonError::TrailingData);
	CheckMessageError("{\"type\":\"hello\",\"bad\":01}", JsonError::InvalidNumber);
	CheckMessageError("{\"type\":\"hello\",\"bad\":\"\\uD800\"}", JsonError::InvalidUnicode);
	CheckMessageError("{\"type\":\"hello\",\"bad\":\"\\x\"}", JsonError::InvalidEscape);

	std::string InvalidUtf8 = "{\"type\":\"";
	InvalidUtf8.push_back(static_cast<char>(0xC0));
	InvalidUtf8 += "\"}";
	CheckMessageError(InvalidUtf8, JsonError::InvalidUnicode);

	JsonLimits Tiny;
	Tiny.MaximumDocumentBytes = Text.size() - 1;
	CHECK(!ParseDeviceExplorerMessage(
		{ reinterpret_cast<const std::uint8_t*>(Text.data()), Text.size() }, Message, Tiny, &Error));
	CHECK(Error == JsonError::DocumentTooLarge);
	Tiny = {};
	Tiny.MaximumDepth = 1;
	const std::string Nested = "{\"type\":\"hello\",\"nested\":{}}";
	CHECK(!ParseDeviceExplorerMessage(
		{ reinterpret_cast<const std::uint8_t*>(Nested.data()), Nested.size() }, Message, Tiny, &Error));
	CHECK(Error == JsonError::MaximumDepthExceeded);
	Tiny = {};
	Tiny.MaximumNodeCount = 1;
	CHECK(!ParseDeviceExplorerMessage(
		{ reinterpret_cast<const std::uint8_t*>(Text.data()), Text.size() }, Message, Tiny, &Error));
	CHECK(Error == JsonError::MaximumNodeCountExceeded);

	CHECK(!ParseJson({ nullptr, 1 }, StringWithNull, {}, &Error));
	CHECK(Error == JsonError::InvalidInput);

	// Keep this near the default node limit: duplicate detection and lookup must
	// remain indexed instead of turning a bounded 1 MiB document into O(n^2).
	std::string WideObject = "{";
	constexpr std::size_t WideMemberCount = 99000;
	for (std::size_t Index = 0; Index < WideMemberCount; ++Index)
	{
		if (Index > 0) WideObject.push_back(',');
		WideObject += "\"k" + std::to_string(Index) + "\":0";
	}
	WideObject.push_back('}');
	JsonValue WideValue;
	CHECK(ParseJson(
		{ reinterpret_cast<const std::uint8_t*>(WideObject.data()), WideObject.size() }, WideValue, {}, &Error));
	const std::vector<std::string>* WideKeys = WideValue.TryGetObjectKeys();
	CHECK(WideKeys != nullptr && WideKeys->size() == WideMemberCount);
	CHECK(WideValue.FindMember("k98999") != nullptr);
	const std::string EmbeddedLookup = "prefix-k98999-suffix";
	CHECK(WideValue.FindMember(std::string_view(EmbeddedLookup).substr(7, 6)) != nullptr);
}

void TestWebSocketRoundTrip()
{
	WebSocketFrame Source;
	Source.Opcode = WebSocketOpcode::Text;
	Source.Payload.assign(130, static_cast<std::uint8_t>('x'));
	std::vector<std::uint8_t> Encoded;
	CHECK(EncodeWebSocketFrame(Source, WebSocketRole::Client, 0x12345678, Encoded));
	CHECK(Encoded.size() == Source.Payload.size() + 8);
	CHECK(Encoded[1] == (0x80 | 126));

	WebSocketDecoder ServerDecoder(WebSocketRole::Server);
	for (const std::uint8_t Value : Encoded)
	{
		CHECK(ServerDecoder.Consume({ &Value, 1 }));
	}
	WebSocketFrame Decoded;
	CHECK(ServerDecoder.Drain(Decoded));
	CHECK(Decoded.Opcode == WebSocketOpcode::Text);
	CHECK(Decoded.Final);
	CHECK(Decoded.Payload == Source.Payload);
	CHECK(!ServerDecoder.Drain(Decoded));

	WebSocketFrame Pong;
	Pong.Opcode = WebSocketOpcode::Pong;
	Pong.Payload = Bytes("ping-data");
	CHECK(EncodeWebSocketFrame(Pong, WebSocketRole::Server, 0, Encoded));
	WebSocketDecoder ClientDecoder(WebSocketRole::Client);
	CHECK(ClientDecoder.Consume({ Encoded.data(), Encoded.size() }));
	CHECK(ClientDecoder.Drain(Decoded));
	CHECK(Decoded.Opcode == WebSocketOpcode::Pong);
	CHECK(Decoded.Payload == Pong.Payload);
}

void TestOpaqueWebSocketDecoder()
{
	WebSocketFrame Source;
	Source.Opcode = WebSocketOpcode::Text;
	Source.Payload = Bytes("opaque");
	std::vector<std::uint8_t> Encoded;
	CHECK(EncodeWebSocketFrame(Source, WebSocketRole::Server, 0, Encoded));

	WebSocketDecoderHandle* Decoder = CreateWebSocketDecoder(WebSocketRole::Client);
	CHECK(Decoder != nullptr);
	CHECK(ConsumeWebSocketBytes(Decoder, { Encoded.data(), Encoded.size() }));
	CHECK(GetWebSocketDecoderError(Decoder) == WebSocketError::None);
	WebSocketFrame Decoded;
	CHECK(DrainWebSocketFrame(Decoder, Decoded));
	CHECK(Decoded.Payload == Source.Payload);
	DestroyWebSocketDecoder(Decoder);
	CHECK(!ConsumeWebSocketBytes(nullptr, { Encoded.data(), Encoded.size() }));
	CHECK(GetWebSocketDecoderError(nullptr) == WebSocketError::InvalidInput);
}

void TestWebSocketLengthBoundaries()
{
	for (const std::size_t PayloadSize : { 0U, 1U, 125U, 126U, 65535U, 65536U })
	{
		WebSocketFrame Source;
		Source.Opcode = WebSocketOpcode::Binary;
		Source.Payload.resize(PayloadSize);
		for (std::size_t Index = 0; Index < Source.Payload.size(); ++Index)
		{
			Source.Payload[Index] = static_cast<std::uint8_t>(Index & 0xFF);
		}
		std::vector<std::uint8_t> Encoded;
		CHECK(EncodeWebSocketFrame(Source, WebSocketRole::Client, 0x89ABCDEF, Encoded));
		WebSocketDecoder Decoder(WebSocketRole::Server);
		std::size_t Offset = 0;
		while (Offset < Encoded.size())
		{
			const std::size_t Chunk = std::min<std::size_t>(17, Encoded.size() - Offset);
			CHECK(Decoder.Consume({ Encoded.data() + Offset, Chunk }));
			Offset += Chunk;
		}
		WebSocketFrame Decoded;
		CHECK(Decoder.Drain(Decoded));
		CHECK(Decoded.Payload == Source.Payload);
	}
}

void TestFragmentationAndUtf8()
{
	WebSocketFrame First;
	First.Opcode = WebSocketOpcode::Text;
	First.Final = false;
	First.Payload = { 'a', 0xE2 };
	WebSocketFrame Ping;
	Ping.Opcode = WebSocketOpcode::Ping;
	Ping.Payload = Bytes("p");
	WebSocketFrame Last;
	Last.Opcode = WebSocketOpcode::Continuation;
	Last.Payload = { 0x82, 0xAC };

	std::vector<std::uint8_t> Stream;
	for (const WebSocketFrame* Frame : { &First, &Ping, &Last })
	{
		std::vector<std::uint8_t> Encoded;
		CHECK(EncodeWebSocketFrame(*Frame, WebSocketRole::Client, 0x01020304, Encoded));
		Stream.insert(Stream.end(), Encoded.begin(), Encoded.end());
	}
	WebSocketDecoder Decoder(WebSocketRole::Server);
	CHECK(Decoder.Consume({ Stream.data(), Stream.size() }));
	WebSocketFrame Decoded;
	CHECK(Decoder.Drain(Decoded) && Decoded.Opcode == WebSocketOpcode::Text);
	CHECK(Decoder.Drain(Decoded) && Decoded.Opcode == WebSocketOpcode::Ping);
	CHECK(Decoder.Drain(Decoded) && Decoded.Opcode == WebSocketOpcode::Continuation);

	WebSocketFrame InvalidText;
	InvalidText.Opcode = WebSocketOpcode::Text;
	InvalidText.Payload = { 0xC0, 0x80 };
	std::vector<std::uint8_t> Ignored;
	WebSocketError Error = WebSocketError::None;
	CHECK(!EncodeWebSocketFrame(InvalidText, WebSocketRole::Client, 0, Ignored, &Error));
	CHECK(Error == WebSocketError::InvalidUtf8);
}

void TestInvalidFrames()
{
	WebSocketDecoder ServerDecoder(WebSocketRole::Server);
	const std::uint8_t Unmasked[] = { 0x81, 0x00 };
	CHECK(!ServerDecoder.Consume({ Unmasked, sizeof(Unmasked) }));
	CHECK(ServerDecoder.GetError() == WebSocketError::InvalidMask);

	const std::uint8_t Rsv[] = { 0xC1, 0x80, 0, 0, 0, 0 };
	WebSocketDecoder RsvDecoder(WebSocketRole::Server);
	CHECK(!RsvDecoder.Consume({ Rsv, sizeof(Rsv) }));
	CHECK(RsvDecoder.GetError() == WebSocketError::InvalidReservedBits);

	const std::uint8_t UnknownOpcode[] = { 0x83, 0x80, 0, 0, 0, 0 };
	WebSocketDecoder OpcodeDecoder(WebSocketRole::Server);
	CHECK(!OpcodeDecoder.Consume({ UnknownOpcode, sizeof(UnknownOpcode) }));
	CHECK(OpcodeDecoder.GetError() == WebSocketError::UnknownOpcode);

	const std::uint8_t NonMinimal[] = { 0x81, 0xFE, 0, 125 };
	WebSocketDecoder LengthDecoder(WebSocketRole::Server);
	CHECK(!LengthDecoder.Consume({ NonMinimal, sizeof(NonMinimal) }));
	CHECK(LengthDecoder.GetError() == WebSocketError::NonMinimalLength);

	const std::uint8_t Invalid64BitLength[] = { 0x82, 0xFF, 0x80, 0, 0, 0, 0, 0, 0, 0 };
	WebSocketDecoder HighBitDecoder(WebSocketRole::Server);
	CHECK(!HighBitDecoder.Consume({ Invalid64BitLength, sizeof(Invalid64BitLength) }));
	CHECK(HighBitDecoder.GetError() == WebSocketError::FrameTooLarge);

	const std::uint8_t FragmentedPing[] = { 0x09, 0x80, 0, 0, 0, 0 };
	WebSocketDecoder ControlDecoder(WebSocketRole::Server);
	CHECK(!ControlDecoder.Consume({ FragmentedPing, sizeof(FragmentedPing) }));
	CHECK(ControlDecoder.GetError() == WebSocketError::InvalidControlFrame);

	WebSocketFrame BadClose;
	BadClose.Opcode = WebSocketOpcode::Close;
	BadClose.Payload = { 0x03 };
	std::vector<std::uint8_t> Encoded;
	WebSocketError Error = WebSocketError::None;
	CHECK(!EncodeWebSocketFrame(BadClose, WebSocketRole::Server, 0, Encoded, &Error));
	CHECK(Error == WebSocketError::InvalidClosePayload);

	WebSocketFrame BadCloseReason;
	BadCloseReason.Opcode = WebSocketOpcode::Close;
	BadCloseReason.Payload = { 0x03, 0xE8, 0xC0, 0x80 };
	CHECK(!EncodeWebSocketFrame(BadCloseReason, WebSocketRole::Server, 0, Encoded, &Error));
	CHECK(Error == WebSocketError::InvalidUtf8);

	WebSocketLimits Limits;
	Limits.MaximumFramePayloadBytes = 16;
	Limits.MaximumMessagePayloadBytes = 3;
	WebSocketFrame TooLarge;
	TooLarge.Opcode = WebSocketOpcode::Binary;
	TooLarge.Payload = { 1, 2, 3, 4 };
	CHECK(EncodeWebSocketFrame(TooLarge, WebSocketRole::Client, 0, Encoded));
	WebSocketDecoder LimitedDecoder(WebSocketRole::Server, Limits);
	CHECK(!LimitedDecoder.Consume({ Encoded.data(), Encoded.size() }));
	CHECK(LimitedDecoder.GetError() == WebSocketError::MessageTooLarge);

	WebSocketLimits QueueLimits;
	QueueLimits.MaximumQueuedFrames = 8;
	std::vector<std::uint8_t> EmptyFrames;
	WebSocketFrame Empty;
	Empty.Opcode = WebSocketOpcode::Binary;
	for (std::size_t Index = 0; Index < QueueLimits.MaximumQueuedFrames + 1; ++Index)
	{
		std::vector<std::uint8_t> OneFrame;
		CHECK(EncodeWebSocketFrame(Empty, WebSocketRole::Server, 0, OneFrame));
		EmptyFrames.insert(EmptyFrames.end(), OneFrame.begin(), OneFrame.end());
	}
	WebSocketDecoder QueueLimitedDecoder(WebSocketRole::Client, QueueLimits);
	CHECK(!QueueLimitedDecoder.Consume({ EmptyFrames.data(), EmptyFrames.size() }));
	CHECK(QueueLimitedDecoder.GetError() == WebSocketError::FrameQueueFull);
}

void TestDeterministicMalformedInputs()
{
	std::uint32_t State = 0xD1C0DEC5U;
	const auto NextByte = [&State]()
	{
		State = State * 1664525U + 1013904223U;
		return static_cast<std::uint8_t>(State >> 24);
	};
	for (std::size_t Iteration = 0; Iteration < 20000; ++Iteration)
	{
		State = State * 1664525U + 1013904223U;
		std::vector<std::uint8_t> Input((State >> 16) % 513U);
		for (std::uint8_t& Byte : Input) Byte = NextByte();
		const ByteView View{ Input.data(), Input.size() };

		JsonValue Json;
		JsonError JsonFailure = JsonError::None;
		(void) ParseJson(View, Json, {}, &JsonFailure);
		(void) ParseMdnsAnnouncement(View);
		(void) ParseMdnsQuery(View, DeviceExplorerMdnsServiceName, "instance.local", "host.local");
		WebSocketUpgradeRequest Request;
		(void) ParseWebSocketUpgradeRequest(View, Request);
		WebSocketUpgradeResponse Response;
		(void) ParseWebSocketUpgradeResponse(View, "invalid-random-accept", Response);
		WebSocketDecoder Client(WebSocketRole::Client);
		WebSocketDecoder Server(WebSocketRole::Server);
		(void) Client.Consume(View);
		(void) Server.Consume(View);
	}
}

void TestWebSocketBufferedLimit()
{
	WebSocketLimits Limits;
	Limits.MaximumFramePayloadBytes = 16;
	Limits.MaximumMessagePayloadBytes = 16;

	WebSocketFrame Source;
	Source.Opcode = WebSocketOpcode::Binary;
	Source.Payload.assign(16, 0x42);
	std::vector<std::uint8_t> Encoded;
	CHECK(EncodeWebSocketFrame(Source, WebSocketRole::Server, 0, Encoded));
	std::vector<std::uint8_t> Stream = Encoded;
	Stream.insert(Stream.end(), Encoded.begin(), Encoded.end());
	CHECK(Stream.size() > Limits.MaximumFramePayloadBytes + 14);

	WebSocketDecoder Decoder(WebSocketRole::Client, Limits);
	CHECK(Decoder.Consume({ Stream.data(), Stream.size() }));
	WebSocketFrame Decoded;
	CHECK(Decoder.Drain(Decoded));
	CHECK(Decoded.Payload == Source.Payload);
	CHECK(Decoder.Drain(Decoded));
	CHECK(Decoded.Payload == Source.Payload);
	CHECK(!Decoder.Drain(Decoded));

	WebSocketDecoder NullDecoder(WebSocketRole::Client, Limits);
	CHECK(!NullDecoder.Consume({ nullptr, 1 }));
	CHECK(NullDecoder.GetError() == WebSocketError::InvalidInput);

	const std::uint8_t Oversized[] = { 0x82, 17 };
	WebSocketDecoder OversizedDecoder(WebSocketRole::Client, Limits);
	CHECK(!OversizedDecoder.Consume({ Oversized, sizeof(Oversized) }));
	CHECK(OversizedDecoder.GetError() == WebSocketError::FrameTooLarge);
}

void TestRegisteredCloseCodes()
{
	CHECK(IsValidWebSocketCloseCode(1012));
	CHECK(IsValidWebSocketCloseCode(1013));
	CHECK(IsValidWebSocketCloseCode(1014));
	CHECK(!IsValidWebSocketCloseCode(1005));
	CHECK(!IsValidWebSocketCloseCode(1006));
	CHECK(!IsValidWebSocketCloseCode(1015));
}
}    // namespace

int main()
{
	TestHttpUpgrade();
	TestHostManifest();
	TestMdnsCodec();
	TestJsonCodec();
	TestWebSocketRoundTrip();
	TestOpaqueWebSocketDecoder();
	TestWebSocketLengthBoundaries();
	TestFragmentationAndUtf8();
	TestInvalidFrames();
	TestDeterministicMalformedInputs();
	TestWebSocketBufferedLimit();
	TestRegisteredCloseCodes();
	if (Failures != 0)
	{
		std::cerr << Failures << " test(s) failed\n";
		return 1;
	}
	std::cout << "DeviceExplorerWire tests passed\n";
	return 0;
}
