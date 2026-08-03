#include "DeviceExplorerHttpUpgrade.h"
#include "DeviceExplorerMdns.h"
#include "DeviceExplorerProtocol.h"
#include "DeviceExplorerWebSocket.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
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
	CHECK(DeviceExplorer::DeviceProtocolVersion == 9);
	const std::string Key = "dGhlIHNhbXBsZSBub25jZQ==";
	std::string Accept;
	CHECK(MakeWebSocketAccept(Key, Accept));
	CHECK(Accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
	CHECK(!MakeWebSocketAccept("not-base64", Accept));

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
	CHECK(SplitResponseResult.Status == HttpUpgradeStatus::Complete);
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
	Source.Token = "golden-token";
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
	CHECK(Parsed.Announcement.Token == Source.Token);
	CHECK(Parsed.Announcement.DevicePort == Source.DevicePort);
	CHECK(Parsed.Announcement.DashboardPort == Source.DashboardPort);
	CHECK(Parsed.Announcement.ProtocolVersion == Source.ProtocolVersion);
	CHECK(Parsed.Announcement.TimeToLive == Source.TimeToLive);
	CHECK(Parsed.Announcement.IPv4Addresses == Source.IPv4Addresses);

	Source.TimeToLive = 0;
	CHECK(EncodeMdnsAnnouncement(Source, Packet, &Error));
	const MdnsAnnouncementParseResult Goodbye = ParseMdnsAnnouncement({ Packet.data(), Packet.size() });
	CHECK(Goodbye.Status == MdnsStatus::Complete);
	CHECK(Goodbye.Announcement.TimeToLive == 0);

	std::vector<std::uint8_t> MissingToken = Packet;
	const std::vector<std::uint8_t> TokenKey = Bytes("token=");
	const auto TokenPosition = std::search(MissingToken.begin(), MissingToken.end(), TokenKey.begin(), TokenKey.end());
	CHECK(TokenPosition != MissingToken.end());
	if (TokenPosition != MissingToken.end())
	{
		*(TokenPosition + 1) = static_cast<std::uint8_t>('a');
	}
	const MdnsAnnouncementParseResult MissingTokenResult =
		ParseMdnsAnnouncement({ MissingToken.data(), MissingToken.size() });
	CHECK(MissingTokenResult.Status == MdnsStatus::Error);
	CHECK(MissingTokenResult.Error == MdnsError::MissingToken);

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
	TestMdnsCodec();
	TestWebSocketRoundTrip();
	TestWebSocketLengthBoundaries();
	TestFragmentationAndUtf8();
	TestInvalidFrames();
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
